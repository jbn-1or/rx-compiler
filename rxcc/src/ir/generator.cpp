// 将已通过语义检查的程序转换为 IR，当前只支持空的、无参数、返回 unit 的普通函数。
// 先命名并检查所有源函数，再创建 IR 函数和机器入口 main，不忽略未调用的函数。
// 这里只构建模块，输出 LLVM 文本由 artifact/printer 完成。
#include "ir/generator.hpp"
#include "ast/ast.hpp"
#include "ir/builder.hpp"
#include "ir/check.hpp"
#include "sema/result.hpp"
#include <ostream>
#include <stdexcept>

namespace rx {
namespace {

// 判断函数节点是否直接出现在当前 crate 的顶层，不通过名称猜测是否为同一个函数。
bool is_top_level_function(const Crate &crate, const Function *function) {
    for (const auto &item : crate.items)
        if (item.get() == function)
            return true;
    return false;
}

// 检查源 main 的记录是否正确，以及所有函数是否都能由当前生成器处理。
// 即使某个函数没有被调用，也不能忽略其中尚不支持的参数或函数体。
bool validate_functions(const Crate &crate, const SemanticResult &semantics,
                         std::ostream &diagnostics) {
    if (!semantics.main_function || semantics.main_function->index >= semantics.functions.size()) {
        diagnostics << "IR 内部错误：缺少有效的源 main 语义记录\n";
        return false;
    }
    // 源 main 必须是无接收者、无参数、返回 unit 的顶层函数。
    const auto &main = *semantics.functions[semantics.main_function->index];
    if (main.id != *semantics.main_function || main.name != "main" || main.owner ||
        !main.parameters.empty() || main.receiver || !main.return_type.is_unit()) {
        diagnostics << "IR 内部错误：源 main 与语义结果不一致\n";
        return false;
    }
    for (const auto &owned : semantics.functions) {
        const auto &function = *owned; // assign_function_names 已检查指针、ID 和声明记录。
        if (function.owner) {
            diagnostics << "IR 生成尚未支持方法或关联函数（P2 仅生成空的普通函数）\n";
            return false;
        }
        if (!is_top_level_function(crate, function.node)) {
            diagnostics << "IR 内部错误：语义结果中的函数不属于当前 Crate\n";
            return false;
        }
        if (function.node->has_generic_parameters || !function.node->body ||
            !function.parameters.empty() || !function.node->parameters.empty() ||
            !function.return_type.is_unit()) {
            diagnostics << "IR 生成尚未支持此函数签名（P2 仅支持无参 unit 函数）\n";
            return false;
        }
        // 当前不支持语句和块尾表达式，发现后直接报错，不能当作空函数生成。
        const auto &body = *function.node->body;
        if (!body.statements.empty() || body.tail) {
            diagnostics << "IR 生成尚未支持非空函数体（P3 及后续 lowering 待实现）\n";
            return false;
        }
    }
    return true;
}

} // namespace

// 根据源函数的编号生成内部名称，避免与机器入口 main 或运行时函数同名。
// 函数 ID 和声明记录不一致时输出错误信息，不返回生成到一半的名称表。
std::optional<std::vector<std::string>> assign_function_names(
    const SemanticResult &semantics, std::ostream &diagnostics) {
    std::vector<std::string> names;
    names.reserve(semantics.functions.size());
    for (std::size_t index = 0; index < semantics.functions.size(); ++index) {
        const auto &function = semantics.functions[index];
        if (!function || !function->node || function->id.index != index) {
            diagnostics << "IR 内部错误：无效的源函数身份记录\n";
            return std::nullopt;
        }
        // 再通过 AST 节点查询声明表，确认记录的确实是同一个函数 ID。
        const auto declaration = semantics.function_declarations.find(function->node);
        if (declaration == semantics.function_declarations.end() || declaration->second != function->id) {
            diagnostics << "IR 内部错误：源函数声明与身份不一致\n";
            return std::nullopt;
        }
        names.push_back("__rx_fn_" + std::to_string(function->id.index));
    }
    return names;
}

// 生成空源函数和机器入口 main；遇到不支持的代码或内部构建错误时返回空值。
std::optional<IRArtifact> generate_ir(const Crate &crate, const SemanticResult &semantics,
                                      std::ostream &diagnostics) {
    const auto function_names = assign_function_names(semantics, diagnostics);
    if (!function_names || !validate_functions(crate, semantics, diagnostics))
        return std::nullopt;

    try {
        IRArtifact artifact("rx");
        auto &module = artifact.module();
        ir::IRBuilder builder(module);
        // functions[源 FunctionId.index] 保存对应的 IRFunctionId，不直接混用两种 ID。
        std::vector<ir::IRFunctionId> functions;
        for (const auto &name : *function_names)
            functions.push_back(module.add_function(name, {ir::Type::Void, {}}, ir::Linkage::Internal));
        // 所有函数都有 ID 后，再创建各自的入口块；空函数只需添加 ret void。
        for (const auto id : functions) {
            builder.set_insert_point(module.function(id).add_block());
            builder.ret();
        }
        // 源 main 返回 unit，对应内部 void 函数；机器入口按 C ABI 调用它后返回 i32 0。
        const auto entry = module.add_function("main", {ir::Type::I32, {}}, ir::Linkage::External);
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
