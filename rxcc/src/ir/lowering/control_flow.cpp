// 生成 if、短路运算、循环和循环跳转的控制流。
#include "ir/lowering.hpp"
#include "ast/ast.hpp"
#include "ir/builder.hpp"
#include <optional>
#include <stdexcept>
#include <variant>

namespace rx {

// 条件只求值一次，只有正常完成的分支才能进入汇合块。
// 调用方需要数据时，正常分支先按 sema 的计划转换，再写入共同的结果 slot。
std::optional<EmitResult>
GenerationContext::emit_if(const IfExpression &expression, LocalPlaces &locals,
                           ResultUse use) {
    const auto &record = semantics.expressions.at(&expression);
    const auto &result_type = record.type;
    const bool needs_value = use == ResultUse::Value &&
                             !record.diverges && !result_type.is_unit();
    if (needs_value && !is_storable_value(result_type)) {
        diagnostics << "IR 生成尚未支持此 if 结果类型（P9）\n";
        return std::nullopt;
    }
    // 条件在当前块求值一次；条件自身提前 return 时，整个 if 不再发射。
    auto condition_result = emit_value(*expression.condition, locals);
    if (!condition_result)
        return std::nullopt;
    if (path_ended(*condition_result))
        return EmitResult{NoContinuation{}};
    // 语义检查保证 if 条件是 bool，这里直接取它对应的 i1 计算值。
    const auto condition = std::get<ir::Operand>(*condition_result);

    // 条件可能包含嵌套控制流，插入点在求值后以 builder 记录的当前块为准。
    const auto insertion = builder.insert_point();
    auto &function = module.function(insertion->function);

    std::optional<ir::Operand> result_slot;
    if (needs_value)
        result_slot = allocate_storage_slot(
            ir::storage_type_of(result_type));

    // 无 else 时，false 边直接进入 merge；否则等第一条正常路径再创建 merge。
    const auto then_block = function.add_block();
    const auto false_block = function.add_block();
    std::optional<ir::BlockId> merge_block;
    if (!expression.else_)
        merge_block = false_block;
    builder.cond_branch(condition, then_block, false_block);

    const auto emit_branch = [&](const Expression &branch, ir::BlockId block,
                                  std::size_t index) {
        builder.set_insert_point(block);
        auto result = result_slot
            ? emit_prepared_value(branch, locals, {&expression, UseKind::IfBranch, index})
            : emit_discarded_expression(branch, locals);
        if (!result)
            return false;
        if (path_ended(*result))
            return true;
        if (result_slot)
            store_prepared_value(*result_slot, *result, result_type);
        if (!merge_block)
            merge_block = function.add_block();
        builder.branch(*merge_block);
        return true;
    };
    if (!emit_branch(*expression.then, then_block, 0) ||
        (expression.else_ && !emit_branch(*expression.else_, false_block, 1)))
        return std::nullopt;

    // 两支都提前结束时没有结果路径，外层不能继续读取 slot。
    if (!merge_block)
        return EmitResult{NoContinuation{}};
    builder.set_insert_point(*merge_block);
    if (result_slot)
        return read_stored_value(*result_slot, result_type);
    return EmitResult{UnitResult{}};
}

// 生成短路逻辑：先求左侧，再由左侧决定是否进入右侧。
// && 的 false 边、|| 的 true 边直接写短路结果；右侧只在需要时求值。
std::optional<EmitResult>
GenerationContext::emit_logical(const BinaryExpression &expression,
                                LocalPlaces &locals, ResultUse use) {
    const bool is_and =
        expression.operation == BinaryOperator::LogicalAnd;

    // 左侧总是先求值；左侧提前 return 时，右侧和外层都不能继续发射。
    auto left_result = emit_value(*expression.left, locals);
    if (!left_result)
        return std::nullopt;
    if (path_ended(*left_result))
        return EmitResult{NoContinuation{}};
    const auto left_value = std::get<ir::Operand>(*left_result);

    // 条件可能包含嵌套控制流，插入点在求值后以 builder 记录的当前块为准。
    const auto insertion = builder.insert_point();
    auto &function = module.function(insertion->function);

    // 丢弃结果时短路边可以直接到出口，不需要专门的写槽块。
    std::optional<ir::Operand> result_slot;
    if (use == ResultUse::Value)
        result_slot = allocate_slot(ir::Type::I1);
    const auto short_block = function.add_block();
    const auto rhs_block = function.add_block();
    const auto merge_block = result_slot ? function.add_block() : short_block;
    // && 在左侧为 true 时才走右侧；|| 在左侧为 false 时才走右侧。
    if (is_and)
        builder.cond_branch(left_value, rhs_block, short_block);
    else
        builder.cond_branch(left_value, short_block, rhs_block);

    // 短路分支直接给出结果：&& 给出 false，|| 给出 true。
    // 结果 slot 是 i8，已知的 0/1 可以直接按 i8 常量写入，无需先 zext。
    if (result_slot) {
        builder.set_insert_point(short_block);
        store_value(ir::Operand::constant(ir::Type::I8, is_and ? 0u : 1u), *result_slot);
        builder.branch(merge_block);
    }

    // 右侧路径只在运行时需要时执行；右侧提前结束时没有正常结果边。
    builder.set_insert_point(rhs_block);
    auto right_result = emit_expression(*expression.right, locals, use);
    if (!right_result)
        return std::nullopt;
    if (!path_ended(*right_result)) {
        if (result_slot)
            store_value(std::get<ir::Operand>(*right_result), *result_slot);
        builder.branch(merge_block);
    }

    builder.set_insert_point(merge_block);
    if (result_slot)
        return EmitResult{load_value(*result_slot, ir::Type::I1)};
    return EmitResult{UnitResult{}};
}

// 生成 while：当前块先跳到 cond，cond 每轮重新求值并选择 body 或 exit；
// body 正常完成时跳回 cond，提前 return 的分支不再加回边。
std::optional<EmitResult>
GenerationContext::emit_while(const WhileExpression &expression,
                              LocalPlaces &locals) {
    const auto insertion = builder.insert_point();
    auto &function = module.function(insertion->function);

    // 条件放在独立块中，回边跳回同一个块，因此每轮都会重新执行条件。
    const auto condition_block = function.add_block();
    builder.branch(condition_block);

    builder.set_insert_point(condition_block);
    auto condition_result = emit_value(*expression.condition, locals);
    if (!condition_result)
        return std::nullopt;
    // 条件自身提前 return 时，循环没有正常进入 body 或 exit 的路径。
    if (path_ended(*condition_result))
        return EmitResult{NoContinuation{}};
    const auto condition = std::get<ir::Operand>(*condition_result);

    // 条件正常完成后才需要 body 和 exit；body 回跳 cond，条件 false 进入 exit。
    const auto body_block = function.add_block();
    const auto exit_block = function.add_block();
    builder.cond_branch(condition, body_block, exit_block);

    // while 的 continue 回到条件块，break 跳到 exit；跳转目标由 sema 的 LoopId 固定。
    loop_frames_.push_back(
        {semantics.while_expressions.at(&expression), condition_block,
         exit_block, std::nullopt});
    // 循环体按 unit 生成；正常结束后回到条件块重新判断。
    builder.set_insert_point(body_block);
    auto body_result = emit_discarded_expression(*expression.body, locals);
    loop_frames_.pop_back();
    if (!body_result)
        return std::nullopt;
    if (!path_ended(*body_result))
        builder.branch(condition_block);

    // 条件 false 的正常出口继续执行 while 之后的语句。
    builder.set_insert_point(exit_block);
    return EmitResult{UnitResult{}};
}

// 生成 loop：body 正常完成时跳回自己继续下一轮。
// 有数据结果时在入口分配 result slot，break 写入后跳 exit，exit 再读回结果。
// exit 块只在首次遇到正常 break 时创建；完全没有正常 break 时循环不会退出。
std::optional<EmitResult>
GenerationContext::emit_loop(const LoopExpression &expression,
                             LocalPlaces &locals, ResultUse use) {
    const auto &record = semantics.expressions.at(&expression);
    const auto &result_type = record.type;
    // 只有调用方要消费 break 的值时才分配结果槽。
    std::optional<Place> result;
    if (use == ResultUse::Value && !record.diverges && !result_type.is_unit()) {
        if (!is_storable_value(result_type)) {
            diagnostics << "IR 生成尚未支持此 loop 结果类型（P9）\n";
            return std::nullopt;
        }
        result = Place{allocate_storage_slot(ir::storage_type_of(result_type)), &result_type};
    }

    const auto insertion = builder.insert_point();
    auto &function = module.function(insertion->function);

    // 当前块先进入循环体；回边直接跳回同一个块，不复制循环体代码。
    const auto body_block = function.add_block();
    builder.branch(body_block);
    builder.set_insert_point(body_block);
    // loop 的 continue 回到 body；break 的 exit 由第一次正常 break 懒创建。
    loop_frames_.push_back(
        {semantics.loop_expressions.at(&expression), body_block, std::nullopt,
         result});
    auto body_result = emit_discarded_expression(*expression.body, locals);
    // break 可能已在生成循环体时创建出口。
    const auto exit = loop_frames_.back().break_target;
    loop_frames_.pop_back();
    if (!body_result)
        return std::nullopt;
    // 正常走到循环体末尾时继续下一轮；return/break/continue 已经结束本路径。
    if (!path_ended(*body_result))
        builder.branch(body_block);
    // 没有正常 break 的 loop 没有出口，外层后续代码不可达。
    if (!exit)
        return EmitResult{NoContinuation{}};
    builder.set_insert_point(*exit);
    // 退出边都已经写入结果后，exit 才读取共同 slot 得到 loop 的值。
    if (result)
        return read_stored_value(result->address, *result->type);
    return EmitResult{UnitResult{}};
}

// 无标签的跳转只可能命中栈顶；用 sema 的目标验证 IR 循环帧同步。
GenerationContext::LoopFrame &GenerationContext::active_loop(LoopId target) {
    auto &frame = loop_frames_.back();
    if (frame.id != target)
        throw std::logic_error("循环帧与语义跳转目标不一致");
    return frame;
}

// 生成 break：先按 sema 记录求值并转换结果，写 loop result slot 后跳到退出块。
std::optional<EmitResult>
GenerationContext::emit_break(const BreakExpression &expression,
                              LocalPlaces &locals) {
    const auto loop_id = semantics.break_targets.at(&expression);
    // 不持有帧引用跨越子表达式：嵌套循环可能让帧栈扩容。
    const auto result = active_loop(loop_id).result;
    if (expression.value) {
        auto prepared = result
            ? emit_prepared_value(*expression.value, locals, {&expression, UseKind::BreakValue, 0})
            : emit_discarded_expression(*expression.value, locals);
        if (!prepared || path_ended(*prepared))
            return prepared;
        if (result)
            store_prepared_value(result->address, *prepared, *result->type);
    }

    // 子表达式的嵌套帧已弹出，首次正常 break 才创建出口。
    auto &frame = loop_frames_.back();
    if (!frame.break_target)
        frame.break_target = module.function(builder.insert_point()->function).add_block();
    builder.branch(*frame.break_target);
    return EmitResult{NoContinuation{}};
}

// 生成 continue：while 回条件块，loop 回循环体块。
std::optional<EmitResult>
GenerationContext::emit_continue(const ContinueExpression &expression,
                                 LocalPlaces &) {
    builder.branch(active_loop(semantics.continue_targets.at(&expression)).continue_target);
    return EmitResult{NoContinuation{}};
}

} // namespace rx
