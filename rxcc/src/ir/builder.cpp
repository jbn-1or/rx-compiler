// 实现向基本块添加 call 和 ret 指令的过程，同时检查类型并分配结果值的 ID。
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
}

// 检查块是否存在于当前模块，再将它选为后续指令的添加位置。
// 这里只选择块，不会清除块中已有的终结指令。
void IRBuilder::set_insert_point(BlockId block) {
    module_.function(block.function).block(block);
    insertion_ = block;
}
// 根据当前选中的块 ID 查找基本块；没有选中块时返回 nullptr。
// 不长期保存块指针，避免添加新块后，旧指针因 vector 扩容而失效。
BasicBlock *IRBuilder::current_block() const {
    return insertion_ ? &module_.function(insertion_->function).block(*insertion_) : nullptr;
}
// 判断当前是否选中了一个尚未结束、可以继续添加 IR 指令的基本块。
bool IRBuilder::is_open() const {
    const auto *block = current_block();
    return block && !block->terminator;
}
// 返回当前块所属的函数；没有选中块或块已有终结指令时报告错误。
Function &IRBuilder::open_function() const {
    require(is_open(), "IR 指令需要尚未终结的插入块");
    return module_.function(insertion_->function);
}
// 判断当前块能否继续添加指令，以及操作数的类型和所属函数是否正确。
bool IRBuilder::can_use(const Operand &operand) const {
    return is_open() && module_.function(insertion_->function).contains(operand);
}
// 检查操作数能否用于当前函数；检查失败时抛出错误，不添加指令。
void IRBuilder::require_operand(const Operand &operand) const {
    require(can_use(operand), "IR 操作数类型、身份或函数归属无效");
}

// 向当前块添加普通指令；有数据结果时先分配值 ID 并记录类型。
Operand IRBuilder::append(Type type, Operation operation) {
    auto &function = open_function();
    const auto value = type == Type::Void ? Operand{} : function.new_value(type);
    current_block()->instructions.push_back({
        value.value_id() ? std::optional{*value.value_id()} : std::nullopt,
        type, std::move(operation)});
    return value;
}

// 检查被调用函数是否存在、实参数量和类型是否匹配，再添加 call 指令。
std::optional<Operand> IRBuilder::call(IRFunctionId id, std::vector<Operand> arguments) {
    open_function();
    const auto &signature = module_.function(id).signature;
    require(arguments.size() == signature.parameters.size(), "IR call 参数数量不匹配");
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        require_operand(arguments[i]);
        require(arguments[i].type == signature.parameters[i], "IR call 参数类型不匹配");
    }
    // 返回 void 的调用也要添加到块中，但没有可供后续指令使用的结果值。
    const auto result = append(signature.result, Call{id, std::move(arguments)});
    return signature.result == Type::Void ? std::nullopt : std::optional{result};
}
// 给当前块添加终结指令，然后取消选中的块，防止继续向它添加指令。
void IRBuilder::terminate(Terminator terminator) {
    open_function();
    current_block()->terminator = std::move(terminator);
    clear_insert_point();
}
// 检查返回值是否匹配函数返回类型，再添加 ret void 或带值的 ret 指令。
void IRBuilder::ret(std::optional<Operand> value) {
    const auto result = open_function().signature.result;
    if (value)
        require_operand(*value);
    require(value ? value->type == result : result == Type::Void,
            "IR ret 与函数结果类型不匹配");
    terminate(Return{value});
}
} // namespace rx::ir
