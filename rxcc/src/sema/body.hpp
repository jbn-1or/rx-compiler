// 声明函数体检查器 BodyChecker，集中管理表达式检查及相关工具的接口。
// 通过 SemanticAnalyzer 访问全局符号和声明检查服务，共用诊断收集器。
// 当前包含函数体入口、局部作用域、字面量/路径/块与普通调用检查，
// 以及 place、转换工具；其余表达式类别和循环状态由后续步骤补齐。

#pragma once

#include "ast/ast.hpp"
#include "diagnostics.hpp"
#include "sema/body_state.hpp"
#include "sema/expression.hpp"
#include <optional>
#include <string>
#include <vector>

namespace rx {

class SemanticAnalyzer;
struct FunctionSymbol;

// 单个函数的检查上下文；符号和定型服务由 SemanticAnalyzer 持有。
// 4.3 起保存局部作用域与当前函数/结构体，4.7 再增加循环栈。
// 各类表达式的成员定义分散在对应 .cpp，见实现计划 B.0.1。
class BodyChecker {
public:
    explicit BodyChecker(SemanticAnalyzer &analyzer);

    // 检查一个函数的函数体：登记参数和接收者，再以声明返回类型检查块。
    void check_function(const FunctionSymbol &fn);

private:
    SemanticAnalyzer &analyzer_;
    Diagnostics &diagnostics_;

    std::vector<Scope> scopes_;
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
        bool diverges = false;  // 语句类型为 !（如 return），块也应为 !
        bool has_error = false; // 含 Error 恢复结果，避免再用 () 触发连锁错误
    };
    void enter_function(const FunctionSymbol &fn);
    StatementResult check_statement(const Statement &statement);
    void push_scope();
    void pop_scope();
    void declare_local(const std::string &name, const Binding &binding);
    std::optional<Binding> lookup_local(const std::string &name) const;

    // expression.cpp（4.3）：字面量分派与值路径。
    ExpressionResult check_integer_literal(const IntegerLiteral &literal,
                                           const SemanticType *expected);
    ExpressionResult check_value_path(const PathExpression &path);

    // call.cpp（4.4）：普通调用与内置 I/O。
    ExpressionResult check_call(const CallExpression &call);
    bool check_arguments(
        const std::vector<SemanticType> &parameter_types,
        const std::vector<std::shared_ptr<Expression>> &arguments, Span at);

    // place.cpp：place 物化与一次内置解引用。
    PlaceAccessInformation
    materialize_place(const PlaceAccessInformation &place) const;
    ExpressionResult dereference_place(const SemanticType &base_type,
                                       const PlaceAccessInformation &base_place,
                                       Span at);

    // coercion.cpp：一对一强制转换、固定目标报错与多分支 LUB。
    bool can_coerce(const SemanticType &from,
                    const PlaceAccessInformation &place,
                    const SemanticType &to) const;
    void require_coerce(const ExpressionResult &from, const SemanticType &to,
                        Span at);
    SemanticType compute_least_upper_bound(
        const std::vector<ExpressionResult> &results) const;
};

} // namespace rx
