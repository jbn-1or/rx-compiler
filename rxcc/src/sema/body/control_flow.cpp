// 分支与 loop 的固定目标通过 require_coerce 检查，无目标时独立定型后求 LUB。
// 所有分支及不可达 break 仍参与检查；while 条件屏障限制跳转目标。
#include "sema/body/checker.hpp"
#include <algorithm>
#include <utility>

namespace rx {

// 查找 break/continue 最近的跳转目标，只看循环帧栈顶。
// 栈顶是 while 条件屏障时返回 BlockedByBarrier，不继续向外找，
// 保证条件里的跳转不会越过屏障绑到外层循环。
BodyChecker::JumpTargetLookup BodyChecker::find_jump_target() const {
    // 帧栈为空说明当前不在任何 loop/while 里。
    if (loop_frames_.empty())
        return {JumpTargetStatus::NotFound, 0};
    const std::size_t index = loop_frames_.size() - 1;
    // 帧只有 loop、while 和条件屏障，栈顶就决定最近的目标或屏障。
    return {loop_frames_[index].kind == LoopFrame::Kind::WhileConditionBarrier
                ? JumpTargetStatus::BlockedByBarrier : JumpTargetStatus::Found,
            index};
}

// 检查 if/while 的条件：以 bool 为期望类型定型条件表达式，
// 再在固定目标处核对能否转换成 bool，同时保留条件的发散状态。
ExpressionResult BodyChecker::check_bool_condition(const Expression &condition,
                                                    Span at) {
    const auto boolean = SemanticType::bool_();
    const auto result = check_expression(condition, &boolean);
    if (!require_coerce(result, boolean, at))
        return ExpressionResult::error();
    return result;
}

// 检查 if 表达式：条件必须是 bool；无 else 时整个 if 固定为 ()。
// 有 else 且外层有期望类型时，两支都按该类型检查并逐支核对；
// 没有期望类型时两支独立定型，再求公共类型作为 if 的结果。
// 类型与发散分别汇总；条件发散或两支均发散时，整个 if 发散。
ExpressionResult BodyChecker::check_if(const IfExpression &expression,
                                       const SemanticType *expected) {
    // 缺少条件或 then 分支属于语法/内部错误，直接失败。
    if (!expression.condition || !expression.then)
        return ExpressionResult::error();
    // 条件错误不阻断分支检查，避免漏掉两支里的独立诊断。
    const auto condition =
        check_bool_condition(*expression.condition, expression.condition->span);
    const bool condition_ok = !condition.type.is_error();
    if (!expression.else_) {
        // 没有 else：if 的结果只能是 ()，then 分支按 () 期望检查。
        const auto unit = SemanticType::unit();
        const auto then_result = check_expression(*expression.then, &unit);
        const bool then_ok = require_coerce(then_result, unit, expression.then->span);
        if (!condition_ok || !then_ok)
            return ExpressionResult::error();
        return ExpressionResult::value(unit, condition.diverges);
    }
    // 有 else：外层期望类型同时传给两支，固定目标才能逐支核对。
    const auto then_result = check_expression(*expression.then, expected);
    const auto else_result = check_expression(*expression.else_, expected);
    const bool diverges = condition.diverges ||
                          (then_result.diverges && else_result.diverges);
    if (expected) {
        // 两支分别检查，不能用短路逻辑跳过后一支的诊断。
        const bool then_ok =
            require_coerce(then_result, *expected, expression.then->span);
        const bool else_ok =
            require_coerce(else_result, *expected, expression.else_->span);
        if (!condition_ok || !then_ok || !else_ok)
            return ExpressionResult::error();
        // 两支类型均为 ! 时，结果仍取 ! 而不是期望类型。
        if (then_result.type.is_never() && else_result.type.is_never())
            return ExpressionResult::value(SemanticType::never());
        return ExpressionResult::value(*expected, diverges);
    }
    if (!condition_ok || then_result.type.is_error() || else_result.type.is_error())
        return ExpressionResult::error();
    // 没有外层期望：用两支的公共类型给整个 if 定型。
    const auto common = compute_least_upper_bound({then_result, else_result});
    if (common.is_error()) {
        diagnostics_.error(expression.span, "if branches do not have a common type");
        return ExpressionResult::error();
    }
    return ExpressionResult::value(common, diverges);
}

// 检查 while 表达式。条件先放入屏障帧，禁止 break/continue 越过条件
// 绑到外层循环；循环体固定为 ()，while 自身无论条件如何都固定为 ()。
ExpressionResult BodyChecker::check_while(const WhileExpression &expression) {
    if (!expression.condition || !expression.body)
        return ExpressionResult::error();
    // 条件里的跳转只能看到屏障帧：即使外层是 loop 也不能从条件跳出去。
    loop_frames_.push_back({LoopFrame::Kind::WhileConditionBarrier, std::nullopt, {}});
    const auto condition =
        check_bool_condition(*expression.condition, expression.condition->span);
    loop_frames_.pop_back();

    const auto unit = SemanticType::unit();
    // 循环体单独压入 while 帧，break/continue 才允许绑到本循环。
    loop_frames_.push_back({LoopFrame::Kind::While, std::nullopt, {}});
    const auto body = check_expression(*expression.body, &unit);
    loop_frames_.pop_back();
    // 循环体是块，非尾语句后必须能收敛到 ()。
    const bool body_ok = require_coerce(body, unit, expression.body->span);
    // 条件或循环体任一不合法，整体记为 Error。
    if (condition.type.is_error() || !body_ok)
        return ExpressionResult::error();
    // 即使条件恒为 true，while 的静态类型也固定为 ()。
    return ExpressionResult::value(unit, condition.diverges);
}

// 检查 loop 表达式。帧里记录外层期望类型（如有），循环体固定按 () 检查；
// 所有 break 的值先收集到帧里，循环结束后再按固定目标或公共类型定型。
// 无 break 或全部 break 值类型为 ! 时，loop 类型为 !；发散状态单独汇总。
ExpressionResult BodyChecker::check_loop(const LoopExpression &expression,
                                         const SemanticType *expected) {
    if (!expression.body)
        return ExpressionResult::error();
    // 记下外层期望，供循环体内的 break 提前核对；帧同时收集 break 值。
    const auto target = expected ? std::optional(*expected) : std::nullopt;
    loop_frames_.push_back({LoopFrame::Kind::Loop, target, {}});
    const auto unit = SemanticType::unit();
    const auto body = check_expression(*expression.body, &unit);
    // 检查期间可能压入嵌套循环导致扩容，先整体取走帧再出栈。
    LoopFrame frame = std::move(loop_frames_.back());
    loop_frames_.pop_back();
    // 循环体是块，非尾语句后必须能收敛到 ()。
    if (!require_coerce(body, unit, expression.body->span))
        return ExpressionResult::error();
    // 任意 break 值出错，整个 loop 记为 Error。
    if (std::any_of(frame.break_values.begin(), frame.break_values.end(),
                    [](const auto &value) { return value.type.is_error(); }))
        return ExpressionResult::error();
    // 没有 break 或全部 break 的操作数为 ! 时，loop 仍为 !。
    // all_of 对空区间返回 true，正好覆盖“无 break”的情况。
    if (std::all_of(frame.break_values.begin(), frame.break_values.end(),
                    [](const auto &value) { return value.type.is_never(); }))
        return ExpressionResult::value(SemanticType::never());
    // break 操作数也可能有具体类型但发散；循环类型仍按 break 值定型。
    const bool diverges = std::all_of(
        frame.break_values.begin(), frame.break_values.end(),
        [](const auto &value) { return value.diverges; });
    // 有外层固定目标时，break 值已在 check_break 中逐一核对。
    if (frame.expected)
        return ExpressionResult::value(*frame.expected, diverges);
    // 没有外层期望：用所有 break 值的公共类型给 loop 定型。
    const auto common = compute_least_upper_bound(frame.break_values);
    if (common.is_error()) {
        diagnostics_.error(
            expression.span, "loop break values do not have a common type");
        return ExpressionResult::error();
    }
    return ExpressionResult::value(common, diverges);
}

// 检查 break。先找最近可跳转的循环：不在循环里或被 while 条件屏障挡住
// 都会报错，但 break 的值仍要遍历以保留独立诊断。while 只允许裸 break；
// loop 的 break 值收进帧里，留给 check_loop 汇总。break 本身类型为 !。
ExpressionResult BodyChecker::check_break(const BreakExpression &expression) {
    const auto target = find_jump_target();
    if (target.status != JumpTargetStatus::Found) {
        // 非法目标：报错后仍检查操作数，避免漏掉名字/类型诊断。
        diagnostics_.error(expression.span,
            target.status == JumpTargetStatus::BlockedByBarrier
                ? "break cannot target a loop from a while condition"
                : "break outside a loop");
        if (expression.value)
            check_expression(*expression.value, nullptr);
        return ExpressionResult::value(SemanticType::never());
    }
    if (loop_frames_[target.index].kind == LoopFrame::Kind::While) {
        // while 循环不产生值，只接受裸 break；带值时操作数仅做错误恢复。
        if (expression.value) {
            diagnostics_.error(expression.span, "while loops only allow bare break");
            check_expression(*expression.value, nullptr);
        }
        return ExpressionResult::value(SemanticType::never());
    }
    // 子表达式可能压入循环导致 vector 扩容，不能持有帧内的引用。
    const auto expected = loop_frames_[target.index].expected;
    auto result = expression.value
                      ? check_expression(*expression.value,
                                         expected ? &*expected : nullptr)
                      : ExpressionResult::value(SemanticType::unit());
    const Span at = expression.value ? expression.value->span : expression.span;
    // 有固定目标时当场核对 break 值的兼容性，失败记为 Error。
    if (expected && !require_coerce(result, *expected, at))
        result = ExpressionResult::error();
    // 记录本次 break 的值，等 loop 退出时统一定型。
    loop_frames_[target.index].break_values.push_back(result);
    return ExpressionResult::value(SemanticType::never());
}

// 检查 continue。它不携带值，只需确认存在可跳转的循环；
// 被 while 条件屏障挡住与在循环外一样都算非法目标。结果类型为 !。
ExpressionResult BodyChecker::check_continue(const ContinueExpression &expression) {
    const auto target = find_jump_target();
    if (target.status != JumpTargetStatus::Found)
        diagnostics_.error(expression.span,
            target.status == JumpTargetStatus::BlockedByBarrier
                ? "continue cannot target a loop from a while condition"
                : "continue outside a loop");
    // 即使目标非法也返回 !，避免错误恢复时再产生级联类型错误。
    return ExpressionResult::value(SemanticType::never());
}

} // namespace rx
