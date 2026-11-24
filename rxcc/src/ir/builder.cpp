// 实现存储、扩展/截断、call 和 ret 指令的构建，同时检查类型并分配值 ID。
// 无法添加指令时抛出 logic_error，由 generate_ir 捕获并输出错误信息。

#include "ir/builder.hpp"
#include <stdexcept>
#include <utility>

namespace rx::ir {
namespace {
// 条件不满足时抛出 logic_error，并附上说明原因的错误信息。
void require(bool condition, const char *message) {
    if (!condition)
        throw std::logic_error(message);
}

// 校验操作数的类型和函数归属；定义顺序与支配关系由模块检查器验证。
void require_operand(const Function &function, const Operand &operand) {
    require(function.contains(operand), "IR 操作数类型、身份或函数归属无效");
}
} // namespace

// 检查块是否存在于当前模块，再将它选为后续指令的添加位置。
void IRBuilder::set_insert_point(BlockId block_id) {
    module_.function(block_id.function).block(block_id);
    insertion_ = block_id;
}

// 返回当前块所属的函数；没有选中块或块已有终结指令时报告错误。
Function &IRBuilder::open_function() const {
    require(insertion_.has_value(), "IR 指令需要插入块");
    auto &function = module_.function(insertion_->function);
    require(!function.block(*insertion_).terminator, "IR 指令不能添加到已终结的块");
    return function;
}

// 向当前块添加普通指令；有数据结果时先分配值 ID 并记录类型。
Operand IRBuilder::append(Function &function, Type type, Operation operation) {
    Operand value;
    std::optional<ValueId> result;
    // 有结果时才分配值 ID；void 指令只记录操作本身。
    if (type != Type::Void) {
        value = function.new_value(type);
        result = *value.value_id();
    }
    function.block(*insertion_).instructions.push_back(
        {result, type, std::move(operation)});
    return value;
}

// 检查被调用函数是否存在、实参数量和类型是否匹配，再添加 call 指令。
std::optional<Operand> IRBuilder::call(IRFunctionId function_id,
                                       std::vector<Operand> arguments) {
    auto &function = open_function();
    const auto &signature = module_.function(function_id).abi.signature;
    require(arguments.size() == signature.parameters.size(),
            "IR call 参数数量不匹配");
    for (std::size_t argument_index = 0;
         argument_index < arguments.size(); ++argument_index) {
        require_operand(function, arguments[argument_index]);
        require(arguments[argument_index].type ==
                    signature.parameters[argument_index],
                "IR call 参数类型不匹配");
    }
    // 返回 void 的调用也要添加到块中，但没有可供后续指令使用的结果值。
    const auto result = append(function, signature.result,
                               Call{function_id, std::move(arguments)});
    if (signature.result == Type::Void)
        return std::nullopt;
    return result;
}

// 在函数入口分配固定存储，返回可用于后续读写的地址。
Operand IRBuilder::entry_alloca(StorageType storage_type, uint32_t alignment) {
    auto &function = open_function();
    require(valid_storage(storage_type, alignment),
            "IR alloca 存储类型或对齐无效");
    // 单独保存入口分配，不改变当前插入点或正文中已有指令的顺序。
    const auto result = function.new_value(Type::Ptr);
    function.entry_allocas.push_back(
        {*result.value_id(), Type::Ptr, Alloca{storage_type, alignment}});
    return result;
}

// 添加 GEP 指令：基址和每个下标都必须可用，下标统一为 i32，结果类型为 ptr。
Operand IRBuilder::get_element_ptr(StorageType source_type, Operand base,
                                  std::vector<Operand> indices) {
    auto &function = open_function();
    require_operand(function, base);
    require(base.type == Type::Ptr, "IR getelementptr 基址必须为 ptr");
    for (const auto &index : indices)
        require_operand(function, index);
    require(valid_gep_indices(module_, source_type, indices),
            "IR getelementptr 下标与源存储类型不匹配");
    return append(function, Type::Ptr, GetElementPtr{std::move(source_type), base,
                                           std::move(indices)});
}

// 从地址读取指定存储类型的值，并为读取结果分配值 ID。
Operand IRBuilder::load(Type storage_type, Operand address,
                        uint32_t alignment) {
    auto &function = open_function();
    require(valid_storage(storage_type, alignment),
            "IR load 存储类型或对齐无效");
    // 不透明指针不包含存储类型，必须分别检查显式类型和地址。
    require_operand(function, address);
    require(address.type == Type::Ptr, "IR load 地址必须为 ptr");
    return append(function, storage_type, Load{storage_type, address, alignment});
}

// 将指定类型的值写入地址；store 有副作用，但不产生结果值。
void IRBuilder::store(Type storage_type, Operand value, Operand address,
                      uint32_t alignment) {
    auto &function = open_function();
    require(valid_storage(storage_type, alignment),
            "IR store 存储类型或对齐无效");
    // 所有检查先于发射，失败时不会向块中追加部分指令。
    require_operand(function, value);
    require_operand(function, address);
    require(value.type == storage_type && address.type == Type::Ptr,
            "IR store 值或地址类型无效");
    append(function, Type::Void, Store{value, address, alignment});
}

// 添加二元运算；两侧类型必须相同，并且是该指令允许的 i1/i32。
Operand IRBuilder::binary(BinaryOp operation, Operand left, Operand right) {
    auto &function = open_function();
    require_operand(function, left);
    require_operand(function, right);
    require(left.type == right.type &&
                valid_binary_type(operation, left.type),
            "IR 二元运算的操作数类型无效");
    return append(function, left.type, BinaryOperation{operation, left, right});
}

// 添加比较指令；两侧类型相同，结果固定为 i1，谓词必须适合该类型。
Operand IRBuilder::compare(Comparison comparison, Operand left,
                           Operand right) {
    auto &function = open_function();
    require_operand(function, left);
    require_operand(function, right);
    require(left.type == right.type &&
                valid_comparison(comparison, left.type),
            "IR 比较的操作数类型或谓词无效");
    return append(function, Type::I1, Compare{comparison, left, right});
}

// 只允许把较窄整数无符号扩展成较宽整数，失败时不改变块或值表。
Operand IRBuilder::zext(Type target_type, Operand value) {
    auto &function = open_function();
    require_operand(function, value);
    // 位宽必须严格变大，例如 i1→i8、i1→i32 或 i8→i32。
    require(is_narrower_integer(value.type, target_type),
            "IR zext 只支持较窄整数到较宽整数");
    return append(function, target_type, ZExt{value, target_type});
}

// 只允许把较宽整数截断成较窄整数，失败时不改变块或值表。
Operand IRBuilder::trunc(Type target_type, Operand value) {
    auto &function = open_function();
    require_operand(function, value);
    // 源和目标都必须是整数，且位宽严格变小；非整数的位宽 0 不能当作窄整数。
    require(is_narrower_integer(target_type, value.type),
            "IR trunc 只支持较宽整数到较窄整数");
    return append(function, target_type, Trunc{value, target_type});
}

// 检查跳转目标是否属于当前函数、存在且不是入口块。
// 入口块只能由函数调用进入；跳转进入入口会制造无法用普通 CFG 表达的前驱。
void IRBuilder::require_target(const Function &function,
                               BlockId target) const {
    require(target.function == function.id &&
                target.index < function.blocks.size(),
            "IR 跳转目标不存在或不属于当前函数");
    require(target.index != 0, "IR 跳转不能以入口块为目标");
}

// 给当前块添加终结指令，然后取消选中的块，防止继续向它添加指令。
void IRBuilder::terminate(Function &function, Terminator terminator) {
    function.block(*insertion_).terminator = std::move(terminator);
    insertion_.reset();
}

// 检查返回值是否匹配函数返回类型，再添加 ret void 或带值的 ret 指令。
void IRBuilder::ret(std::optional<Operand> value) {
    auto &function = open_function();
    const auto result = function.abi.signature.result;
    // 有返回值时先检查它属于当前函数，再比较类型是否匹配。
    if (value)
        require_operand(function, *value);
    const bool valid = value ? value->type == result : result == Type::Void;
    require(valid, "IR ret 与函数结果类型不匹配");
    terminate(function, Return{value});
}

// 添加无条件跳转；检查目标后关闭当前插入点。
void IRBuilder::branch(BlockId target) {
    auto &function = open_function();
    require_target(function, target);
    terminate(function, Branch{target});
}

// 添加条件跳转；条件和两个目标都先检查，失败时不修改当前块。
void IRBuilder::cond_branch(Operand condition, BlockId when_true,
                            BlockId when_false) {
    auto &function = open_function();
    require_operand(function, condition);
    require(condition.type == Type::I1, "IR condbr 条件必须为 i1");
    require_target(function, when_true);
    require_target(function, when_false);
    terminate(function, CondBranch{condition, when_true, when_false});
}

} // namespace rx::ir
