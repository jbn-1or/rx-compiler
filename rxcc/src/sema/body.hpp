// 声明函数体检查器 BodyChecker，集中管理表达式检查及相关工具的接口。
// 通过 SemanticAnalyzer 访问全局符号和声明检查服务，共用诊断收集器。
// 当前包含检查入口占位及 place、转换工具，局部作用域和循环状态待后续补齐。

#pragma once

#include "ast/ast.hpp"
#include "diagnostics.hpp"
#include "sema/expression.hpp"
#include <vector>

namespace rx {

class SemanticAnalyzer;

// 单个函数的检查上下文；符号和定型服务由 SemanticAnalyzer 持有。
// 当前仅有 4.1 占位入口和 4.2 工具，尚未接入 run()。
// 4.3 起在此增加局部作用域/当前函数，4.7 再增加循环栈。
// 各类表达式的成员定义分散在对应 .cpp，见实现计划 B.0.1。
class BodyChecker {
public:
    explicit BodyChecker(SemanticAnalyzer &analyzer);

private:
    SemanticAnalyzer &analyzer_;
    Diagnostics &diagnostics_;

    // 函数体检查的统一入口。expected == nullptr 表示没有期望类型；
    // 返回表达式按自身规则得到的类型与 place，不在这里做外层强制转换。
    ExpressionResult check_expression(const Expression &expression,
                                      const SemanticType *expected);
    ExpressionResult check_block(const BlockExpression &block,
                                 const SemanticType *expected);

    // place.cpp（4.2）：place 物化与一次内置解引用。
    PlaceAccessInformation
    materialize_place(const PlaceAccessInformation &place) const;
    ExpressionResult dereference_place(const SemanticType &base_type,
                                       const PlaceAccessInformation &base_place,
                                       Span at);

    // coercion.cpp（4.2）：一对一强制转换、固定目标报错与多分支 LUB。
    bool can_coerce(const SemanticType &from,
                    const PlaceAccessInformation &place,
                    const SemanticType &to) const;
    void require_coerce(const ExpressionResult &from, const SemanticType &to,
                        Span at);
    SemanticType compute_least_upper_bound(
        const std::vector<ExpressionResult> &results) const;
};

} // namespace rx
