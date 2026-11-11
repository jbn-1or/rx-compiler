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
        case Type::Void: return "void";
        case Type::I32: return "i32";
    }
    return "";
}

// 用指定类型和 32 个二进制位创建常量操作数，不重新推导源语言整数类型。
Operand Operand::constant(Type type, uint32_t bits) { return {type, bits}; }
// 用指定类型和值 ID 创建操作数；所属函数和类型是否匹配由使用方检查。
Operand Operand::local(Type type, ValueId id) { return {type, id}; }

// 判断操作数是否保存了 i32 常量；void 和空操作数都不是有效的数据常量。
bool valid_constant(const Operand &operand) {
    return operand.type == Type::I32 && operand.constant_bits();
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

// 判断值 ID 是否属于本函数、类型是否与记录一致；常量只需检查是否为 i32。
// 值类型表中有记录，不代表参数表或指令中已经定义了这个值。
bool Function::contains(const Operand &operand) const {
    if (const auto *value = operand.value_id())
        return value->function == id && value->index < value_types.size() &&
               operand.type != Type::Void && value_types[value->index] == operand.type;
    // 常量没有所属函数，也不占用值编号，可以在任何函数中使用。
    return valid_constant(operand);
}

// 创建模块并为它分配唯一编号，用来区分不同模块中下标相同的函数。
Module::Module(std::string module_name) : name(std::move(module_name)) {
    // 用原子计数器保证并发创建的模块编号不重复；它不负责保护模块中的数据。
    static std::atomic<uint64_t> next_identity{1};
    identity_ = next_identity.fetch_add(1, std::memory_order_relaxed);
}

// 检查函数名能否直接写入 LLVM 文本而不加引号；首字符不能是数字。
bool valid_symbol_name(const std::string &name) {
    const auto initial = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               c == '-' || c == '$' || c == '.' || c == '_';
    };
    return !name.empty() && initial(name.front()) &&
           std::all_of(name.begin(), name.end(), [&](char c) {
               return initial(c) || (c >= '0' && c <= '9');
           });
}

// 检查名称和参数、返回类型，创建函数并分配 ID，函数体随后再添加。
IRFunctionId Module::add_function(std::string function_name, Signature signature,
                                  Linkage linkage) {
    // 名称不能重复，参数不能是 void；检查失败时不向模块添加函数。
    if (!valid_symbol_name(function_name) || find_function(function_name) ||
        !*type_name(signature.result) ||
        std::any_of(signature.parameters.begin(), signature.parameters.end(),
                    [](Type type) { return type == Type::Void || !*type_name(type); }))
        throw std::logic_error("IR 函数名称重复、非法或签名无效");
    // 用模块编号和当前函数数量组成新函数的 ID。
    const IRFunctionId id{identity_, functions_.size()};
    auto function = std::make_unique<Function>();
    function->id = id;
    function->name = std::move(function_name);
    function->linkage = linkage;
    function->signature = std::move(signature);
    // 先为参数分配值 ID，后续指令结果接着编号，不从 0 重新开始。
    for (const auto type : function->signature.parameters)
        function->parameters.push_back(function->new_value(type));
    functions_.push_back(std::move(function));
    return id;
}

// 按名称查找函数；找不到时返回 nullptr，不自动创建新函数。
Function *Module::find_function(const std::string &function_name) {
    for (const auto &function : functions_)
        if (function->name == function_name)
            return function.get();
    return nullptr;
}
// 在只读模块中按名称查找函数，找不到时返回 nullptr。
const Function *Module::find_function(const std::string &function_name) const {
    for (const auto &function : functions_)
        if (function->name == function_name)
            return function.get();
    return nullptr;
}
// 检查 ID 中的模块编号、函数下标和表中记录，判断函数是否属于本模块。
bool Module::contains(IRFunctionId id) const {
    return id.module == identity_ && id.index < functions_.size() &&
           functions_[id.index]->id == id;
}
// 按 ID 返回可修改的函数；ID 无效或属于其他模块时抛出 logic_error。
Function &Module::function(IRFunctionId id) {
    if (!contains(id))
        throw std::logic_error("IR 函数身份无效或不属于当前模块");
    return *functions_[id.index];
}
// 按 ID 返回只读函数，同样检查 ID 是否有效且属于本模块。
const Function &Module::function(IRFunctionId id) const {
    if (!contains(id))
        throw std::logic_error("IR 函数身份无效或不属于当前模块");
    return *functions_[id.index];
}

} // namespace rx::ir
