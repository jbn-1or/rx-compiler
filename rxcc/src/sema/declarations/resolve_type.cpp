// 将 AST 类型转换为统一的 SemanticType，解析类型名并检查类型参数数量。
// 在当前结构体上下文中展开 Self，通过常量求值器确定数组长度。
// 解析失败时记录诊断并返回 Error，供后续检查恢复使用。

#include "sema/sema.hpp"
#include "sema/type_utils.hpp"

namespace rx {

// 类型位置、关联路径和结构体构造共享同一套类型名与类型实参规则。
// Self       → 使用 current 对应的 StructSymbol
// 基本类型   → basic_type_from_name
// Box / Vec  → 检查一个类型实参 → 递归 
SemanticType SemanticAnalyzer::resolve_type_segment(
    const PathSegment &seg, const StructSymbol *current, Span at) {
    if (seg.name == "self") {
        diagnostics_.error(at, "self is not a type name");
        return SemanticType::error();
    }
    // Self 只表示当前结构体/impl 目标
    if (seg.name == "Self") {
        // 当前没有可供 Self 展开的结构体上下文。
        if (!current) {
            diagnostics_.error(
                at,
                "Self is not available outside a struct or impl");
            return SemanticType::error();
        }
        // Self 不接受类型实参。
        if (!seg.generics.empty()) {
            diagnostics_.error(at,
                         "structs have no type parameters: Self");
            return SemanticType::error();
        }
        return SemanticType::struct_(current);
    }

    // 基本类型不接受类型实参。
    const SemanticType basic = basic_type_from_name(seg.name);
    if (!basic.is_error()) {
        if (!seg.generics.empty()) {
            diagnostics_.error(at,
                         "primitives take no type parameters: " +
                             seg.name);
            return SemanticType::error();
        }
        return basic;
    }

    // Box/Vec 恰好一个具体类型实参
    if (seg.name == "Box" || seg.name == "Vec") {
        if (seg.generics.size() != 1) {
            diagnostics_.error(
                at,
                seg.name + " requires exactly one concrete type argument");
            return SemanticType::error();
        }
        if (!seg.generics[0])
            return SemanticType::error();
        // 从 AST 类型节点得到 SemanticType
        const SemanticType inner = resolve_type(*seg.generics[0], current);
        if (inner.is_error())
            return SemanticType::error();
        return seg.name == "Box" ? SemanticType::boxed(inner)
                                 : SemanticType::vec(inner);
    }

    // 用户结构体：名义类型，不接受类型实参。
    const auto it = type_namespace.find(seg.name);
    if (it == type_namespace.end()) {  // 没找到
        diagnostics_.error(at, "unresolved type: " + seg.name);
        return SemanticType::error();
    }
    if (!seg.generics.empty()) { // 不能有实参
        diagnostics_.error(at,
                     "structs have no type parameters: " + seg.name);
        return SemanticType::error();
    }
    return SemanticType::struct_(it->second);
}

// 把基本类型、Unit、引用、数组、Box/Vec、用户结构体和 Self 转换成语义类型；
SemanticType SemanticAnalyzer::resolve_type(const Type &type,
                                    const StructSymbol *current) {
    // `()`：函数/控制流结果，也是省略返回类型时的类型。
    if (dynamic_cast<const UnitType *>(&type))
        return SemanticType::unit();
    // PathType → resolve_type_segment(唯一的路径段, current, span)
    if (const auto *pt = dynamic_cast<const PathType *>(&type)) {
        // 类型位置没有 associated type；多段路径（如 S::T）无法解析。
        if (pt->segments.size() != 1) {
            diagnostics_.error(type.span, "unresolved type: " + pt->name());
            return SemanticType::error();
        }

        return resolve_type_segment(pt->segments[0], current, type.span);
    }

    // &T / &mut T → 递归 resolve_type(内层) → 构造引用类型
    if (const auto *rt = dynamic_cast<const ReferenceType *>(&type)) {
        if (!rt->inner)
            return SemanticType::error();
        const SemanticType inner = resolve_type(*rt->inner, current);
        if (inner.is_error())
            return SemanticType::error();
        return SemanticType::reference(rt->is_mut, inner);
    }

    // ArrayType → 递归 resolve_type(元素)
    //           → evaluate_constant_value(长度, usize, current)
    //           → 核对 usize 和值 → 构造数组类型
    // [T; N]：元素递归解析，长度按 usize 常量求值后存成 uint64_t。
    if (const auto *at = dynamic_cast<const ArrayType *>(&type)) {
        if (!at->element)
            return SemanticType::error();
        const SemanticType element = resolve_type(*at->element, current);
        if (element.is_error())
            return SemanticType::error();

        const SemanticType usize_type =
            SemanticType::int_(SemanticType::IntKind::Usize);
        // 得到数组长度
        const EvaluatedConstant length =
            evaluate_constant_value(at->length, &usize_type, current);
        // 求值器已报 C1/C2/C4/C5 等错误时不再重复报 C3。
        if (!length.ok || length.type.is_error())
            return SemanticType::error();
        if (!length.type.same_type(usize_type)) {  // 数组长度不是 usize
            diagnostics_.error(at->length.span,
                         "array length must be a usize constant");
            return SemanticType::error();
        }
        // usize 求值结果必须以 uint64_t 保存；否则视为不可用的数组长度。
        const auto *len = std::get_if<uint64_t>(&length.value);
        if (!len) {
            diagnostics_.error(at->length.span,
                         "array length must be a usize constant");
            return SemanticType::error();
        }
        return SemanticType::array(element, *len);
    }

    diagnostics_.error(type.span, "internal error: unrecognized type node");
    return SemanticType::error();
}

} // namespace rx
