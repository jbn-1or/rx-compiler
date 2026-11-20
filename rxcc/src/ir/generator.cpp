// 将已通过语义检查的程序转换为 IR，支持整数/bool 存储、标量调用、I/O 和提前 return。
// 先确认所有源函数的支持范围，再命名并创建 IR 函数和机器入口 main，不忽略未调用的函数。
// 这里只构建模块，输出 LLVM 文本由 artifact/printer 完成。
#include "ir/generator.hpp"
#include "ast/ast.hpp"
#include "ir/builder.hpp"
#include "ir/check.hpp"
#include "sema/result.hpp"
#include <algorithm>
#include <map>
#include <ostream>
#include <stdexcept>
#include <variant>

namespace rx {
namespace {

// 判断源类型是否是当前生成器支持的标量（整数或 bool）。
bool is_scalar(const SemanticType &type) {
    return type.is_int() || type.kind == SemanticType::Kind::Bool;
}

// 判断参数是否为当前生成器支持的标量类型。
bool is_scalar_parameter(const ResolvedParameter &parameter) {
    return is_scalar(parameter.type);
}

// 计算值使用的 IR 类型：bool 为 i1，整数为 i32。
ir::Type lower_scalar_type(const SemanticType &type) {
    return type.kind == SemanticType::Kind::Bool ? ir::Type::I1 : ir::Type::I32;
}

// 局部存储使用的 IR 类型：bool 用 i8，整数用 i32。
ir::Type slot_type(const SemanticType &type) {
    return type.kind == SemanticType::Kind::Bool ? ir::Type::I8 : ir::Type::I32;
}

// bool 的 i8 存储单元按 1 字节对齐，整数按 4 字节对齐。
uint32_t slot_alignment(const SemanticType &type) {
    return type.kind == SemanticType::Kind::Bool ? 1u : 4u;
}

// 只检查所有函数是否在当前 lowering 的支持范围内，语义正确性由 sema 保证。
bool validate_functions(const SemanticResult &semantics,
                        std::ostream &diagnostics) {
    for (const auto &owned : semantics.functions) {
        const auto &function = *owned;
        // 当前生成器只接收顶层普通函数。
        if (function.owner) {
            diagnostics << "IR 生成尚未支持方法或关联函数\n";
            return false;
        }
        // 当前接受整数或 bool 形参，以及整数、bool 或 unit 返回类型。
        const bool supported_return = function.return_type.is_unit() ||
                                      is_scalar(function.return_type);
        const bool supported_parameters =
            std::all_of(function.parameters.begin(), function.parameters.end(),
                        is_scalar_parameter);
        if (!supported_parameters || !supported_return) {
            diagnostics << "IR 生成尚未支持此函数签名"
                           "（仅支持整数/bool 参数以及整数/bool/unit 返回）\n";
            return false;
        }
        // 函数体的语句列表和尾表达式会在生成时按源码顺序处理。
    }
    return true;
}

// 只保存每个局部绑定的存储地址，源类型直接从语义结果读取。
using LocalPlaces = std::map<LocalId, ir::Operand>;

// 表达式正常完成但没有数据值，例如 () 或已经产生副作用的 unit 调用。
struct UnitResult {};
// 当前执行路径已经由 return 结束，外层不能再生成任何指令。
struct NoContinuation {};
// 生成一条表达式或语句得到的三种执行结果；出错仍用 std::nullopt 表示。
using EmitResult = std::variant<ir::Operand, UnitResult, NoContinuation>;

// 判断一次生成结果是否已经结束当前路径。
bool path_ended(const EmitResult &result) {
    return std::holds_alternative<NoContinuation>(result);
}

// 保存一次 IR 生成过程中不变的环境；局部变量表按函数和块作用域单独传递。
class GenerationContext {
public:
    // 保存这次 IR 生成使用的引用，不拥有任何对象。
    GenerationContext(const SemanticResult &semantics,
                      const std::vector<ir::IRFunctionId> &functions,
                      ir::Module &module, ir::IRBuilder &builder,
                      std::ostream &diagnostics)
        : semantics(semantics), functions(functions), module(module),
          builder(builder), diagnostics(diagnostics) {}

    // 生成一条 let 或表达式语句。
    std::optional<EmitResult> emit_statement(const Statement &statement,
                                            LocalPlaces &locals);
    // 计算标量值表达式（整数或 bool），结果直接保存 IR 操作数。
    std::optional<EmitResult> emit_value(const Expression &expression,
                                        LocalPlaces &locals);
    // 只生成能正常完成并产生 unit 的表达式。
    std::optional<EmitResult> emit_unit_expression(const Expression &expression,
                                                  LocalPlaces &locals);
    // 把标量写入它的存储单元；bool 先扩展成 i8，整数直接写 i32。
    void store_scalar(const ir::Operand &value, const ir::Operand &address);

private:
    // 计算整数、bool 字面量以及局部变量或常量路径。
    ir::Operand emit_scalar(const Expression &expression,
                             const LocalPlaces &locals);
    // 从存储单元读回标量；bool 读取 i8 后再截断成 i1。
    ir::Operand load_scalar(const ir::Operand &address,
                             const SemanticType &type);
    // 根据普通调用目标生成函数或 builtin 调用。
    std::optional<EmitResult> emit_call(const CallExpression &call,
                                       LocalPlaces &locals);
    // 先生成初始化器，再为 LocalId 分配并登记存储位置。
    std::optional<EmitResult> emit_let(const LetStatement &statement,
                                      LocalPlaces &locals);
    // 生成 return：先求值操作数，再发 ret 并结束当前路径。
    std::optional<EmitResult> emit_return(const ReturnExpression &expression,
                                         LocalPlaces &locals);

    // 语义分析结果，提供表达式、路径和调用的记录。
    const SemanticResult &semantics;
    // 源 FunctionId 下标到已登记 IRFunctionId 的映射。
    const std::vector<ir::IRFunctionId> &functions;
    // 正在构建的 IR 模块。
    ir::Module &module;
    // 向当前基本块添加指令的构建器。
    ir::IRBuilder &builder;
    // 收集 IR 生成期间错误信息的输出流。
    std::ostream &diagnostics;
};

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

} // namespace

// 根据源函数的编号生成内部名称，避免与机器入口 main 或运行时函数同名。
std::vector<std::string>
assign_function_names(const SemanticResult &semantics) {
    std::vector<std::string> names;
    names.reserve(semantics.functions.size());
    for (const auto &function : semantics.functions)
        names.push_back("__rx_fn_" + std::to_string(function->id.index));
    return names;
}

// 生成源函数和机器入口 main；遇到不支持的代码或内部构建错误时返回空值。
std::optional<IRArtifact> generate_ir(const SemanticResult &semantics,
                                      std::ostream &diagnostics) {
    // 全 crate 支持边界检查失败时，不构建模块。
    if (!validate_functions(semantics, diagnostics))
        return std::nullopt;
    const auto function_names = assign_function_names(semantics);

    try {
        IRArtifact artifact;
        auto &module = artifact.module();
        ir::IRBuilder builder(module);
        // functions[源 FunctionId.index] 保存对应的IRFunctionId，不直接混用两种 ID。
        std::vector<ir::IRFunctionId> functions;
        GenerationContext context(semantics, functions, module, builder,
                                diagnostics);
        // 先登记所有函数，使前向调用可以直接按 FunctionId 解析。
        for (std::size_t function_index = 0;
             function_index < function_names.size(); ++function_index) {
            const auto &source = *semantics.functions[function_index];
            std::vector<ir::Type> parameters;
            parameters.reserve(source.parameters.size());
            // 每个形参按源类型映射成 i1 或 i32。
            for (const auto &parameter : source.parameters)
                parameters.push_back(lower_scalar_type(parameter.type));
            const auto result = source.return_type.is_unit()
                                    ? ir::Type::Void
                                    : lower_scalar_type(source.return_type);
            functions.push_back(module.add_function(
                function_names[function_index],
                {result, std::move(parameters)}, ir::Linkage::Internal));
        }
        // 按源码语句顺序生成所有函数，包括未被调用的函数。
        for (std::size_t function_index = 0;
             function_index < functions.size(); ++function_index) {
            builder.set_insert_point(
                module.function(functions[function_index]).add_block());
            LocalPlaces locals;
            const auto &source = *semantics.functions[function_index];
            const auto &parameter_locals = semantics.function_locals.at(source.id);
            // 每个 IR 参数存入其 LocalId 对应的独立存储单元。
            for (std::size_t parameter_index = 0;
                 parameter_index < source.parameters.size();
                 ++parameter_index) {
                const auto local_id =
                    parameter_locals.parameters[parameter_index];
                const auto &local_type =
                    semantics.locals[local_id.index].type;
                // bool 形参存入 i8 slot，整数存入 i32 slot。
                const auto address = builder.entry_alloca(
                    slot_type(local_type), slot_alignment(local_type));
                const auto argument =
                    module.function(functions[function_index])
                        .argument(parameter_index);
                context.store_scalar(argument, address);
                locals.emplace(local_id, address);
            }
            // 函数体语句按源码顺序生成；提前 return 后停止整条路径。
            bool path_open = true;
            for (const auto &statement : source.node->body->statements) {
                auto result = context.emit_statement(*statement, locals);
                if (!result)
                    return std::nullopt;
                // 已结束的路径不再处理后续语句和函数尾值。
                if (path_ended(*result)) {
                    path_open = false;
                    break;
                }
            }
            // 只有仍可继续的路径才补函数尾部与正常 ret。
            if (path_open) {
                const auto &tail = source.node->body->tail;
                if (is_scalar(source.return_type)) {
                    const auto value = context.emit_value(*tail, locals);
                    if (!value)
                        return std::nullopt;
                    // 尾表达式本身提前 return 时，它已经发过 ret。
                    if (!path_ended(*value)) {
                        builder.ret(std::get<ir::Operand>(*value));
                    }
                } else {
                    // unit 尾表达式只执行副作用，随后仍正常返回 void。
                    if (!tail) {
                        builder.ret();
                    } else {
                        auto result = context.emit_unit_expression(*tail, locals);
                        if (!result)
                            return std::nullopt;
                        if (!path_ended(*result))
                            builder.ret();
                    }
                }
            }
        }
        // 源main返回unit，对应内部void函数；机器入口按C调用约定调用它后返回i32 0。
        const auto entry = module.add_function("main", {ir::Type::I32, {}},
                                               ir::Linkage::External);
        builder.set_insert_point(module.function(entry).add_block());
        builder.call(functions[semantics.main_function->index]);
        builder.ret(ir::Operand::constant(ir::Type::I32, 0));
        // 只有通过结构检查的完整模块才交付给调用方；失败时随 artifact 一起释放。
        if (!ir::check_module(module, diagnostics))
            return std::nullopt;
        return artifact;
    } catch (const std::logic_error &error) {
        // IR 构建失败属于编译器内部错误，不是源程序没有通过类型检查。
        diagnostics << "IR 内部错误：" << error.what() << '\n';
        return std::nullopt;
    }
}

} // namespace rx
