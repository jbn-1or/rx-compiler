// 先解析调用签名，再按签名检查实参；调用位置的期望不参与签名选择。
#include "sema/body/checker.hpp"
#include "sema/sema.hpp"

namespace rx {

std::vector<SemanticType> CallableSignature::explicit_parameters() const {
    std::vector<SemanticType> result;
    result.reserve(parameters.size() + (receiver ? 1 : 0));
    if (receiver)
        result.push_back(*receiver);
    result.insert(result.end(), parameters.begin(), parameters.end());
    return result;
}

CallableSignature function_signature(const FunctionSymbol &function) {
    CallableSignature signature;
    signature.return_type = function.return_type;
    if (function.receiver)
        signature.receiver = function.receiver->type;
    for (const auto &parameter : function.parameters)
        signature.parameters.push_back(parameter.type);
    return signature;
}

bool BodyChecker::check_arguments(
    const std::vector<SemanticType> &parameter_types,
    const std::vector<std::shared_ptr<Expression>> &arguments, Span at) {
    bool ok = parameter_types.size() == arguments.size();
    if (!ok) {
        diagnostics_.error(at, "call expects " +
                                   std::to_string(parameter_types.size()) +
                                   " argument(s), found " +
                                   std::to_string(arguments.size()));
    }
    // 数量错误也检查已有实参，保留其中独立的名字和类型诊断。
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (!arguments[index]) {
            diagnostics_.error(at, "missing argument expression");
            ok = false;
            continue;
        }
        const SemanticType *expected = index < parameter_types.size()
                                           ? &parameter_types[index] : nullptr;
        const auto argument = check_expression(*arguments[index], expected);
        const bool argument_ok = expected
                                     ? require_coerce(argument, *expected,
                                                      arguments[index]->span)
                                     : !argument.type.is_error();
        ok = argument_ok && ok;
    }
    return ok;
}

ExpressionResult BodyChecker::check_call(const CallExpression &call) {
    if (!call.callee)
        return ExpressionResult::error();
    const auto *path = dynamic_cast<const PathExpression *>(call.callee.get());
    if (!path) {
        diagnostics_.error(call.callee->span, "expression is not callable");
        return ExpressionResult::error();
    }
    if (path->segments.empty()) {
        diagnostics_.error(path->span, "internal error: empty call path");
        return ExpressionResult::error();
    }
    if (path->segments.size() == 1)
        return check_ordinary_call(*path, call);
    const auto signature = resolve_qualified_callee(*path);
    if (!signature ||
        !check_arguments(signature->explicit_parameters(), call.arguments, call.span))
        return ExpressionResult::error();
    return ExpressionResult::value(signature->return_type);
}

ExpressionResult BodyChecker::check_ordinary_call(const PathExpression &path,
                                                 const CallExpression &call) {
    const auto &segment = path.segments[0];
    // 局部值遮蔽函数时不能回退到外层函数名。
    if (lookup_local(segment.name)) {
        diagnostics_.error(path.span, "local binding " + segment.name +
                                         " is not callable");
        return ExpressionResult::error();
    }
    const auto value = analyzer_.function_constant_namespace.find(segment.name);
    if (value == analyzer_.function_constant_namespace.end()) {
        if (analyzer_.type_namespace.count(segment.name))
            diagnostics_.error(path.span, "struct " + segment.name +
                                             " is not callable; use struct literal syntax");
        else
            diagnostics_.error(path.span, "unresolved value name: " + segment.name);
        return ExpressionResult::error();
    }
    const Symbol &symbol = *value->second;
    if (symbol.kind == Symbol::Kind::Constant) {
        diagnostics_.error(path.span, "constant " + segment.name + " is not callable");
        return ExpressionResult::error();
    }
    if (!segment.generics.empty()) {
        diagnostics_.error(path.span,
                           "functions have no type parameters: " + segment.name);
        return ExpressionResult::error();
    }
    const auto signature = symbol.kind == Symbol::Kind::Function
                               ? std::optional(function_signature(
                                     static_cast<const FunctionSymbol &>(symbol)))
                               : builtin_signature(segment.name);
    if (!signature) {
        diagnostics_.error(path.span, "unknown built-in function: " + segment.name);
        return ExpressionResult::error();
    }
    if (!check_arguments(signature->explicit_parameters(), call.arguments, call.span))
        return ExpressionResult::error();
    return ExpressionResult::value(signature->return_type);
}

std::optional<CallableSignature>
BodyChecker::resolve_qualified_callee(const PathExpression &path) {
    if (path.segments.size() != 2) {
        diagnostics_.error(path.span, "call path must have exactly two segments");
        return std::nullopt;
    }
    const auto &member = path.segments[1];
    if (!member.generics.empty()) {
        diagnostics_.error(path.span,
                           "functions have no type parameters: " + member.name);
        return std::nullopt;
    }
    // 类型前缀不查局部值；Self 和具体类型实参统一由类型解析器处理。
    const auto owner = analyzer_.resolve_type_segment(
        path.segments[0], current_owner_, path.span);
    if (owner.is_error())
        return std::nullopt;
    if (owner.kind == SemanticType::Kind::Struct)
        return resolve_struct_member(*owner.struct_symbol, member, path.span);
    if (member.name == "new") {
        if (auto constructor = container_constructor_signature(owner))
            return constructor;
    }
    const auto signature = builtin_method_signature(owner, member.name);
    if (!signature) {
        diagnostics_.error(path.span, "type " + owner.toString() +
                                         " has no member named " + member.name);
        return std::nullopt;
    }
    if (member.name == "clone" && !analyzer_.has_trait(owner, Derive::Clone)) {
        diagnostics_.error(path.span, "type " + owner.toString() +
                                         " does not support clone");
        return std::nullopt;
    }
    return signature;
}

std::optional<CallableSignature>
BodyChecker::resolve_struct_member(const StructSymbol &structure,
                                  const PathSegment &member, Span at) {
    const auto item = structure.associated_items.find(member.name);
    if (item != structure.associated_items.end()) {
        if (item->second->kind == Symbol::Kind::Constant) {
            diagnostics_.error(at, "constant " + structure.name + "::" + member.name +
                                       " is not callable");
            return std::nullopt;
        }
        return function_signature(static_cast<const FunctionSymbol &>(*item->second));
    }
    const auto owner = SemanticType::struct_(&structure);
    // 显式关联调用先查 inherent 成员，未找到时才回退到派生 Clone。
    if (member.name == "clone" && analyzer_.has_trait(owner, Derive::Clone))
        return builtin_method_signature(owner, member.name);
    diagnostics_.error(at, "struct " + structure.name +
                               " has no associated item named " + member.name);
    return std::nullopt;
}

} // namespace rx
