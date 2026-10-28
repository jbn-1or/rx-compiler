// 声明函数体检查器 BodyChecker，集中管理表达式检查及相关工具的接口。
// 通过 SemanticAnalyzer 访问全局符号和声明检查服务，共用诊断收集器。
// checker.cpp 集中函数 → 块 → 语句 → 表达式分派；具体表达式处理器按类别实现。
// context.hpp 定义共享结果与状态，calls/ 集中各种调用检查。

#pragma once

#include "ast/ast.hpp"
#include "diagnostics.hpp"
#include "sema/body/context.hpp"
#include "sema/body/calls/call.hpp"
#include <optional>
#include <string>
#include <vector>

namespace rx {

class SemanticAnalyzer;
struct FunctionSymbol;

// 单个函数的检查上下文；符号和定型服务由 SemanticAnalyzer 持有。
// 保存局部作用域、当前函数/结构体和循环栈；全局信息通过 analyzer_ 访问。
class BodyChecker {
public:
    explicit BodyChecker(SemanticAnalyzer &analyzer);

    // 检查一个函数的函数体：登记参数和接收者，再以声明返回类型检查块。
    void check_function(const FunctionSymbol &fn);

private:
    SemanticAnalyzer &analyzer_;
    Diagnostics &diagnostics_;

    std::vector<Scope> scopes_;
    std::vector<LoopFrame> loop_frames_;
    const FunctionSymbol *current_function_ = nullptr;
    StructSymbol *current_owner_ = nullptr;
    bool current_has_self_ = false;

    // 函数体检查的统一入口。expected == nullptr 表示没有期望类型；
    // 返回表达式按自身规则得到的类型与 place，不在这里做外层强制转换。
    ExpressionResult check_expression(const Expression &expression,
                                      const SemanticType *expected);
    ExpressionResult check_block(const BlockExpression &block,
                                 const SemanticType *expected);

    // 函数入口、语句、作用域和局部名字。
    // 一条非尾语句的检查摘要，供 check_block 决定无尾块的类型。
    struct StatementResult {
        bool diverges = false;  // 语句中的表达式结果为 !，用于判断无尾块是否发散。
        bool has_error = false; // 表达式失败或固定目标不兼容；不汇总嵌套的全部诊断。
    };
    void enter_function(const FunctionSymbol &fn);
    StatementResult check_statement(const Statement &statement);
    void push_scope();
    void pop_scope();
    void declare_local(const std::string &name, const Binding &binding);
    std::optional<Binding> lookup_local(const std::string &name) const;

    // values.cpp：字面量和值路径；统一分派在 checker.cpp。
    ExpressionResult check_integer_literal(const IntegerLiteral &literal,
                                           const SemanticType *expected);
    ExpressionResult check_value_path(const PathExpression &path);

    // calls/call.cpp：普通调用、内置 I/O 与多段被调用者解析。
    ExpressionResult check_call(const CallExpression &call);
    // 单段被调用者——局部绑定优先（N13），再查值名字空间与同名结构体。
    ExpressionResult check_ordinary_call(const PathExpression &path,
                                         const CallExpression &call);
    // `Type::member` 形式的两段被调用者解析，返回统一的调用签名。
    std::optional<CallableSignature>
    resolve_qualified_callee(const PathExpression &path);
    // 在结构体关联项里找签名；找不到时按派生 Clone 回退。
    std::optional<CallableSignature> resolve_struct_member(
        const StructSymbol &structure, const PathSegment &member, Span at);
    // calls/method.cpp：点调用候选查找、接收者调整与普通实参检查。
    ExpressionResult check_method_call(const MethodCallExpression &call);
    std::optional<CallableSignature> lookup_method_signature(
        const SemanticType &candidate, const std::string &name);
    std::optional<ResolvedMethod> resolve_method(const SemanticType &receiver_type,
                                                 const std::string &name);
    bool check_method_receiver(const ExpressionResult &receiver,
                               const ResolvedMethod &method, Span at);
    bool check_arguments(
        const std::vector<SemanticType> &parameter_types,
        const std::vector<std::shared_ptr<Expression>> &arguments, Span at);

    // operators.cpp：运算、赋值、借用、解引用与 cast。
    ExpressionResult check_binary(const BinaryExpression &expression);
    ExpressionResult check_unary(const UnaryExpression &expression);
    ExpressionResult check_assignment(const AssignmentExpression &expression);
    ExpressionResult check_cast(const CastExpression &expression);

    // aggregate.cpp：结构体、字段、数组和索引。
    ExpressionResult
    check_struct_expression(const StructExpression &expression);
    ExpressionResult check_field(const FieldExpression &expression);
    ExpressionResult check_array(const ArrayExpression &expression,
                                 const SemanticType *expected);
    ExpressionResult check_index(const IndexExpression &expression);

    // control_flow.cpp：if、loop、while 和跳转。
    ExpressionResult check_if(const IfExpression &expression,
                              const SemanticType *expected);
    ExpressionResult check_loop(const LoopExpression &expression,
                                const SemanticType *expected);
    ExpressionResult check_while(const WhileExpression &expression);
    ExpressionResult check_break(const BreakExpression &expression);
    ExpressionResult check_continue(const ContinueExpression &expression);
    bool check_bool_condition(const Expression &condition, Span at);

    enum class JumpTargetStatus { Found, BlockedByBarrier, NotFound };
    struct JumpTargetLookup {
        JumpTargetStatus status = JumpTargetStatus::NotFound;
        std::size_t index = 0;
    };
    JumpTargetLookup find_jump_target() const;

    // conversions.cpp：place 物化与一次内置解引用。
    PlaceAccessInformation
    materialize_place(const PlaceAccessInformation &place) const;
    ExpressionResult dereference_place(const SemanticType &base_type,
                                       const PlaceAccessInformation &base_place,
                                       Span at);

    // conversions.cpp：一对一强制转换、固定目标报错与多分支 LUB。
    bool can_coerce(const SemanticType &from,
                    const PlaceAccessInformation &place,
                    const SemanticType &to) const;
    // 已有 Error 时静默失败；不兼容时报错；不改写表达式本身的类型。
    bool require_coerce(const ExpressionResult &from, const SemanticType &to,
                        Span at);
    SemanticType compute_least_upper_bound(
        const std::vector<ExpressionResult> &results) const;
};

} // namespace rx
