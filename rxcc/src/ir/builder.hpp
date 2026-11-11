// 提供向基本块添加 call 和 ret 指令的接口，并检查参数和返回值的类型。
// 添加指令前先选择一个基本块；添加 ret 后，不再继续向该块添加指令。
#pragma once
#include "ir/model.hpp"

namespace rx::ir {

// 记录当前要向哪个基本块添加指令，并修改调用方提供的模块。
class IRBuilder {
public:
    // 使用已有模块创建构建器，不负责释放模块，初始时没有选中的基本块。
    explicit IRBuilder(Module &module) : module_(module) {}
    // 选择后续指令要添加到的基本块；已经结束的块仍不能继续添加指令。
    void set_insert_point(BlockId block);
    // 取消当前选中的基本块，不删除已经添加的指令。
    void clear_insert_point() { insertion_.reset(); }
    // 返回当前选中块的 ID；没有选中块时返回空值。
    std::optional<BlockId> insert_point() const { return insertion_; }
    // 返回当前选中的基本块；没有选中块时返回 nullptr。
    BasicBlock *current_block() const;
    // 判断当前是否选中了一个尚未结束、可以继续添加 IR 指令的基本块。
    bool is_open() const;
    // 判断当前块是否可继续添加指令，且操作数的类型和所属函数是否正确。
    // 不检查值是否已在使用前定义，这项检查由 check_module 完成。
    bool can_use(const Operand &operand) const;

    // 添加函数调用并返回结果操作数；被调用函数返回 void 时返回空值。
    std::optional<Operand> call(IRFunctionId function, std::vector<Operand> arguments = {});
    // 添加与当前函数返回类型匹配的 ret 指令，然后取消选中的基本块。
    void ret(std::optional<Operand> value = std::nullopt);

private:
    // 返回当前块所属的函数；没有选中块或块已经结束时抛出 logic_error。
    Function &open_function() const;
    // 添加普通指令；有数据结果时分配值 ID，无结果时返回空操作数。
    Operand append(Type type, Operation operation);
    // 设置当前块的终结指令，并取消选中的基本块。
    void terminate(Terminator terminator);
    // 检查操作数能否用于当前函数；不符合条件时抛出 logic_error。
    void require_operand(const Operand &operand) const;
    // 正在构建的模块引用；使用构建器期间，调用方不能释放这个模块。
    Module &module_;
    // 当前选中块的 ID；未选择块或添加终结指令后为空。
    std::optional<BlockId> insertion_;
};

} // namespace rx::ir
