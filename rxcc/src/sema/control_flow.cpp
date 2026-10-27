// 实现函数体中的 if、loop、while、break、continue 检查。
// 控制流共享表达式结果约定：if/loop/while 产生值，break/continue 产生 !；
// 循环栈把跳转绑定到最近的可跳转循环，并收集 loop 的 break 值。
//
// 具体检查：
// - if：
// - while：
// - loop：
// - break/continue：
// - 不可达的 break 仍参与 loop 结果；所有分支/循环体照常检查（M11）。

#include "sema/body.hpp"
#include "sema/sema.hpp"
#include <optional>
#include <utility>
#include <vector>

namespace rx {

namespace {

// 检查失败时统一返回 Error + 空 place，让上层静默放行并避免重复报错。
ExpressionResult failed_expression() {
    return ExpressionResult{SemanticType::error(), PlaceAccessInformation{}};
}

// 构造一个不可作为 place 的值表达式结果，供 if/loop/while 使用。
ExpressionResult value_expression(SemanticType type) {
    return ExpressionResult{std::move(type), PlaceAccessInformation{}};
}

// 构造 Never 结果：break/continue 及所有 break 均为 ! 的 loop 使用。
ExpressionResult never_expression() {
    return ExpressionResult{SemanticType::never(), PlaceAccessInformation{}};
}

} // namespace

// 从循环栈顶往下找 break/continue 的直接目标。
// 返回值：Found 时带目标帧下标；遇到 while 条件屏障立即 Blocked；
// 栈中没有循环则为 NotFound。
BodyChecker::JumpTargetLookup BodyChecker::find_jump_target() const {
    // 完全没有进入循环：break/continue 都是循环外跳转。
    if (loop_frames_.empty())
        return {JumpTargetStatus::NotFound, 0};

    // 从最内层向外查找，最近的可跳转循环优先。
    for (std::size_t offset = 0; offset < loop_frames_.size(); ++offset) {
        const std::size_t index = loop_frames_.size() - 1 - offset;
        const LoopFrame &frame = loop_frames_[index];
        // while 条件内的跳转不能越出该条件去找当前 while 或外层循环。
        // 这里必须立即返回 Blocked，不能跳过屏障继续扫描。
        if (frame.kind == LoopFrame::Kind::WhileConditionBarrier)
            return {JumpTargetStatus::BlockedByBarrier, index};
        // 最近的 while/loop 才是 break/continue 的直接目标。
        if (frame.kind == LoopFrame::Kind::While ||
            frame.kind == LoopFrame::Kind::Loop)
            return {JumpTargetStatus::Found, index};
    }
    return {JumpTargetStatus::NotFound, 0};
}

// 检查 if/while 的条件：期望类型固定为 bool，允许 ! 通过。
// 返回 false 表示条件已经报错；调用方仍需继续检查循环体/分支以覆盖不可达代码。
bool BodyChecker::check_bool_condition(const Expression &condition, Span at) {
    const SemanticType bool_type = SemanticType::bool_();
    // 条件处于固定目标位置，把 bool 作为期望传给条件表达式。
    const ExpressionResult result = check_expression(condition, &bool_type);
    // 条件本身已经出错，不再重复报“必须是 bool”。
    if (result.type.is_error())
        return false;
    // 只允许 bool 或 !；整数、引用等在这里统一报一次转换错误。
    if (!can_coerce(result.type, result.place, bool_type)) {
        require_coerce(result, bool_type, at);
        return false;
    }
    return true;
}

// 检查 if 表达式。
// 有 else 且外层有固定目标时，两支都按目标检查并转换；
// 无固定目标时两支独立定型后求 LUB；无 else 时 then 必须以 () 为目标且整个 if 为 ()。
// 所有分支都会检查，不按条件常量删支。
ExpressionResult BodyChecker::check_if(const IfExpression &expression,
                                       const SemanticType *expected) {
    // AST 缺少条件或 then 块时无法继续。
    if (!expression.condition || !expression.then)
        return failed_expression();

    // 条件必须是 bool 或 !；条件出错时仍继续检查各分支。
    const bool condition_ok =
        check_bool_condition(*expression.condition, expression.condition->span);

    // 没有 else：整个 if 类型固定为 ()，then 也必须以 () 为目标。
    if (!expression.else_) {
        const SemanticType unit = SemanticType::unit();
        // 无 else：then 的期望类型固定为 ()，而不是 if 外层收到的目标。
        const ExpressionResult then_result =
            check_expression(*expression.then, &unit);
        bool then_ok = true;
        // then 自身出错时不再补一次 () 转换错误。
        if (then_result.type.is_error()) {
            then_ok = false;
        } else if (!can_coerce(then_result.type, then_result.place, unit)) {
            // then 产生了 () / ! 之外的值，例如尾部整数。
            require_coerce(then_result, unit, expression.then->span);
            then_ok = false;
        }
        // 无 else 的 if 类型恒为 ()，即使 then 是 !；条件或 then 出错才返回 Error。
        if (!condition_ok || !then_ok)
            return failed_expression();
        return value_expression(unit);
    }

    // 外层目标已出错：两支仍需检查以覆盖不可达代码，但整个 if 返回 Error。
    if (expected && expected->is_error()) {
        const ExpressionResult then_result =
            check_expression(*expression.then, expected);
        const ExpressionResult else_result =
            check_expression(*expression.else_, expected);
        (void)then_result;
        (void)else_result;
        return failed_expression();
    }

    const ExpressionResult then_result =
        check_expression(*expression.then, expected);
    const ExpressionResult else_result =
        check_expression(*expression.else_, expected);

    // 有固定目标：两支都按该目标检查并转换，而不是求 LUB。
    if (expected) {
        bool branches_ok = true;
        // then 分支自身出错时保留原诊断，不再重复报类型不匹配。
        if (then_result.type.is_error()) {
            branches_ok = false;
        } else if (!can_coerce(then_result.type, then_result.place,
                               *expected)) {
            // 有固定目标时 then 必须能一对一转换到目标类型。
            require_coerce(then_result, *expected, expression.then->span);
            branches_ok = false;
        }
        // else 分支同样按固定目标逐一检查，不能只看一支。
        if (else_result.type.is_error()) {
            branches_ok = false;
        } else if (!can_coerce(else_result.type, else_result.place,
                               *expected)) {
            require_coerce(else_result, *expected, expression.else_->span);
            branches_ok = false;
        }
        if (!condition_ok || !branches_ok)
            return failed_expression();
        // 有固定目标但两支都发散时仍保留 !，不要强行改成目标类型。
        if (then_result.type.is_never() && else_result.type.is_never())
            return never_expression();
        // 否则固定目标决定 if 的结果类型。
        return value_expression(*expected);
    }

    // 无固定目标：两支都必须成功独立定型，否则无法求共同类型。
    if (!condition_ok || then_result.type.is_error() ||
        else_result.type.is_error()) {
        return failed_expression();
    }

    // 无固定目标时用 LUB 合并两支；! 会被 LUB 忽略，全为 ! 时结果为 !。
    const SemanticType common =
        compute_least_upper_bound({then_result, else_result});
    // 两支无法合并成同一类型，属于 if 结果的 LUB 失败。
    if (common.is_error()) {
        diagnostics_.error(expression.span,
                           "if branches do not have a common type");
        return failed_expression();
    }
    return value_expression(common);
}

// 检查 while 表达式。
// 条件先压入 WhileConditionBarrier，阻止其中裸 break/continue 跳出当前 while；
// 循环体必须以 () 为目标，while 自身类型恒为 ()（即使条件是 true）。
ExpressionResult BodyChecker::check_while(const WhileExpression &expression) {
    // AST 缺少条件或循环体时无法继续。
    if (!expression.condition || !expression.body)
        return failed_expression();

    // 检查条件期间屏障位于栈顶：条件里的跳转不能指向当前 while 或更外层循环。
    loop_frames_.push_back(
        {LoopFrame::Kind::WhileConditionBarrier, std::nullopt, {}});
    const bool condition_ok =
        check_bool_condition(*expression.condition, expression.condition->span);
    // 条件检查结束就撤掉屏障，之后循环体才能正常 break/continue 到当前 while。
    loop_frames_.pop_back();

    const SemanticType unit = SemanticType::unit();
    // 循环体阶段使用 While 帧；它不收集 break 值，while 结果不来自 break。
    loop_frames_.push_back({LoopFrame::Kind::While, std::nullopt, {}});
    const ExpressionResult body_result =
        check_expression(*expression.body, &unit);
    loop_frames_.pop_back();

    bool body_ok = true;
    // 循环体自身出错时不再补一次 () 转换错误。
    if (body_result.type.is_error()) {
        body_ok = false;
    } else if (!can_coerce(body_result.type, body_result.place, unit)) {
        // 循环体尾部的非 () 值必须报错，循环体只能是 () 或 !。
        require_coerce(body_result, unit, expression.body->span);
        body_ok = false;
    }

    // 条件或循环体任一失败，while 整体按 Error 处理；诊断已保留。
    if (!condition_ok || !body_ok)
        return failed_expression();
    // while 表达式恒为 ()，即使 condition 是字面量 true。
    return value_expression(unit);
}

// 检查 loop 表达式。
// 循环体本身必须以 () 为目标；loop 的结果只由指向它的 break 操作数决定。
// 外层固定目标会保存在 Loop 帧中，并在每个 break 现场传给操作数。
ExpressionResult BodyChecker::check_loop(const LoopExpression &expression,
                                         const SemanticType *expected) {
    // AST 缺少循环体时无法收集 break 值。
    if (!expression.body)
        return failed_expression();

    // 复制外层期望到帧中：break 出现时要立刻按该目标检查操作数。
    std::optional<SemanticType> expected_value;
    if (expected)
        expected_value = *expected;

    // Loop 帧负责收集直接指向本循环的 break 值；嵌套循环的 break 不会进入这里。
    loop_frames_.push_back(
        {LoopFrame::Kind::Loop, expected_value, {}});
    const SemanticType unit = SemanticType::unit();
    // 循环体不是 loop 的结果目标，必须以 () 检查。
    const ExpressionResult body_result =
        check_expression(*expression.body, &unit);

    // 先移动出帧再弹出，避免引用在后续检查/扩容后失效。
    LoopFrame frame = std::move(loop_frames_.back());
    loop_frames_.pop_back();

    bool body_ok = true;
    // 循环体自身出错时不再追加 () 转换诊断。
    if (body_result.type.is_error()) {
        body_ok = false;
    } else if (!can_coerce(body_result.type, body_result.place, unit)) {
        // 循环体尾部不是 ()/!，例如 `loop { 1 }`，必须报错。
        require_coerce(body_result, unit, expression.body->span);
        body_ok = false;
    }
    // 循环体不是 ()/! 时 loop 整体失败；break 值仍然已收集，但不再决定结果。
    if (!body_ok)
        return failed_expression();

    // 没有任何 break 指向本循环时，loop 永不返回，类型为 !。
    if (frame.break_values.empty())
        return never_expression();

    // 统计 break 值中的错误与 !：Error 传播失败，全部 ! 时结果仍为 !。
    bool any_error = false;
    bool all_never = true;
    for (const ExpressionResult &value : frame.break_values) {
        if (value.type.is_error())
            any_error = true;
        if (!value.type.is_never())
            all_never = false;
    }
    // 任一 break 值已经出错时，loop 结果也回到 Error，避免继续推断结果类型。
    if (any_error)
        return failed_expression();
    // 所有 break 操作数都是 ! 时 loop 仍为 !，不能强行采用固定目标。
    if (all_never)
        return never_expression();

    // 有固定目标时，break 值在 break 现场已经转换完成，结果直接采用目标类型。
    if (frame.expected)
        return value_expression(*frame.expected);

    // 无固定目标时，对所有 break 操作数求 LUB。
    const SemanticType common = compute_least_upper_bound(frame.break_values);
    // break 值无法合并成同一类型，属于 loop 结果的 LUB 失败。
    if (common.is_error()) {
        diagnostics_.error(expression.span,
                           "loop break values do not have a common type");
        return failed_expression();
    }
    return value_expression(common);
}

// 检查 break 表达式。
// break 必须找到最近的可跳转循环；while 只允许裸 break，loop 允许带值 break。
// 带值 break 在出现位置就按目标 loop 的期望类型转换，并把结果登记到目标帧。
ExpressionResult BodyChecker::check_break(const BreakExpression &expression) {
    const JumpTargetLookup target = find_jump_target();
    // 位于 while 条件中：即使外层存在循环，也不能越过条件屏障去跳转。
    if (target.status == JumpTargetStatus::BlockedByBarrier) {
        diagnostics_.error(
            expression.span,
            "break cannot target a loop from a while condition");
        return never_expression();
    }
    // 完全不在循环中。
    if (target.status == JumpTargetStatus::NotFound) {
        diagnostics_.error(expression.span, "break outside a loop");
        return never_expression();
    }

    const LoopFrame::Kind kind = loop_frames_[target.index].kind;
    if (kind == LoopFrame::Kind::While) {
        // while 没有结果值，因此只允许 break;，带值 break 直接报错。
        if (expression.value) {
            diagnostics_.error(expression.span,
                               "while loops only allow bare break");
        }
        return never_expression();
    }

    // 复制目标帧的期望类型，避免检查 break 操作数期间循环栈扩容导致引用失效。
    const std::optional<SemanticType> expected =
        loop_frames_[target.index].expected;
    bool ok = true;
    ExpressionResult result;
    if (expression.value) {
        // 带值 break：操作数在 break 现场接收 loop 的目标类型。
        result = check_expression(*expression.value,
                                  expected ? &*expected : nullptr);
        // 操作数自身已经出错时不再重复报目标类型不匹配。
        if (result.type.is_error()) {
            ok = false;
        } else if (expected &&
                   !can_coerce(result.type, result.place, *expected)) {
            // 有固定目标时，break 值必须能一对一转换到目标类型。
            require_coerce(result, *expected, expression.value->span);
            ok = false;
        }
    } else {
        // 裸 break 等价于贡献 ()，同样要满足 loop 的固定目标。
        result = ExpressionResult{SemanticType::unit(),
                                  PlaceAccessInformation{}};
        if (expected &&
            !can_coerce(result.type, result.place, *expected)) {
            require_coerce(result, *expected, expression.span);
            ok = false;
        }
    }

    // 转换失败时用 Error 占位，让 loop 结果整体失败而不是保留错误操作数类型。
    if (!ok)
        result = failed_expression();
    // 只登记到直接目标帧；之后嵌套/外层循环各用自己的帧。
    loop_frames_[target.index].break_values.push_back(result);
    return never_expression();
}

// 检查 continue 表达式。
// continue 与 break 使用同一套目标查找和条件屏障规则，但没有值，类型恒为 !。
ExpressionResult
BodyChecker::check_continue(const ContinueExpression &expression) {
    const JumpTargetLookup target = find_jump_target();
    // while 条件中的 continue 同样不能越过屏障跳向当前或外层循环。
    if (target.status == JumpTargetStatus::BlockedByBarrier) {
        diagnostics_.error(
            expression.span,
            "continue cannot target a loop from a while condition");
        return never_expression();
    }
    // continue 不在任何循环中。
    if (target.status == JumpTargetStatus::NotFound) {
        diagnostics_.error(expression.span, "continue outside a loop");
        return never_expression();
    }
    // 找到 while/loop 即可；continue 不携带值，也不影响循环结果类型。
    return never_expression();
}

} // namespace rx
