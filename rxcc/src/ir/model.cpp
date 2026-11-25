// 实现 IR 类型名称、操作数创建，以及函数、值和基本块 ID 的分配。
// 这里检查对象是否属于对应模块或函数，值是否先定义后使用由 check.cpp 检查。

#include "ir/model.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <stdexcept>
#include <utility>

namespace rx::ir {

// 返回类型在 LLVM 文本中的名称；不支持的类型返回空串，供其他函数检查。
const char *type_name(Type type) {
    switch (type) {
        case Type::Void:
            return "void";
        case Type::I1:
            return "i1";
        case Type::I8:
            return "i8";
        case Type::I32:
            return "i32";
        case Type::Ptr:
            return "ptr";
    }
    return "";
}

// 判断类型是否是已定义的 Type 枚举值。
bool is_known_type(Type type) { return *type_name(type) != '\0'; }

// 返回整数类型的位宽；非整数类型（void、指针）返回 0。
unsigned integer_width(Type type) {
    switch (type) {
        case Type::I1: return 1;
        case Type::I8: return 8;
        case Type::I32: return 32;
        default: return 0;
    }
}

// 零位宽代表非整数，不能充当扩展的源或截断的目标。
bool is_narrower_integer(Type narrow, Type wide) {
    const auto width = integer_width(narrow);
    return width != 0 && width < integer_width(wide);
}

// 返回二元运算在 LLVM 文本中的指令名，例如 add、sdiv、urem 或 xor。
const char *binary_op_name(BinaryOp operation) {
    switch (operation) {
        case BinaryOp::Add: return "add";
        case BinaryOp::Subtract: return "sub";
        case BinaryOp::Multiply: return "mul";
        case BinaryOp::SignedDivide: return "sdiv";
        case BinaryOp::SignedRemainder: return "srem";
        case BinaryOp::UnsignedDivide: return "udiv";
        case BinaryOp::UnsignedRemainder: return "urem";
        case BinaryOp::And: return "and";
        case BinaryOp::Or: return "or";
        case BinaryOp::Xor: return "xor";
        case BinaryOp::ShiftLeft: return "shl";
        case BinaryOp::ArithmeticShiftRight: return "ashr";
        case BinaryOp::LogicalShiftRight: return "lshr";
    }
    return "";
}

// 判断二元指令是否允许该操作数类型：算术/移位只用于 i32，位运算用于 i1 和 i32。
bool valid_binary_type(BinaryOp operation, Type type) {
    switch (operation) {
        case BinaryOp::Add:
        case BinaryOp::Subtract:
        case BinaryOp::Multiply:
        case BinaryOp::SignedDivide:
        case BinaryOp::SignedRemainder:
        case BinaryOp::UnsignedDivide:
        case BinaryOp::UnsignedRemainder:
        case BinaryOp::ShiftLeft:
        case BinaryOp::ArithmeticShiftRight:
        case BinaryOp::LogicalShiftRight:
            return type == Type::I32;
        case BinaryOp::And:
        case BinaryOp::Or:
        case BinaryOp::Xor:
            return type == Type::I1 || type == Type::I32;
    }
    return false;
}

// 返回比较谓词在 LLVM icmp 文本中的拼写，例如 slt 或 uge。
const char *comparison_name(Comparison comparison) {
    switch (comparison) {
        case Comparison::Equal: return "eq";
        case Comparison::NotEqual: return "ne";
        case Comparison::SignedLessThan: return "slt";
        case Comparison::SignedLessThanOrEqual: return "sle";
        case Comparison::SignedGreaterThan: return "sgt";
        case Comparison::SignedGreaterThanOrEqual: return "sge";
        case Comparison::UnsignedLessThan: return "ult";
        case Comparison::UnsignedLessThanOrEqual: return "ule";
        case Comparison::UnsignedGreaterThan: return "ugt";
        case Comparison::UnsignedGreaterThanOrEqual: return "uge";
    }
    return "";
}

// 判断比较谓词能否用于该操作数类型：bool 只有相等和无符号比较，
// 避免把 i1 的 true 当成有符号的 -1。
bool valid_comparison(Comparison comparison, Type operand_type) {
    if (operand_type != Type::I1 && operand_type != Type::I32)
        return false;
    switch (comparison) {
        case Comparison::SignedLessThan:
        case Comparison::SignedLessThanOrEqual:
        case Comparison::SignedGreaterThan:
        case Comparison::SignedGreaterThanOrEqual:
            return operand_type == Type::I32;
        case Comparison::Equal:
        case Comparison::NotEqual:
        case Comparison::UnsignedLessThan:
        case Comparison::UnsignedLessThanOrEqual:
        case Comparison::UnsignedGreaterThan:
        case Comparison::UnsignedGreaterThanOrEqual:
            return true;
    }
    // 未知谓词不能进入输出器，否则会生成缺少谓词的 icmp。
    return false;
}

namespace {
// i1 只用于计算，存储 bool 时使用 i8。
bool valid_scalar_storage(Type type) {
    return type == Type::I32 || type == Type::I8 || type == Type::Ptr;
}

// 递归检查数组元素；命名结构体的字段和实际布局由布局服务核对。
bool valid_storage_shape(const StorageType &type) {
    switch (type.kind) {
        case StorageType::Kind::Unit:
            return false;
        case StorageType::Kind::Scalar:
            return valid_scalar_storage(type.scalar_type);
        case StorageType::Kind::Array:
            return type.element && valid_storage_shape(*type.element);
        case StorageType::Kind::Struct:
        case StorageType::Kind::Vec:
            return true;
    }
    return false;
}
} // namespace

// 标量读写需要支持的存储类型和非零的 2 的幂对齐。
bool valid_storage(Type type, uint32_t alignment) {
    return std::has_single_bit(alignment) && valid_scalar_storage(type);
}

// 对齐属于整次分配，不必在检查数组元素时重复验证。
bool valid_storage(const StorageType &type, uint32_t alignment) {
    return std::has_single_bit(alignment) && valid_storage_shape(type);
}

// 用指定类型和 32 个二进制位创建常量操作数，不重新推导源语言整数类型。
Operand Operand::constant(Type type, uint32_t bits) {
    return {type, bits};
}
// 用指定类型和值 ID 创建操作数；所属函数和类型是否匹配由使用方检查。
Operand Operand::local(Type type, ValueId value_id) {
    return {type, value_id};
}

// 判断操作数的位模式是否落在其类型允许的范围内：
// i1 只能是 0/1，i8 最大 0xff，i32 接受全部 32 位；
// ptr 只允许 null，其他指针常量没有可表达的来源。
bool valid_constant(const Operand &operand) {
    const auto *bits = operand.constant_bits();
    if (!bits)
        return false;
    switch (operand.type) {
        case Type::I1:
            return *bits <= 1;
        case Type::I8:
            return *bits <= 0xff;
        case Type::I32:
            return true;
        case Type::Ptr:
            return *bits == 0;
        default:
            return false;
    }
}

// 保存函数身份、名称、签名和链接属性，并为参数预分配值 ID。
Function::Function(IRFunctionId function_id, std::string name,
                   FunctionABI function_abi, Linkage linkage)
    : id(function_id), name(std::move(name)), linkage(linkage),
      abi(std::move(function_abi)) {
    value_types.reserve(abi.parameters.size());
    for (const auto &parameter : abi.parameters)
        value_types.push_back(parameter.type);
}

// 为指令结果分配值 ID，并把类型添加到当前函数的值类型表。
Operand Function::new_value(Type type) {
    // id 是当前函数的成员；新值的下标就是追加类型前的表长度。
    const ValueId value{id, value_types.size()};
    value_types.push_back(type);
    return Operand::local(type, value);
}

// 添加一个没有指令的基本块，用所属函数 ID 和添加前的块数量组成块 ID。
BlockId Function::add_block() {
    const BlockId result{id, blocks.size()};
    blocks.push_back({result, {}, std::nullopt});
    return result;
}

// 按 ID 返回可修改的基本块；不属于本函数或下标越界时抛出异常。
BasicBlock &Function::block(BlockId block_id) {
    if (block_id.function != id)
        throw std::logic_error("IR 基本块不属于当前函数");
    return blocks.at(block_id.index);
}
// 按 ID 返回只读基本块，同样检查所属函数和下标。
const BasicBlock &Function::block(BlockId block_id) const {
    if (block_id.function != id)
        throw std::logic_error("IR 基本块不属于当前函数");
    return blocks.at(block_id.index);
}

// 判断值 ID 是否属于本函数、类型是否与记录一致；常量检查类型和位模式是否合法。
// 值类型表中有记录，不代表参数表或指令中已经定义了这个值。
bool Function::contains(const Operand &operand) const {
    if (const auto *value = operand.value_id())
        return value->function == id && value->index < value_types.size() &&
               operand.type != Type::Void &&
               value_types[value->index] == operand.type;
    // 常量没有所属函数，也不占用值编号，可以在任何函数中使用。
    return valid_constant(operand);
}

// 创建模块并为它分配唯一编号，用来区分不同模块中下标相同的函数。
// 用原子计数器保证并发创建模块时编号不重复；它不负责保护模块内部数据。
Module::Module() {
    static std::atomic<uint64_t> next_identity{1};
    identity_ = next_identity.fetch_add(1, std::memory_order_relaxed);
}

namespace {

// 判断字符能否作为符号名首字符；数字不能出现在首位。
bool is_symbol_initial_character(char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') || character == '-' ||
           character == '$' || character == '.' || character == '_';
}

// 判断字符能否出现在符号名中；首字符之外还允许数字。
bool is_symbol_character(char character) {
    return is_symbol_initial_character(character) ||
           (character >= '0' && character <= '9');
}

// 判断 ABI 中的快照或结果槽是否使用聚合存储。
bool is_aggregate_storage(const StorageType &storage) {
    return storage.kind == StorageType::Kind::Array ||
           storage.kind == StorageType::Kind::Struct ||
           storage.kind == StorageType::Kind::Vec;
}

} // namespace

// 检查函数名能否直接写入 LLVM 文本而不加引号；首字符不能是数字。
bool valid_symbol_name(const std::string &name) {
    return !name.empty() && is_symbol_initial_character(name.front()) &&
           std::all_of(name.begin(), name.end(), is_symbol_character);
}

// 检查 ABI 的形参表、按值聚合标记和隐藏结果指针位置是否自洽。
bool valid_function_abi(const FunctionABI &abi) {
    if (!is_known_type(abi.result))
        return false;
    for (const auto &parameter : abi.parameters) {
        if (parameter.type == Type::Void || !is_known_type(parameter.type))
            return false;
        // 只有按值传递的聚合需要快照，IR 参数必须是指向该快照的 ptr。
        if (!parameter.by_value_storage)
            continue;
        if (parameter.type != Type::Ptr || !is_aggregate_storage(*parameter.by_value_storage))
            return false;
    }
    if (abi.result_storage) {
        // 返回存储必须是聚合，不能是无数据类型或标量。
        if (!is_aggregate_storage(*abi.result_storage))
            return false;
        // 结果写入调用者提供的存储，IR 函数本身不直接返回数据。
        if (abi.result != Type::Void)
            return false;
        // 先确认有首参数，再检查它是否是隐藏结果指针。
        if (abi.parameters.empty())
            return false;
        const auto &result_pointer = abi.parameters.front();
        if (result_pointer.type != Type::Ptr)
            return false;
        // 隐藏结果指针直接传地址，不能再标记为按值聚合参数。
        if (result_pointer.by_value_storage)
            return false;
    }
    return true;
}

// 名称和 ABI 均有效时才登记函数，并按其签名分配参数 ID。
IRFunctionId Module::add_function(std::string function_name,
                                  FunctionABI abi, Linkage linkage) {
    if (!valid_function_abi(abi))
        throw std::logic_error("IR FunctionABI 形状无效");
    if (!valid_symbol_name(function_name) || find_function(function_name))
        throw std::logic_error("IR 函数名称重复或非法");
    // 用模块编号和当前函数数量组成新函数的 ID。
    const IRFunctionId function_id{identity_, functions_.size()};
    auto function = std::make_unique<Function>(
        function_id, std::move(function_name), std::move(abi), linkage);
    const auto &stored = functions_.emplace_back(std::move(function));
    try {
        functions_by_name_.emplace(stored->name, stored.get());
    } catch (...) {
        functions_.pop_back();
        throw;
    }
    return function_id;
}

// 按名称查找函数；找不到时返回 nullptr，不自动创建新函数。
Function *Module::find_function(const std::string &function_name) {
    const auto found = functions_by_name_.find(function_name);
    return found == functions_by_name_.end() ? nullptr : found->second;
}
// 在只读模块中按名称查找函数，找不到时返回 nullptr。
const Function *Module::find_function(const std::string &function_name) const {
    const auto found = functions_by_name_.find(function_name);
    return found == functions_by_name_.end() ? nullptr : found->second;
}
// 检查 ID 中的模块编号、函数下标和表中记录，判断函数是否属于本模块。
bool Module::contains(IRFunctionId function_id) const {
    return function_id.module == identity_ &&
           function_id.index < functions_.size() &&
           functions_[function_id.index]->id == function_id;
}
// 按存储类型逐层检查 GEP 下标：第一个下标是基址偏移，
// 后续下标分别选择字段或数组元素；字段/Vec 字段必须是非负常量且在范围内。
bool valid_gep_indices(const Module &module, const StorageType &source_type,
                       const std::vector<Operand> &indices) {
    if (indices.size() < 2 || indices.front().type != Type::I32)
        return false;
    const StorageType *current = &source_type;
    // 第一个下标只沿基址移动，不改变被投影的对象类型。
    for (std::size_t position = 1; position < indices.size(); ++position) {
        const auto &index = indices[position];
        if (index.type != Type::I32)
            return false;
        switch (current->kind) {
            case StorageType::Kind::Struct: {
                const auto *bits = index.constant_bits();
                if (!bits || current->struct_index >= module.named_types().size())
                    return false;
                const auto &named = module.named_type(current->struct_index);
                if (!named.defined || *bits >= named.fields.size())
                    return false;
                current = &named.fields[*bits];
                break;
            }
            case StorageType::Kind::Array:
                if (!current->element)
                    return false;
                current = current->element.get();
                break;
            case StorageType::Kind::Vec: {
                const auto *bits = index.constant_bits();
                // Vec 的三个字段都是标量，选中后不能再投影。
                return bits && *bits < 3 && position + 1 == indices.size();
            }
            case StorageType::Kind::Unit:
            case StorageType::Kind::Scalar:
            default:
                return false;
        }
    }
    return true;
}

// 登记一个命名结构体类型；名称必须可以直接写入 LLVM 文本且不能重复。
std::size_t Module::add_named_type(std::string type_name) {
    if (!valid_symbol_name(type_name))
        throw std::logic_error("IR 类型名称非法");
    for (const auto &existing : named_types_)
        if (existing.name == type_name)
            throw std::logic_error("IR 类型名称重复");
    NamedType type;
    type.name = std::move(type_name);
    named_types_.push_back(std::move(type));
    return named_types_.size() - 1;
}

// 为一个已登记类型补齐字段；每个类型只能定义一次。
void Module::define_named_type(std::size_t index,
                               std::vector<StorageType> fields) {
    if (index >= named_types_.size())
        throw std::logic_error("IR 类型下标无效");
    auto &type = named_types_[index];
    if (type.defined)
        throw std::logic_error("IR 类型字段重复定义");
    type.fields = std::move(fields);
    type.defined = true;
}

// 按类型表下标读取命名结构体；越界时抛出 logic_error。
const NamedType &Module::named_type(std::size_t index) const {
    if (index >= named_types_.size())
        throw std::logic_error("IR 类型下标无效");
    return named_types_[index];
}

// 按 ID 返回可修改的函数；ID 无效或属于其他模块时抛出 logic_error。
Function &Module::function(IRFunctionId function_id) {
    if (!contains(function_id))
        throw std::logic_error("IR 函数身份无效或不属于当前模块");
    return *functions_[function_id.index];
}
// 按 ID 返回只读函数，同样检查 ID 是否有效且属于本模块。
const Function &Module::function(IRFunctionId function_id) const {
    if (!contains(function_id))
        throw std::logic_error("IR 函数身份无效或不属于当前模块");
    return *functions_[function_id.index];
}

} // namespace rx::ir
