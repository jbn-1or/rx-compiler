// 实现 BodyChecker 的构造及函数体检查入口。
// 当前表达式和代码块检查仍为占位实现，尚未接入完整语义检查流程。
// 后续在此实现函数入口、局部作用域和块遍历，分类表达式规则由对应模块处理。

#include "sema/body.hpp"
#include "sema/sema.hpp"

namespace rx {

BodyChecker::BodyChecker(SemanticAnalyzer &analyzer)
    : analyzer_(analyzer), diagnostics_(analyzer.diagnostics_) {}

// 4.1 的检查入口占位；4.3 再补齐具体表达式规则。
// 当前 run() 尚未接入函数体；此处返回 Error 不代表检查成功。
ExpressionResult BodyChecker::check_expression(const Expression &,
                                               const SemanticType *) {
    return ExpressionResult{SemanticType::error(), PlaceAccessInformation{}};
}

ExpressionResult BodyChecker::check_block(const BlockExpression &,
                                        const SemanticType *) {
    return ExpressionResult{SemanticType::error(), PlaceAccessInformation{}};
}

} // namespace rx
