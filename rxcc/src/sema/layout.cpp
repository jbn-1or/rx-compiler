// 根据已解析的字段类型，检查结构体是否存在按值包含形成的无限布局环。
// 沿结构体和数组字段递归检查；引用、Box 和 Vec 提供间接存储，终止该路径的递归。
// 所有结构体都参与检查，包括未被使用的声明。

#include "sema/sema.hpp"

namespace rx {

// 从每个结构体出发做三色 DFS，共享同一张颜色表。
void SemanticAnalyzer::check_inline_layout_cycles() {
    LayoutVisitStates states;
    for (const auto &sym : structs_)
        visit_layout(sym.get(), states);
}

// 三色 DFS：Visiting 表示当前递归栈上存在该结构体，再次遇到即为内联环。
void SemanticAnalyzer::visit_layout(const StructSymbol *sym,
                                LayoutVisitStates &states) {
    if (!sym)
        return;

    LayoutVisitState &state = states[sym];
    if (state == LayoutVisitState::Visited)
        return;
    if (state == LayoutVisitState::Visiting) {
        diagnostics_.error(sym->span, "infinite inline layout cycle");
        return;
    }

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
            // 在访问表中找到这个结构体的visit态
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

} // namespace rx
