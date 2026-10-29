// 全局声明定型与验证：字段/函数签名 → main 返回类型 → 布局 → derive。
// run 按这个顺序调用；常量求值和语法类型解析分别见同目录的专用文件。
// has_trait 同时作为函数体中运算、数组重复和 Clone 查找的共享查询。
// 主要用于resolve_declared_types，check_main_return_type，check_inline_layout_cycles，check_derives

#include "sema/sema.hpp"
#include <cassert>

namespace rx {

// 遍历所有结构体和函数，把 AST 类型节点定型为 SemanticType。

// 单个字段/签名解析失败时保留 Error，不中断其余项。
void SemanticAnalyzer::resolve_declared_types() {
    // 对每个structsysymbol
    for (auto &sym : structs_) {
        // 处理 struct 定义中的字段
        for (const auto &field : sym->fields) {
            sym->field_types[field.name] =
                field.type ? resolve_type(*field.type, sym.get())
                           : SemanticType::error();
        }
    }
    // 对每个FunctionSymbol
    for (auto &sym : functions_)
        resolve_function_signature(*sym, sym->owner);
}

// 定型一个函数的参数、返回类型和接收者形态。
// `fn f(mut x: i32) -> &mut S` → parameters={i32, is_mut=true}、return_type=&mut S。
void SemanticAnalyzer::resolve_function_signature(FunctionSymbol &fn,
                                                  StructSymbol *owner) {
    assert(fn.node && fn.node->return_type);
    fn.parameters.clear();
    fn.receiver.reset();
    fn.parameters.reserve(fn.node->parameters.size());
    // 每个普通参数 → resolve_type(param.type, owner) → fn.parameters
    for (const auto &param : fn.node->parameters) {
        assert(param.type);
        fn.parameters.push_back(
            ResolvedParameter{resolve_type(*param.type, owner), param.is_mut});
    }
    // 返回类型 → resolve_type(return_type, owner) 
    fn.return_type = resolve_type(*fn.node->return_type, owner);
    // 如有 self，结合 owner 和接收者写法 → fn.receiver
    if (!fn.node->self_parameter)
        return;
    const auto kind = fn.node->self_parameter->kind;
    const bool by_reference = kind == SelfParameter::Kind::Reference ||
                              kind == SelfParameter::Kind::MutableReference;
    const bool is_mut = kind == SelfParameter::Kind::MutableValue ||
                        kind == SelfParameter::Kind::MutableReference;
    const SemanticType owner_type = owner ? SemanticType::struct_(owner)
                                          : SemanticType::error();
    const SemanticType receiver_type = by_reference && !owner_type.is_error()
                                          ? SemanticType::reference(is_mut, owner_type)
                                          : owner_type;
    // &mut self 的可变性属于引用，绑定本身仍不可重新赋值。
    fn.receiver = ResolvedReceiver{receiver_type, !by_reference && is_mut};
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

// 从每个结构体出发做三色 DFS，共享同一张颜色表。
void SemanticAnalyzer::check_inline_layout_cycles() {
    LayoutVisitStates states;
    for (const auto &sym : structs_)
        visit_layout(sym.get(), states);
}

// dfs检查结构体是否能完善的定义
void SemanticAnalyzer::visit_layout(const StructSymbol *sym,
                                LayoutVisitStates &states) {
    if (!sym)
        return;

    LayoutVisitState &state = states[sym];
    if (state == LayoutVisitState::Visited)
        return;
    // 递归边在 visit_layout_type 中检测环，并用字段 span 报错。
    assert(state != LayoutVisitState::Visiting);

    state = LayoutVisitState::Visiting;
    // 检查结构体内的所有字段
    for (const auto &field : sym->fields) {
        const auto it = sym->field_types.find(field.name);
        // 字段类型解析失败时已报过错，不在这里制造连锁布局错误。
        if (it == sym->field_types.end() || it->second.is_error())
            continue;
        visit_layout_type(it->second, field.span, states);
    }
    state = LayoutVisitState::Visited;
}

// 数组按值内联元素，继续向下；引用/Box/Vec 是间接层，打断内联。
void SemanticAnalyzer::visit_layout_type(const SemanticType &type, Span at,
                                     LayoutVisitStates &states) {
    switch (type.kind) {
        case SemanticType::Kind::Struct: {
            if (!type.struct_symbol)
                return;
            // 若目标结构体为 Visiting，说明按值字段指回当前递归栈，形成了环。
            const auto it = states.find(type.struct_symbol);
            if (it != states.end() &&
                it->second == LayoutVisitState::Visiting) {
                diagnostics_.error(at, "infinite inline layout cycle");
                return;
            }
            visit_layout(type.struct_symbol, states);
            return;
        }
        // 若为数组则对其类型递归检查
        case SemanticType::Kind::Array:
            if (type.inner)
                visit_layout_type(*type.inner, at, states);
            return;
        case SemanticType::Kind::Unit:
        case SemanticType::Kind::Never:
        case SemanticType::Kind::Error:
        case SemanticType::Kind::Int:
        case SemanticType::Kind::Bool:
        case SemanticType::Kind::Reference:
        case SemanticType::Kind::Box:
        case SemanticType::Kind::Vec:
            return;
    }
}

// 查询类型是否具备 trait 能力（D6）；结构体递归时用 visiting 假设成立，
// 保证递归类型能够终止（D7）。Error 视为满足，避免连锁诊断。
bool SemanticAnalyzer::has_trait(const SemanticType &type, Derive trait) const {
    std::set<std::pair<const StructSymbol *, Derive>> visiting;
    return has_trait_impl(type, trait, visiting);
}

bool SemanticAnalyzer::has_trait_impl(
    const SemanticType &type, Derive trait,
    std::set<std::pair<const StructSymbol *, Derive>> &visiting) const {
    switch (type.kind) {
        case SemanticType::Kind::Int:
        case SemanticType::Kind::Bool:
            return true;
        case SemanticType::Kind::Error:
            return true;
        case SemanticType::Kind::Unit:
        case SemanticType::Kind::Never:
            return false;
        case SemanticType::Kind::Reference:
            if (!type.inner)
                return false;
            // &T 无条件 Copy/Clone；&mut T 两者都没有。
            if (trait == Derive::Copy || trait == Derive::Clone)
                return !type.is_mut;
            // PartialEq/Eq 继续查询引用目标的能力。
            return has_trait_impl(*type.inner, trait, visiting);
        case SemanticType::Kind::Array:
            return type.inner && has_trait_impl(*type.inner, trait, visiting);
        case SemanticType::Kind::Box:
        case SemanticType::Kind::Vec:
            if (trait == Derive::Copy)
                return false;
            return type.inner && has_trait_impl(*type.inner, trait, visiting);
        case SemanticType::Kind::Struct: {
            const StructSymbol *sym = type.struct_symbol;
            // 结构体必须显式请求该 trait。
            if (!sym || !sym->derives.count(trait))
                return false;
            // Copy 必须显式请求 Clone；Eq 必须显式请求 PartialEq。
            if (trait == Derive::Copy && !sym->derives.count(Derive::Clone))
                return false;
            if (trait == Derive::Eq && !sym->derives.count(Derive::PartialEq))
                return false;

            const auto key = std::make_pair(sym, trait);
            if (visiting.count(key))
                return true;
            visiting.insert(key);
            bool ok = true;
            // 所有字段都必须具备该能力。
            for (const auto &field : sym->field_types) {
                if (!has_trait_impl(field.second, trait, visiting)) {
                    ok = false;
                    break;
                }
            }
            visiting.erase(key);
            return ok;
        }
    }
    return false;
}

// 逐个结构体、逐个显式 derive 检查配套要求和字段能力。
void SemanticAnalyzer::check_derives() {
    for (auto &sym : structs_) {
        for (Derive trait : sym->derives)
            check_one_derive(*sym, trait);
    }
}

// 检查struct是否能有一个 derive 的能力
void SemanticAnalyzer::check_one_derive(StructSymbol &sym, Derive trait) {
    // 检查一个 derive：Copy 必须显式带 Clone，Eq 必须显式带 PartialEq；
    if (trait == Derive::Copy && !sym.derives.count(Derive::Clone))
        diagnostics_.error(sym.span, "Copy requires an explicit Clone derive");
    if (trait == Derive::Eq && !sym.derives.count(Derive::PartialEq))
        diagnostics_.error(
            sym.span, "Eq requires an explicit PartialEq derive");
    // 每个已定型字段 → has_trait
    for (const auto &field : sym.fields) {
        const auto it = sym.field_types.find(field.name);
        if (it == sym.field_types.end())
            continue;

        if (!has_trait(it->second, trait)) {
            diagnostics_.error(field.span, "field " + field.name +
                                          " does not support " + name(trait));
        }
    }
}

} // namespace rx
