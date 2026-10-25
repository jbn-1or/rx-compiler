// 查询类型是否具备 Copy、Clone、PartialEq 和 Eq 能力，并验证结构体的 derive 请求。
// 检查派生所需的配套声明及字段能力，使用访问集合保证递归类型的查询能够终止。
// 能力查询同时供后续运算、数组重复和方法调用检查复用。

#include "sema/sema.hpp"

namespace rx {

// 查询类型是否具备 trait 能力（D6）；结构体递归时用 visiting 假设成立，
// 保证递归类型能够终止（D7）。Error 视为满足，避免连锁诊断。
bool SemanticAnalyzer::has_trait(
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
            // 别的trait解引用递归查
            return has_trait(*type.inner, trait, visiting);
        case SemanticType::Kind::Array:
            return type.inner && has_trait(*type.inner, trait, visiting);
        case SemanticType::Kind::Box:
        case SemanticType::Kind::Vec:
            if (trait == Derive::Copy)
                return false;
            return type.inner && has_trait(*type.inner, trait, visiting);
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
                if (!has_trait(field.second, trait, visiting)) {
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

// 检查一个 derive：Copy 必须显式带 Clone，Eq 必须显式带 PartialEq；
// 然后要求每个字段都具备该能力。
void SemanticAnalyzer::check_one_derive(StructSymbol &sym, Derive trait) {
    if (trait == Derive::Copy && !sym.derives.count(Derive::Clone))
        diagnostics_.error(sym.span, "Copy requires an explicit Clone derive");
    if (trait == Derive::Eq && !sym.derives.count(Derive::PartialEq))
        diagnostics_.error(
            sym.span, "Eq requires an explicit PartialEq derive");

    for (const auto &field : sym.fields) {
        const auto it = sym.field_types.find(field.name);
        if (it == sym.field_types.end())
            continue;

        std::set<std::pair<const StructSymbol *, Derive>> visiting;
        if (!has_trait(it->second, trait, visiting)) {
            diagnostics_.error(field.span, "field " + field.name +
                                          " does not support " + name(trait));
        }
    }
}

} // namespace rx
