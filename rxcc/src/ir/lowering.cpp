// 编排一个函数体的生成，并按表达式或语句种类分派到 lowering/ 中的实现。
// 这里只保留块遍历、路径结束传播和函数入口；不实现具体运算、存取或调用。
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
    return emit_expression(expression, locals, ResultUse::Value);
}

// 统一分派表达式；块和 if 根据调用方需求决定是否保留正常结果。
std::optional<EmitResult>
GenerationContext::emit_expression(const Expression &expression,
                                   LocalPlaces &locals, ResultUse use) {
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
    // break/continue 只改变控制流，跳转后当前路径结束。
    if (const auto *break_expression =
            dynamic_cast<const BreakExpression *>(&expression))
        return emit_break(*break_expression, locals);
    if (const auto *continue_expression =
            dynamic_cast<const ContinueExpression *>(&expression))
        return emit_continue(*continue_expression, locals);

    // 调用表达式由统一的目标解析和参数求值逻辑生成。
    if (const auto *call = dynamic_cast<const CallExpression *>(&expression))
        return emit_call(*call, locals);

    if (const auto *if_expression =
            dynamic_cast<const IfExpression *>(&expression))
        return emit_if(*if_expression, locals, use);
    // loop 的值来自 break 写入的结果 slot；unit/never 不分配数据 slot。
    if (const auto *loop = dynamic_cast<const LoopExpression *>(&expression))
        return emit_loop(*loop, locals);

    // 显式 unit 不需要生成 IR 指令。
    if (dynamic_cast<const UnitExpression *>(&expression))
        return EmitResult{UnitResult{}};
    // 赋值正常完成后产生 unit；先求 RHS，再求目标并写入。
    if (const auto *assignment =
            dynamic_cast<const AssignmentExpression *>(&expression))
        return emit_assignment(*assignment, locals);
    // while 的条件在独立块中每轮重新求值，正常退出后继续外层语句。
    if (const auto *while_expression =
            dynamic_cast<const WhileExpression *>(&expression))
        return emit_while(*while_expression, locals);
    // 每个绑定已有唯一 LocalId，块内不需要再次维护词法作用域。
    if (const auto *block =
            dynamic_cast<const BlockExpression *>(&expression)) {
        if (use == ResultUse::Value) {
            const auto &type = semantics.expressions.at(block).type;
            if (!is_supported_value(type) && !type.is_never()) {
                diagnostics << "IR 生成尚未支持此块值类型\n";
                return std::nullopt;
            }
        }
        auto result = emit_statements(*block, locals);
        if (!result || path_ended(*result))
            return result;
        // 块用于循环体等结果被丢弃的位置时，尾表达式自身可能是有类型的发散值。
        if (!block->tail)
            return EmitResult{UnitResult{}};
        return use == ResultUse::Value
                   ? emit_value(*block->tail, locals)
                   : emit_discarded_expression(*block->tail, locals);
    }
    diagnostics << "IR 生成尚未支持此表达式\n";
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
    return emit_discarded_expression(*expression.expression, locals);
}

// 块表达式和函数体遵循同样的语句顺序和提前退出规则。
std::optional<EmitResult>
GenerationContext::emit_statements(const BlockExpression &block,
                                   LocalPlaces &locals) {
    for (const auto &statement : block.statements) {
        auto result = emit_statement(*statement, locals);
        if (!result || path_ended(*result))
            return result;
    }
    return EmitResult{UnitResult{}};
}

// 按表达式自身的类型求值；正常结果可丢弃，但提前离开的路径必须传给外层。
std::optional<EmitResult>
GenerationContext::emit_discarded_expression(const Expression &expression,
                                             LocalPlaces &locals) {
    const auto &type = semantics.expressions.at(&expression).type;
    // 丢弃块结果时先处理语句，不必为不可达的尾值准备存储。
    if (!type.is_unit() && !type.is_never() && !is_supported_value(type) &&
        !dynamic_cast<const BlockExpression *>(&expression)) {
        diagnostics << "IR 生成尚未支持丢弃此类型的表达式结果\n";
        return std::nullopt;
    }
    auto result = emit_expression(expression, locals, ResultUse::Discard);
    if (!result || path_ended(*result))
        return result;
    return EmitResult{UnitResult{}};
}

// 生成一个完整函数体：入口 slot、语句、尾表达式和正常返回。
bool GenerationContext::emit_function_body(const FunctionSymbol &function,
                                           ir::IRFunctionId id) {
    builder.set_insert_point(module.function(id).add_block());
    LocalPlaces locals;
    const auto &parameter_locals = semantics.function_locals.at(function.id);
    // 每个 IR 参数存入其 LocalId 对应的独立存储单元。
    for (std::size_t parameter_index = 0;
         parameter_index < function.parameters.size(); ++parameter_index) {
        const auto local_id = parameter_locals.parameters[parameter_index];
        const auto &local_type = semantics.locals[local_id.index].type;
        // bool 形参存入 i8 slot，整数存入 i32 slot，引用形参保存 ptr。
        const auto address = allocate_slot(lower_value_type(local_type));
        const auto argument = module.function(id).argument(parameter_index);
        store_value(argument, address);
        locals.emplace(local_id, address);
    }
    // 函数体语句按源码顺序生成；提前 return 后停止整条路径。
    const auto statements = emit_statements(*function.node->body, locals);
    if (!statements)
        return false;
    if (path_ended(*statements))
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
    auto result = emit_discarded_expression(*tail, locals);
    if (!result)
        return false;
    if (!path_ended(*result))
        builder.ret();
    return true;
}

} // namespace rx
