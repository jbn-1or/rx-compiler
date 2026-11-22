// 编排一个函数体的生成，并按表达式或语句种类分派到 lowering/ 中的实现。
// 这里只保留块作用域、路径结束传播和函数入口；不实现具体运算、存取或调用。
#include "ir/lowering.hpp"
#include "ast/ast.hpp"
#include "ir/builder.hpp"

namespace rx {

// 判断一次生成结果是否已经结束当前路径。
bool path_ended(const EmitResult &result) {
    return std::holds_alternative<NoContinuation>(result);
}

// 判断一次 place 生成是否已经结束当前路径。
bool path_ended(const PlaceResult &result) {
    return std::holds_alternative<NoContinuation>(result);
}

// 保存这次 IR 生成使用的引用，不拥有任何对象。
GenerationContext::GenerationContext(
    const SemanticResult &semantics,
    const std::vector<ir::IRFunctionId> &functions, ir::Module &module,
    ir::IRBuilder &builder, std::ostream &diagnostics)
    : semantics(semantics), functions(functions), module(module),
      builder(builder), diagnostics(diagnostics) {}

// 生成需要数据值的表达式；路径已结束时返回 NoContinuation 而不是伪造值。
std::optional<EmitResult>
GenerationContext::emit_value(const Expression &expression,
                              LocalPlaces &locals) {
    // 二元运算先求值两侧，再按源类型选择指令。
    if (const auto *binary =
            dynamic_cast<const BinaryExpression *>(&expression))
        return emit_binary(*binary, locals);
    // 一元运算分两类：借用/解引用走 place，取负/取反读取操作数及其引用目标。
    if (const auto *unary = dynamic_cast<const UnaryExpression *>(&expression))
        return emit_unary(*unary, locals);
    // as 转换只改源类型或把 bool 零扩展，不引入数值转换。
    if (const auto *cast = dynamic_cast<const CastExpression *>(&expression))
        return emit_cast(*cast, locals);
    // 字面量和路径不产生嵌套求值，复用基础读取逻辑。
    // 引用路径只读出指针，不会顺便读取目标内容。
    if (dynamic_cast<const IntegerLiteral *>(&expression) ||
        dynamic_cast<const BooleanLiteral *>(&expression) ||
        dynamic_cast<const PathExpression *>(&expression)) {
        return EmitResult{emit_leaf(expression, locals)};
    }

    // return 不需要也不能产生操作数，只结束当前路径。
    if (const auto *return_expression =
            dynamic_cast<const ReturnExpression *>(&expression))
        return emit_return(*return_expression, locals);

    // 调用表达式由统一的目标解析和参数求值逻辑生成。
    if (const auto *call = dynamic_cast<const CallExpression *>(&expression))
        return emit_call(*call, locals);

    // 需要值的块可能是标量/引用块，也可能是整块路径都已结束的 never 块。
    if (const auto *block =
            dynamic_cast<const BlockExpression *>(&expression)) {
        const auto &type = semantics.expressions.at(block).type;
        // 聚合块值随 P7 的布局实现。
        if (!is_supported_value(type) && !type.is_never()) {
            diagnostics << "IR 生成尚未支持聚合块值（P7）\n";
            return std::nullopt;
        }
        LocalPlaces nested_locals = locals;
        for (const auto &statement : block->statements) {
            auto result = emit_statement(*statement, nested_locals);
            if (!result)
                return std::nullopt;
            // 块内提前 return 后，后续语句和尾值都不再执行。
            if (path_ended(*result))
                return EmitResult{NoContinuation{}};
        }
        // 正常到达块尾时，语义检查已保证这里有标量/引用或 never 尾表达式。
        return emit_value(*block->tail, nested_locals);
    }

    diagnostics << "IR 生成尚未支持此值表达式\n";
    return std::nullopt;
}

// 只生成能正常完成并产生 unit 的表达式；unit 不对应 IR 操作数。
std::optional<EmitResult>
GenerationContext::emit_unit_expression(const Expression &expression,
                                        LocalPlaces &locals) {
    // return 的静态类型是 never，但可出现在需要 unit 的尾表达式位置。
    if (const auto *return_expression =
            dynamic_cast<const ReturnExpression *>(&expression))
        return emit_return(*return_expression, locals);
    // 显式 unit 不需要生成 IR 指令。
    if (dynamic_cast<const UnitExpression *>(&expression))
        return EmitResult{UnitResult{}};
    // unit 调用只保留副作用。
    if (const auto *call = dynamic_cast<const CallExpression *>(&expression))
        return emit_call(*call, locals);
    // 赋值正常完成后产生 unit；先求 RHS，再求目标并写入。
    if (const auto *assignment =
            dynamic_cast<const AssignmentExpression *>(&expression))
        return emit_assignment(*assignment, locals);
    // 块内绑定使用独立映射，退出该块时不泄漏到外层。
    if (const auto *block =
            dynamic_cast<const BlockExpression *>(&expression)) {
        LocalPlaces nested_locals = locals;
        // 先按源码顺序生成块内语句，再处理可选的尾表达式。
        for (const auto &statement : block->statements) {
            auto result = emit_statement(*statement, nested_locals);
            if (!result)
                return std::nullopt;
            // 块内提前 return 后，后续语句和尾表达式都不再执行。
            if (path_ended(*result))
                return result;
        }
        // 正常到达空尾部表示 unit 完成。
        if (!block->tail)
            return EmitResult{UnitResult{}};
        return emit_unit_expression(*block->tail, nested_locals);
    }
    diagnostics << "IR 生成尚未支持此 unit 表达式\n";
    return std::nullopt;
}

// 生成 let 和表达式语句，并按表达式语义类型选择值、unit 或 never 路径。
std::optional<EmitResult>
GenerationContext::emit_statement(const Statement &statement,
                                  LocalPlaces &locals) {
    // let 语句负责生成初始化并建立局部变量映射。
    if (const auto *binding = dynamic_cast<const LetStatement *>(&statement))
        return emit_let(*binding, locals);
    const auto &expression = static_cast<const ExpressionStatement &>(statement);
    const auto &type = semantics.expressions.at(expression.expression.get()).type;
    // 标量或引用表达式语句丢弃值但保留调用等副作用。
    if (is_supported_value(type)) {
        auto result = emit_value(*expression.expression, locals);
        if (!result)
            return std::nullopt;
        // 路径已由 return 结束时，继续把 NoContinuation 传给外层。
        if (path_ended(*result))
            return result;
        return EmitResult{UnitResult{}};
    }
    // unit 表达式执行完后正常继续下一条语句。
    if (type.is_unit())
        return emit_unit_expression(*expression.expression, locals);
    // 当前 never 语句只支持 return；循环等发散表达式留到 P6。
    if (type.is_never())
        return emit_value(*expression.expression, locals);
    diagnostics << "IR 生成尚未支持此表达式语句类型\n";
    return std::nullopt;
}

// 生成一个完整函数体：入口 slot、语句、尾表达式和正常返回。
bool GenerationContext::emit_function_body(const FunctionSymbol &function,
                                           ir::IRFunctionId id) {
    builder.set_insert_point(module.function(id).add_block());
    // 临时 slot 只在当前函数内复用；切换函数时清空，避免不同函数共用位置。
    temporary_slots.clear();
    LocalPlaces locals;
    const auto &parameter_locals = semantics.function_locals.at(function.id);
    // 每个 IR 参数存入其 LocalId 对应的独立存储单元。
    for (std::size_t parameter_index = 0;
         parameter_index < function.parameters.size(); ++parameter_index) {
        const auto local_id = parameter_locals.parameters[parameter_index];
        const auto &local_type = semantics.locals[local_id.index].type;
        // bool 形参存入 i8 slot，整数存入 i32 slot，引用形参保存 ptr。
        const auto address = builder.entry_alloca(slot_type(local_type),
                                                  slot_alignment(local_type));
        const auto argument = module.function(id).argument(parameter_index);
        store_value(argument, address);
        locals.emplace(local_id, address);
    }
    // 函数体语句按源码顺序生成；提前 return 后停止整条路径。
    bool path_open = true;
    for (const auto &statement : function.node->body->statements) {
        auto result = emit_statement(*statement, locals);
        if (!result)
            return false;
        // 已结束的路径不再处理后续语句和函数尾值。
        if (path_ended(*result)) {
            path_open = false;
            break;
        }
    }
    // 只有仍可继续的路径才补函数尾部与正常 ret。
    if (!path_open)
        return true;
    const auto &tail = function.node->body->tail;
    if (is_supported_value(function.return_type)) {
        const auto value = emit_value(*tail, locals);
        if (!value)
            return false;
        // 尾表达式本身提前 return 时，它已经发过 ret。
        if (!path_ended(*value)) {
            // 函数尾值是固定使用位置；使用 sema 记录到 FunctionBody 的计划。
            auto coerced = emit_coercion_at(
                UseSite{function.node, UseKind::FunctionBody, 0},
                std::get<ir::Operand>(*value));
            if (!coerced)
                return false;
            builder.ret(*coerced);
        }
        return true;
    }
    // unit 尾表达式只执行副作用，随后仍正常返回 void。
    if (!tail) {
        builder.ret();
        return true;
    }
    auto result = emit_unit_expression(*tail, locals);
    if (!result)
        return false;
    if (!path_ended(*result))
        builder.ret();
    return true;
}

} // namespace rx
