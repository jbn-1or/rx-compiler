#include "ir/builder.hpp"
#include <stdexcept>
#include <utility>

namespace rx::ir {
namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::logic_error(message);
}
}

void IRBuilder::set_insert_point(BlockId block) {
    module_.function(block.function).block(block);
    insertion_ = block;
}
BasicBlock *IRBuilder::current_block() const {
    return insertion_ ? &module_.function(insertion_->function).block(*insertion_) : nullptr;
}
bool IRBuilder::is_open() const {
    const auto *block = current_block();
    return block && !block->terminator;
}
Function &IRBuilder::open_function() const {
    require(is_open(), "IR 指令需要尚未终结的插入块");
    return module_.function(insertion_->function);
}
bool IRBuilder::can_use(const Operand &operand) const {
    return is_open() && module_.function(insertion_->function).contains(operand);
}
void IRBuilder::require_operand(const Operand &operand) const {
    require(can_use(operand), "IR 操作数类型、身份或函数归属无效");
}

Operand IRBuilder::append(Type type, Operation operation) {
    auto &function = open_function();
    const auto value = type == Type::Void ? Operand{} : function.new_value(type);
    current_block()->instructions.push_back({
        value.value_id() ? std::optional{*value.value_id()} : std::nullopt,
        type, std::move(operation)});
    return value;
}

std::optional<Operand> IRBuilder::call(IRFunctionId id, std::vector<Operand> arguments) {
    open_function();
    const auto &signature = module_.function(id).signature;
    require(arguments.size() == signature.parameters.size(), "IR call 参数数量不匹配");
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        require_operand(arguments[i]);
        require(arguments[i].type == signature.parameters[i], "IR call 参数类型不匹配");
    }
    const auto result = append(signature.result, Call{id, std::move(arguments)});
    return signature.result == Type::Void ? std::nullopt : std::optional{result};
}
void IRBuilder::terminate(Terminator terminator) {
    open_function();
    current_block()->terminator = std::move(terminator);
    clear_insert_point();
}
void IRBuilder::ret(std::optional<Operand> value) {
    const auto result = open_function().signature.result;
    if (value)
        require_operand(*value);
    require(value ? value->type == result : result == Type::Void,
            "IR ret 与函数结果类型不匹配");
    terminate(Return{value});
}
} // namespace rx::ir
