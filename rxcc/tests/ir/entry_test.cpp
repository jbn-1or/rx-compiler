// P2：真实源码的空函数、双入口、命名与失败边界。
#include "RxLexer.h"
#include "RxParser.h"
#include "ast_builder.hpp"
#include "ir/generator.hpp"
#include "sema/sema.hpp"
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace rx::ir;
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



rx::IRArtifact generate(const rx::Crate &crate, const rx::SemanticResult &sema) {
    std::ostringstream diagnostics;
    auto artifact = rx::generate_ir(crate, sema, diagnostics);
    check(artifact && diagnostics.str().empty(), "P2 有效输入必须成功生成模块");
    return std::move(*artifact);
}

void check_entry(const std::string &source) {
    const auto crate = parse_crate(source);
    const auto sema = analyze(*crate);
    const auto artifact = generate(*crate, sema);
    const auto &module = artifact.module();
    const auto *main = module.find_function("__rx_fn_" + std::to_string(sema.main_function->index));
    const auto *entry = module.find_function("main");
    check(module.target_triple == rv32_triple && module.data_layout == rv32_data_layout,
          "目标必须是 RV32IM/ILP32");
    check(main && main->linkage == Linkage::Internal && main->signature == Signature{Type::Void, {}} &&
              main->blocks.size() == 1 && main->blocks[0].instructions.empty() &&
              !std::get<Return>(*main->blocks[0].terminator).value, "源入口应为空的 internal void()");
    check(entry && entry != main && entry->linkage == Linkage::External &&
              entry->signature == Signature{Type::I32, {}} && entry->blocks.size() == 1,
          "机器入口应为独立的 external i32()");
    const auto &block = entry->blocks[0];
    check(block.instructions.size() == 1 && !block.instructions[0].result &&
              std::get<Call>(block.instructions[0].operation).function == main->id &&
              std::get<Return>(*block.terminator).value == Operand::constant(Type::I32, 0),
          "机器入口只调用源入口并返回 0");
    check(module.functions().size() == sema.functions.size() + 1, "空辅助函数不能被静默丢弃");
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics) && !text.str().empty() && diagnostics.str().empty(),
          "空函数模块必须通过自检并输出文本");
}

void check_rejected(const std::string &source) {
    const auto crate = parse_crate(source);
    const auto sema = analyze(*crate);
    std::ostringstream diagnostics;
    check(!rx::generate_ir(*crate, sema, diagnostics) && diagnostics.str().find("尚未支持") != std::string::npos,
          "P3 及后续功能必须明确拒绝，不能生成空程序冒充成功");
}

void check_function_names() {
    const auto crate = parse_crate(R"(
        struct Data {}
        fn __rx_fn_3() {}
        impl Data { fn method(&self) {} fn make() -> Self { Self {} } }
        fn main() {}
    )");
    auto sema = analyze(*crate);
    std::ostringstream diagnostics;
    const auto names = rx::assign_function_names(sema, diagnostics);
    check(names && names->size() == 4 && sema.main_function->index == 3 && names->at(3) == "__rx_fn_3",
          "名字根据稳定身份分配，不能假定 main 的编号或使用用户拼写");
    std::set<std::string> unique;
    for (const auto &name : *names)
        check(unique.insert(name).second && name != "main" && name != "println_i32", "内部名字必须唯一");
    check(rx::assign_function_names(sema, diagnostics) == names, "分配名称必须确定");
    check(!rx::generate_ir(*crate, sema, diagnostics), "方法命名不表示方法 lowering 已实现");
    sema.functions[0]->id = rx::FunctionId{1};
    check(!rx::assign_function_names(sema, diagnostics), "损坏的源函数身份必须拒绝");
}

void run_tests() {
    check_function_names();
    check_entry("fn main() {}");
    check_entry("fn main() -> () {}");
    check_entry("fn __rx_fn_1() {} fn main() {} fn helper() {}");
    check_entry("const LEN: usize = 2; struct Data { values: [i32; LEN] } impl Data { const VALUE: i32 = 7; } fn main() {}");
    for (const auto &source : {
             "fn main() { let x = 1; }",
             "fn main() { println_i32(42); }",
             "fn main() { main(); }",
             "fn main() { return; }",
             "fn main() { (); }",
             "fn main() { true; }",
             "fn main() { 1 + 2; }",
             "fn main() { { } }",
             "fn main() { let x = return; }",
             "fn main() {} fn unused() { println_i32(99); }",
             "fn main() {} fn unused(value: i32) {}",
             "fn main() {} fn unused() -> i32 { 7 }",
             "struct Data {} impl Data { fn unused(&self) {} } fn main() {}"})
        check_rejected(source);

    const auto first = parse_crate("fn main() {}");
    const auto second = parse_crate("fn main() {}");
    auto sema = analyze(*first);
    std::ostringstream errors;
    check(!rx::generate_ir(*second, sema, errors) && errors.str().find("内部错误") != std::string::npos,
          "不能使用其他 AST 的语义结果");
    sema.main_function.reset();
    check(!rx::generate_ir(*first, sema, errors), "缺少 main 身份必须拒绝");

    std::optional<rx::IRArtifact> independent;
    std::ostringstream before, after;
    {
        const auto crate = parse_crate("fn main() {}");
        const auto result = analyze(*crate);
        independent.emplace(generate(*crate, result));
        check(independent->verify_and_print(before, errors), "模块应可输出");
    }
    check(independent->verify_and_print(after, errors) && before.str() == after.str(),
          "完成模块不依赖 AST/sema 生存期");
}
}

int main(int argc, char **argv) {
    try {
        if (argc == 1) {
            run_tests();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--emit") {
            const auto crate = parse_crate("fn main() {}");
            const auto sema = analyze(*crate);
            const auto artifact = generate(*crate, sema);
            return artifact.verify_and_print(std::cout, std::cerr) ? 0 : 1;
        }
        return 2;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
