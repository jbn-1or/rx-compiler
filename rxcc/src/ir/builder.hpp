// 提供向基本块添加指令的接口，并检查操作数、存储和返回值的类型。
// 添加指令前先选择一个基本块；返回或跳转后关闭该插入点。
#pragma once
#include "ir/model.hpp"

namespace rx::ir {

// 记录当前要向哪个基本块添加指令，并修改调用方提供的模块。
class IRBuilder {
public:
    // 使用已有模块创建构建器，不负责释放模块，初始时没有选中的基本块。
    explicit IRBuilder(Module &module) : module_(module) {}
    // 选择后续指令要添加到的基本块；已经结束的块仍不能继续添加指令。
    void set_insert_point(BlockId block_id);
    // 返回当前选中块的 ID；没有选中块时返回空值。
    std::optional<BlockId> insert_point() const { return insertion_; }

    // 添加函数调用并返回结果操作数；被调用函数返回 void 时返回空值。
    std::optional<Operand> call(IRFunctionId function_id,
                                std::vector<Operand> arguments = {});
    // 在当前函数入口分配固定存储，不改变当前插入点。
    Operand entry_alloca(StorageType storage_type, uint32_t alignment);
    // 从基址按存储类型和下标计算字段/数组地址，结果为 ptr，不附加 inbounds。
    Operand get_element_ptr(StorageType source_type, Operand base,
                            std::vector<Operand> indices);
    // 从地址读取指定存储类型的值，并为结果分配值 ID；保存引用的 ptr slot
    // 读取后仍是地址，不会自动读取目标内容。
    Operand load(Type storage_type, Operand address, uint32_t alignment);
    // 把指定类型的值写入地址；store 有副作用，但不产生结果。
    void store(Type storage_type, Operand value, Operand address,
               uint32_t alignment);
    // 省略显式存储类型，直接使用待写入值的类型。
    void store(Operand value, Operand address, uint32_t alignment) {
        store(value.type, value, address, alignment);
    }
    // 添加二元运算；两侧类型必须相同，结果类型也与操作数相同。
    Operand binary(BinaryOp operation, Operand left, Operand right);
    // 添加比较指令；两侧类型必须相同，结果类型固定为 i1。
    Operand compare(Comparison comparison, Operand left, Operand right);
    // 把较窄整数零扩展成较宽整数，例如 bool 写入 i8 存储单元。
    Operand zext(Type target_type, Operand value);
    // 把较宽整数截断成较窄整数，例如从 i8 存储单元读回 bool。
    Operand trunc(Type target_type, Operand value);
    // 添加与当前函数返回类型匹配的 ret 指令，然后取消选中的基本块。
    void ret(std::optional<Operand> value = std::nullopt);
    // 添加无条件跳转；目标必须在当前函数且不能是入口块，发出后关闭当前插入点。
    void branch(BlockId target);
    // 添加条件跳转；条件必须是 i1，两个目标都在当前函数且不能是入口块，
    // 发出后关闭当前插入点。
    void cond_branch(Operand condition, BlockId when_true, BlockId when_false);

private:
    // 返回当前块所属的函数；没有选中块或块已经结束时抛出 logic_error。
    Function &open_function() const;
    // 添加普通指令；有数据结果时分配值 ID，无结果时返回空操作数。
    Operand append(Function &function, Type type, Operation operation);
    // 检查跳转目标是否属于当前函数、存在且不是入口块。
    void require_target(const Function &function, BlockId target) const;
    // 设置当前块的终结指令，并取消选中的基本块。
    void terminate(Function &function, Terminator terminator);
    Module &module_; // 正在构建的模块引用；使用构建器期间，调用方不能释放这个模块。
    std::optional<BlockId> insertion_; // 当前选中块的 ID；未选择块或添加终结指令后为空。
};

} // namespace rx::ir
