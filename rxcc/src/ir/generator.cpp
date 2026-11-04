// 用语义检查确定的源 main 创建内部函数及符合 C 接口的机器入口。
// 现在只生成空函数体，函数调用和其他函数体留待后续阶段。

#include "ir/generator.hpp"
#include "ast/ast.hpp"
#include "sema/result.hpp"
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <string>

namespace rx {

std::optional<IRArtifact> generate_ir(const Crate &crate,
                                      const SemanticResult &semantics,
                                      llvm::raw_ostream &diagnostics) {
    if (!semantics.main_function ||
        semantics.main_function->index >= semantics.functions.size() ||
        !semantics.functions[semantics.main_function->index]) {
        diagnostics << "IR 内部错误：缺少有效的源 main 语义记录\n";
        return std::nullopt;
    }
    const auto &source = *semantics.functions[semantics.main_function->index];
    bool belongs_to_crate = false;
    for (const auto &item : crate.items) {
        if (item.get() == source.node) {
            belongs_to_crate = true;
            break;
        }
    }
    const auto found = semantics.function_declarations.find(source.node);
    if (!belongs_to_crate || source.id != *semantics.main_function ||
        found == semantics.function_declarations.end() || found->second != source.id ||
        source.owner || source.name != "main" || !source.node ||
        !source.node->body || !source.parameters.empty() || source.receiver ||
        !source.return_type.is_unit()) {
        diagnostics << "IR 内部错误：源 main 与语义结果不一致\n";
        return std::nullopt;
    }
    // 不能忽略其他函数或非空函数体，否则会错误地声称程序已生成。
    if (semantics.functions.size() != 1 || !source.node->body->statements.empty() ||
        source.node->body->tail) {
        diagnostics << "IR 生成尚未支持非空 main 或其他函数\n";
        return std::nullopt;
    }

    IRArtifact artifact("rx");
    auto &context = artifact.context();
    auto &module = artifact.module();
    llvm::IRBuilder<> builder(context);

    const auto source_name = "__rx_fn_" + std::to_string(source.id.index);
    auto *source_type = llvm::FunctionType::get(builder.getVoidTy(), false);
    auto *source_function = llvm::Function::Create(
        source_type, llvm::Function::InternalLinkage, source_name, module);
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", source_function));
    builder.CreateRetVoid();
    builder.ClearInsertionPoint();

    auto *entry_type = llvm::FunctionType::get(builder.getInt32Ty(), false);
    auto *entry = llvm::Function::Create(
        entry_type, llvm::Function::ExternalLinkage, "main", module);
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", entry));
    builder.CreateCall(source_function);
    builder.CreateRet(builder.getInt32(0));
    builder.ClearInsertionPoint();

    if (llvm::verifyModule(module, &diagnostics))
        return std::nullopt;
    return artifact;
}

} // namespace rx
