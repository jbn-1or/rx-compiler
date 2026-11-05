// 从真实 Rx 源码验证入口 IR 生成、未支持功能的拒绝和模块输出。

#include "RxLexer.h"
#include "RxParser.h"
#include "ast_builder.hpp"
#include "ir/artifact.hpp"
#include "ir/generator.hpp"
#include "sema/sema.hpp"
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::shared_ptr<rx::Crate> parse_crate(const std::string &source) {
    antlr4::ANTLRInputStream input(source);
    RxLexer lexer(&input);
    antlr4::CommonTokenStream tokens(&lexer);
    RxParser parser(&tokens);
    auto *tree = parser.crate();
    check(lexer.getNumberOfSyntaxErrors() == 0 &&
              parser.getNumberOfSyntaxErrors() == 0,
          "测试源码必须通过解析");
    rx::Diagnostics diagnostics;
    rx::AstBuilder builder(diagnostics);
    auto crate = builder.build(tree);
    check(crate && diagnostics.ok(), "测试源码必须生成 AST");
    return crate;
}

rx::SemanticResult analyze(const rx::Crate &crate) {
    rx::Diagnostics diagnostics;
    auto result = rx::SemanticAnalyzer(diagnostics).analyze(crate);
    for (const auto &diagnostic : diagnostics.items)
        std::cerr << diagnostic.message << '\n';
    check(result && diagnostics.ok(), "测试源码必须通过语义分析");
    return std::move(*result);
}

void check_entry(const std::string &source) {
    const auto crate = parse_crate(source);
    const auto sema = analyze(*crate);
    check(sema.main_function.has_value(), "语义结果必须包含入口 ID");
    const auto id = *sema.main_function;
    check(id.index < sema.functions.size() &&
              sema.functions.at(id.index)->id == id &&
              sema.functions.at(id.index)->name == "main" &&
              sema.function_declarations.at(sema.functions.at(id.index)->node) == id,
          "入口 ID 必须对应真实的函数声明");

    std::string errors;
    llvm::raw_string_ostream diagnostics(errors);
    auto artifact = rx::generate_ir(*crate, sema, diagnostics);
    diagnostics.flush();
    check(artifact.has_value() && errors.empty(), "空入口应成功生成 IR 且没有诊断");
    auto &module = artifact->module();
    check(module.getTargetTriple().str() == "riscv32-unknown-unknown-elf" &&
              module.getDataLayoutStr() == "e-m:e-p:32:32-i64:64-n32-S128",
          "模块目标和数据布局必须符合 RV32");
    const auto *source_main = module.getFunction("__rx_fn_" + std::to_string(id.index));
    const auto *entry = module.getFunction("main");
    check(source_main && entry && source_main != entry,
          "模块必须包含真实 ID 对应的源入口和机器入口");
    check(source_main->hasInternalLinkage() && source_main->getReturnType()->isVoidTy() &&
              source_main->arg_empty() && !source_main->isVarArg() &&
              source_main->size() == 1,
          "源入口必须是有函数体的 internal void() 函数");
    const auto &source_block = source_main->getEntryBlock();
    check(source_block.size() == 1 &&
              llvm::isa<llvm::ReturnInst>(source_block.front()) &&
              llvm::cast<llvm::ReturnInst>(source_block.front()).getReturnValue() == nullptr,
          "空源入口必须只返回 void");
    check(entry->hasExternalLinkage() && entry->getReturnType()->isIntegerTy(32) &&
              entry->arg_empty() && !entry->isVarArg() && entry->size() == 1,
          "机器入口必须是有函数体的 external i32() 函数");
    const auto &block = entry->getEntryBlock();
    check(block.getName() == "entry" && block.size() == 2,
          "机器入口块必须恰有调用和返回两条指令");
    auto instruction = block.begin();
    const auto *call = llvm::dyn_cast<llvm::CallInst>(&*instruction++);
    const auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&*instruction);
    check(call && call->getCalledFunction() == source_main && call->arg_empty(),
          "机器入口必须先调用源入口且不传参数");
    const auto *value = ret ? llvm::dyn_cast_or_null<llvm::ConstantInt>(ret->getReturnValue()) : nullptr;
    check(value && value->getType()->isIntegerTy(32) && value->isZero(),
          "机器入口必须返回 i32 0");
    check(!llvm::verifyModule(module, &diagnostics), "生成模块必须通过 LLVM verifier");
    std::string output;
    llvm::raw_string_ostream printed(output);
    check(artifact->verify_and_print(printed, diagnostics), "生成模块必须能够校验并打印");
    printed.flush();
    diagnostics.flush();
    check(errors.empty() && !output.empty(), "校验输出应包含 IR 且没有诊断");
}

void check_rejected(const std::string &source) {
    const auto crate = parse_crate(source);
    const auto sema = analyze(*crate);
    std::string errors;
    llvm::raw_string_ostream diagnostics(errors);
    auto artifact = rx::generate_ir(*crate, sema, diagnostics);
    diagnostics.flush();
    check(!artifact && errors.find("尚未支持") != std::string::npos,
          "不支持的有效程序必须拒绝生成模块并说明尚未支持");
}

void check_function_names() {
    const auto crate = parse_crate(R"(
        struct Data {}
        fn __rx_fn_3() {}
        impl Data {
            fn __rx_fn_3(&self) {}
            fn make() -> Self { Self {} }
        }
        fn main() {}
    )");
    auto sema = analyze(*crate);
    std::string errors;
    llvm::raw_string_ostream diagnostics(errors);
    auto names = rx::assign_function_names(sema, diagnostics);
    check(names && names->size() == 4 && errors.empty(),
          "所有普通函数、方法和关联函数都应获得内部名");
    check(sema.main_function->index == 3 && names->at(3) == "__rx_fn_3",
          "main 不应被假定为 FunctionId 0，内部名也不取用户拼写");
    std::set<std::string> unique;
    for (const auto &function : sema.functions) {
        const auto &name = names->at(function->id.index);
        check(name == "__rx_fn_" + std::to_string(function->id.index) &&
                  unique.insert(name).second && name != "main" &&
                  name != "println_i32",
              "内部名必须按 FunctionId 分配且与入口和运行时符号分开");
    }
    check(rx::assign_function_names(sema, diagnostics) == names,
          "重复分配应产生稳定的函数命名");
    check(!rx::generate_ir(*crate, sema, diagnostics),
          "命名完成不代表已支持其他函数体，不得静默丢弃");
    errors.clear();
    sema.functions[0]->id = rx::FunctionId{1};
    check(!rx::assign_function_names(sema, diagnostics) &&
              errors.find("内部错误") != std::string::npos,
          "损坏的函数身份不得产生冲突命名");
}

void run_tests() {
    check_function_names();
    check_entry("fn main() {}");
    check_entry(R"(
        const LEN: usize = 2;
        struct Data { values: [i32; LEN] }
        impl Data { const VALUE: i32 = 7; }
        fn main() {}
    )");
    const auto invalid = parse_crate("struct Data {}");
    rx::Diagnostics diagnostics;
    const auto result = rx::SemanticAnalyzer(diagnostics).analyze(*invalid);
    check(!result && !diagnostics.ok(), "缺少 main 必须在语义分析时失败");
    check_rejected("fn main() { println_i32(1); }");
    check_rejected("fn main() { main(); }");
    check_rejected("fn main() {} fn unused() {}");
    check_rejected("struct Data {} impl Data { fn unused(&self) {} } fn main() {}");

    const auto first = parse_crate("fn main() {}");
    const auto second = parse_crate("fn main() {}");
    const auto sema = analyze(*first);
    std::string errors;
    llvm::raw_string_ostream output(errors);
    const auto wrong_crate = rx::generate_ir(*second, sema, output);
    output.flush();
    check(!wrong_crate && errors.find("内部错误") != std::string::npos,
          "不得使用另一个语法树的语义结果生成入口");
}

int emit() {
    const auto crate = parse_crate("fn main() {}");
    const auto sema = analyze(*crate);
    auto artifact = rx::generate_ir(*crate, sema, llvm::errs());
    if (!artifact)
        return 1;
    return artifact->verify_and_print(llvm::outs(), llvm::errs()) ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc == 1) {
            run_tests();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--emit")
            return emit();
        std::cerr << "用法: ir_entry_test [--emit]\n";
        return 2;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
