// 分支与 loop 的固定目标通过 require_coerce 检查，无目标时独立定型后求 LUB。
// 所有分支及不可达 break 仍参与检查；while 条件屏障限制跳转目标。
#include "sema/body/checker.hpp"
#include <algorithm>
#include <utility>

namespace rx {

BodyChecker::JumpTargetLookup BodyChecker::find_jump_target() const {
    if (loop_frames_.empty())
        return {JumpTargetStatus::NotFound, 0};
    const std::size_t index = loop_frames_.size() - 1;
    // 帧只有 loop、while 和条件屏障，栈顶就决定最近的目标或屏障。
    return {loop_frames_[index].kind == LoopFrame::Kind::WhileConditionBarrier
                ? JumpTargetStatus::BlockedByBarrier : JumpTargetStatus::Found,
            index};
}

bool BodyChecker::check_bool_condition(const Expression &condition, Span at) {
    const auto boolean = SemanticType::bool_();
    const auto result = check_expression(condition, &boolean);
    return require_coerce(result, boolean, at);
}

ExpressionResult BodyChecker::check_if(const IfExpression &expression,
                                       const SemanticType *expected) {
    if (!expression.condition || !expression.then)
        return ExpressionResult::error();
    const bool condition_ok =
        check_bool_condition(*expression.condition, expression.condition->span);
    if (!expression.else_) {
        const auto unit = SemanticType::unit();
        const auto then_result = check_expression(*expression.then, &unit);
        const bool then_ok = require_coerce(then_result, unit, expression.then->span);
        if (!condition_ok || !then_ok)
            return ExpressionResult::error();
        return ExpressionResult::value(unit);
    }
    const auto then_result = check_expression(*expression.then, expected);
    const auto else_result = check_expression(*expression.else_, expected);
    if (expected) {
        // 两支分别检查，不能用短路逻辑跳过后一支的诊断。
        const bool then_ok =
            require_coerce(then_result, *expected, expression.then->span);
        const bool else_ok =
            require_coerce(else_result, *expected, expression.else_->span);
        if (!condition_ok || !then_ok || !else_ok)
            return ExpressionResult::error();
        if (then_result.type.is_never() && else_result.type.is_never())
            return ExpressionResult::value(SemanticType::never());
        return ExpressionResult::value(*expected);
    }
    if (!condition_ok || then_result.type.is_error() || else_result.type.is_error())
        return ExpressionResult::error();
    const auto common = compute_least_upper_bound({then_result, else_result});
    if (common.is_error()) {
        diagnostics_.error(expression.span, "if branches do not have a common type");
        return ExpressionResult::error();
    }
    return ExpressionResult::value(common);
}

ExpressionResult BodyChecker::check_while(const WhileExpression &expression) {
    if (!expression.condition || !expression.body)
        return ExpressionResult::error();
    loop_frames_.push_back({LoopFrame::Kind::WhileConditionBarrier, std::nullopt, {}});
    const bool condition_ok =
        check_bool_condition(*expression.condition, expression.condition->span);
    loop_frames_.pop_back();

    const auto unit = SemanticType::unit();
    loop_frames_.push_back({LoopFrame::Kind::While, std::nullopt, {}});
    const auto body = check_expression(*expression.body, &unit);
    loop_frames_.pop_back();
    const bool body_ok = require_coerce(body, unit, expression.body->span);
    if (!condition_ok || !body_ok)
        return ExpressionResult::error();
    // 即使条件恒为 true，while 的静态类型也固定为 ()。
    return ExpressionResult::value(unit);
}

ExpressionResult BodyChecker::check_loop(const LoopExpression &expression,
                                         const SemanticType *expected) {
    if (!expression.body)
        return ExpressionResult::error();
    const auto target = expected ? std::optional(*expected) : std::nullopt;
    loop_frames_.push_back({LoopFrame::Kind::Loop, target, {}});
    const auto unit = SemanticType::unit();
    const auto body = check_expression(*expression.body, &unit);
    LoopFrame frame = std::move(loop_frames_.back());
    loop_frames_.pop_back();
    if (!require_coerce(body, unit, expression.body->span))
        return ExpressionResult::error();
    if (std::any_of(frame.break_values.begin(), frame.break_values.end(),
                    [](const auto &value) { return value.type.is_error(); }))
        return ExpressionResult::error();
    // 没有 break 或全部 break 的操作数为 ! 时，loop 仍为 !。
    if (std::all_of(frame.break_values.begin(), frame.break_values.end(),
                    [](const auto &value) { return value.type.is_never(); }))
        return ExpressionResult::value(SemanticType::never());
    if (frame.expected)
        return ExpressionResult::value(*frame.expected);
    const auto common = compute_least_upper_bound(frame.break_values);
    if (common.is_error()) {
        diagnostics_.error(
            expression.span, "loop break values do not have a common type");
        return ExpressionResult::error();
    }
    return ExpressionResult::value(common);
}

ExpressionResult BodyChecker::check_break(const BreakExpression &expression) {
    const auto target = find_jump_target();
    if (target.status != JumpTargetStatus::Found) {
        diagnostics_.error(expression.span,
            target.status == JumpTargetStatus::BlockedByBarrier
                ? "break cannot target a loop from a while condition"
                : "break outside a loop");
        if (expression.value)
            check_expression(*expression.value, nullptr);
        return ExpressionResult::value(SemanticType::never());
    }
    if (loop_frames_[target.index].kind == LoopFrame::Kind::While) {
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
    if (expected && !require_coerce(result, *expected, at))
        result = ExpressionResult::error();
    loop_frames_[target.index].break_values.push_back(result);
    return ExpressionResult::value(SemanticType::never());
}

ExpressionResult BodyChecker::check_continue(const ContinueExpression &expression) {
    const auto target = find_jump_target();
    if (target.status != JumpTargetStatus::Found)
        diagnostics_.error(expression.span,
            target.status == JumpTargetStatus::BlockedByBarrier
                ? "continue cannot target a loop from a while condition"
                : "continue outside a loop");
    return ExpressionResult::value(SemanticType::never());
}

} // namespace rx
