// 将已通过语义检查的程序转换为 IR，支持整数局部绑定及 println_i32。
// 先命名并检查所有源函数，再创建 IR 函数和机器入口 main，不忽略未调用的函数。
// 这里只构建模块，输出 LLVM 文本由 artifact/printer 完成。
#include "ir/generator.hpp"
#include "ast/ast.hpp"
#include "ir/builder.hpp"
#include "ir/check.hpp"
#include "sema/result.hpp"
#include <map>
#include <ostream>
#include <stdexcept>
#include <unordered_set>

namespace rx {
namespace {

// 检查源 main 的记录是否正确，以及所有函数是否都能由当前生成器处理。
// 即使某个函数没有被调用，也不能忽略其中尚不支持的参数或函数体。
bool validate_functions(const Crate &crate, const SemanticResult &semantics,
                        std::ostream &diagnostics) {
    if (!semantics.main_function ||
        semantics.main_function->index >= semantics.functions.size()) {
        diagnostics << "IR 内部错误：缺少有效的源 main 语义记录\n";
        return false;
    }
    // 源 main 必须是无接收者、无值参数和生命周期参数、返回 unit 的顶层函数。
    const auto &main = *semantics.functions[semantics.main_function->index];
    if (main.id != *semantics.main_function || main.name != "main" ||
        main.owner || main.node->has_generic_parameters ||
        !main.parameters.empty() || main.receiver ||
        !main.return_type.is_unit()) {
        diagnostics << "IR 内部错误：源 main 与语义结果不一致\n";
        return false;
    }
    std::unordered_set<const Function *> top_level;
    for (const auto &item : crate.items)
        if (const auto *function = dynamic_cast<const Function *>(item.get()))
            top_level.insert(function);
    for (const auto &owned : semantics.functions) {
        const auto &function =
            *owned; // assign_function_names 已检查指针、ID 和声明记录。
        if (function.owner) {
            diagnostics << "IR 生成尚未支持方法或关联函数\n";
            return false;
        }
        if (!top_level.erase(function.node)) {
            diagnostics << "IR 内部错误：语义结果中的函数不属于当前 Crate\n";
            return false;
        }
        if (!function.node->body || !function.parameters.empty() ||
            !function.node->parameters.empty() ||
            !function.return_type.is_unit()) {
            diagnostics
                << "IR 生成尚未支持此函数签名（P2 仅支持无参 unit 函数）\n";
            return false;
        }
        // 块尾表达式留给后续小步；语句在生成时逐一检查，不能静默忽略。
        const auto &body = *function.node->body;
        if (body.tail) {
            diagnostics << "IR 生成尚未支持块尾表达式\n";
            return false;
        }
    }
    if (!top_level.empty()) {
        diagnostics << "IR 内部错误：当前 Crate 的顶层函数缺少语义记录\n";
        return false;
    }
    return true;
}

struct IntegerValue {
    ir::Operand operand;
    SemanticType type;
};

struct IntegerPlace {
    ir::Operand address;
    SemanticType type;
};

using LocalPlaces = std::map<LocalId, IntegerPlace>;

std::optional<IntegerValue> emit_integer(const Expression &expression,
                                         const SemanticResult &semantics,
                                         const LocalPlaces &locals,
                                         ir::IRBuilder &builder,
                                         std::ostream &diagnostics) {
    // 整数文字直接使用语义阶段保存的类型和位模式。
    if (const auto *literal =
            dynamic_cast<const IntegerLiteral *>(&expression)) {
        const auto fact = semantics.integer_literals.find(literal);
        // 字面量记录必须存在，且已定型为整数。
        if (fact == semantics.integer_literals.end() ||
            !fact->second.type.is_int()) {
            diagnostics << "IR 内部错误：缺少有效的整数文字语义记录\n";
            return std::nullopt;
        }
        return IntegerValue{
            ir::Operand::constant(ir::Type::I32, fact->second.bits),
            fact->second.type};
    }
    const auto *path = dynamic_cast<const PathExpression *>(&expression);
    // 除文字外，目前只支持路径形式的整数表达式。
    if (!path) {
        diagnostics << "IR 生成尚未支持此整数表达式\n";
        return std::nullopt;
    }
    const auto target = semantics.paths.find(path);
    // 路径必须已有语义阶段确定的目标身份。
    if (target == semantics.paths.end()) {
        diagnostics << "IR 内部错误：缺少路径目标记录\n";
        return std::nullopt;
    }
    // 局部路径从对应 LocalId 的存储中读取。
    if (const auto *id = std::get_if<LocalId>(&target->second)) {
        const auto local = locals.find(*id);
        // 绑定身份、已分配的存储及源类型必须一致。
        if (id->index >= semantics.locals.size() ||
            semantics.locals[id->index].id != *id || local == locals.end() ||
            !semantics.locals[id->index].type.same_type(local->second.type)) {
            diagnostics << "IR 内部错误：局部路径没有有效的绑定或存储\n";
            return std::nullopt;
        }
        return IntegerValue{
            builder.load(ir::Type::I32, local->second.address, 4),
            local->second.type};
    }
    // 常量路径直接读取求值缓存，不生成运行时初始化。
    if (const auto *id = std::get_if<ConstantId>(&target->second)) {
        // 常量编号必须指向同一身份的有效符号。
        if (id->index >= semantics.constants.size() ||
            !semantics.constants[id->index] ||
            semantics.constants[id->index]->id != *id) {
            diagnostics << "IR 内部错误：无效的常量身份记录\n";
            return std::nullopt;
        }
        const auto &value = semantics.constants[id->index]->evaluated;
        // 常量必须已成功求值。
        if (!value.ok) {
            diagnostics << "IR 内部错误：缺少常量求值缓存\n";
            return std::nullopt;
        }
        // 当前只生成整数常量值。
        if (!value.type.is_int()) {
            diagnostics << "IR 生成尚未支持非整数常量\n";
            return std::nullopt;
        }
        uint32_t bits;
        // 区分缓存的有符号和无符号表示，统一取低 32 位。
        if (const auto *signed_value = std::get_if<int64_t>(&value.value))
            bits = static_cast<uint32_t>(*signed_value);
        else if (const auto *unsigned_value =
                     std::get_if<uint64_t>(&value.value))
            bits = static_cast<uint32_t>(*unsigned_value);
        else {
            diagnostics << "IR 内部错误：整数常量缓存的值无效\n";
            return std::nullopt;
        }
        return IntegerValue{ir::Operand::constant(ir::Type::I32, bits),
                            value.type};
    }
    diagnostics << "IR 生成尚未支持函数路径值\n";
    return std::nullopt;
}

bool emit_let(const LetStatement &statement, const SemanticResult &semantics,
              LocalPlaces &locals, ir::IRBuilder &builder,
              std::ostream &diagnostics) {
    const auto binding = semantics.let_bindings.find(&statement);
    // let 必须有有效且尚未分配存储的绑定身份。
    if (binding == semantics.let_bindings.end() ||
        binding->second.index >= semantics.locals.size() ||
        semantics.locals[binding->second.index].id != binding->second ||
        locals.contains(binding->second)) {
        diagnostics << "IR 内部错误：缺少有效的 let 绑定记录\n";
        return false;
    }
    const auto &type = semantics.locals[binding->second.index].type;
    // 当前只为整数局部绑定分配存储。
    if (!type.is_int()) {
        diagnostics << "IR 生成尚未支持非整数局部绑定\n";
        return false;
    }
    // 合法的 let 必须带初始化器。
    if (!statement.initializer) {
        diagnostics << "IR 内部错误：let 缺少初始化器\n";
        return false;
    }
    auto value = emit_integer(*statement.initializer, semantics, locals,
                              builder, diagnostics);
    // 初始化器生成失败时，不继续分配或写入新绑定。
    if (!value)
        return false;
    // 源整数类型必须一致，不能因 IR 都是 i32 而隐式转换。
    if (!value->type.same_type(type)) {
        diagnostics << "IR 内部错误：let 初始化器类型与绑定不一致\n";
        return false;
    }
    auto address = builder.entry_alloca(ir::Type::I32, 4);
    builder.store(ir::Type::I32, value->operand, address, 4);
    locals.emplace(binding->second, IntegerPlace{address, type});
    return true;
}

bool emit_println(const Statement &statement, const SemanticResult &semantics,
                  ir::Module &module, ir::IRBuilder &builder,
                  std::optional<ir::IRFunctionId> &println,
                  const LocalPlaces &locals, std::ostream &diagnostics) {
    const auto *expression =
        dynamic_cast<const ExpressionStatement *>(&statement);
    const auto *call =
        expression
            ? dynamic_cast<const CallExpression *>(expression->expression.get())
            : nullptr;
    const auto *path =
        call ? dynamic_cast<const PathExpression *>(call->callee.get())
             : nullptr;
    // 限定调用没有 ordinary_calls 记录，不能把尚未支持误报成语义事实缺失。
    if (!path || path->segments.size() != 1) {
        diagnostics << "IR 生成尚未支持此语句或限定调用\n";
        return false;
    }
    const auto target = semantics.ordinary_calls.find(call);
    // 单段普通调用必须已有目标记录。
    if (target == semantics.ordinary_calls.end()) {
        diagnostics << "IR 内部错误：缺少普通调用目标记录\n";
        return false;
    }
    const auto *builtin = std::get_if<BuiltinIO>(&target->second);
    // 只接受语义阶段选定的内置 println_i32，不按名称猜测。
    if (!builtin || *builtin != BuiltinIO::PrintlnI32) {
        diagnostics << "IR 生成尚未支持此调用目标\n";
        return false;
    }
    const auto signature = semantics.builtin_signatures.find(*builtin);
    // 内置目标必须有对应的语义签名。
    if (signature == semantics.builtin_signatures.end()) {
        diagnostics << "IR 内部错误：缺少内置调用签名\n";
        return false;
    }
    const auto i32 = SemanticType::int_(SemanticType::IntKind::I32);
    const auto &source_signature = signature->second;
    // 签名须为无接收者的 (i32) -> unit，且调用恰有一个实参。
    if (source_signature.receiver || !source_signature.return_type.is_unit() ||
        source_signature.parameters.size() != 1 ||
        !source_signature.parameters[0].same_type(i32) ||
        call->arguments.size() != 1) {
        diagnostics << "IR 内部错误：println_i32 调用与签名不一致\n";
        return false;
    }
    // 实参数量正确后，还需确认 AST 节点存在。
    if (!call->arguments[0]) {
        diagnostics << "IR 内部错误：println_i32 缺少实参\n";
        return false;
    }
    auto value = emit_integer(*call->arguments[0], semantics, locals, builder,
                              diagnostics);
    // 实参生成失败时，不发射打印调用。
    if (!value)
        return false;
    // println_i32 只接受源类型 i32，其他整数不能隐式传入。
    if (!value->type.same_type(i32)) {
        diagnostics << "IR 内部错误：println_i32 实参类型不一致\n";
        return false;
    }
    // 首次使用时登记外部声明，后续调用复用同一身份。
    if (!println)
        println = module.add_function("println_i32",
                                      {ir::Type::Void, {ir::Type::I32}},
                                      ir::Linkage::External);
    builder.call(*println, {value->operand});
    return true;
}

} // namespace

// 根据源函数的编号生成内部名称，避免与机器入口 main 或运行时函数同名。
// 函数 ID 和声明记录不一致时输出错误信息，不返回生成到一半的名称表。
std::optional<std::vector<std::string>>
assign_function_names(const SemanticResult &semantics,
                      std::ostream &diagnostics) {
    std::vector<std::string> names;
    names.reserve(semantics.functions.size());
    for (std::size_t index = 0; index < semantics.functions.size(); ++index) {
        const auto &function = semantics.functions[index];
        if (!function || !function->node || function->id.index != index) {
            diagnostics << "IR 内部错误：无效的源函数身份记录\n";
            return std::nullopt;
        }
        // 再通过 AST 节点查询声明表，确认记录的确实是同一个函数 ID。
        const auto declaration =
            semantics.function_declarations.find(function->node);
        if (declaration == semantics.function_declarations.end() ||
            declaration->second != function->id) {
            diagnostics << "IR 内部错误：源函数声明与身份不一致\n";
            return std::nullopt;
        }
        names.push_back("__rx_fn_" + std::to_string(function->id.index));
    }
    return names;
}

// 生成源函数和机器入口 main；遇到不支持的代码或内部构建错误时返回空值。
std::optional<IRArtifact> generate_ir(const Crate &crate,
                                      const SemanticResult &semantics,
                                      std::ostream &diagnostics) {
    const auto function_names = assign_function_names(semantics, diagnostics);
    if (!function_names || !validate_functions(crate, semantics, diagnostics))
        return std::nullopt;

    try {
        IRArtifact artifact("rx");
        auto &module = artifact.module();
        ir::IRBuilder builder(module);
        // functions[源 FunctionId.index] 保存对应的
        // IRFunctionId，不直接混用两种 ID。
        std::vector<ir::IRFunctionId> functions;
        for (const auto &name : *function_names)
            functions.push_back(module.add_function(name, {ir::Type::Void, {}},
                                                    ir::Linkage::Internal));
        std::optional<ir::IRFunctionId> println;
        // 按源码语句顺序生成所有函数，包括未被调用的函数。
        for (std::size_t index = 0; index < functions.size(); ++index) {
            builder.set_insert_point(
                module.function(functions[index]).add_block());
            LocalPlaces locals;
            for (const auto &statement :
                 semantics.functions[index]->node->body->statements) {
                if (!statement) {
                    diagnostics << "IR 内部错误：函数体包含空语句\n";
                    return std::nullopt;
                }
                const auto *binding =
                    dynamic_cast<const LetStatement *>(statement.get());
                if (binding
                        ? !emit_let(*binding, semantics, locals, builder,
                                    diagnostics)
                        : !emit_println(*statement, semantics, module, builder,
                                        println, locals, diagnostics))
                    return std::nullopt;
            }
            builder.ret();
        }
        // 源 main 返回 unit，对应内部 void 函数；机器入口按 C ABI 调用它后返回
        // i32 0。
        const auto entry = module.add_function("main", {ir::Type::I32, {}},
                                               ir::Linkage::External);
        builder.set_insert_point(module.function(entry).add_block());
        builder.call(functions.at(semantics.main_function->index));
        builder.ret(ir::Operand::constant(ir::Type::I32, 0));
        // 检查通过后才返回模块；失败时自动释放已经创建的函数和指令。
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
