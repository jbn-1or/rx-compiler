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
      layouts_(module), builder(builder), diagnostics(diagnostics) {}

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
    // 局部路径、显式解引用和投影都从原位置取值；借用和写入则直接用 emit_place。
    if (semantics.expressions.at(&expression).place.is_place) {
        auto place = emit_place(expression, locals);
        if (!place)
            return std::nullopt;
        if (path_ended(*place))
            return NoContinuation{};
        const auto &source = std::get<Place>(*place);
        return read_stored_value(source.address, *source.type,
                                 StorageOrigin::SourcePlace);
    }
    // 二元运算先求值两侧，再按源类型选择指令。
    if (const auto *binary =
            dynamic_cast<const BinaryExpression *>(&expression))
        return emit_binary(*binary, locals, use);
    // 解引用已按 place 处理；其余一元表达式是借用、取负或取反。
    if (const auto *unary = dynamic_cast<const UnaryExpression *>(&expression))
        return emit_unary(*unary, locals);
    // as 转换只改源类型或把 bool 零扩展，不引入数值转换。
    if (const auto *cast = dynamic_cast<const CastExpression *>(&expression))
        return emit_cast(*cast, locals);
    // 排除局部路径后，只剩字面量和编译期常量路径。
    if (dynamic_cast<const IntegerLiteral *>(&expression) ||
        dynamic_cast<const BooleanLiteral *>(&expression) ||
        dynamic_cast<const PathExpression *>(&expression)) {
        return emit_constant(expression);
    }

    // 结构体和数组构造返回独立临时存储。
    if (const auto *structure =
            dynamic_cast<const StructExpression *>(&expression))
        return emit_struct(*structure, locals);
    if (const auto *array = dynamic_cast<const ArrayExpression *>(&expression))
        return emit_array(*array, locals);

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
        return emit_loop(*loop, locals, use);

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
    if (!type.is_unit() && !type.is_never() && !is_storable_value(type) &&
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
    auto &ir_function = module.function(id);
    current_return_type_ = &function.return_type;
    function_result_pointer_.reset();
    std::size_t parameter_offset = 0;
    // 聚合返回的第一个降级形参由调用者提供，保存最终结果的地址。
    if (ir_function.abi.result_storage) {
        function_result_pointer_ = ir_function.argument(0);
        parameter_offset = 1;
    }
    const auto &parameter_locals = semantics.function_locals.at(function.id);
    // 每个源形参存入自己的 LocalId 存储；按值聚合先从实参快照复制到独立 slot。
    for (std::size_t parameter_index = 0;
         parameter_index < function.parameters.size(); ++parameter_index) {
        const auto local_id = parameter_locals.parameters[parameter_index];
        const auto &local_type = semantics.locals[local_id.index].type;
        const auto &abi_storage =
            ir_function.abi.parameter_storage[parameter_offset + parameter_index];
        const auto argument =
            ir_function.argument(parameter_offset + parameter_index);
        const bool by_value =
            abi_storage.kind != ir::StorageType::Kind::Unit;
        const auto address = allocate_storage_slot(
            by_value ? abi_storage : ir::storage_type_of(local_type));
        if (by_value)
            copy_value(address, argument, local_type);
        else
            store_value(argument, address);
        locals.emplace(local_id, address);
    }
    // 函数体语句按源码顺序生成；提前 return 后停止整条路径。
    const auto statements = emit_statements(*function.node->body, locals);
    if (!statements)
        return false;
    if (path_ended(*statements))
        return true;
    return emit_return_value(function.node->body->tail.get(),
                             {function.node, UseKind::FunctionBody, 0}, locals).has_value();
}

} // namespace rx
