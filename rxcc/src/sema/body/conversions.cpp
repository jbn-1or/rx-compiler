// 函数体检查共用的适配规则：place 物化、单步解引用、隐式转换与公共类型。
// 表达式处理器返回原始类型和权限，调用方在固定目标处检查兼容性。

#include "sema/body/checker.hpp"
#include <cassert>

namespace rx {

// 值表达式需要存储位置时物化为可写临时 place；已有 place 保留原权限，
// 不能把不可变 place 变成可变副本。
PlaceAccessInformation BodyChecker::materialize_place(
    const PlaceAccessInformation &place) const {
    if (place.is_place)
        return place;
    return PlaceAccessInformation{true, true, false};
}

// 对引用或 Box 做一次解引用，返回内层类型及权限：&T / &mut T / Box<T>。
// 值基表达式先物化为临时 place；其他类型报错并返回 Error。
ExpressionResult BodyChecker::dereference_place(
    const SemanticType &base_type,
    const PlaceAccessInformation &base_place, Span at) {
    if (base_type.is_error())
        return ExpressionResult::error();

    const PlaceAccessInformation place = materialize_place(base_place);

    if (base_type.kind == SemanticType::Kind::Reference) {
        if (!base_type.inner)
            return ExpressionResult::error();
        // 解引用共享引用会设置 crossed_shared；后续即使遇到 &mut，也不能恢复写权限。
        // x: &&mut i32 时 **x 不可写；x: &mut &mut i32 且未跨共享访问时可以写。
        PlaceAccessInformation dereferenced{true, false, true};
        if (base_type.is_mut) {
            dereferenced.is_mutable = !place.crossed_shared;
            dereferenced.crossed_shared = place.crossed_shared;
        }
        return ExpressionResult::from_place(*base_type.inner, dereferenced);
    }
    // 解引用 Box 保留原 place 的可写性和共享访问限制。
    if (base_type.kind == SemanticType::Kind::Box) {
        if (!base_type.inner)
            return ExpressionResult::error();
        return ExpressionResult::from_place(
            *base_type.inner,
            PlaceAccessInformation{true, place.is_mutable,
                                   place.crossed_shared});
    }

    diagnostics_.error(at, "type " + base_type.toString() +
                               " cannot be dereferenced");
    return ExpressionResult::error();
}

// 源类型和 place、目标类型 → 是否兼容，不写诊断。转换只做判定，不改变源 ExpressionResult。
bool BodyChecker::can_coerce(const SemanticType &from,
                             const PlaceAccessInformation &place,
                             const SemanticType &to) const {
    assert(!from.is_error() && !to.is_error());
    // Never 路径不产生值，可以转换到任意目标。
    if (from.is_never())
        return true;

    const bool target_is_mutable_reference =
        to.kind == SemanticType::Kind::Reference && to.is_mut;

    // 可变重借用不能跨过共享访问；必须先于同类型判断。
    if (target_is_mutable_reference && place.crossed_shared)
        return false;

    // 同类型直接保持；&mut T → &T 也在此后按引用解引用规则处理。
    if (from.same_type(to))
        return true;

    // 只有引用到引用才可能发生强制转换。
    if (from.kind != SemanticType::Kind::Reference ||
        to.kind != SemanticType::Kind::Reference || !from.inner ||
        !to.inner)
        return false;

    // 可变目标要求源外层为 &mut；共享目标允许 &T 和 &mut T。
    if (target_is_mutable_reference && !from.is_mut)
        return false;

    // 从源引用的内部类型开始逐层内置解引用，直到与目标内部类型完全相等。
    const SemanticType *current = from.inner.get();
    for (;;) {
        if (current->same_type(*to.inner))
            return true;

        if (current->kind == SemanticType::Kind::Reference) {
            if (!current->inner)
                return false;
            // 可变目标不能经过共享引用；共享目标允许。
            if (target_is_mutable_reference && !current->is_mut)
                return false;
            current = current->inner.get();
            continue;
        }

        if (current->kind == SemanticType::Kind::Box) {
            if (!current->inner)
                return false;
            current = current->inner.get();
            continue;
        }

        // Vec、数组、整数、结构体等都不产生隐式解引用/切片转换。
        return false;
    }
}

// 表达式结果、固定目标、位置 → 是否兼容；不兼容时报诊断；转换本身不改写结果类型。
bool BodyChecker::require_coerce(const ExpressionResult &from,
                                const SemanticType &to, Span at) {
    if (from.type.is_error() || to.is_error())
        return false;
    if (can_coerce(from.type, from.place, to))
        return true;
    diagnostics_.error(at, "type mismatch: expected " + to.toString() +
                               ", found " + from.type.toString());
    return false;
}

// 无期望类型时，为一组表达式结果寻找公共类型
// 调用方必须先用 nullptr 期望独立检查每个结果，不能把 LUB 目标反过来
// 作为后续字面量的期望类型。找不到公共类型时只返回 Error，具体诊断由调用方决定。
SemanticType BodyChecker::compute_least_upper_bound(
    const std::vector<ExpressionResult> &results) const {
    // 任一结果已经出错时不再连锁诊断。
    for (const auto &result : results) {
        if (result.type.is_error())
            return SemanticType::error();
    }

    bool has_target = false;
    SemanticType target;

    for (std::size_t index = 0; index < results.size(); ++index) {
        const ExpressionResult &result = results[index];
        if (result.type.is_never())
            continue;
        // 当前无目标类型，设为当前类型
        if (!has_target) {
            target = result.type;
            has_target = true;
            continue;
        }
        // 可以类型转换，公共类型不变
        if (can_coerce(result.type, result.place, target))
            continue;

        // 当前结果不能转成现有目标时，只有此前所有原始结果都能转成
        // 当前结果，才允许把目标更新为当前结果。
        bool all_previous_convert = true;
        for (std::size_t previous = 0; previous < index; ++previous) {
            const ExpressionResult &earlier = results[previous];
            if (earlier.type.is_never())
                continue;
            if (!can_coerce(earlier.type, earlier.place, result.type)) {
                all_previous_convert = false;
                break;
            }
        }
        if (!all_previous_convert)
            return SemanticType::error();
        target = result.type;
    }

    // 全部结果都是 !（含空结果集）时，LUB 为 !。
    if (!has_target)
        return SemanticType::never();

    // 最终目标确定后逐项复核；尤其要检查首个结果本身能否适配目标，
    // 避免漏掉可变重借用等 place 权限限制。
    for (const auto &result : results) {
        if (!can_coerce(result.type, result.place, target))
            return SemanticType::error();
    }
    return target;
}

} // namespace rx
