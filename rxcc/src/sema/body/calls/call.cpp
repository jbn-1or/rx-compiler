// 普通调用与显式 S::method(...) 调用，负责解析签名、检查实参
#include "sema/body/checker.hpp"
#include "sema/sema.hpp"

namespace rx {

// 生成显式 S::method(...) 调用使用的参数表。
// 接收者作为第一个参数排在最前面，后面才是声明中的普通参数。
std::vector<SemanticType> CallableSignature::explicit_parameters() const {
    std::vector<SemanticType> result;
    result.reserve(parameters.size() + (receiver ? 1 : 0));
    // 显式调用写全接收者，例如 S::method(&s, arg)。
    if (receiver)
        result.push_back(*receiver);
    result.insert(result.end(), parameters.begin(), parameters.end());
    return result;
}

// 把函数符号转换成调用签名：返回类型、可选接收者和普通参数类型。
CallableSignature function_signature(const FunctionSymbol &function) {
    CallableSignature signature;
    signature.return_type = function.return_type;
    // 只有方法带接收者，普通函数留空。
    if (function.receiver)
        signature.receiver = function.receiver->type;
    for (const auto &parameter : function.parameters)
        signature.parameters.push_back(parameter.type);
    return signature;
}

// 按参数表检查实参：先核对数量，再逐个以对应参数类型为期望定型并转换。
// 数量不符时仍检查已有实参，保留其中独立的名字/类型诊断；
// 多出的实参没有对应参数类型，只独立定型。
// 实参都是必经子表达式，另行汇总它们的发散状态。
bool BodyChecker::check_arguments(
    const std::vector<SemanticType> &parameter_types,
    const std::vector<std::shared_ptr<Expression>> &arguments, Span at,
    bool &diverges) {
    diverges = false;
    bool ok = parameter_types.size() == arguments.size();
    // 数量不匹配只报一次，但后续仍逐项检查实参。
    if (!ok) {
        diagnostics_.error(at, "call expects " +
                                   std::to_string(parameter_types.size()) +
                                   " argument(s), found " +
                                   std::to_string(arguments.size()));
    }
    // 数量错误也检查已有实参，保留其中独立的名字和类型诊断。
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        // 语法树缺少实参节点时记失败，避免访问空指针。
        if (!arguments[index]) {
            diagnostics_.error(at, "missing argument expression");
            ok = false;
            continue;
        }
        // 多出的实参没有期望类型，只能独立定型。
        const SemanticType *expected = index < parameter_types.size()
                                           ? &parameter_types[index] : nullptr;
        const auto argument = check_expression(*arguments[index], expected);
        diverges = diverges || argument.diverges;
        // 有对应参数时按固定目标核对；多出的实参只要求自身无错。
        const bool argument_ok = expected
                                     ? require_coerce(argument, *expected,
                                                      arguments[index]->span)
                                     : !argument.type.is_error();
        // 累积失败但继续检查后面的实参。
        ok = argument_ok && ok;
    }
    return ok;
}

// 普通调用和 S::f(...)
// 检查调用表达式：把被调者分为单段路径（普通函数/内置函数）和
// 多段路径（S::method 显式关联调用），分别解析签名后统一检查实参。
ExpressionResult BodyChecker::check_call(const CallExpression &call) {
    if (!call.callee)
        return ExpressionResult::error();
    // 只有路径表达式可以被调用。
    const auto *path = dynamic_cast<const PathExpression *>(call.callee.get());
    if (!path) {
        diagnostics_.error(call.callee->span, "expression is not callable");
        return ExpressionResult::error();
    }
    if (path->segments.empty()) {
        diagnostics_.error(path->span, "internal error: empty call path");
        return ExpressionResult::error();
    }
    // 单段路径：普通函数或内置函数。
    if (path->segments.size() == 1)
        return check_ordinary_call(*path, call);
    // 多段路径：先解析 S::method 的签名，再按显式参数表检查实参。
    const auto signature = resolve_qualified_callee(*path);
    bool diverges = false;
    if (!signature ||
        !check_arguments(signature->explicit_parameters(), call.arguments, call.span,
                         diverges))
        return ExpressionResult::error();
    return ExpressionResult::value(signature->return_type, diverges);
}

// 检查单段调用：名字只能指向普通函数或内置函数。
// 局部绑定、类型名和常量都不可调用；实参按解析出的签名逐项检查。
ExpressionResult BodyChecker::check_ordinary_call(const PathExpression &path,
                                                 const CallExpression &call) {
    const auto &segment = path.segments[0];
    // 局部值遮蔽函数时不能回退到外层函数名。
    if (lookup_local(segment.name)) {
        diagnostics_.error(path.span, "local binding " + segment.name +
                                         " is not callable");
        return ExpressionResult::error();
    }
    // 值命名空间里找不到名字时，区分“类型名被当函数”和“名字不存在”。
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
    // 常量不是函数。
    if (symbol.kind == Symbol::Kind::Constant) {
        diagnostics_.error(path.span, "constant " + segment.name + " is not callable");
        return ExpressionResult::error();
    }
    // 函数不支持类型实参。
    if (!segment.generics.empty()) {
        diagnostics_.error(path.span,
                           "functions have no type parameters: " + segment.name);
        return ExpressionResult::error();
    }
    // 普通函数取声明签名；其余值名尝试按内置函数解析。
    const auto signature = symbol.kind == Symbol::Kind::Function
                               ? std::optional(function_signature(
                                     static_cast<const FunctionSymbol &>(symbol)))
                               : builtin_signature(segment.name);
    if (!signature) {
        diagnostics_.error(path.span, "unknown built-in function: " + segment.name);
        return ExpressionResult::error();
    }
    // 正常的顶层函数和内置 I/O 没有接收者，直接使用原参数表。
    // 收集阶段已诊断的非法顶层接收者仍沿用显式参数表做错误恢复。
    bool diverges = false;
    const bool arguments_ok = signature->receiver
        ? check_arguments(signature->explicit_parameters(), call.arguments, call.span,
                          diverges)
        : check_arguments(signature->parameters, call.arguments, call.span, diverges);
    if (!arguments_ok)
        return ExpressionResult::error();
    return ExpressionResult::value(signature->return_type, diverges);
}

// 解析 S::method 这类显式关联调用的签名。路径必须恰好两段：
// 第一段是类型（含 Self），第二段是关联函数或内置方法。
std::optional<CallableSignature>
BodyChecker::resolve_qualified_callee(const PathExpression &path) {
    if (path.segments.size() != 2) {
        diagnostics_.error(path.span, "call path must have exactly two segments");
        return std::nullopt;
    }
    const auto &member = path.segments[1];
    // 成员名不支持类型实参。
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
    // 结构体先查声明里的关联项。
    if (owner.kind == SemanticType::Kind::Struct)
        return resolve_struct_member(*owner.struct_symbol, member, path.span);
    // 其他类型只提供内置构造器 new 和少量内置方法。
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
    // clone 额外要求类型实现 Clone。
    if (member.name == "clone" && !analyzer_.has_trait(owner, Derive::Clone)) {
        diagnostics_.error(path.span, "type " + owner.toString() +
                                         " does not support clone");
        return std::nullopt;
    }
    return signature;
}

// 在结构体的关联项里查找 S::member：普通关联函数直接取声明签名，
// 关联常量不可调用；声明里没有的 clone 可回退到派生的内置实现。
std::optional<CallableSignature>
BodyChecker::resolve_struct_member(const StructSymbol &structure,
                                  const PathSegment &member, Span at) {
    const auto item = structure.associated_items.find(member.name);
    if (item != structure.associated_items.end()) {
        // 关联常量不是可调用项。
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
