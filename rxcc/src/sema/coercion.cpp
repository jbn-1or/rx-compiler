// 实现隐式类型转换判定、固定目标的类型检查及多结果的最小上界计算。
// 转换判定同时考虑类型与访问权限，限制跨共享访问的可变重借用。
// 保留表达式的原始检查结果，仅判断兼容性并在需要时报告类型不匹配。

#include "sema/body.hpp"

namespace rx {

// 一对一转换判定（CV5/CV6）。转换只做判定，不改变源 ExpressionResult。
// true：可以转换。同类不需转换或合法隐式转换，false：不能转换为目标类
bool BodyChecker::can_coerce(const SemanticType &from,
                             const PlaceAccessInformation &place,
                             const SemanticType &to) const {
    // 错误恢复与发散：Error 静默放行，Never 可转任意目标。
    if (from.is_error() || to.is_error() || from.is_never())
        return true;

    const bool target_is_mutable_reference =
        to.kind == SemanticType::Kind::Reference && to.is_mut;

    // 可变重借用不能跨过共享访问；必须先于同类型判断。
    if (target_is_mutable_reference && place.crossed_shared)
        return false;

    // 同类型直接保持；&mut T → &T 也在此后按引用解引用规则处理。
    if (from.equals(to))
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
    while (current) {
        if (current->equals(*to.inner))
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
    return false;
}

// 在固定目标位置统一报告一次不兼容错误；转换本身不改写结果类型。
void BodyChecker::require_coerce(const ExpressionResult &from,
                                const SemanticType &to, Span at) {
    if (can_coerce(from.type, from.place, to))
        return;
    diagnostics_.error(at, "type mismatch: expected " + to.toString() +
                               ", found " + from.type.toString());
}

// 无期望类型时，为一组表达式结果寻找公共类型
// 调用方必须先用 nullptr 期望独立检查每个结果，不能把 LUB 目标反过来
// 作为后续字面量的期望类型。失败属未定义行为：不追加诊断，返回 Error。
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
