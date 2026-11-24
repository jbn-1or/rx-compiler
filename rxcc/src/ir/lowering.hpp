// 声明函数体 lowering 共用的结果、place 和生成上下文；成员实现按职责分在 lowering/。
// lowering.cpp 保留分派和函数体入口，crate 级编排仍由 generator.cpp 负责。
#pragma once

#include "ir/lowering/value.hpp"
#include "ir/model.hpp"
#include "sema/result.hpp"
#include <functional>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace rx {

struct FunctionSymbol;
struct IfExpression;
struct ArrayExpression;
struct StructExpression;
struct FieldExpression;
struct IndexExpression;
struct StructSymbol;
struct LoopExpression;
struct WhileExpression;
struct BreakExpression;
struct ContinueExpression;
struct MethodCallExpression;

namespace ir {
class IRBuilder;
class Module;
} // namespace ir

// 保存每个局部绑定在入口分配中的地址；名称解析仍来自语义结果。
using LocalPlaces = std::map<LocalId, ir::Operand>;

// 已经求得的独立聚合值；address 是入口分配的值存储，不等同于源变量的 place。
struct AggregateValue {
    ir::Operand address; // 聚合值存储地址，IR 类型必须为 ptr。
    const SemanticType *type; // 这个值保存的源聚合类型。
};
// 表达式正常完成但没有数据值，例如 () 或已经产生副作用的 unit 调用。
struct UnitResult {};
// 当前执行路径已经由 return 结束，外层不能再生成任何指令。
struct NoContinuation {};
// 一条表达式或语句的生成结果；生成失败仍用外层 std::nullopt 表示。
using EmitResult =
    std::variant<ir::Operand, AggregateValue, UnitResult, NoContinuation>;

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

// 保存 lowering 环境；局部地址表按函数维护，词法作用域已由 sema 解析。
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
    // 生成所有已登记但还没有函数体的辅助函数，供生成器在函数体之后调用。
    bool emit_pending_helpers();

private:
    enum class ResultUse { Value, Discard }; // 调用方是否消费表达式的数据结果。

    // IR 生成期的循环帧：用 sema 固定的 LoopId 查跳转目标，不重新选最近循环。
    struct LoopFrame {
        LoopId id; // 这个帧对应的语义循环身份。
        ir::BlockId continue_target; // while 回到条件块，loop 回到循环体块。
        std::optional<ir::BlockId> break_target; // 退出块；loop 在首次 break 时创建。
        std::optional<Place> result; // 仅在调用方需要 break 的值时保存结果位置。
    };

    // 辅助函数的不同操作；同一具体类型可以分别需要 clone 和相等实现。
    enum class HelperKind {
        Clone,    // void clone_T(ptr destination, ptr source)
        Equality, // i1 eq_T(ptr left, ptr right)
    };

    // 一个按“操作种类 + 完整具体类型”登记的辅助函数。
    // 先登记身份和签名，真正生成函数体时再标记 Generating/Complete。
    struct Helper {
        enum class State {
            Pending,    // 已登记，尚未开始生成函数体。
            Generating, // 正在生成函数体；递归请求直接复用这个身份。
            Complete,   // 函数体已经完成。
        };
        HelperKind kind = HelperKind::Clone; // 这个辅助函数执行哪种操作。
        ir::IRFunctionId function; // 分配好的 IR 函数身份。
        SemanticType type; // 该辅助函数处理的完整具体类型。
        State state = State::Pending; // 函数体生成状态。
    };

    // lowering.cpp：表达式和语句分派。
    // 生成一条 let 或表达式语句。
    std::optional<EmitResult> emit_statement(const Statement &statement,
                                             LocalPlaces &locals);
    // 按顺序生成块内语句，遇到失败或路径结束立即返回。
    std::optional<EmitResult> emit_statements(const BlockExpression &block,
                                              LocalPlaces &locals);
    // 统一分派；块和 if 根据使用方式决定是否生成数据结果。
    std::optional<EmitResult> emit_expression(const Expression &expression,
                                              LocalPlaces &locals, ResultUse use);
    // 计算需要数据值的表达式，包括整数、bool 和引用。
    std::optional<EmitResult> emit_value(const Expression &expression,
                                         LocalPlaces &locals);
    // 按语义类型求值，保留副作用与控制流，但丢弃正常结果。
    std::optional<EmitResult> emit_discarded_expression(const Expression &expression,
                                                        LocalPlaces &locals);

    // lowering/aggregate.cpp：聚合构造、值快照和存储搬运。
    // 查找字段在声明列表中的序号；sema 已保证字段存在。
    std::size_t field_declaration_index(const StructSymbol &structure,
                                        const std::string &name) const;
    // 按源码顺序构造结构体或数组，返回独立临时存储中的聚合值。
    std::optional<EmitResult> emit_struct(const StructExpression &expression,
                                          LocalPlaces &locals);
    std::optional<EmitResult> emit_array(const ArrayExpression &expression,
                                         LocalPlaces &locals);
    // 求值一个固定使用位置并按 sema 的记录调整到目标类型。
    std::optional<EmitResult>
    emit_prepared_value(const Expression &expression, LocalPlaces &locals,
                        const UseSite &site);
    // 对已经求好的结果执行 sema 记录的调整；聚合值保持同一存储。
    std::optional<EmitResult> adjust_value(const EmitResult &value,
                                           const UseSite &site);
    // 把已准备好的标量或聚合值写入目标存储。
    void store_prepared_value(const ir::Operand &destination,
                              const EmitResult &prepared,
                              const SemanticType &type);
    // 按完整聚合结构把 source 存储搬运到 destination；数组使用有界循环。
    void copy_value(const ir::Operand &destination, const ir::Operand &source,
                    const SemanticType &type);
    // 源位置可能被后续表达式修改，私有结果槽则已经是独立的值。
    enum class StorageOrigin { SourcePlace, PrivateSlot };
    std::optional<EmitResult> read_stored_value(
        const ir::Operand &address, const SemanticType &type,
        StorageOrigin origin = StorageOrigin::PrivateSlot);
    // 对每个下标生成 body；小数组直接展开，大数组生成计数循环。
    void for_each_array_index(
        uint64_t count,
        const std::function<void(ir::Operand)> &body);

    // lowering/control_flow.cpp：if 的分块、跳转和正常边汇合。
    // 仅在调用方需要数据结果时创建 slot，供正常分支写入并在汇合块读回。
    std::optional<EmitResult> emit_if(const IfExpression &expression,
                                     LocalPlaces &locals, ResultUse use);
    // 生成 && / ||：先求左操作数，只有需要时再求右操作数。
    std::optional<EmitResult> emit_logical(const BinaryExpression &expression,
                                           LocalPlaces &locals, ResultUse use);
    // 生成 while：cond 每轮重新求值，body 正常结束回到 cond，false 边进入 exit。
    std::optional<EmitResult> emit_while(const WhileExpression &expression,
                                         LocalPlaces &locals);
    // 生成 loop：body 正常结束跳回自身；有结果时 break 写入口 slot，exit 读回。
    std::optional<EmitResult> emit_loop(const LoopExpression &expression,
                                        LocalPlaces &locals, ResultUse use);
    // 生成 break/continue：查 sema 记录的 LoopId，再跳到当前循环帧的目标块。
    std::optional<EmitResult> emit_break(const BreakExpression &expression,
                                         LocalPlaces &locals);
    std::optional<EmitResult> emit_continue(
        const ContinueExpression &expression, LocalPlaces &locals);
    LoopFrame &active_loop(LoopId target);

    // lowering/value.cpp：基础值、存储表示和固定临时 slot。
    // 按存储类型和布局服务分配入口 slot，标量与聚合共用。
    ir::Operand allocate_storage_slot(const ir::StorageType &storage_type);
    // 按标量的 RV32 存储类型和对齐要求分配入口 slot。
    ir::Operand allocate_slot(ir::Type value_type);
    // 计算字面量或常量路径，不读取局部变量。
    ir::Operand emit_constant(const Expression &expression);
    // 从存储单元读回普通值；bool 读取 i8 后再截断成 i1，引用读取 ptr，整数读取 i32。
    ir::Operand load_value(const ir::Operand &address,
                           const SemanticType &type);
    ir::Operand load_value(const ir::Operand &address, ir::Type value_type);
    // 把普通值写入它的存储单元；bool 先扩展成 i8，引用写 ptr，整数写 i32。
    void store_value(const ir::Operand &value, const ir::Operand &address);
    // 把值物化到入口分配的临时 slot；初始化 store 留在当前求值位置。
    ir::Operand materialize_value(ir::Operand value);

    // lowering/reference.cpp：地址投影、借用、解引用和使用位置转换。
    // 计算位置表达式的地址；引用本身不在这里读取目标内容。
    std::optional<PlaceResult> emit_place(const Expression &expression,
                                          LocalPlaces &locals);
    // 求值字段/索引的基表达式，沿引用取得底层聚合的原位置或临时存储。
    std::optional<PlaceResult> emit_base_place(const Expression &expression,
                                               LocalPlaces &locals);
    // 沿已确认的引用自动解引用后，用 GEP 投影字段或数组元素。
    std::optional<PlaceResult> emit_field_place(const FieldExpression &expression,
                                                LocalPlaces &locals);
    std::optional<PlaceResult> emit_index_place(const IndexExpression &expression,
                                                LocalPlaces &locals);
    // 生成 & / &mut：已有 place 直接取地址，值表达式只物化一次。
    std::optional<EmitResult> emit_borrow(const UnaryExpression &expression,
                                          LocalPlaces &locals);
    // 按 sema 已选定的路径执行引用调整；每步只读取一层指针。
    std::optional<ir::Operand> emit_coercion(ir::Operand value,
                                             const CoercionPlan &plan);
    // 读取某个固定使用位置的调整路径，并把准备好的值调整到目标类型。
    std::optional<ir::Operand> emit_coercion_at(const UseSite &site,
                                                 ir::Operand value);

    // lowering/operators.cpp：操作数读取时机和标量运算。
    // 先求值左操作数，再求值右操作数，最后按源类型选择二元指令。
    std::optional<EmitResult> emit_binary(const BinaryExpression &expression,
                                          LocalPlaces &locals, ResultUse use);
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
    // 显式 return 与函数尾返回共用发射逻辑；site 标识各自的语义转换记录。
    std::optional<EmitResult> emit_return_value(const Expression *value,
                                                const UseSite &site, LocalPlaces &locals);
    std::optional<EmitResult> emit_return(const ReturnExpression &expression,
                                          LocalPlaces &locals);
    // 生成普通或复合赋值：RHS 优先，目标地址只求一次，最后写入。
    std::optional<EmitResult>
    emit_assignment(const AssignmentExpression &expression,
                     LocalPlaces &locals);

    // lowering/call.cpp：普通调用、点方法调用与 builtin 调用。
    // 根据语义目标准备实参并生成调用。
    std::optional<EmitResult> emit_call(const CallExpression &call,
                                        LocalPlaces &locals);
    // 点方法：先按已选路径求值接收者一次，再按顺序求普通实参并调用。
    std::optional<EmitResult> emit_method_call(const MethodCallExpression &call,
                                               LocalPlaces &locals);
    // 按 ReceiverAdjustment 把接收者准备成声明的接收者值或地址。
    std::optional<EmitResult> emit_receiver(const Expression &receiver,
                                            const ResolvedMethod &method,
                                            LocalPlaces &locals);
    // 把已经按调用顺序求好的实参传给目标函数，并按 ABI 处理聚合结果。
    std::optional<EmitResult> emit_target_call(ir::IRFunctionId target,
                                               std::vector<EmitResult> arguments,
                                               const SemanticType &result_type);
    // Box::<T>::new：申请 T 的存储并把已求好的值复制/移动到新分配。
    std::optional<EmitResult> emit_box_new(const SemanticType &box_type,
                                           std::vector<EmitResult> prepared);
    // Vec::<T>::new：生成 {null, 0, 0} 头，不申请零字节缓冲区。
    std::optional<EmitResult> emit_vec_new(const SemanticType &vec_type);
    // Vec 的 len/is_empty：从真实头的长度字段读取，header 必须指向 {ptr,i32,i32}。
    std::optional<EmitResult> emit_vec_query(BuiltinCallKind kind,
                                             ir::Operand header);
    // Vec::push：按当前头决定是否增长，搬运活动元素后写入新元素并更新 len。
    std::optional<EmitResult> emit_vec_push(const SemanticType &vec_type,
                                            ir::Operand header,
                                            EmitResult element);
    // Vec::remove：先保存被移除值，再把后缀左移一位并更新 len。
    std::optional<EmitResult> emit_vec_remove(const SemanticType &vec_type,
                                              ir::Operand header,
                                              ir::Operand index);

    // lowering/clone.cpp：按具体类型执行深克隆并登记辅助函数。
    // 从保存 T 的 source 地址克隆，返回 T 的计算值或独立聚合存储。
    std::optional<EmitResult> emit_clone(const SemanticType &type,
                                         ir::Operand source);
    // 在 destination 中生成 T 的深克隆；复合类型内部按成员继续分派。
    void emit_clone_into(const ir::Operand &destination,
                         const ir::Operand &source,
                         const SemanticType &type);
    // 克隆一个字段或元素：复合类型调用自己的辅助函数，标量/引用直接复制。
    void emit_clone_member(const ir::Operand &destination,
                           const ir::Operand &source,
                           const SemanticType &type);

    // lowering/equality.cpp：按内容比较两个同源类型操作数。
    // 先借用两侧，再生成一次 ==；!= 只对这次相等结果取反。
    std::optional<EmitResult> emit_equality(const BinaryExpression &expression,
                                            LocalPlaces &locals);
    // 准备一侧的借用地址；已有 place 保留原位置，值表达式只物化一次。
    std::optional<PlaceResult>
    emit_equality_operand(const Expression &expression, LocalPlaces &locals);
    // 比较 left/right 地址中的 T，返回 i1；复合类型内部按字段/元素分派。
    ir::Operand emit_equality_into(const ir::Operand &left,
                                   const ir::Operand &right,
                                   const SemanticType &type);
    // 比较一个嵌套字段或元素；复合类型调用自己的辅助函数，标量直接比较。
    ir::Operand emit_equality_member(const ir::Operand &left,
                                     const ir::Operand &right,
                                     const SemanticType &type);

    // 调用 __rx_alloc，首次使用时声明外部 C ABI，后续复用。
    ir::Operand emit_alloc(ir::Operand size, ir::Operand align);
    // 对运行时 count 个下标生成 body；循环条件和下标都在运行时读取。
    void for_each_runtime_index(
        ir::Operand count,
        const std::function<void(ir::Operand)> &body);

    const SemanticResult &semantics; // 语义分析结果，提供表达式、路径和调用的记录。
    const std::vector<ir::IRFunctionId> &functions; // 源 FunctionId 下标到已登记 IRFunctionId 的映射。
    ir::Module &module; // 正在构建的 IR 模块。
    ir::LayoutService layouts_; // 复用命名结构体布局缓存，提供 slot 对齐和搬运步长。
    ir::IRBuilder &builder; // 向当前基本块添加指令的构建器。
    std::ostream &diagnostics; // 收集 IR 生成期间错误信息的输出流。
    // 当前正在生成的循环帧，按嵌套顺序保存；break/continue 用 LoopId 定位目标。
    std::vector<LoopFrame> loop_frames_;
    // 查找或首次登记某个操作/具体类型的辅助函数；返回可立即调用的身份。
    ir::IRFunctionId request_helper(HelperKind kind, const SemanticType &type);
    // 按请求顺序保存全部辅助函数；函数体在主函数体之后统一生成。
    std::vector<Helper> helpers_;
    // “操作种类 + 完整类型键”到 helpers_ 下标的映射，同一项只登记一次。
    std::unordered_map<std::string, std::size_t> helper_indices_;
    const SemanticType *current_return_type_ = nullptr; // 当前函数的源返回类型。
    std::optional<ir::Operand> function_result_pointer_; // 聚合返回的调用者结果地址。
};

} // namespace rx
