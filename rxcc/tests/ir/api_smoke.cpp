// 独立验证 LLVM API、模块生命周期和输出检查，不从 Rx 源码生成 IR。

#include "ir/artifact.hpp"
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

// 只允许在尚未结束的块中插入指令，不把 builder 的插入位置视为保证。
void check_open_block(const llvm::IRBuilder<> &builder) {
    const auto *block = builder.GetInsertBlock();
    check(block && !block->getTerminator(), "当前基本块不能继续插入指令");
}

rx::IRArtifact make_module(bool invalid) {
    rx::IRArtifact artifact("rx-api-smoke");
    auto &context = artifact.context();
    auto &module = artifact.module();
    llvm::IRBuilder<> builder(context);

    auto *source_type = llvm::FunctionType::get(builder.getVoidTy(), false);
    auto *source = llvm::Function::Create(
        source_type, llvm::Function::InternalLinkage, "__rx_fn_0", module);
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", source));
    if (!invalid) {
        check_open_block(builder);
        builder.CreateRetVoid();
        builder.ClearInsertionPoint();
    }

    auto *entry_type = llvm::FunctionType::get(builder.getInt32Ty(), false);
    auto *entry = llvm::Function::Create(
        entry_type, llvm::Function::ExternalLinkage, "main", module);
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", entry));
    check_open_block(builder);
    builder.CreateCall(source);
    check_open_block(builder);
    builder.CreateRet(builder.getInt32(0));
    builder.ClearInsertionPoint();
    return artifact;
}

void run_tests() {
    static_assert(std::is_nothrow_move_constructible_v<rx::IRArtifact>);
    static_assert(!std::is_move_assignable_v<rx::IRArtifact>);
    auto artifact = make_module(false);
    const auto *context = &artifact.context();
    const auto *module = &artifact.module();
    rx::IRArtifact moved(std::move(artifact));
    check(&moved.context() == context && &moved.module() == module,
          "移动后 context 和 module 的地址应保持不变");
    check(moved.module().getTargetTriple().str() == "riscv32-unknown-unknown-elf" &&
              moved.module().getDataLayoutStr() == "e-m:e-p:32:32-i64:64-n32-S128",
          "目标和数据布局应与 RV32 一致");
    std::string text, errors;
    llvm::raw_string_ostream output(text), diagnostics(errors);
    check(moved.verify_and_print(output, diagnostics), "合法模块应通过检查");
    output.flush();
    diagnostics.flush();
    check(errors.empty() && text.find("define i32 @main()") != std::string::npos,
          "合法模块应输出入口且没有检查错误");

    auto invalid = make_module(true);
    std::string bad_text, bad_errors;
    llvm::raw_string_ostream bad_output(bad_text), bad_diagnostics(bad_errors);
    check(!invalid.verify_and_print(bad_output, bad_diagnostics),
          "缺少结束指令的模块必须被拒绝");
    bad_output.flush();
    bad_diagnostics.flush();
    check(bad_text.empty() && !bad_errors.empty(), "非法模块不得输出 IR，且应有诊断");

    llvm::IRBuilder<> builder(moved.context());
    bool rejected = false;
    try {
        check_open_block(builder);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    check(rejected, "没有插入位置时不得发出指令");
    builder.SetInsertPoint(&moved.module().getFunction("main")->getEntryBlock());
    rejected = false;
    try {
        check_open_block(builder);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    check(rejected, "已结束的基本块不得继续插入指令");
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--test") {
        run_tests();
        return 0;
    }
    if (argc > 2 || (argc == 2 && std::string_view(argv[1]) != "--invalid"))
        return 2;
    auto artifact = make_module(argc == 2);
    return artifact.verify_and_print(llvm::outs(), llvm::errs()) ? 0 : 1;
}
