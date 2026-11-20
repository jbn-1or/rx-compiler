// 实现一个函数体内部的 lowering：标量读写、表达式、语句、调用、块和 return。
// 不负责 crate 级函数登记、签名和机器入口，那些由 generator.cpp 编排。
#include "ir/lowering.hpp"
#include "ast/ast.hpp"
#include "ir/builder.hpp"
#include "sema/result.hpp"
#include <stdexcept>
#include <utility>

namespace rx {
namespace {

// 局部存储使用的 IR 类型：bool 用 i8，整数用 i32。
ir::Type slot_type(const SemanticType &type) {
    return type.kind == SemanticType::Kind::Bool ? ir::Type::I8 : ir::Type::I32;
}

// bool 的 i8 存储单元按 1 字节对齐，整数按 4 字节对齐。
uint32_t slot_alignment(const SemanticType &type) {
    return type.kind == SemanticType::Kind::Bool ? 1u : 4u;
}

} // namespace

// 判断源类型是否是当前生成器支持的标量（整数或 bool）。
bool is_scalar(const SemanticType &type) {
    return type.is_int() || type.kind == SemanticType::Kind::Bool;
}

// 计算值使用的 IR 类型：bool 为 i1，整数为 i32。
ir::Type lower_scalar_type(const SemanticType &type) {
    return type.kind == SemanticType::Kind::Bool ? ir::Type::I1 : ir::Type::I32;
}

// 判断一次生成结果是否已经结束当前路径。
bool path_ended(const EmitResult &result) {
    return std::holds_alternative<NoContinuation>(result);
}

// 保存这次 IR 生成使用的引用，不拥有任何对象。
GenerationContext::GenerationContext(
    const SemanticResult &semantics,
    const std::vector<ir::IRFunctionId> &functions, ir::Module &module,
    ir::IRBuilder &builder, std::ostream &diagnostics)
    : semantics(semantics), functions(functions), module(module),
      builder(builder), diagnostics(diagnostics) {}

// 把当前支持的标量表达式（整数或 bool）生成为 IR 操作数。
ir::Operand GenerationContext::emit_scalar(const Expression &expression,
                                           const LocalPlaces &locals) {
    // 整数文字直接使用语义阶段保存的类型和位模式。
    if (const auto *literal =
            dynamic_cast<const IntegerLiteral *>(&expression)) {
        const auto &literal_record = semantics.integer_literals.at(literal);
        return ir::Operand::constant(ir::Type::I32, literal_record.bits);
    }
    // true/false 直接生成 i1 常量，false 为 0，true 为 1。
    if (const auto *boolean =
            dynamic_cast<const BooleanLiteral *>(&expression)) {
        return ir::Operand::constant(ir::Type::I1, boolean->value ? 1u : 0u);
    }
    const auto &path = static_cast<const PathExpression &>(expression);
    const auto &target = semantics.paths.at(&path);
    // 局部变量按 LocalId 找存储位置，不能按名称处理遮蔽。
    if (const auto *local_id = std::get_if<LocalId>(&target)) {
        const auto &type = semantics.locals[local_id->index].type;
        // 标量读取统一经过 load_scalar，bool 会自动补 trunc。
        return load_scalar(locals.at(*local_id), type);
    }
    // 常量路径从已求值缓存取值，不生成运行时存储。
    const auto constant_id = std::get<ConstantId>(target);
    const auto &value = semantics.constants[constant_id.index]->evaluated;
    uint32_t bits = 0;
    // bool 缓存保存 true/false，先转成 0/1；整数区分有符号与无符号表示。
    if (value.type.kind == SemanticType::Kind::Bool)
        bits = std::get<bool>(value.value) ? 1u : 0u;
    else if (value.type.int_kind == SemanticType::IntKind::I32 ||
             value.type.int_kind == SemanticType::IntKind::Isize)
        bits = static_cast<uint32_t>(std::get<int64_t>(value.value));
    else
        bits = static_cast<uint32_t>(std::get<uint64_t>(value.value));
    return ir::Operand::constant(lower_scalar_type(value.type), bits);
}

// 把标量写入它的存储单元；bool 先 zext 成 i8，整数直接写 i32。
void GenerationContext::store_scalar(const ir::Operand &value,
                                     const ir::Operand &address) {
    // bool 的存储宽度是 i8，写入前先把 i1 零扩展。
    if (value.type == ir::Type::I1) {
        builder.store(ir::Type::I8, builder.zext(ir::Type::I8, value),
                      address, 1);
        return;
    }
    builder.store(ir::Type::I32, value, address, 4);
}

// 从存储单元读回标量；bool 先 load i8 再 trunc 成 i1。
ir::Operand GenerationContext::load_scalar(const ir::Operand &address,
                                           const SemanticType &type) {
    // bool 的读取必须经过 i8，不能直接把 i8 当 i1 使用。
    if (type.kind == SemanticType::Kind::Bool)
        return builder.trunc(ir::Type::I1,
                             builder.load(ir::Type::I8, address, 1));
    return builder.load(ir::Type::I32, address, 4);
}

// 生成需要数据值的表达式；路径已结束时返回 NoContinuation 而不是伪造值。
std::optional<EmitResult>
GenerationContext::emit_value(const Expression &expression,
                              LocalPlaces &locals) {
    // 字面量和路径不产生嵌套求值，复用基础标量读取逻辑。
    if (dynamic_cast<const IntegerLiteral *>(&expression) ||
        dynamic_cast<const BooleanLiteral *>(&expression) ||
        dynamic_cast<const PathExpression *>(&expression)) {
        return EmitResult{emit_scalar(expression, locals)};
    }

    // return 不需要也不能产生操作数，只结束当前路径。
    if (const auto *return_expression =
            dynamic_cast<const ReturnExpression *>(&expression))
        return emit_return(*return_expression, locals);

    // 调用表达式由统一的目标解析和参数求值逻辑生成。
    if (const auto *call = dynamic_cast<const CallExpression *>(&expression))
        return emit_call(*call, locals);

    // 需要值的块可能是标量块，也可能是整块路径都已结束的 never 块。
    if (const auto *block =
            dynamic_cast<const BlockExpression *>(&expression)) {
        const auto &type = semantics.expressions.at(block).type;
        // let 等位置也能出现非标量块，但当前还没有对应的值表示。
        if (!is_scalar(type) && !type.is_never()) {
            diagnostics << "IR 生成尚未支持非标量块值\n";
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
        // 正常到达块尾时，语义检查已保证这里有标量或 never 尾表达式。
        return emit_value(*block->tail, nested_locals);
    }

    diagnostics << "IR 生成尚未支持此标量表达式\n";
    return std::nullopt;
}

// 按语义记录生成用户函数或 I/O 调用，并返回值、unit 或已结束的路径。
std::optional<EmitResult>
GenerationContext::emit_call(const CallExpression &call,
                             LocalPlaces &locals) {
    const auto &callee = static_cast<const PathExpression &>(*call.callee);
    // 只有单段普通调用能使用 ordinary_calls 记录。
    if (callee.segments.size() != 1) {
        diagnostics << "IR 生成尚未支持限定调用\n";
        return std::nullopt;
    }
    const auto &target = semantics.ordinary_calls.at(&call);

    ir::IRFunctionId ir_target;
    if (const auto *function_id = std::get_if<FunctionId>(&target)) {
        ir_target = functions[function_id->index];
    } else {
        // builtin 使用语义阶段保存的签名，再映射到受控运行时符号。
        const auto builtin = std::get<BuiltinIO>(target);
        const auto &signature = semantics.builtin_signatures.at(builtin);
        const char *name = nullptr;
        switch (builtin) {
        case BuiltinIO::GetI32: name = "get_i32"; break;
        case BuiltinIO::PrintI32: name = "print_i32"; break;
        case BuiltinIO::PrintlnI32: name = "println_i32"; break;
        }
        // 三个 I/O builtin 的参数固定为 i32，签名已由语义阶段登记。
        std::vector<ir::Type> lowered_parameters(signature.parameters.size(),
                                                 ir::Type::I32);
        const ir::Signature lowered_signature{
            signature.return_type.is_unit() ? ir::Type::Void : ir::Type::I32,
            std::move(lowered_parameters)};
        if (const auto *existing = module.find_function(name)) {
            ir_target = existing->id;
        } else {
            ir_target = module.add_function(name, lowered_signature,
                                            ir::Linkage::External);
        }
    }

    std::vector<ir::Operand> arguments;
    arguments.reserve(call.arguments.size());
    // 每个参数先独立完成求值，再继续下一个参数。
    for (const auto &source_argument : call.arguments) {
        auto argument = emit_value(*source_argument, locals);
        if (!argument)
            return std::nullopt;
        // 前面的实参提前 return 后，后续实参和最终 call 都不能生成。
        if (path_ended(*argument))
            return EmitResult{NoContinuation{}};
        arguments.push_back(std::get<ir::Operand>(*argument));
    }
    const auto value = builder.call(ir_target, std::move(arguments));
    // 标量调用产生 SSA 值；unit 调用只有副作用。
    if (value)
        return EmitResult{*value};
    return EmitResult{UnitResult{}};
}

// 先求值标量 let 初始化器，再为其 LocalId 分配并登记存储位置。
std::optional<EmitResult>
GenerationContext::emit_let(const LetStatement &statement,
                            LocalPlaces &locals) {
    const auto binding = semantics.let_bindings.at(&statement);
    // 在新绑定进入映射前先生成初始化器。
    auto initializer = emit_value(*statement.initializer, locals);
    // 初始化器不支持时立即失败，不创建未初始化的存储位置。
    if (!initializer)
        return std::nullopt;
    // 初始化器提前 return 时不分配存储，也不执行 store。
    if (path_ended(*initializer))
        return EmitResult{NoContinuation{}};
    const auto &type = semantics.locals[binding.index].type;
    // 当前局部存储只支持整数和 bool。
    if (!is_scalar(type)) {
        diagnostics << "IR 生成尚未支持非标量局部绑定\n";
        return std::nullopt;
    }
    // bool 使用 i8 存储单元，整数使用 i32；写入统一经过 store_scalar。
    auto address = builder.entry_alloca(slot_type(type), slot_alignment(type));
    store_scalar(std::get<ir::Operand>(*initializer), address);
    // 初始化已生成后才登记新绑定，支持 `let x = x` 读取旧 x。
    locals.emplace(binding, address);
    return EmitResult{UnitResult{}};
}

// 生成 return：先按操作数的源类型求值，再发 ret 并结束当前路径。
std::optional<EmitResult>
GenerationContext::emit_return(const ReturnExpression &expression,
                               LocalPlaces &locals) {
    if (!expression.value) {
        builder.ret();
        return EmitResult{NoContinuation{}};
    }
    const auto &value_record = semantics.expressions.at(expression.value.get());
    // 返回 () 的路径只发 ret void，不产生数据操作数。
    if (value_record.type.is_unit()) {
        auto result = emit_unit_expression(*expression.value, locals);
        if (!result)
            return std::nullopt;
        // 操作数内部的 return 已经发过 ret，外层不能再补一条。
        if (path_ended(*result))
            return result;
        builder.ret();
        return EmitResult{NoContinuation{}};
    }
    auto result = emit_value(*expression.value, locals);
    if (!result)
        return std::nullopt;
    // 操作数本身提前结束路径时，由它负责发 ret。
    if (path_ended(*result))
        return result;
    builder.ret(std::get<ir::Operand>(*result));
    return EmitResult{NoContinuation{}};
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
    // 标量表达式语句丢弃值但保留调用等副作用。
    if (is_scalar(type)) {
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
    LocalPlaces locals;
    const auto &parameter_locals = semantics.function_locals.at(function.id);
    // 每个 IR 参数存入其 LocalId 对应的独立存储单元。
    for (std::size_t parameter_index = 0;
         parameter_index < function.parameters.size(); ++parameter_index) {
        const auto local_id = parameter_locals.parameters[parameter_index];
        const auto &local_type = semantics.locals[local_id.index].type;
        // bool 形参存入 i8 slot，整数存入 i32 slot。
        const auto address = builder.entry_alloca(slot_type(local_type),
                                                  slot_alignment(local_type));
        const auto argument = module.function(id).argument(parameter_index);
        store_scalar(argument, address);
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
    if (is_scalar(function.return_type)) {
        const auto value = emit_value(*tail, locals);
        if (!value)
            return false;
        // 尾表达式本身提前 return 时，它已经发过 ret。
        if (!path_ended(*value))
            builder.ret(std::get<ir::Operand>(*value));
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