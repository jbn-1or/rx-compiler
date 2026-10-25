// 解析结构体字段、函数参数和返回类型，将结果写入对应符号。
// 同时保留参数可变性与 self 接收者形式，供函数体和调用检查使用。
// 签名定型后检查 main 的返回类型是否为 unit。

#include "sema/sema.hpp"

namespace rx {

// 遍历所有结构体和函数，把 AST 类型节点定型为 SemanticType。
// 单个字段/签名解析失败时保留 Error，不中断其余项。
void SemanticAnalyzer::resolve_declared_types() {
    for (auto &sym : structs_) {
        // 处理 struct 定义中的字段
        for (const auto &field : sym->fields) {
            sym->field_types[field.name] =
                field.type ? resolve_type(*field.type, sym.get())
                           : SemanticType::error();
        }
    }
    for (auto &sym : functions_)
        resolve_function_signature(*sym, sym->owner);
}

// 定型一个函数的参数、返回类型和接收者形态。
// `fn f(mut x: i32) -> &mut S` → parameters={i32, is_mut=true}、return_type=&mut S。
void SemanticAnalyzer::resolve_function_signature(FunctionSymbol &fn,
                                                  StructSymbol *owner) {
    fn.parameters.clear();
    fn.self_parameter = ResolvedReceiver{};
    if (!fn.node) {
        fn.return_type = SemanticType::error();
        return;
    }

    // 处理函数形式参数
    fn.parameters.reserve(fn.node->parameters.size());
    for (const auto &param : fn.node->parameters) {
        const SemanticType type =
            param.type ? resolve_type(*param.type, owner)
                       : SemanticType::error();
        fn.parameters.push_back(ResolvedParameter{type, param.is_mut});
    }
    // 处理函数返回类型
    fn.return_type =
        fn.node->return_type ? resolve_type(*fn.node->return_type, owner)
                             : SemanticType::error();

    if (!fn.node->self_parameter)
        return;
    fn.self_parameter.present = true;
    switch (fn.node->self_parameter->kind) {
        case SelfParameter::Kind::Value:
            break;
        case SelfParameter::Kind::MutableValue:
            fn.self_parameter.is_mut = true;
            break;
        case SelfParameter::Kind::Reference:
            fn.self_parameter.by_reference = true;
            break;
        case SelfParameter::Kind::MutableReference:
            fn.self_parameter.by_reference = true;
            fn.self_parameter.is_mut = true;
            break;
    }
}

// 检查 main 的返回类型（E4）。
// 避免把 ConstantSymbol 当成 FunctionSymbol；返回类型解析失败时静默放行。
void SemanticAnalyzer::check_main_return_type() {
    const auto it = function_constant_namespace.find("main");
    // 没有 main（E1 已报）或 main 不是函数时跳过。
    if (it == function_constant_namespace.end() ||
        it->second->kind != Symbol::Kind::Function)
        return;
    const auto *fn = static_cast<const FunctionSymbol *>(it->second);
    if (fn->return_type.is_error() || fn->return_type.is_unit())
        return;

    const Span at = fn->node && fn->node->return_type
                        ? fn->node->return_type->span
                        : fn->span;
    diagnostics_.error(at, "main must return unit");
}

} // namespace rx
