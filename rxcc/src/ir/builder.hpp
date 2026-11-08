// 小范围、有类型的构建接口；无效请求在修改模块前抛出 logic_error。
#pragma once
#include "ir/model.hpp"

namespace rx::ir {

class IRBuilder {
public:
    explicit IRBuilder(Module &module) : module_(module) {}
    void set_insert_point(BlockId block);
    void clear_insert_point() { insertion_.reset(); }
    std::optional<BlockId> insert_point() const { return insertion_; }
    BasicBlock *current_block() const;
    bool is_open() const;
    bool can_use(const Operand &operand) const;

    std::optional<Operand> call(IRFunctionId function, std::vector<Operand> arguments = {});
    void ret(std::optional<Operand> value = std::nullopt);

private:
    Function &open_function() const;
    Operand append(Type type, Operation operation);
    void terminate(Terminator terminator);
    void require_operand(const Operand &operand) const;
    Module &module_;
    std::optional<BlockId> insertion_;
};

} // namespace rx::ir
