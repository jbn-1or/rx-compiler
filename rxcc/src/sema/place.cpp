// 实现值的临时 place 物化，以及引用和 Box 的单步解引用。
// 计算解引用后的可写性，并保留访问路径上的共享限制。
// 供字段、索引、借用及方法接收者调整复用。

#include "sema/body.hpp"

namespace rx {

// 值表达式需要存储位置时物化为可写临时 place；已有 place 保留原权限，
// 不能把不可变 place 变成可变副本。
PlaceAccessInformation BodyChecker::materialize_place(
    const PlaceAccessInformation &place) const {
    if (place.is_place)
        return place;
    return PlaceAccessInformation{true, true, false};
}

// 执行一次内置解引用：&T / &mut T / Box<T>。
// 值基表达式先物化为临时 place；其他类型报错并返回 Error。
ExpressionResult BodyChecker::dereference_place(
    const SemanticType &base_type,
    const PlaceAccessInformation &base_place, Span at) {
    if (base_type.is_error())
        return ExpressionResult{SemanticType::error(),
                                PlaceAccessInformation{}};

    const PlaceAccessInformation place = materialize_place(base_place);

    if (base_type.kind == SemanticType::Kind::Reference) {
        if (!base_type.inner)
            return ExpressionResult{SemanticType::error(),
                                    PlaceAccessInformation{}};
        // 共享引用总是只读且跨过一层共享访问，只要曾跨过共享引用，可变引用也不可写
        // x = &&mut i32，**x不可写，x = &mut &mut i32可以
        PlaceAccessInformation dereferenced{true, false, true};
        if (base_type.is_mut) {
            dereferenced.is_mutable = !place.crossed_shared;
            dereferenced.crossed_shared = place.crossed_shared;
        }
        return ExpressionResult{*base_type.inner, dereferenced};
    }
    // box直接开盒
    if (base_type.kind == SemanticType::Kind::Box) {
        if (!base_type.inner)
            return ExpressionResult{SemanticType::error(),
                                    PlaceAccessInformation{}};
        return ExpressionResult{
            *base_type.inner,
            PlaceAccessInformation{true, place.is_mutable,
                                   place.crossed_shared}};
    }

    diagnostics_.error(at, "type " + base_type.toString() +
                               " cannot be dereferenced");
    return ExpressionResult{SemanticType::error(), PlaceAccessInformation{}};
}

} // namespace rx
