// P2/P3：真实源码的入口、整数打印、局部存储与失败边界。
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



rx::IRArtifact generate(const rx::SemanticResult &sema) {
    std::ostringstream diagnostics;
    auto artifact = rx::generate_ir(sema, diagnostics);
    if (!artifact || !diagnostics.str().empty())
        throw std::runtime_error("已支持的有效输入必须成功生成模块: " + diagnostics.str());
    return std::move(*artifact);
}

void check_entry(const std::string &source) {
    const auto crate = parse_crate(source);
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    const auto &module = artifact.module();
    const auto *main = module.find_function("__rx_fn_" + std::to_string(sema.main_function->index));
    const auto *entry = module.find_function("main");
    check(module.target_triple == rv32_triple && module.data_layout == rv32_data_layout,
          "目标必须是 RV32IM/ILP32");
    check(main && main->linkage == Linkage::Internal && main->signature == Signature{Type::Void, {}} &&
              main->blocks.size() == 1 && main->blocks[0].instructions.empty() &&
              !main->blocks[0].terminator->value, "源入口应为空的 internal void()");
    check(entry && entry != main && entry->linkage == Linkage::External &&
              entry->signature == Signature{Type::I32, {}} && entry->blocks.size() == 1,
          "机器入口应为独立的 external i32()");
    const auto &block = entry->blocks[0];
    check(block.instructions.size() == 1 && !block.instructions[0].result &&
              std::get<Call>(block.instructions[0].operation).function == main->id &&
              block.terminator->value == Operand::constant(Type::I32, 0),
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
    const auto artifact = rx::generate_ir(sema, diagnostics);
    if (artifact || diagnostics.str().find("尚未支持") == std::string::npos)
        throw std::runtime_error("本应拒绝的输入未被拒绝：" + source + "；诊断：" + diagnostics.str());
}

void check_semantic_rejected(const std::string &source) {
    const auto crate = parse_crate(source);
    rx::Diagnostics diagnostics;
    const auto result = rx::SemanticAnalyzer(diagnostics).analyze(*crate);
    check(!result || !diagnostics.ok(), "非法源码必须由语义分析拒绝");
}

void check_lowering_rejected(const std::string &source) {
    const auto crate = parse_crate(source);
    const auto sema = analyze(*crate);
    std::ostringstream diagnostics;
    check(!rx::generate_ir(sema, diagnostics) &&
              diagnostics.str().find("尚未支持") != std::string::npos,
          "有效源码的缺失能力必须报告 lowering 尚未支持");
}

void check_function_names() {
    const auto crate = parse_crate(R"(
        struct Data {}
        fn __rx_fn_3() {}
        impl Data { fn method(&self) {} fn make() -> Self { Self {} } }
        fn main() {}
    )");
    const auto sema = analyze(*crate);
    std::ostringstream diagnostics;
    const auto names = rx::assign_function_names(sema);
    check(names.size() == 4 && sema.main_function->index == 3 && names.at(3) == "__rx_fn_3",
          "名字根据稳定身份分配，不能假定 main 的编号或使用用户拼写");
    std::set<std::string> unique;
    for (const auto &name : names)
        check(unique.insert(name).second && name != "main" && name != "println_i32", "内部名字必须唯一");
    check(rx::assign_function_names(sema) == names, "分配名称必须确定");
    check(!rx::generate_ir(sema, diagnostics), "方法命名不表示方法 lowering 已实现");
}

void check_println() {
    const auto crate = parse_crate(
        "fn unused() { println_i32(99); } fn main() { println_i32(42); println_i32(0i32); }");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    const auto &module = artifact.module();
    const auto *println = module.find_function("println_i32");
    check(println && println->is_declaration() && println->linkage == Linkage::External &&
              println->signature == Signature{Type::Void, {Type::I32}} &&
              module.functions().size() == sema.functions.size() + 2,
          "所有调用应共享一个运行时声明");
    const auto *main = module.find_function("__rx_fn_1");
    const auto &instructions = main->blocks[0].instructions;
    check(instructions.size() == 2 && !instructions[0].result && !instructions[1].result &&
              std::get<Call>(instructions[0].operation).function == println->id &&
              std::get<Call>(instructions[0].operation).arguments ==
                  std::vector<Operand>{Operand::constant(Type::I32, 42)} &&
              std::get<Call>(instructions[1].operation).arguments ==
                  std::vector<Operand>{Operand::constant(Type::I32, 0)},
          "打印必须保留语句顺序、常量和 void call 的无结果语义");
    check(module.find_function("__rx_fn_0")->blocks[0].instructions.size() == 1,
          "未调用函数的打印也必须生成");
}

void check_locals() {
    const auto crate = parse_crate(
        "const N: i32 = 42; fn main() { let x = N; println_i32(x); "
        "let x = x; println_i32(x); println_i32(x); }");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics) && diagnostics.str().empty(),
          "局部变量模块必须通过自检");
    const auto ir = text.str();
    const auto first_alloca = ir.find("alloca i32");
    const auto second_alloca = ir.find("alloca i32", first_alloca + 1);
    const auto first_store = ir.find("store i32");
    const auto first_load = ir.find("load i32", first_store);
    const auto first_call = ir.find("call void @println_i32", first_load);
    const auto init_load = ir.find("load i32", first_call);
    const auto second_store = ir.find("store i32", init_load);
    const auto second_load = ir.find("load i32", second_store);
    const auto second_call = ir.find("call void @println_i32", second_load);
    const auto third_load = ir.find("load i32", second_call);
    const auto third_call = ir.find("call void @println_i32", third_load);
    check(first_alloca != std::string::npos && second_alloca != std::string::npos &&
              first_store != std::string::npos && first_load != std::string::npos &&
              first_call != std::string::npos && init_load != std::string::npos &&
              second_store != std::string::npos && second_load != std::string::npos &&
              second_call != std::string::npos && third_load != std::string::npos &&
              third_call != std::string::npos && second_alloca < first_store,
          "遮蔽需要两个入口 slot；初始化与每次读取必须按源码顺序执行");
}

// 使用真实语义结果覆盖有符号、无符号和 bool 常量，不人为破坏语义记录。
void check_scalar_constants() {
    const auto crate = parse_crate(R"(
        const MIN: i32 = -2147483648;
        const MAX: u32 = 4294967295u32;
        const INDEX: usize = 4294967295usize;
        const OFFSET: isize = -1isize;
        const NO: bool = false;
        fn signed() -> i32 { MIN }
        fn unsigned() -> u32 { MAX }
        fn index() -> usize { INDEX }
        fn offset() -> isize { OFFSET }
        fn flag() -> bool { NO }
        fn literal() -> u32 { 4294967295u32 }
        fn main() {}
    )");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    const std::vector<Operand> expected{
        Operand::constant(Type::I32, 0x80000000u),
        Operand::constant(Type::I32, 0xffffffffu),
        Operand::constant(Type::I32, 0xffffffffu),
        Operand::constant(Type::I32, 0xffffffffu),
        Operand::constant(Type::I1, 0),
        Operand::constant(Type::I32, 0xffffffffu)};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto *function = artifact.module().find_function(
            "__rx_fn_" + std::to_string(index));
        check(function && function->blocks[0].instructions.empty() &&
                  function->blocks[0].terminator->value == expected[index],
              "常量和字面量必须保留语义阶段确定的类型与 32 位位模式");
    }
}

void check_unit_calls_and_blocks() {
    const auto crate = parse_crate(
        "fn main() { ping(); { ping(); (); } println_i32(8); } "
        "fn ping() { println_i32(7); }");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    const auto *main = artifact.module().find_function(
        "__rx_fn_" + std::to_string(sema.main_function->index));
    const auto *ping = artifact.module().find_function("__rx_fn_1");
    check(main && ping && main->blocks.size() == 1 &&
              main->blocks[0].instructions.size() == 3 &&
              !main->blocks[0].instructions[0].result &&
              std::get<Call>(main->blocks[0].instructions[0].operation).function == ping->id &&
              !main->blocks[0].instructions[1].result &&
              std::get<Call>(main->blocks[0].instructions[1].operation).function == ping->id &&
              std::get<Call>(main->blocks[0].instructions[2].operation).function ==
                  artifact.module().find_function("println_i32")->id &&
              std::get<Call>(main->blocks[0].instructions[2].operation).arguments ==
                  std::vector<Operand>{Operand::constant(Type::I32, 8)},
          "前向 unit 调用及嵌套块必须顺序生成，unit 不产生 SSA 结果");

    // 源 main 允许作为普通 unit 函数被调用；此用例只检查符号连接，不执行递归。
    const auto recursive = parse_crate("fn main() { main(); }");
    const auto recursive_sema = analyze(*recursive);
    const auto recursive_ir = generate(recursive_sema);
    const auto *source_main = recursive_ir.module().find_function(
        "__rx_fn_" + std::to_string(recursive_sema.main_function->index));
    check(source_main && source_main->blocks[0].instructions.size() == 1 &&
              std::get<Call>(source_main->blocks[0].instructions[0].operation).function ==
                  source_main->id,
          "源 main 调用必须指向内部源函数而非机器入口");
}

void check_integer_calls_and_returns() {
    const auto crate = parse_crate(R"(
        fn main() { println_i32(echo(7)); println_i32(choose(mark(1), mark(2), 7));
                    println_i32(nine(1, 2, 3, 4, 5, 6, 7, 8, 9)); println_i32(nested()); }
        fn echo(value: i32) -> i32 { value }
        fn mark(value: i32) -> i32 { println_i32(value); value }
        fn choose(a: i32, b: i32, result: i32) -> i32 { result }
        fn nine(a: i32, b: i32, c: i32, d: i32, e: i32,
                f: i32, g: i32, h: i32, i: i32) -> i32 { i }
        fn nested() -> i32 { { let value = 9; value } }
    )");
    const auto sema = analyze(*crate);
    check_semantic_rejected("fn main() { println_i32(true); }");
    check_lowering_rejected("fn main() { let x: bool = 1 == 1; }");
    const auto artifact = generate(sema);
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics) && diagnostics.str().empty(),
          "整数参数和返回模块必须通过 IR 自检");
    const auto ir = text.str();
    check(ir.find("define internal i32 @__rx_fn_1(i32") != std::string::npos &&
              ir.find("alloca i32, align 4") != std::string::npos &&
              ir.find("store i32 %v_0, ptr") != std::string::npos &&
              ir.find("ret i32") != std::string::npos,
          "i32 形参须有独立 slot，整数尾值须返回 i32");
    check(ir.find("call i32 @__rx_fn_2(i32 1)") <
                  ir.find("call i32 @__rx_fn_2(i32 2)") &&
              ir.find("call i32 @__rx_fn_2(i32 2)") <
                  ir.find("call i32 @__rx_fn_3(i32") &&
              ir.find("call i32 @__rx_fn_4(i32 1, i32 2, i32 3, i32 4, i32 5, "
                      "i32 6, i32 7, i32 8, i32 9)") != std::string::npos &&
              ir.find("define internal i32 @__rx_fn_4(i32") != std::string::npos &&
              ir.find("define internal i32 @__rx_fn_5()") != std::string::npos,
          "调用实参必须按从左至右求值，且支持九参数、前向调用和整数块尾");
    check(ir.find("call i32 @__rx_fn_1(i32 7)") != std::string::npos &&
              ir.find("call void @println_i32(i32") != std::string::npos,
          "调用结果应可传给下一次调用并继续执行");

}

void check_early_return() {
    const auto crate = parse_crate(R"(
        fn value() -> i32 { return 7; println_i32(99); 99 }
        fn stop() { return; println_i32(99); }
        fn pair(a: i32, b: i32) -> i32 { a }
        fn mark(value: i32) -> i32 { println_i32(value); value }
        fn main() { println_i32(value()); stop(); println_i32(pair(mark(1), return)); }
    )");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    const auto &module = artifact.module();
    const auto *value = module.find_function("__rx_fn_0");
    const auto *stop = module.find_function("__rx_fn_1");
    const auto *pair = module.find_function("__rx_fn_2");
    const auto *mark = module.find_function("__rx_fn_3");
    // return 7 已写出 ret，后面的 println_i32(99) 与尾值 99 都不生成。
    check(value && value->blocks.size() == 1 &&
              value->blocks[0].instructions.empty() &&
              value->blocks[0].terminator &&
              value->blocks[0].terminator->value ==
                  Operand::constant(Type::I32, 7),
          "整数 return 后必须直接 ret，且不得继续生成语句或尾值");
    // 裸 return 直接结束 unit 函数，不再补任何正常返回。
    check(stop && stop->blocks.size() == 1 &&
              stop->blocks[0].instructions.empty() &&
              stop->blocks[0].terminator &&
              !stop->blocks[0].terminator->value,
          "裸 return 必须生成唯一的 ret void");
    // main 的第二个实参提前 return：先执行 mark(1)，随后不能调用 pair 或外层 println。
    const auto *main = module.find_function(
        "__rx_fn_" + std::to_string(sema.main_function->index));
    const auto &main_instructions = main->blocks[0].instructions;
    check(main && main->blocks.size() == 1 && main_instructions.size() == 4 &&
              std::get<Call>(main_instructions[0].operation).function ==
                  value->id &&
              std::get<Call>(main_instructions[1].operation).function ==
                  module.find_function("println_i32")->id &&
              std::get<Call>(main_instructions[2].operation).function ==
                  stop->id &&
              std::get<Call>(main_instructions[3].operation).function ==
                  mark->id &&
              main->blocks[0].terminator &&
              !main->blocks[0].terminator->value,
          "实参提前 return 后必须跳过后续实参、pair 调用和 println");
    check(pair && pair->blocks.size() == 1 && pair->blocks[0].terminator,
          "未被实际执行的函数仍要完整生成");

    // never 初始化器不分配 slot，也不执行后续打印。
    const auto never_crate =
        parse_crate("fn main() { let x = return; println_i32(99); }");
    const auto never_sema = analyze(*never_crate);
    const auto never_artifact = generate(never_sema);
    std::ostringstream never_text, never_diagnostics;
    check(never_artifact.verify_and_print(never_text, never_diagnostics) &&
              never_diagnostics.str().empty() &&
              never_text.str().find("alloca") == std::string::npos &&
              never_text.str().find("println_i32") == std::string::npos,
          "never let 不得分配存储或生成后续副作用");
    const auto *never_main = never_artifact.module().find_function("__rx_fn_0");
    check(never_main && never_main->blocks[0].terminator &&
              !never_main->blocks[0].terminator->value,
          "never let 所在函数应直接 ret void");

    // 嵌套块与 return 的实参都只结束当前路径，不影响另一个函数。
    const auto nested = parse_crate(
        "fn inner() -> i32 { return { return 3; }; } "
        "fn outer() -> i32 { 4 } fn main() { { return; } "
        "println_i32(inner()); println_i32(outer()); }");
    const auto nested_sema = analyze(*nested);
    const auto nested_artifact = generate(nested_sema);
    const auto *nested_inner =
        nested_artifact.module().find_function("__rx_fn_0");
    const auto *nested_outer =
        nested_artifact.module().find_function("__rx_fn_1");
    const auto *nested_main = nested_artifact.module().find_function(
        "__rx_fn_" + std::to_string(nested_sema.main_function->index));
    check(nested_inner && nested_inner->blocks[0].terminator &&
              nested_inner->blocks[0].terminator->value ==
                  Operand::constant(Type::I32, 3) &&
              nested_outer && nested_outer->blocks[0].terminator &&
              nested_outer->blocks[0].terminator->value ==
                  Operand::constant(Type::I32, 4) &&
              nested_main && nested_main->blocks[0].instructions.empty(),
          "一个函数的提前 return 不得影响其他函数或 main 的后续路径");

    // unit 返回值先执行副作用；标量块在语句中 return 后不再读取尾表达式。
    const auto unit = parse_crate(
        "fn finish() { return { println_i32(1); () }; println_i32(99); } "
        "fn number() -> i32 { { let value: bool = { return 7; }; 99 } } "
        "fn main() { finish(); println_i32(number()); return (); }");
    const auto unit_sema = analyze(*unit);
    const auto unit_artifact = generate(unit_sema);
    const auto *finish = unit_artifact.module().find_function("__rx_fn_0");
    const auto *number = unit_artifact.module().find_function("__rx_fn_1");
    check(finish && finish->blocks[0].instructions.size() == 1 &&
              !finish->blocks[0].terminator->value &&
              number && number->blocks[0].instructions.empty() &&
              number->blocks[0].terminator->value ==
                  Operand::constant(Type::I32, 7),
          "unit return 保留副作用，嵌套 return 不生成尾值或未完成的绑定");
}

void check_bool_boundary() {
    const auto crate = parse_crate(R"(
        const YES: bool = true;
        fn identity(flag: bool) -> bool { let saved = flag; saved }
        fn denied() -> bool { return false; }
        fn main() { let yes = identity(true); let no = identity(false);
                    let copy = YES; let filled = { true }; }
    )");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics) &&
              diagnostics.str().empty(),
          "bool 边界模块必须通过 IR 自检");
    const auto ir = text.str();
    check(ir.find("define internal i1 @__rx_fn_0(i1 %v_0)") !=
                  std::string::npos &&
              ir.find("define internal i1 @__rx_fn_1()") != std::string::npos &&
              ir.find("alloca i8, align 1") != std::string::npos &&
              ir.find("= zext i1 %v_0 to i8") != std::string::npos &&
              ir.find("= trunc i8 %v_") != std::string::npos &&
              ir.find("call i1 @__rx_fn_0(i1 true)") != std::string::npos &&
              ir.find("call i1 @__rx_fn_0(i1 false)") != std::string::npos &&
              ir.find("ret i1 false") != std::string::npos,
          "bool 参数/局部/返回使用 i1，slot 使用 i8，并显式 zext/trunc");
    check(ir.find("alloca i32") == std::string::npos,
          "全部为 bool 的样例不能分配 i32 slot");
    check(ir.find("zext i1 true to i8") != std::string::npos,
          "bool 常量路径必须按 i1 常量处理");
}

// 混合整数和 bool，并在内层用不同类型遮蔽同名变量，检查按 LocalId 读取类型。
void check_mixed_scalar_storage() {
    const auto crate = parse_crate(R"(
        fn mixed(number: i32, flag: bool, count: usize) -> bool { flag }
        fn main() {
            let number = 7;
            let flag = true;
            { let number = false; let flag = 3i32; }
            let copy = number;
            let saved = flag;
            let result = mixed(copy, saved, 2usize);
            println_i32(number);
        }
    )");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    const auto *mixed = artifact.module().find_function("__rx_fn_0");
    check(mixed && mixed->signature ==
              Signature{Type::I1, {Type::I32, Type::I1, Type::I32}},
          "混合形参和 bool 返回值必须保留各自的 IR 类型");
    const auto *main = artifact.module().find_function("__rx_fn_1");
    bool found_mixed_call = false;
    for (const auto &instruction : main->blocks[0].instructions) {
        const auto *call = std::get_if<Call>(&instruction.operation);
        if (call && call->function == mixed->id) {
            found_mixed_call = true;
            check(instruction.type == Type::I1 &&
                      call->arguments[0].type == Type::I32 &&
                      call->arguments[1].type == Type::I1 &&
                      call->arguments[2] == Operand::constant(Type::I32, 2),
                  "退出不同类型的同名遮蔽后，调用仍须读取外层绑定并保留 bool 值");
        }
    }
    check(found_mixed_call, "必须生成混合标量调用");
}

void run_tests() {
    check_function_names();
    check_println();
    check_locals();
    check_scalar_constants();
    check_unit_calls_and_blocks();
    check_integer_calls_and_returns();
    check_early_return();
    check_bool_boundary();
    check_mixed_scalar_storage();
    check_entry("fn main() {}");
    check_entry("fn main() -> () {}");
    check_entry("fn __rx_fn_1() {} fn main() {} fn helper() {}");
    check_entry("const LEN: usize = 2; struct Data { values: [i32; LEN] } impl Data { const VALUE: i32 = 7; } fn main() {}");
    for (const auto &source : {
             "fn main() { let x = 1 + 2; }",
             "fn main() { let mut x = 1; x = 2; }",
             "fn main() { Box::<i32>::new(1); }",
             "fn main() { Vec::<i32>::new(); }",
             "fn main() { 1 + 2; }",
             "fn main() { let x = 1 == 1; }",
             "fn main() { let x = { (); }; }",
             "fn main() { let x = { [1, 2] }; }",
             "fn main() { let x = loop {}; }",
             "fn unused(value: &i32) {} fn main() {}",
             "fn unused() -> [i32; 1] { [1] } fn main() {}",
             "fn main() {} fn unused() { println_i32(99); let x = [1, 2]; }",
             "struct Data {} impl Data { fn unused(&self) {} } fn main() {}"})
        check_rejected(source);

    std::ostringstream errors;
    std::optional<rx::IRArtifact> independent;
    std::ostringstream before, after;
    {
        const auto crate = parse_crate("fn main() {}");
        const auto result = analyze(*crate);
        independent.emplace(generate(result));
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
            const auto artifact = generate(sema);
            return artifact.verify_and_print(std::cout, std::cerr) ? 0 : 1;
        }
        return 2;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
