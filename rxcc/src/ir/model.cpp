// 实现 IR 类型名称、操作数创建，以及函数、值和基本块 ID 的分配。
// 这里检查对象是否属于对应模块或函数，值是否先定义后使用由 check.cpp 检查。

#include "ir/model.hpp"
#include <algorithm>
#include <atomic>
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

// 检查存储类型和对齐是否合法：类型为 i32 或 i8，且对齐值是 2 的幂。
bool valid_storage(Type type, uint32_t alignment) {
    return (type == Type::I32 || type == Type::I8) && alignment != 0 &&
           (alignment & (alignment - 1)) == 0;
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
// i1 只能是 0/1，i8 最大 0xff，i32 接受全部 32 位；其他类型不能是常量。
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
        default:
            return false;
    }
}

// 保存函数身份、名称、签名和链接属性，并为参数预分配值 ID。
Function::Function(IRFunctionId function_id, std::string name,
                   Signature signature, Linkage linkage)
    : id(function_id), name(std::move(name)), linkage(linkage),
      signature(std::move(signature)) {
    value_types.reserve(this->signature.parameters.size());
    parameters.reserve(this->signature.parameters.size());
    for (const auto type : this->signature.parameters)
        parameters.push_back(new_value(type));
}

// 为参数或指令结果分配值 ID，并把类型添加到当前函数的值类型表。
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

// 判断类型是否不能作为函数参数；void 和未知类型都不合法。
bool invalid_parameter_type(Type type) {
    return type == Type::Void || !is_known_type(type);
}

} // namespace

// 检查函数名能否直接写入 LLVM 文本而不加引号；首字符不能是数字。
bool valid_symbol_name(const std::string &name) {
    return !name.empty() && is_symbol_initial_character(name.front()) &&
           std::all_of(name.begin(), name.end(), is_symbol_character);
}

// 检查名称和参数、返回类型，创建函数并分配 ID，函数体随后再添加。
IRFunctionId Module::add_function(std::string function_name,
                                  Signature signature, Linkage linkage) {
    // 名称不能重复，参数不能是 void；检查失败时不向模块添加函数。
    if (!valid_symbol_name(function_name) || find_function(function_name) ||
        !is_known_type(signature.result) ||
        std::any_of(signature.parameters.begin(),
                    signature.parameters.end(), invalid_parameter_type))
        throw std::logic_error("IR 函数名称重复、非法或签名无效");
    // 用模块编号和当前函数数量组成新函数的 ID。
    const IRFunctionId function_id{identity_, functions_.size()};
    auto function = std::make_unique<Function>(
        function_id, std::move(function_name), std::move(signature), linkage);
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
