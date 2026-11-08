// P2：只生成空的无参 unit 源函数与机器入口；完整 sema 由驱动先行执行。
#include "ir/generator.hpp"
#include "ast/ast.hpp"
#include "ir/builder.hpp"
#include "ir/check.hpp"
#include "sema/result.hpp"
#include <ostream>
#include <stdexcept>

namespace rx {
namespace {

bool is_top_level_function(const Crate &crate, const Function *function) {
    for (const auto &item : crate.items)
        if (item.get() == function)
            return true;
    return false;
}

bool validate_functions(const Crate &crate, const SemanticResult &semantics,
                         std::ostream &diagnostics) {
    if (!semantics.main_function || semantics.main_function->index >= semantics.functions.size()) {
        diagnostics << "IR 内部错误：缺少有效的源 main 语义记录\n";
        return false;
    }
    const auto &main = *semantics.functions[semantics.main_function->index];
    if (main.id != *semantics.main_function || main.name != "main" || main.owner ||
        !main.parameters.empty() || main.receiver || !main.return_type.is_unit()) {
        diagnostics << "IR 内部错误：源 main 与语义结果不一致\n";
        return false;
    }
    for (const auto &owned : semantics.functions) {
        const auto &function = *owned; // assign_function_names 已核对符号及声明身份。
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
        // 不丢弃任何未支持的语句/尾表达式，也不只检查 main 或可达函数。
        const auto &body = *function.node->body;
        if (!body.statements.empty() || body.tail) {
            diagnostics << "IR 生成尚未支持非空函数体（P3 及后续 lowering 待实现）\n";
            return false;
        }
    }
    return true;
}

} // namespace

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
        const auto declaration = semantics.function_declarations.find(function->node);
        if (declaration == semantics.function_declarations.end() || declaration->second != function->id) {
            diagnostics << "IR 内部错误：源函数声明与身份不一致\n";
            return std::nullopt;
        }
        names.push_back("__rx_fn_" + std::to_string(function->id.index));
    }
    return names;
}

std::optional<IRArtifact> generate_ir(const Crate &crate, const SemanticResult &semantics,
                                      std::ostream &diagnostics) {
    const auto function_names = assign_function_names(semantics, diagnostics);
    if (!function_names || !validate_functions(crate, semantics, diagnostics))
        return std::nullopt;

    try {
        IRArtifact artifact("rx");
        auto &module = artifact.module();
        ir::IRBuilder builder(module);
        std::vector<ir::IRFunctionId> functions;
        for (const auto &name : *function_names)
            functions.push_back(module.add_function(name, {ir::Type::Void, {}}, ir::Linkage::Internal));
        for (const auto id : functions) {
            builder.set_insert_point(module.function(id).add_block());
            builder.ret();
        }
        const auto entry = module.add_function("main", {ir::Type::I32, {}}, ir::Linkage::External);
        builder.set_insert_point(module.function(entry).add_block());
        builder.call(functions.at(semantics.main_function->index));
        builder.ret(ir::Operand::constant(ir::Type::I32, 0));
        if (!ir::check_module(module, diagnostics))
            return std::nullopt;
        return artifact;
    } catch (const std::logic_error &error) {
        diagnostics << "IR 内部错误：" << error.what() << '\n';
        return std::nullopt;
    }
}

} // namespace rx
