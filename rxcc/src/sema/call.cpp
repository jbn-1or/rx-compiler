// 实现普通函数调用与内置 I/O 的检查。
// 当前只解析单段 PathExpression callee；多段关联调用和点调用留到 Phase 5。
// 实参检查集中在 check_arguments，供后续关联调用复用。

#include "sema/body.hpp"
#include "sema/call.hpp"
#include "sema/sema.hpp"

namespace rx {

namespace {

ExpressionResult failed_call() {
    return ExpressionResult{SemanticType::error(), PlaceAccessInformation{}};
}

} // namespace

// 按声明参数类型逐个检查实参。
// 数量不等时只报一次数量错误，不越界访问；类型不兼容时由 require_coerce 定位到实参。
bool BodyChecker::check_arguments(
    const std::vector<SemanticType> &parameter_types,
    const std::vector<std::shared_ptr<Expression>> &arguments, Span at) {
    // 形参和实参数量对不上
    if (parameter_types.size() != arguments.size()) {
        diagnostics_.error(at, "call expects " +
                                   std::to_string(parameter_types.size()) +
                                   " argument(s), found " +
                                   std::to_string(arguments.size()));
        return false;
    }
    // 一次核对每个实参
    bool ok = true;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (!arguments[index]) {
            diagnostics_.error(at, "missing argument expression");
            ok = false;
            continue;
        }
        // 实参的期望类型来自被调声明，外层调用位置的期望不改变签名。
        const ExpressionResult argument =
            check_expression(*arguments[index], &parameter_types[index]);
        if (!can_coerce(argument.type, argument.place,
                        parameter_types[index])) {
            require_coerce(argument, parameter_types[index],
                           arguments[index]->span);
            ok = false;
        }
    }
    return ok;
}

// 解析并检查一次普通调用。
// 只支持单段 PathExpression callee；局部名字优先，且命中局部后不回退。
ExpressionResult BodyChecker::check_call(const CallExpression &call) {
    if (!call.callee)
        return failed_call();

    const auto *path = dynamic_cast<const PathExpression *>(call.callee.get());
    if (!path) {
        // Rx 没有函数指针/函数值调用；非路径 callee 不能解析为函数。
        diagnostics_.error(call.callee->span, "expression is not callable");
        return failed_call();
    }
    if (path->segments.empty())
        return failed_call();

    // 多段路径（S::new、Box::<i32>::new 等）和点方法留到 Phase 5。
    // 这里只占位，不报错，避免把后续阶段才实现的接受程序误拒。
    if (path->segments.size() != 1)
        return failed_call();

    const PathSegment &segment = path->segments[0];

    // N13：局部绑定优先；命中局部就不再回退到同名函数/内置函数。
    if (lookup_local(segment.name)) {
        diagnostics_.error(path->span,
                           "local binding " + segment.name +
                               " is not callable");
        return failed_call();
    }

    // 值名字空间：函数、常量、内置 I/O 共用。
    const auto value = analyzer_.function_constant_namespace.find(segment.name);
    if (value != analyzer_.function_constant_namespace.end()) {
        const Symbol *symbol = value->second;

        // 顶层/关联常量都不是可调用对象。
        if (symbol->kind == Symbol::Kind::Constant) {
            diagnostics_.error(path->span,
                               "constant " + segment.name +
                                   " is not callable");
            return failed_call();
        }

        if (symbol->kind == Symbol::Kind::Function) {
            // G2：当前语言没有泛型函数。
            if (!segment.generics.empty()) {
                diagnostics_.error(path->span,
                                   "functions have no type parameters: " +
                                       segment.name);
                return failed_call();
            }
            const auto *function =
                static_cast<const FunctionSymbol *>(symbol);
            std::vector<SemanticType> parameter_types;
            parameter_types.reserve(function->parameters.size());
            for (const auto &parameter : function->parameters)
                parameter_types.push_back(parameter.type);

            if (!check_arguments(parameter_types, call.arguments, call.span))
                return failed_call();
            // 调用结果是声明的返回类型；函数体是否发散不改变签名。
            return ExpressionResult{function->return_type,
                                    PlaceAccessInformation{}};
        }

        if (symbol->kind == Symbol::Kind::Builtin) {
            // 内置 I/O 同样没有类型参数。
            if (!segment.generics.empty()) {
                diagnostics_.error(path->span,
                                   "builtin functions have no type parameters: " +
                                       segment.name);
                return failed_call();
            }
            const auto signature = builtin_signature(segment.name);
            if (!signature) {
                diagnostics_.error(
                    path->span,
                    "unknown built-in function: " + segment.name);
                return failed_call();
            }
            if (!check_arguments(signature->parameters, call.arguments,
                                 call.span))
                return failed_call();
            return ExpressionResult{signature->return_type,
                                    PlaceAccessInformation{}};
        }
    }

    // 上方已经判断值名字空间未命中，同名类型名判为不可调用结构体。
    // 这样 struct S 与 fn S() 可以并存，调用按值名字空间解析。
    if (analyzer_.type_namespace.count(segment.name)) {
        diagnostics_.error(path->span,
                           "struct " + segment.name +
                               " is not callable; use struct literal syntax");
        return failed_call();
    }

    // N14：既不是值，也不是可调用结构体。
    diagnostics_.error(path->span,
                       "unresolved value name: " + segment.name);
    return failed_call();
}

} // namespace rx
