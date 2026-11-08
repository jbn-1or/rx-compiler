#include "ir/model.hpp"
#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <utility>

namespace rx::ir {

const char *type_name(Type type) {
    switch (type) {
        case Type::Void: return "void";
        case Type::I32: return "i32";
    }
    return "";
}

Operand Operand::constant(Type type, uint32_t bits) { return {type, bits}; }
Operand Operand::local(Type type, ValueId id) { return {type, id}; }

bool valid_constant(const Operand &operand) {
    return operand.type == Type::I32 && operand.constant_bits();
}

Operand Function::new_value(Type type) {
    const ValueId value{id, value_types.size()};
    value_types.push_back(type);
    return Operand::local(type, value);
}

BlockId Function::add_block() {
    const BlockId result{id, blocks.size()};
    blocks.push_back({result, {}, std::nullopt});
    return result;
}

BasicBlock &Function::block(BlockId block_id) {
    if (block_id.function != id)
        throw std::logic_error("IR 基本块不属于当前函数");
    return blocks.at(block_id.index);
}
const BasicBlock &Function::block(BlockId block_id) const {
    if (block_id.function != id)
        throw std::logic_error("IR 基本块不属于当前函数");
    return blocks.at(block_id.index);
}

bool Function::contains(const Operand &operand) const {
    if (const auto *value = operand.value_id())
        return value->function == id && value->index < value_types.size() &&
               operand.type != Type::Void && value_types[value->index] == operand.type;
    return valid_constant(operand);
}

Module::Module(std::string module_name) : name(std::move(module_name)) {
    static std::atomic<uint64_t> next_identity{1};
    identity_ = next_identity.fetch_add(1, std::memory_order_relaxed);
}

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

IRFunctionId Module::add_function(std::string function_name, Signature signature,
                                  Linkage linkage) {
    if (!valid_symbol_name(function_name) || find_function(function_name) ||
        !*type_name(signature.result) ||
        std::any_of(signature.parameters.begin(), signature.parameters.end(),
                    [](Type type) { return type == Type::Void || !*type_name(type); }))
        throw std::logic_error("IR 函数名称重复、非法或签名无效");
    const IRFunctionId id{identity_, functions_.size()};
    auto function = std::make_unique<Function>();
    function->id = id;
    function->name = std::move(function_name);
    function->linkage = linkage;
    function->signature = std::move(signature);
    for (const auto type : function->signature.parameters)
        function->parameters.push_back(function->new_value(type));
    functions_.push_back(std::move(function));
    return id;
}

Function *Module::find_function(const std::string &function_name) {
    for (const auto &function : functions_)
        if (function->name == function_name)
            return function.get();
    return nullptr;
}
const Function *Module::find_function(const std::string &function_name) const {
    for (const auto &function : functions_)
        if (function->name == function_name)
            return function.get();
    return nullptr;
}
bool Module::contains(IRFunctionId id) const {
    return id.module == identity_ && id.index < functions_.size() &&
           functions_[id.index]->id == id;
}
Function &Module::function(IRFunctionId id) {
    if (!contains(id))
        throw std::logic_error("IR 函数身份无效或不属于当前模块");
    return *functions_[id.index];
}
const Function &Module::function(IRFunctionId id) const {
    if (!contains(id))
        throw std::logic_error("IR 函数身份无效或不属于当前模块");
    return *functions_[id.index];
}

} // namespace rx::ir
