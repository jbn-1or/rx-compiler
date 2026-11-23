// 声明函数体 lowering 共用的结果、place 和生成上下文；成员实现按职责分在 lowering/。
// lowering.cpp 保留分派和函数体入口，crate 级编排仍由 generator.cpp 负责。
#pragma once

#include "ir/lowering/value.hpp"
#include "ir/model.hpp"
#include "sema/result.hpp"
#include <map>
#include <optional>
#include <ostream>
#include <unordered_map>
#include <variant>
#include <vector>

namespace rx {

struct FunctionSymbol;
struct IfExpression;
struct LoopExpression;
struct WhileExpression;
struct BreakExpression;
struct ContinueExpression;

namespace ir {
class IRBuilder;
class Module;
} // namespace ir

// 保存每个局部绑定在入口分配中的地址；名称解析仍来自语义结果。
using LocalPlaces = std::map<LocalId, ir::Operand>;

// 表达式正常完成但没有数据值，例如 () 或已经产生副作用的 unit 调用。
struct UnitResult {};
// 当前执行路径已经由 return 结束，外层不能再生成任何指令。
struct NoContinuation {};
// 一条表达式或语句的生成结果；生成失败仍用外层 std::nullopt 表示。
using EmitResult = std::variant<ir::Operand, UnitResult, NoContinuation>;

// 保存一次 place 求值得到的地址和该位置中保存的源类型。
// 引用变量自身的 slot 保存指针；读取 slot 与读取目标内容是两次不同的 load。
struct Place {
    ir::Operand address; // 位置地址，IR 类型必须为 ptr。
    const SemanticType *type; // 位置中保存的源类型，决定后续如何读取。
};
// place 求值结果；路径已经结束时不再产生地址。
using PlaceResult = std::variant<Place, NoContinuation>;

// 判断一次生成结果是否已经结束当前路径。
bool path_ended(const EmitResult &result);
// 判断一次 place 生成是否已经结束当前路径。
bool path_ended(const PlaceResult &result);

// 保存一次函数体生成过程中不变的环境；局部变量表按函数和块作用域单独传递。
class GenerationContext {
public:
    // 保存这次生成使用的引用，不拥有任何对象。
    GenerationContext(const SemanticResult &semantics,
                      const std::vector<ir::IRFunctionId> &functions,
                      ir::Module &module, ir::IRBuilder &builder,
                      std::ostream &diagnostics);

    // 生成一个完整函数体：参数写入各自 slot，再按语句和尾表达式生成并正常返回。
    // 返回 false 表示遇到尚未支持或内部不一致的情况，原因已写入 diagnostics。
    bool emit_function_body(const FunctionSymbol &function, ir::IRFunctionId id);

private:
    // IR 生成期的循环帧：用 sema 固定的 LoopId 查跳转目标，不重新选最近循环。
    struct LoopFrame {
        LoopId id; // 这个帧对应的语义循环身份。
        ir::BlockId continue_target; // while 回到条件块，loop 回到循环体块。
        std::optional<ir::BlockId> break_target; // 退出块；loop 在首次 break 时创建。
        const SemanticType *result_type = nullptr; // 非空表示 loop 有数据结果。
        std::optional<ir::Operand> result_slot; // 有数据结果时保存入口 result slot。
    };

    // lowering.cpp：表达式和语句分派。
    // 生成一条 let 或表达式语句。
    std::optional<EmitResult> emit_statement(const Statement &statement,
                                             LocalPlaces &locals);
    // 计算需要数据值的表达式，包括整数、bool 和引用。
    std::optional<EmitResult> emit_value(const Expression &expression,
                                         LocalPlaces &locals);
    // 生成 unit/发散表达式，或生成不需要块结果的代码块。
    std::optional<EmitResult> emit_unit_expression(const Expression &expression,
                                                     LocalPlaces &locals);
    // 按语义类型求值，保留副作用与控制流，但丢弃正常结果。
    std::optional<EmitResult> emit_discarded_expression(const Expression &expression,
                                                        LocalPlaces &locals);

    // lowering/control_flow.cpp：if 的分块、跳转和正常边汇合。
    // unit/never if 不分配结果 slot；整数值 if 用入口 result slot 汇合两支结果。
    std::optional<EmitResult> emit_if_unit(const IfExpression &expression,
                                            LocalPlaces &locals);
    std::optional<EmitResult> emit_if_value(const IfExpression &expression,
                                             LocalPlaces &locals);
    // 生成 && / ||：先求左操作数，只有需要时再求右操作数。
    std::optional<EmitResult> emit_logical(const BinaryExpression &expression,
                                           LocalPlaces &locals);
    // 生成 while：cond 每轮重新求值，body 正常结束回到 cond，false 边进入 exit。
    std::optional<EmitResult> emit_while(const WhileExpression &expression,
                                         LocalPlaces &locals);
    // 生成 loop：body 正常结束跳回自身；有结果时 break 写入口 slot，exit 读回。
    std::optional<EmitResult> emit_loop(const LoopExpression &expression,
                                        LocalPlaces &locals);
    // 生成 break/continue：查 sema 记录的 LoopId，再跳到当前循环帧的目标块。
    std::optional<EmitResult> emit_break(const BreakExpression &expression,
                                         LocalPlaces &locals);
    std::optional<EmitResult> emit_continue(
        const ContinueExpression &expression, LocalPlaces &locals);
    // 按 LoopId 查找生成期的循环帧；找不到说明消 lower 与 sema 身份不一致。
    LoopFrame *find_loop_frame(LoopId id);

    // lowering/value.cpp：基础值、存储表示和固定临时 slot。
    // 选择局部 slot 的存储类型，供 let 与函数形参共同使用。
    static ir::Type slot_type(const SemanticType &type);
    // bool 按字节对齐，整数和引用按 RV32 的四字节对齐。
    static uint32_t slot_alignment(const SemanticType &type);
    // 计算字面量、路径以及引用本地读取；引用路径只加载保存的指针。
    ir::Operand emit_leaf(const Expression &expression,
                           const LocalPlaces &locals);
    // 从存储单元读回普通值；bool 读取 i8 后再截断成 i1，引用读取 ptr，整数读取 i32。
    ir::Operand load_value(const ir::Operand &address,
                           const SemanticType &type);
    // 把普通值写入它的存储单元；bool 先扩展成 i8，引用写 ptr，整数写 i32。
    void store_value(const ir::Operand &value, const ir::Operand &address);
    // 把已求得的值写入以 AST 静态位置为键的固定临时 slot，并返回地址。
    // 同一位置重复执行时复用入口分配，初始化 store 留在当前求值位置。
    ir::Operand materialize_once(const Expression &position, ir::Operand value);

    // lowering/reference.cpp：地址投影、借用、解引用和使用位置转换。
    // 计算位置表达式的地址；引用本身不在这里读取目标内容。
    std::optional<PlaceResult> emit_place(const Expression &expression,
                                          LocalPlaces &locals);
    // 生成 & / &mut：已有 place 直接取地址，值表达式只物化一次。
    std::optional<EmitResult> emit_borrow(const UnaryExpression &expression,
                                          LocalPlaces &locals);
    // 生成一次内置解引用：先得到地址，再按目标源类型读取值。
    std::optional<EmitResult>
    emit_dereference(const UnaryExpression &expression, LocalPlaces &locals);
    // 按 sema 已选定的路径执行引用调整；每步只读取一层指针。
    std::optional<ir::Operand> emit_coercion(ir::Operand value,
                                             const CoercionPlan &plan);
    // 读取某个固定使用位置的调整路径，并把准备好的值调整到目标类型。
    std::optional<ir::Operand> emit_coercion_at(const UseSite &site,
                                                 ir::Operand value);

    // lowering/operators.cpp：操作数读取时机和标量运算。
    // 先求值左操作数，再求值右操作数，最后按源类型选择二元指令。
    std::optional<EmitResult> emit_binary(const BinaryExpression &expression,
                                          LocalPlaces &locals);
    // 生成一元取负或按位取反；共享引用操作数在求值后读取目标。
    std::optional<EmitResult> emit_unary(const UnaryExpression &expression,
                                         LocalPlaces &locals);
    // 生成 as 转换：整数换源类型，bool 到整数先零扩展。
    std::optional<EmitResult> emit_cast(const CastExpression &expression,
                                        LocalPlaces &locals);
    // 只接收已求好的标量操作数，按源类型选择 add/sdiv/udiv/icmp/移位等指令。
    std::optional<ir::Operand>
    emit_binary_operation(BinaryOperator operation,
                          const SemanticType &operand_type,
                          ir::Operand left, ir::Operand right);
    // 完成标量操作数：直接标量已可用；一层共享引用此时才读取目标。
    ir::Operand finish_scalar_operand(ir::Operand value,
                                       const SemanticType &type);

    // lowering/statements.cpp：绑定、写入和返回。
    // 先生成初始化器，再为 LocalId 分配并登记存储位置。
    std::optional<EmitResult> emit_let(const LetStatement &statement,
                                       LocalPlaces &locals);
    // 生成 return：先求值操作数，再发 ret 并结束当前路径。
    std::optional<EmitResult> emit_return(const ReturnExpression &expression,
                                          LocalPlaces &locals);
    // 生成普通或复合赋值：RHS 优先，目标地址只求一次，最后写入。
    std::optional<EmitResult>
    emit_assignment(const AssignmentExpression &expression,
                     LocalPlaces &locals);

    // lowering/call.cpp：普通调用与 builtin 调用。
    // 根据语义目标准备实参并生成调用。
    std::optional<EmitResult> emit_call(const CallExpression &call,
                                        LocalPlaces &locals);

    const SemanticResult &semantics; // 语义分析结果，提供表达式、路径和调用的记录。
    const std::vector<ir::IRFunctionId> &functions; // 源 FunctionId 下标到已登记 IRFunctionId 的映射。
    ir::Module &module; // 正在构建的 IR 模块。
    ir::IRBuilder &builder; // 向当前基本块添加指令的构建器。
    std::ostream &diagnostics; // 收集 IR 生成期间错误信息的输出流。
    // 当前函数内按 AST 静态位置缓存的临时 slot；切换函数时清空，避免跨函数共用。
    std::unordered_map<const Expression *, ir::Operand> temporary_slots;
    // 当前正在生成的循环帧，按嵌套顺序保存；break/continue 用 LoopId 定位目标。
    std::vector<LoopFrame> loop_frames_;
};

} // namespace rx
