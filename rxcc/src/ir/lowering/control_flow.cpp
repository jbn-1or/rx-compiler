// 实现 if 表达式的分块和跳转：条件在入口块求值一次，then/else 各自结束，
// 正常完成的分支跳到同一个 merge 块。unit/never if 不分配结果 slot。
#include "ir/lowering.hpp"
#include "ast/ast.hpp"
#include "ir/builder.hpp"
#include <optional>
#include <variant>

namespace rx {

// 生成 unit if：条件只求值一次，选中的分支执行完后进入 merge；
// 没有 else 时 false 边直接进入 merge。提前结束的分支不再添加跳转。
std::optional<EmitResult>
GenerationContext::emit_if_unit(const IfExpression &expression,
                                LocalPlaces &locals) {
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

    // then 块总是需要；有 else 时先建 else 块，没有 else 时先建 false 边直达的 merge。
    const auto then_block = function.add_block();
    std::optional<ir::BlockId> else_block;
    std::optional<ir::BlockId> merge_block;
    if (expression.else_)
        else_block = function.add_block();
    else
        merge_block = function.add_block();
    builder.cond_branch(condition, then_block,
                        else_block ? *else_block : *merge_block);

    // 生成 then 分支；只有正常完成的分支才需要跳到 merge。
    builder.set_insert_point(then_block);
    auto then_result = emit_unit_expression(*expression.then, locals);
    if (!then_result)
        return std::nullopt;
    if (!path_ended(*then_result)) {
        // 两个分支都正常完成时才第一次需要 merge；延迟创建避免空前驱块。
        if (!merge_block)
            merge_block = function.add_block();
        builder.branch(*merge_block);
    }

    // 有 else 时生成另一条路径；没有 else 的 false 边已经在 condbr 中直达 merge。
    if (else_block) {
        builder.set_insert_point(*else_block);
        auto else_result = emit_unit_expression(*expression.else_, locals);
        if (!else_result)
            return std::nullopt;
        if (!path_ended(*else_result)) {
            if (!merge_block)
                merge_block = function.add_block();
            builder.branch(*merge_block);
        }
    }

    // 两支都提前结束时没有正常后继，外层不应再补正常返回或后续指令。
    if (!merge_block)
        return EmitResult{NoContinuation{}};
    builder.set_insert_point(*merge_block);
    return EmitResult{UnitResult{}};
}

// 生成有标量结果的 if：两支在正常结束时先写入口 result slot，再跳 merge；
// merge 读取 slot 得到 if 的值。这样不需要 phi，也不让分支 SSA 值跨块逃逸。
std::optional<EmitResult>
GenerationContext::emit_if_value(const IfExpression &expression,
                                 LocalPlaces &locals) {
    const auto &result_type =
        semantics.expressions.at(&expression).type;
    // 当前支持标量和标量引用；聚合结果要等 P7 的布局实现。
    if (!is_supported_value(result_type)) {
        diagnostics << "IR 生成尚未支持此 if 结果类型（聚合 P7）\n";
        return std::nullopt;
    }

    // 条件在当前块求值一次；条件自身提前 return 时，整个 if 不再发射。
    auto condition_result = emit_value(*expression.condition, locals);
    if (!condition_result)
        return std::nullopt;
    if (path_ended(*condition_result))
        return EmitResult{NoContinuation{}};
    const auto condition = std::get<ir::Operand>(*condition_result);

    // 条件可能包含嵌套控制流，插入点在求值后以 builder 记录的当前块为准。
    const auto insertion = builder.insert_point();
    auto &function = module.function(insertion->function);

    // 结果 slot 固定在函数入口分配，两支都通过 store 写入同一个内存位置。
    // 整数保存为 i32，bool 保存为 i8；merge 再按源类型读回 i1 或 i32。
    const auto result_slot = builder.entry_alloca(slot_type(result_type),
                                                  slot_alignment(result_type));

    const auto then_block = function.add_block();
    const auto else_block = function.add_block();
    builder.cond_branch(condition, then_block, else_block);

    std::optional<ir::BlockId> merge_block;
    // then 正常完成时先写结果再跳 merge；提前 return 的分支没有正常结果边。
    builder.set_insert_point(then_block);
    auto then_result = emit_value(*expression.then, locals);
    if (!then_result)
        return std::nullopt;
    if (!path_ended(*then_result)) {
        // 每个正常结果边按 sema 记录的最终 LUB 路径转换后再写共同 slot。
        auto prepared = emit_coercion_at(
            UseSite{&expression, UseKind::IfBranch, 0},
            std::get<ir::Operand>(*then_result));
        if (!prepared)
            return std::nullopt;
        merge_block = function.add_block();
        store_value(*prepared, result_slot);
        builder.branch(*merge_block);
    }

    // else 与 then 对称：只有正常完成的分支才写 slot 并跳 merge。
    builder.set_insert_point(else_block);
    auto else_result = emit_value(*expression.else_, locals);
    if (!else_result)
        return std::nullopt;
    if (!path_ended(*else_result)) {
        auto prepared = emit_coercion_at(
            UseSite{&expression, UseKind::IfBranch, 1},
            std::get<ir::Operand>(*else_result));
        if (!prepared)
            return std::nullopt;
        if (!merge_block)
            merge_block = function.add_block();
        store_value(*prepared, result_slot);
        builder.branch(*merge_block);
    }

    // 两支都提前结束时没有结果路径，外层不能继续读取 slot。
    if (!merge_block)
        return EmitResult{NoContinuation{}};
    builder.set_insert_point(*merge_block);
    return EmitResult{load_value(result_slot, result_type)};
}

// 生成短路逻辑：先求左侧，再由左侧决定是否进入右侧。
// && 的 false 边、|| 的 true 边直接写短路结果；右侧只在需要时求值。
std::optional<EmitResult>
GenerationContext::emit_logical(const BinaryExpression &expression,
                                LocalPlaces &locals) {
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

    // 短路结果使用 bool 的 i8 存储形式，merge 读取后再 trunc 回 i1。
    const auto result_slot = builder.entry_alloca(ir::Type::I8, 1);
    const auto short_block = function.add_block();
    const auto rhs_block = function.add_block();
    const auto merge_block = function.add_block();
    // && 在左侧为 true 时才走右侧；|| 在左侧为 false 时才走右侧。
    if (is_and)
        builder.cond_branch(left_value, rhs_block, short_block);
    else
        builder.cond_branch(left_value, short_block, rhs_block);

    // 短路分支直接给出结果：&& 给出 false，|| 给出 true。
    // 结果 slot 是 i8，已知的 0/1 可以直接按 i8 常量写入，无需先 zext。
    builder.set_insert_point(short_block);
    builder.store(ir::Type::I8,
                  ir::Operand::constant(ir::Type::I8, is_and ? 0u : 1u),
                  result_slot, 1);
    builder.branch(merge_block);

    // 右侧路径只在运行时需要时执行；右侧提前结束时没有正常结果边。
    builder.set_insert_point(rhs_block);
    auto right_result = emit_value(*expression.right, locals);
    if (!right_result)
        return std::nullopt;
    if (!path_ended(*right_result)) {
        store_value(std::get<ir::Operand>(*right_result), result_slot);
        builder.branch(merge_block);
    }

    builder.set_insert_point(merge_block);
    return EmitResult{load_value(result_slot, SemanticType::bool_())};
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
         exit_block, nullptr, std::nullopt});
    // 循环体按 unit 生成；正常结束后回到条件块重新判断。
    builder.set_insert_point(body_block);
    auto body_result = emit_unit_expression(*expression.body, locals);
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
                             LocalPlaces &locals) {
    const auto &result_type = semantics.expressions.at(&expression).type;
    // unit/never 没有数据结果；其他受支持的标量或引用才需要 slot。
    const bool has_result =
        !result_type.is_unit() && !result_type.is_never();
    if (has_result && !is_supported_value(result_type)) {
        diagnostics << "IR 生成尚未支持聚合 loop 结果（P7）\n";
        return std::nullopt;
    }
    std::optional<ir::Operand> result_slot;
    if (has_result)
        result_slot = builder.entry_alloca(slot_type(result_type),
                                           slot_alignment(result_type));

    const auto insertion = builder.insert_point();
    auto &function = module.function(insertion->function);

    // 当前块先进入循环体；回边直接跳回同一个块，不复制循环体代码。
    const auto body_block = function.add_block();
    builder.branch(body_block);
    builder.set_insert_point(body_block);
    // loop 的 continue 回到 body；break 的 exit 由第一次正常 break 懒创建。
    loop_frames_.push_back(
        {semantics.loop_expressions.at(&expression), body_block, std::nullopt,
         has_result ? &result_type : nullptr, result_slot});
    auto body_result = emit_unit_expression(*expression.body, locals);
    // 取走帧后再出栈；break 可能已经在生成期间创建了 exit 块。
    const LoopFrame frame = loop_frames_.back();
    loop_frames_.pop_back();
    if (!body_result)
        return std::nullopt;
    // 正常走到循环体末尾时继续下一轮；return/break/continue 已经结束本路径。
    if (!path_ended(*body_result))
        builder.branch(body_block);
    // 没有正常 break 的 loop 没有出口，外层后续代码不可达。
    if (!frame.break_target)
        return EmitResult{NoContinuation{}};
    builder.set_insert_point(*frame.break_target);
    // 退出边都已经写入结果后，exit 才读取共同 slot 得到 loop 的值。
    if (frame.result_type)
        return EmitResult{load_value(*frame.result_slot, *frame.result_type)};
    return EmitResult{UnitResult{}};
}

// 按 LoopId 查找当前正在生成的循环帧。
GenerationContext::LoopFrame *
GenerationContext::find_loop_frame(LoopId id) {
    for (auto frame = loop_frames_.rbegin(); frame != loop_frames_.rend();
         ++frame)
        if (frame->id.index == id.index)
            return &*frame;
    return nullptr;
}

// 生成 break：先按 sema 记录求值并转换结果，写 loop result slot 后跳到退出块。
std::optional<EmitResult>
GenerationContext::emit_break(const BreakExpression &expression,
                              LocalPlaces &locals) {
    const auto loop_id = semantics.break_targets.at(&expression);
    auto *frame = find_loop_frame(loop_id);
    // 跳转目标必须来自当前正在生成的循环；缺失表示 sema 记录与 lowering 不一致。
    if (!frame) {
        diagnostics << "IR 内部错误：break 的目标循环不在生成栈中\n";
        return std::nullopt;
    }
    // 值的求值可能压入嵌套循环并让帧表扩容，先复制是否需要结果的标记。
    const bool loop_has_result = frame->result_type != nullptr;

    // 先求值 break 操作数；操作数自己 return 时不写 slot、不添加 break 边。
    std::optional<ir::Operand> prepared;
    if (expression.value) {
        const auto &value_type =
            semantics.expressions.at(expression.value.get()).type;
        if (value_type.is_unit()) {
            // unit break 只执行操作数的副作用，没有数据需要写入。
            auto value_result = emit_unit_expression(*expression.value, locals);
            if (!value_result)
                return std::nullopt;
            if (path_ended(*value_result))
                return EmitResult{NoContinuation{}};
        } else {
            auto value_result = emit_value(*expression.value, locals);
            if (!value_result)
                return std::nullopt;
            if (path_ended(*value_result))
                return EmitResult{NoContinuation{}};
            // 有值 loop 的每条正常 break 边按 sema 记录的最终 LUB 路径转换。
            if (loop_has_result) {
                auto coerced = emit_coercion_at(
                    UseSite{&expression, UseKind::BreakValue, 0},
                    std::get<ir::Operand>(*value_result));
                if (!coerced)
                    return std::nullopt;
                prepared = *coerced;
            }
        }
    }
    // 有值 loop 的正常 break 必须有可写入的结果；否则 slot 会留下未初始化边。
    if (loop_has_result && !prepared) {
        diagnostics << "IR 内部错误：有值 loop 的 break 缺少可写结果\n";
        return std::nullopt;
    }

    // 重新查找帧：操作数求值期间 vector 可能因嵌套循环扩容。
    frame = find_loop_frame(loop_id);
    if (!frame) {
        diagnostics << "IR 内部错误：break 的目标循环不在生成栈中\n";
        return std::nullopt;
    }
    // loop 没有条件块，第一次正常 break 时才创建 lazily exit 块。
    if (!frame->break_target) {
        const auto insertion = builder.insert_point();
        auto &function = module.function(insertion->function);
        frame->break_target = function.add_block();
    }
    if (prepared)
        store_value(*prepared, *frame->result_slot);
    builder.branch(*frame->break_target);
    return EmitResult{NoContinuation{}};
}

// 生成 continue：while 回条件块，loop 回循环体块。
std::optional<EmitResult>
GenerationContext::emit_continue(const ContinueExpression &expression,
                                 LocalPlaces &) {
    const auto loop_id = semantics.continue_targets.at(&expression);
    auto *frame = find_loop_frame(loop_id);
    if (!frame) {
        diagnostics << "IR 内部错误：continue 的目标循环不在生成栈中\n";
        return std::nullopt;
    }
    builder.branch(frame->continue_target);
    return EmitResult{NoContinuation{}};
}

} // namespace rx