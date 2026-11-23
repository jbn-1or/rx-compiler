// P2–P6.8：真实源码的入口、标量、引用、赋值与控制流的 IR 结构。
#include "RxLexer.h"
#include "RxParser.h"
#include "ast_builder.hpp"
#include "ir/builder.hpp"
#include "ir/generator.hpp"
#include "sema/sema.hpp"
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

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

// 前向字段、聚合尾填充和通过容器形成的递归都使用模块类型表。
void check_named_storage_layouts() {
    const auto crate = parse_crate(R"(
        struct Outer { first: bool, pair: [Inner; 2], inner: Inner, tail: bool }
        struct Inner { flag: bool, value: i32, end: bool }
        struct Node { children: Vec<Node>, next: Box<Node> }
        fn main() {}
    )");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    const auto &module = artifact.module();
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics),
          "前向结构体和容器递归必须通过模块校验");
    LayoutService layouts(module);
    check(layouts.layout(StorageType::struct_of(0)) == Layout{44, 4, {0, 4, 28, 40}} &&
              layouts.layout(StorageType::struct_of(1)) == Layout{12, 4, {0, 4, 8}},
          "前向字段和数组应使用完整子布局，包括结构体尾填充");
    check(layouts.layout(StorageType::struct_of(2)) == Layout{16, 4, {0, 12}},
          "Vec 和 Box 存储不得展开递归元素");
    check(text.str().find("%__rx_struct_0 = type { i8, [2 x %__rx_struct_1], %__rx_struct_1, i8 }") !=
              std::string::npos,
          "LLVM 文本应按声明顺序保留前向命名类型和 bool 存储宽度");
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

// P4.1：按源类型选择有符号/无符号运算和比较，bool 使用 i1 的位运算。
void check_scalar_operations() {
    const auto crate = parse_crate(R"(
        fn main() {
            let max = 2147483647i32;
            let min = -2147483648i32;
            println_i32(max + 1);
            println_i32(min - 1);
            println_i32(65536i32 * 65536i32);
            println_i32(-min);
            println_i32(-7i32 / 3i32);
            println_i32(-7i32 % 3i32);
            println_i32(6i32 & 3i32);
            println_i32(6i32 | 3i32);
            println_i32(6i32 ^ 3i32);
            println_i32(!0i32);
            let u = 4294967295u32;
            let two = 2u32;
            let q = u / two;
            let r = u % two;
            let lt = u < two;
            let bt = false < true;
            let same = bt == false;
            let notb = !bt;
            let andb = bt & same;
        }
    )");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics) &&
              diagnostics.str().empty(),
          "标量运算模块必须通过 IR 自检");
    const auto ir = text.str();
    check(ir.find("add i32") != std::string::npos &&
              ir.find("sub i32 0, -2147483648") != std::string::npos &&
              ir.find("mul i32 65536, 65536") != std::string::npos &&
              ir.find("sdiv i32") != std::string::npos &&
              ir.find("srem i32") != std::string::npos &&
              ir.find("udiv i32") != std::string::npos &&
              ir.find("urem i32") != std::string::npos &&
              ir.find("icmp ult i32") != std::string::npos &&
              ir.find("icmp ult i1 false, true") != std::string::npos &&
              ir.find("and i1") != std::string::npos &&
              ir.find("xor i1") != std::string::npos &&
              ir.find("xor i32 0, -1") != std::string::npos,
          "有符号除法/余数、无符号高位比较和 bool 位运算必须选择各自指令");
    check(ir.find(" nsw ") == std::string::npos &&
              ir.find(" nuw ") == std::string::npos &&
              ir.find(" exact") == std::string::npos &&
              ir.find("noalias") == std::string::npos &&
              ir.find("inbounds") == std::string::npos &&
              ir.find("nonnull") == std::string::npos,
          "P4.3 baseline 不能附加不溢出、精确性或别名承诺");
    check(ir.find("icmp slt i1") == std::string::npos &&
              ir.find("icmp sgt i1") == std::string::npos,
          "bool 排序不能使用有符号谓词");

    // 二元运算必须先求值左操作数，再求值右操作数。
    const auto ordered = parse_crate(
        "fn mark(tag: i32, value: i32) -> i32 { println_i32(tag); value } "
        "fn main() { println_i32(mark(1, 40) + mark(2, 2)); }");
    const auto ordered_sema = analyze(*ordered);
    const auto ordered_artifact = generate(ordered_sema);
    std::ostringstream ordered_text, ordered_diagnostics;
    check(ordered_artifact.verify_and_print(ordered_text,
                                            ordered_diagnostics) &&
              ordered_diagnostics.str().empty(),
          "带副作用的二元运算模块必须通过 IR 自检");
    const auto ordered_ir = ordered_text.str();
    const auto left_mark = ordered_ir.find("i32 1, i32 40)");
    const auto right_mark = ordered_ir.find("i32 2, i32 2)");
    const auto addition = ordered_ir.find("add i32");
    check(left_mark != std::string::npos && right_mark != std::string::npos &&
              addition != std::string::npos && left_mark < right_mark &&
              right_mark < addition,
          "普通二元运算必须先求值左操作数，再求值右操作数");
}

// 用真实源码生成各类标量运算，再用手工 IR 驱动打印结果；不依赖源码 cast。
rx::IRArtifact make_scalar_operation_driver() {
    using Arguments = std::vector<std::pair<uint32_t, uint32_t>>;
    std::vector<Arguments> arguments;
    std::ostringstream source;
    // 每个函数使用参数而非直接折叠字面量；参数组与函数按相同顺序保存。
    const auto add_function = [&](const std::string &type,
                                  const std::string &expression,
                                  bool comparison, const Arguments &inputs) {
        source << "fn operation_" << arguments.size() << "(a: " << type
               << ", b: " << type << ") -> "
               << (comparison ? "bool" : type) << " { " << expression << " }\n";
        arguments.push_back(inputs);
    };
    for (const std::string type : {"i32", "u32", "isize", "usize", "bool"}) {
        const bool boolean = type == "bool";
        const bool signed_integer = type == "i32" || type == "isize";
        // 整数覆盖小于、相等、大于；高位为 1 时，有符号和无符号解释相反。
        const Arguments comparisons = boolean
            ? Arguments{{0, 0}, {0, 1}, {1, 0}, {1, 1}}
            : Arguments{{0xffffffffu, 0}, {1, 1}, {0, 0xffffffffu}};
        for (const std::string operation : {"==", "!=", "<", "<=", ">", ">="})
            add_function(type, "a " + operation + " b", true, comparisons);
        if (boolean) {
            for (const std::string operation : {"&", "|", "^"})
                add_function(type, "a " + operation + " b", false, comparisons);
            add_function(type, "!a", false, comparisons);
            continue;
        }
        // 除数非零，且不含 MIN/-1；加减乘覆盖正负边界和 32 位回绕。
        const Arguments arithmetic = signed_integer
            ? Arguments{{0xfffffff9u, 3}, {0x7fffffffu, 1}, {0x80000000u, 2}}
            : Arguments{{0xffffffffu, 2}, {0, 1}, {65536, 65536}};
        for (const std::string operation : {"+", "-", "*", "/", "%"})
            add_function(type, "a " + operation + " b", false, arithmetic);
        add_function(type, "!a", false, arithmetic);
        if (signed_integer)
            add_function(type, "-a", false, arithmetic);
    }
    source << "fn main() {}\n";
    const auto crate = parse_crate(source.str());
    const auto sema = analyze(*crate);
    auto artifact = generate(sema);
    auto &module = artifact.module();
    const auto names = rx::assign_function_names(sema);
    const auto print = module.add_function(
        "println_i32", {Type::Void, {Type::I32}}, Linkage::External);
    // 保留原入口调用，仅在 ret 前追加驱动；整数直接打印相同的 32 位模式。
    const auto entry = module.find_function("main")->id;
    module.function(entry).blocks[0].terminator.reset();
    IRBuilder builder(module);
    builder.set_insert_point(module.function(entry).blocks[0].id);
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto function = module.find_function(names[index])->id;
        const auto signature = module.function(function).signature;
        for (const auto &[left, right] : arguments[index]) {
            auto value = *builder.call(function, {
                Operand::constant(signature.parameters[0], left),
                Operand::constant(signature.parameters[1], right)});
            if (value.type == Type::I1)
                value = builder.zext(Type::I32, value);
            builder.call(print, {value});
        }
    }
    builder.ret(Operand::constant(Type::I32, 0));
    return artifact;
}

// 追踪每条移位计数的定义，确认实际使用的是前面 count & 31 的结果。
bool shifts_use_masked_counts(const Module &module) {
    for (const auto &owned : module.functions()) {
        const auto &function = *owned;
        std::vector<const Instruction *> definitions(function.value_types.size());
        for (const auto &block : function.blocks) {
            for (const auto &instruction : block.instructions) {
                const auto *binary =
                    std::get_if<BinaryOperation>(&instruction.operation);
                if (binary && (binary->operation == BinaryOp::ShiftLeft ||
                               binary->operation == BinaryOp::ArithmeticShiftRight ||
                               binary->operation == BinaryOp::LogicalShiftRight)) {
                    const auto *count = binary->right.value_id();
                    if (!count || count->function != function.id)
                        return false;
                    const auto *definition = definitions.at(count->index);
                    const auto *mask = definition
                        ? std::get_if<BinaryOperation>(&definition->operation)
                        : nullptr;
                    // 仅仅存在 and 不够：移位必须引用它的结果，且掩码必须为 31。
                    if (!mask || mask->operation != BinaryOp::And ||
                        mask->right != Operand::constant(Type::I32, 31))
                        return false;
                }
                if (instruction.result)
                    definitions.at(instruction.result->index) = &instruction;
            }
        }
    }
    return true;
}

// P4.2：移位量先掩码低五位，cast 只换源类型或把 bool 零扩展。
void check_shifts_and_casts() {
    const auto crate = parse_crate(R"(
        fn main() {
            let zero = 0u32;
            let edge = 31usize;
            let wrap = 32i32;
            let next = 33isize;
            let negative = -1i32;
            println_i32(1i32 << zero);
            println_i32(1i32 << edge);
            println_i32(1i32 << wrap);
            println_i32(1i32 << next);
            println_i32(1i32 << negative);
            println_i32(-8i32 >> 1u32);
            println_i32((4294967288u32 >> 1isize) as i32);
            println_i32((-1i32 as u32) as i32);
            println_i32(2147483648u32 as i32);
            println_i32(true as i32);
            println_i32((true as u32) as i32);
            println_i32((false as isize) as i32);
            println_i32((false as usize) as i32);
        }
    )");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics) &&
              diagnostics.str().empty(),
          "移位和 cast 模块必须通过 IR 自检");
    const auto ir = text.str();
    // 每个移位都必须先出现 and i32 ..., 31，即使右侧是编译期字面量。
    std::size_t shift_count = 0;
    for (auto found = ir.find("shl i32"); found != std::string::npos;
         found = ir.find("shl i32", found + 1))
        ++shift_count;
    check(shift_count == 5, "普通左移必须生成 shl i32");
    check(shifts_use_masked_counts(artifact.module()),
          "每条移位必须使用前面 count & 31 的结果，不能绕过掩码");
    check(ir.find("ashr i32") != std::string::npos &&
              ir.find("lshr i32") != std::string::npos,
          "有符号右移使用 ashr，无符号右移使用 lshr");
    check(ir.find("zext i1 true to i32") != std::string::npos &&
              ir.find("zext i1 false to i32") != std::string::npos &&
              ir.find("sext") == std::string::npos,
          "bool 到整数必须零扩展，不能符号扩展");
    check(ir.find(" exact") == std::string::npos &&
              ir.find(" nsw ") == std::string::npos &&
              ir.find(" nuw ") == std::string::npos,
          "移位不能附加未经证明的精确性或不溢出承诺");
    // 整数 cast 不生成指令：-1 和 0x80000000 的位模式直接交给打印。
    check(ir.find("sub i32 0, 1") != std::string::npos &&
              ir.find("i32 -2147483648)") != std::string::npos,
          "整数 cast 必须保留 32 位位模式");
    // 保持指令数量不变，分别制造错误掩码和绕过掩码的三种移位。
    for (const auto operation : {BinaryOp::And, BinaryOp::ShiftLeft,
                                 BinaryOp::ArithmeticShiftRight,
                                 BinaryOp::LogicalShiftRight}) {
        auto corrupted = generate(sema);
        bool changed = false;
        for (const auto &owned : corrupted.module().functions()) {
            for (auto &block : owned->blocks) {
                for (auto &instruction : block.instructions) {
                    auto *binary = std::get_if<BinaryOperation>(&instruction.operation);
                    if (!changed && binary && binary->operation == operation) {
                        binary->right = Operand::constant(
                            Type::I32, operation == BinaryOp::And ? 30 : 1);
                        changed = true;
                    }
                }
            }
        }
        check(changed && !shifts_use_masked_counts(corrupted.module()),
              "掩码错误或移位未使用掩码结果时，数据依赖检查必须失败");
    }
}

// 检查借用保留原位置，多层引用逐层读取，引用参数和返回值只传递地址。
void check_reference_places() {
    const auto crate = parse_crate(R"(
        fn identity(p: &i32) -> &i32 { p }
        fn explicit(p: &mut i32) -> &mut i32 { return p; }
        fn unused(value: &i32) {}
        fn main() {
            let x = 7;
            let r = &x;
            let rr = &r;
            println_i32(**rr);
            let again = &*r;
            println_i32(*again);
        }
    )");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics),
          "引用存取模块必须通过结构检查");
    const auto &module = artifact.module();
    for (const auto *name : {"__rx_fn_0", "__rx_fn_1"}) {
        const auto &function = *module.find_function(name);
        check(function.signature == Signature{Type::Ptr, {Type::Ptr}} &&
                  function.entry_allocas.size() == 1 &&
                  std::get<Alloca>(function.entry_allocas[0].operation).storage_type == Type::Ptr,
              "共享和可变引用的参数 slot、参数值及返回值都必须使用 ptr");
        const auto &instructions = function.blocks[0].instructions;
        check(instructions.size() == 2, "引用返回不得额外读取目标内容");
        const auto &store = std::get<Store>(instructions[0].operation);
        const auto &load = std::get<Load>(instructions[1].operation);
        check(store.storage_type == Type::Ptr && store.value == function.argument(0) &&
                  load.storage_type == Type::Ptr && load.address == store.address &&
                  function.blocks[0].terminator->value ==
                      Operand::local(Type::Ptr, *instructions[1].result),
              "尾返回和显式 return 都须返回参数保存的地址，而不是参数 slot 的地址");
    }
    const auto &main = *module.find_function("__rx_fn_3");
    check(main.entry_allocas.size() == 4,
          "借用已有位置不得额外分配内容副本，&*r 也应复用原位置");
    const auto &instructions = main.blocks[0].instructions;
    const auto x_address = Operand::local(Type::Ptr, *main.entry_allocas[0].result);
    const auto r_address = Operand::local(Type::Ptr, *main.entry_allocas[1].result);
    const auto rr_address = Operand::local(Type::Ptr, *main.entry_allocas[2].result);
    const auto &borrow_x = std::get<Store>(instructions[1].operation);
    const auto &borrow_r = std::get<Store>(instructions[2].operation);
    check(borrow_x.value == x_address && borrow_x.address == r_address &&
              borrow_r.value == r_address && borrow_r.address == rr_address,
          "&x 保存 x 的原地址，&r 保存引用变量自身 slot 的地址");
    auto address = rr_address;
    for (std::size_t index = 3; index < 6; ++index) {
        const auto &load = std::get<Load>(instructions[index].operation);
        const auto expected_type = index == 5 ? Type::I32 : Type::Ptr;
        check(load.address == address && load.storage_type == expected_type &&
                  load.alignment == 4,
              "**rr 必须先逐层 load ptr，最后才能 load i32");
        address = Operand::local(expected_type, *instructions[index].result);
    }
    check(std::get<Call>(instructions[6].operation).arguments ==
              std::vector<Operand>{address}, "打印应使用最终读取的整数");
    const auto &reborrow = std::get<Load>(instructions[7].operation);
    check(reborrow.storage_type == Type::Ptr && reborrow.address == r_address &&
              std::get<Store>(instructions[8].operation).value ==
                  Operand::local(Type::Ptr, *instructions[7].result),
          "&*r 只读取 r 保存的指针，不得读取或复制整数目标");
}

// 检查临时 slot 全部位于入口，调用只生成一次，初始化仍跟在对应调用之后。
void check_reference_temporaries() {
    const auto crate = parse_crate(R"(
        fn first() -> i32 { let p = &3; *p }
        fn second() -> i32 { let p = &4; *p }
        fn make(value: i32) -> i32 { println_i32(value); value }
        fn main() {
            println_i32(0);
            let a = &mut make(3);
            let b = &make(4);
            println_i32(*a);
            println_i32(*b);
        }
    )");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics),
          "不同函数和不同静态位置的临时物化必须通过结构检查");
    const auto &module = artifact.module();
    for (const auto *name : {"__rx_fn_0", "__rx_fn_1"}) {
        const auto &function = *module.find_function(name);
        check(function.entry_allocas.size() == 2,
              "不同函数必须分别持有自己的临时 slot 和引用绑定 slot");
    }
    const auto &main = *module.find_function("__rx_fn_3");
    check(main.entry_allocas.size() == 4,
          "两个借用位置各需一个临时 slot 和一个引用绑定 slot");
    const auto &instructions = main.blocks[0].instructions;
    std::vector<Operand> temporary_addresses;
    for (std::size_t index = 0; index < instructions.size(); ++index) {
        check(!std::holds_alternative<Alloca>(instructions[index].operation),
              "临时 alloca 不得混入求值正文");
        const auto *call = std::get_if<Call>(&instructions[index].operation);
        if (!call || call->function != module.find_function("__rx_fn_2")->id)
            continue;
        check(index + 2 < instructions.size(), "临时调用后必须初始化并保存借用地址");
        const auto &initialize = std::get<Store>(instructions[index + 1].operation);
        const auto &borrow = std::get<Store>(instructions[index + 2].operation);
        check(initialize.storage_type == Type::I32 && initialize.value ==
                  Operand::local(Type::I32, *instructions[index].result) &&
                  borrow.storage_type == Type::Ptr && borrow.value == initialize.address,
              "先调用一次，再把结果写入临时 slot，最后保存该 slot 的地址");
        temporary_addresses.push_back(initialize.address);
    }
    check(temporary_addresses.size() == 2 &&
              temporary_addresses[0] != temporary_addresses[1],
          "不同借用位置必须各调用一次，并使用不同临时存储");
    const auto ir = text.str();
    const auto body = ir.find("define internal void @__rx_fn_3()");
    const auto first_allocation = ir.find("alloca", body);
    const auto first_effect = ir.find("call void @println_i32(i32 0)", body);
    check(body != std::string::npos && first_allocation != std::string::npos &&
              first_effect != std::string::npos && first_allocation < first_effect &&
              ir.find("alloca", first_effect) == std::string::npos,
          "即使临时借用出现在副作用之后，打印出的 alloca 仍全部位于入口");
}

// 检查借用或地址求值中的 return 会跳过物化、目标读取和后续副作用。
void check_reference_early_return() {
    for (const auto &source : {
             "fn main() { let p = &{ return; 3 }; println_i32(*p); }",
             "fn main() { let p = &mut { return; 3 }; println_i32(*p); }",
             "fn main() { println_i32(*{ return; &3 }); }",
             "fn main() { let p = &*{ return; &3 }; println_i32(*p); }"}) {
        const auto crate = parse_crate(source);
        const auto sema = analyze(*crate);
        const auto artifact = generate(sema);
        const auto &main = *artifact.module().find_function("__rx_fn_0");
        check(main.entry_allocas.empty() && main.blocks[0].instructions.empty() &&
                  main.blocks[0].terminator && !main.blocks[0].terminator->value,
              "引用求值提前 return 后不得分配 slot、读取目标或调用打印");
    }
}

// 检查引用转换只读取所需中间指针；嵌套块和显式 return 不得重复调整。
void check_reference_coercions() {
    const auto crate = parse_crate(R"(
        fn shared(p: &mut i32) -> &i32 { p }
        fn one(p: &&i32) -> &i32 { p }
        fn nested(p: &&&i32) -> &i32 { { { p } } }
        fn explicit(p: &&&i32) -> &i32 { return { { p } }; }
        fn mutable(p: &mut &mut i32) -> &mut i32 { p }
        fn partial(p: &&&i32) -> &&i32 { p }
        fn main() {}
    )");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    const auto names = rx::assign_function_names(sema);
    const std::vector<std::size_t> steps{0, 1, 2, 2, 1, 1};
    for (std::size_t index = 0; index < steps.size(); ++index) {
        const auto &function = *artifact.module().find_function(names[index]);
        const auto &instructions = function.blocks[0].instructions;
        check(function.signature == Signature{Type::Ptr, {Type::Ptr}} &&
                  function.entry_allocas.size() == 1 &&
                  instructions.size() == 2 + steps[index],
              "引用转换不得分配目标副本，也不得多读或漏读中间指针");
        const auto &store = std::get<Store>(instructions[0].operation);
        check(store.value == function.argument(0), "引用形参应先保存原地址");
        auto address = store.address;
        // 第一次读取参数 slot；之后每个 load 对应计划中的一次引用解引用。
        for (std::size_t load_index = 1; load_index < instructions.size(); ++load_index) {
            const auto &load = std::get<Load>(instructions[load_index].operation);
            check(load.storage_type == Type::Ptr && load.address == address &&
                      load.alignment == 4,
                  "转换必须逐层读取上一个指针，不能读取最终 i32 内容");
            address = Operand::local(Type::Ptr, *instructions[load_index].result);
        }
        check(function.blocks[0].terminator->value == address,
              "转换必须返回最后到达的位置地址，包括尚保留引用层级的目标");
    }

    // 带标注 let 的多层转换应在初始化完成后执行一次，再保存最终引用。
    const auto let_crate = parse_crate(R"(
        fn main() {
            let x = 7;
            let r = &x;
            let rr = &r;
            let flat: &i32 = { { rr } };
        }
    )");
    const auto let_sema = analyze(*let_crate);
    const auto let_artifact = generate(let_sema);
    const auto &main = *let_artifact.module().find_function("__rx_fn_0");
    const auto &instructions = main.blocks[0].instructions;
    check(main.entry_allocas.size() == 4 && instructions.size() == 6,
          "有标注 let 的嵌套块转换只需读取绑定指针和一层中间指针");
    const auto &read_binding = std::get<Load>(instructions[3].operation);
    const auto &adjust = std::get<Load>(instructions[4].operation);
    const auto &save = std::get<Store>(instructions[5].operation);
    check(read_binding.storage_type == Type::Ptr &&
              read_binding.address == Operand::local(Type::Ptr, *main.entry_allocas[2].result) &&
              adjust.storage_type == Type::Ptr &&
              adjust.address == Operand::local(Type::Ptr, *instructions[3].result) &&
              save.storage_type == Type::Ptr &&
              save.value == Operand::local(Type::Ptr, *instructions[4].result),
          "let 保存的必须是调整后的引用，而不是外层引用的地址");
}

// 检查每个实参求值后立即完成自身转换，再开始下一个实参，且调用只执行一次。
void check_coercion_argument_order() {
    const auto crate = parse_crate(R"(
        fn first(p: &&i32) -> &&i32 { println_i32(1); p }
        fn second(p: &i32) -> &i32 { println_i32(2); p }
        fn consume(a: &i32, b: &i32) -> i32 { *a + *b }
        fn main() {
            let x = 7;
            let r = &x;
            println_i32(consume(first(&r), second(&x)));
        }
    )");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    const auto names = rx::assign_function_names(sema);
    const auto &module = artifact.module();
    const auto first = module.find_function(names[0])->id;
    const auto second = module.find_function(names[1])->id;
    const auto consume = module.find_function(names[2])->id;
    const auto &instructions = module.find_function(names[3])->blocks[0].instructions;
    std::size_t first_calls = 0;
    for (std::size_t index = 0; index < instructions.size(); ++index) {
        const auto *call = std::get_if<Call>(&instructions[index].operation);
        if (!call || call->function != first)
            continue;
        ++first_calls;
        check(index + 3 < instructions.size(), "实参调用后应完成转换和目标调用");
        const auto &adjust = std::get<Load>(instructions[index + 1].operation);
        const auto &next_call = std::get<Call>(instructions[index + 2].operation);
        const auto &target_call = std::get<Call>(instructions[index + 3].operation);
        check(adjust.storage_type == Type::Ptr &&
                  adjust.address == Operand::local(Type::Ptr, *instructions[index].result) &&
                  next_call.function == second && target_call.function == consume &&
                  target_call.arguments == std::vector<Operand>{
                      Operand::local(Type::Ptr, *instructions[index + 1].result),
                      Operand::local(Type::Ptr, *instructions[index + 2].result)},
              "第一个实参转换必须先于第二个实参求值，最终调用使用两个已准备好的地址");
    }
    check(first_calls == 1, "为取得转换后的地址不得再次调用源函数");
}

// 找到指定函数唯一的一次调用，供顺序检查使用，不依赖打印出的 SSA 编号。
std::size_t unique_call_index(const std::vector<Instruction> &instructions,
                              IRFunctionId function) {
    std::size_t found = instructions.size();
    for (std::size_t index = 0; index < instructions.size(); ++index) {
        const auto *call = std::get_if<Call>(&instructions[index].operation);
        if (call && call->function == function) {
            check(found == instructions.size(), "求值同一操作数或赋值目标不得重复调用");
            found = index;
        }
    }
    check(found != instructions.size(), "必须生成预期的操作数或赋值目标调用");
    return found;
}

// 引用先从左到右各求值一次，两个目标内容都要等右侧调用完成后再读取。
void check_reference_binary_order() {
    for (const auto &[operation, expected] :
         std::vector<std::pair<std::string, BinaryOp>>{
             {"-", BinaryOp::Subtract}, {"^", BinaryOp::Xor}}) {
        const auto crate = parse_crate(
            "fn left(p: &i32) -> &i32 { println_i32(1); p } "
            "fn right(p: &i32) -> &i32 { println_i32(2); p } "
            "fn calculate(a: &i32, b: &i32) -> i32 { left(a) " + operation +
            " right(b) } fn main() {}");
        const auto sema = analyze(*crate);
        const auto artifact = generate(sema);
        std::ostringstream text, diagnostics;
        check(artifact.verify_and_print(text, diagnostics) && diagnostics.str().empty(),
              "共享引用算术和位运算必须通过 IR 自检");
        const auto names = rx::assign_function_names(sema);
        const auto &module = artifact.module();
        const auto &function = *module.find_function(names[2]);
        const auto &instructions = function.blocks[0].instructions;
        const auto left = unique_call_index(instructions, module.find_function(names[0])->id);
        const auto right = unique_call_index(instructions, module.find_function(names[1])->id);
        std::vector<std::size_t> loads;
        for (std::size_t index = 0; index < instructions.size(); ++index) {
            if (const auto *load = std::get_if<Load>(&instructions[index].operation);
                load && load->storage_type == Type::I32)
                loads.push_back(index);
        }
        check(left < right && loads.size() == 2 && right < loads[0] &&
                  loads[0] < loads[1] && loads[1] + 2 == instructions.size(),
              "左右调用必须各一次且按顺序发生，所有整数内容读取都晚于两个调用");
        check(std::get<Load>(instructions[loads[0]].operation).address ==
                  Operand::local(Type::Ptr, *instructions[left].result) &&
                  std::get<Load>(instructions[loads[1]].operation).address ==
                  Operand::local(Type::Ptr, *instructions[right].result),
              "两次内容读取必须分别使用左右调用返回的引用");
        const auto &result = instructions.back();
        const auto &binary = std::get<BinaryOperation>(result.operation);
        check(binary.operation == expected && result.type == Type::I32 &&
                  binary.left == Operand::local(Type::I32, *instructions[loads[0]].result) &&
                  binary.right == Operand::local(Type::I32, *instructions[loads[1]].result) &&
                  function.blocks[0].terminator->value ==
                      Operand::local(Type::I32, *result.result),
              "标量结果必须使用两次最终读取，且不能交换左右操作数");
    }
}

// 混合操作数只延后引用目标读取；直接局部值必须在自身求值时固定，数据互不别名。
void check_mixed_reference_operand_order() {
    const auto crate = parse_crate(R"(
        fn reference(p: &i32) -> &i32 { println_i32(1); p }
        fn direct_left() -> i32 {
            let value = 7; let other = 3;
            ({ println_i32(2); value }) - reference(&other)
        }
        fn direct_right() -> i32 {
            let value = 7; let other = 3;
            reference(&other) - { println_i32(2); value }
        }
        fn main() {}
    )");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics) && diagnostics.str().empty(),
          "直接标量与引用的混合运算必须通过 IR 自检");
    const auto names = rx::assign_function_names(sema);
    const auto &module = artifact.module();
    for (std::size_t side = 0; side < 2; ++side) {
        const auto &function = *module.find_function(names[1 + side]);
        const auto &instructions = function.blocks[0].instructions;
        const auto call = unique_call_index(instructions, module.find_function(names[0])->id);
        const auto mark = unique_call_index(instructions, module.find_function("println_i32")->id);
        std::vector<std::size_t> loads;
        for (std::size_t index = 0; index < instructions.size(); ++index) {
            if (std::holds_alternative<Load>(instructions[index].operation))
                loads.push_back(index);
        }
        check(loads.size() == 2 && mark < loads[0] &&
                  (side == 0 ? loads[0] < call : call < mark) &&
                  call < loads[1] && loads[1] + 2 == instructions.size(),
              "直接值在自身副作用后立即读取；左直接值不能拖到右侧引用调用后");
        const auto &direct = std::get<Load>(instructions[loads[0]].operation);
        const auto &reference = std::get<Load>(instructions[loads[1]].operation);
        check(direct.storage_type == Type::I32 && direct.address ==
                  Operand::local(Type::Ptr, *function.entry_allocas[0].result) &&
                  reference.storage_type == Type::I32 && reference.address ==
                      Operand::local(Type::Ptr, *instructions[call].result),
              "直接值来自自身局部 slot，引用内容来自独立数据的调用返回地址");
        const auto direct_value = Operand::local(Type::I32, *instructions[loads[0]].result);
        const auto reference_value = Operand::local(Type::I32, *instructions[loads[1]].result);
        const auto &binary = std::get<BinaryOperation>(instructions.back().operation);
        check(binary.operation == BinaryOp::Subtract &&
                  binary.left == (side == 0 ? direct_value : reference_value) &&
                  binary.right == (side == 0 ? reference_value : direct_value) &&
                  function.blocks[0].terminator->value ==
                      Operand::local(Type::I32, *instructions.back().result),
              "Direct+Ref 和 Ref+Direct 都必须使用各自时刻已固定的值");
    }
}

// 一元引用运算先完成调用；整数读取 i32，布尔读取 i8 并截断为 i1 后再取反。
void check_reference_unary_operations() {
    const auto crate = parse_crate(R"(
        fn integer(p: &i32) -> &i32 { println_i32(1); p }
        fn boolean(p: &bool) -> &bool { println_i32(2); p }
        fn negative(p: &i32) -> i32 { -integer(p) }
        fn inverted(p: &bool) -> bool { !boolean(p) }
        fn main() {}
    )");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics) && diagnostics.str().empty(),
          "一元共享引用运算必须通过 IR 自检");
    const auto names = rx::assign_function_names(sema);
    const auto &module = artifact.module();
    for (std::size_t kind = 0; kind < 2; ++kind) {
        const auto &function = *module.find_function(names[2 + kind]);
        const auto &instructions = function.blocks[0].instructions;
        const auto call = unique_call_index(instructions, module.find_function(names[kind])->id);
        check(instructions.size() == call + 3 + kind,
              "一元运算在调用后只读取一次内容，bool 额外截断一次");
        const auto &load = std::get<Load>(instructions[call + 1].operation);
        check(load.storage_type == (kind == 0 ? Type::I32 : Type::I8) &&
                  load.alignment == (kind == 0 ? 4u : 1u) &&
                  load.address == Operand::local(Type::Ptr, *instructions[call].result),
              "一元运算必须使用调用返回的引用，按最终标量的存储类型读取");
        auto value = Operand::local(instructions[call + 1].type,
                                    *instructions[call + 1].result);
        if (kind == 1) {
            const auto &trunc = std::get<Trunc>(instructions[call + 2].operation);
            check(trunc.value == value && trunc.target_type == Type::I1,
                  "布尔内容必须从 i8 截断为 i1，不能直接用存储字节运算");
            value = Operand::local(Type::I1, *instructions[call + 2].result);
        }
        const auto &result = instructions.back();
        const auto &binary = std::get<BinaryOperation>(result.operation);
        check(result.type == (kind == 0 ? Type::I32 : Type::I1) &&
                  binary.operation == (kind == 0 ? BinaryOp::Subtract : BinaryOp::Xor) &&
                  binary.left == (kind == 0 ? Operand::constant(Type::I32, 0) : value) &&
                  binary.right == (kind == 0 ? value : Operand::constant(Type::I1, 1)) &&
                  function.blocks[0].terminator->value ==
                      Operand::local(result.type, *result.result),
              "-&i32 应产生 i32 的 0-value，!&bool 应产生 i1 的 value xor true");
    }
}

// 多层排序先取得两侧引用，再逐层读到标量；外层 &T / &mut T 只调整合法方向。
void check_reference_ordering_chains() {
    const auto crate = parse_crate(R"(
        fn left(p: &&&i32) -> &&&i32 { println_i32(1); p }
        fn right(p: &&&i32) -> &&&i32 { println_i32(2); p }
        fn mutable(p: &mut &&i32) -> &mut &&i32 { println_i32(2); p }
        fn matching(a: &&&i32, b: &&&i32) -> bool { left(a) < right(b) }
        fn adjusted(a: &&&i32, b: &mut &&i32) -> bool { left(a) < mutable(b) }
        fn main() {}
    )");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics) && diagnostics.str().empty(),
          "多层引用排序及外层可变转共享调整必须通过 IR 自检");
    const auto names = rx::assign_function_names(sema);
    const auto &module = artifact.module();
    for (std::size_t kind = 0; kind < 2; ++kind) {
        const auto &function = *module.find_function(names[3 + kind]);
        const auto &instructions = function.blocks[0].instructions;
        const auto left = unique_call_index(instructions, module.find_function(names[0])->id);
        const auto right = unique_call_index(instructions, module.find_function(names[1 + kind])->id);
        check(left < right && right + 8 == instructions.size(),
              "多层排序必须先从左到右各调用一次，再读取两条三层引用链并比较");
        // 调用之前只允许读取形参 slot，不能提前读取任意一侧的引用目标。
        for (std::size_t index = 0; index < right; ++index) {
            if (const auto *load = std::get_if<Load>(&instructions[index].operation))
                check(load->storage_type == Type::Ptr &&
                          (load->address == Operand::local(Type::Ptr, *function.entry_allocas[0].result) ||
                           load->address == Operand::local(Type::Ptr, *function.entry_allocas[1].result)),
                      "两侧求值期间只能取出形参中的引用，不能提前沿引用链读取");
        }
        std::vector<Operand> scalars;
        for (std::size_t side = 0; side < 2; ++side) {
            auto address = Operand::local(Type::Ptr, *instructions[side == 0 ? left : right].result);
            for (std::size_t depth = 0; depth < 3; ++depth) {
                const auto &instruction = instructions[right + 1 + side * 3 + depth];
                const auto &load = std::get<Load>(instruction.operation);
                const auto type = depth == 2 ? Type::I32 : Type::Ptr;
                check(load.address == address && load.storage_type == type &&
                          instruction.type == type && load.alignment == 4,
                      "排序必须依次 load ptr、ptr、i32，每一步使用上一层的结果");
                address = Operand::local(type, *instruction.result);
            }
            scalars.push_back(address);
        }
        const auto &comparison = std::get<Compare>(instructions.back().operation);
        check(comparison.predicate == Comparison::SignedLessThan &&
                  comparison.left == scalars[0] && comparison.right == scalars[1] &&
                  function.blocks[0].terminator->value ==
                      Operand::local(Type::I1, *instructions.back().result),
              "引用排序必须比较最终有符号整数，绝不能比较引用地址");
    }
}

// 操作数内部 return 只保留此前调用，跳过右侧求值、引用内容读取和外层运算。
void check_reference_operation_early_return() {
    const auto crate = parse_crate(R"(
        fn left(p: &i32) -> &i32 { println_i32(1); p }
        fn right(p: &i32) -> &i32 { println_i32(2); p }
        fn flag(p: &bool) -> &bool { println_i32(3); p }
        fn leave_left(p: &i32, q: &i32) -> i32 {
            ({ left(p); return 7; p }) - right(q)
        }
        fn leave_right(p: &i32, q: &i32) -> i32 {
            left(p) - { right(q); return 8; q }
        }
        fn leave_negative(p: &i32) -> i32 { -{ left(p); return 9; p } }
        fn leave_not(p: &bool) -> bool { !{ flag(p); return false; p } }
        fn main() {}
    )");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics) && diagnostics.str().empty(),
          "引用运算中提前 return 必须通过 IR 自检");
    const auto names = rx::assign_function_names(sema);
    const auto &module = artifact.module();
    for (std::size_t kind = 0; kind < 4; ++kind) {
        const auto &function = *module.find_function(names[3 + kind]);
        const auto &instructions = function.blocks[0].instructions;
        const auto first = unique_call_index(instructions, module.find_function(names[kind == 3 ? 2 : 0])->id);
        if (kind == 1)
            check(first < unique_call_index(instructions, module.find_function(names[1])->id),
                  "右侧 return 之前仍必须保留从左到右的两次引用求值");
        std::size_t calls = 0;
        for (const auto &instruction : instructions) {
            if (const auto *call = std::get_if<Call>(&instruction.operation)) {
                ++calls;
                check(call->function == module.find_function(names[kind == 3 ? 2 : 0])->id ||
                          (kind == 1 && call->function == module.find_function(names[1])->id),
                      "左侧 return 必须跳过右侧调用，一元 return 不得引入额外副作用");
            } else if (const auto *load = std::get_if<Load>(&instruction.operation)) {
                check(load->storage_type == Type::Ptr,
                      "路径结束后不得为二元或一元引用运算读取最终标量内容");
            } else {
                check(std::holds_alternative<Store>(instruction.operation) &&
                          std::get<Store>(instruction.operation).storage_type == Type::Ptr,
                      "提前 return 只能保留引用参数保存，不能生成外层运算");
            }
        }
        check(calls == (kind == 1 ? 2u : 1u) &&
                  function.blocks[0].terminator->value ==
                      (kind == 3 ? Operand::constant(Type::I1, 0) :
                                   Operand::constant(Type::I32, 7 + kind)),
              "引用运算必须直接返回操作数内部指定的值，且此前调用各保留一次");
    }
}

// 引用相等仍属于 P10；Box 只有显式解引用后的排序合法，不能混作 lowering 负例。
void check_reference_operation_boundaries() {
    for (const auto &source : {
             "fn main() { let a = 1; let b = 2; let same = &a == &b; }",
             "fn main() { let a = 1; let b = 2; let different = &a != &b; }",
             "fn main() { let a = [1]; let b = [2]; let same = &a == &b; }",
             "#[derive(PartialEq)] struct Data { value: i32 } fn main() { "
             "let a = Data { value: 1 }; let b = Data { value: 2 }; let same = &a == &b; }",
             "fn main() { let a = Box::<i32>::new(1); let b = Box::<i32>::new(2); let less = *a < *b; }"})
        check_rejected(source);
    for (const auto &source : {
             "fn main() { let a = Box::<i32>::new(1); let b = Box::<i32>::new(2); let less = a < b; }",
             "fn main() { let mut a = 1; let b = 2; let less = &mut a < &b; }",
             "fn main() { let a = 1; let mut b = 2; let less = &&a < &&mut b; }"})
        check_semantic_rejected(source);
}

// 检查两种赋值先调用 RHS；复合赋值在目标修改旧值后读取，并写回同一地址。
void check_assignment_order() {
    const auto crate = parse_crate(R"(
        fn target(p: &mut i32) -> &mut i32 { println_i32(2); *p = 23; p }
        fn right() -> i32 { println_i32(1); 3 }
        fn reference(p: &i32) -> &i32 { println_i32(1); p }
        fn plain(p: &mut i32) { *target(p) = right(); }
        fn compound(p: &mut i32) { *target(p) += right(); }
        fn shared(p: &mut i32, other: &i32) { *target(p) += reference(other); }
        fn main() {
            let mut x = 10;
            let other = 3;
            plain(&mut x); compound(&mut x); shared(&mut x, &other);
        }
    )");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics) && diagnostics.str().empty(),
          "标量和共享引用 RHS 的赋值模块必须通过 IR 自检");
    const auto names = rx::assign_function_names(sema);
    const auto &module = artifact.module();
    const auto target = module.find_function(names[0])->id;
    for (std::size_t kind = 0; kind < 3; ++kind) {
        const auto &instructions = module.find_function(names[3 + kind])->blocks[0].instructions;
        const auto right_index = unique_call_index(
            instructions, module.find_function(names[kind == 2 ? 2 : 1])->id);
        const auto target_index = unique_call_index(instructions, target);
        check(right_index < target_index, "普通和复合赋值都必须先调用 RHS，再调用目标");
        const auto right = Operand::local(instructions[right_index].type,
                                           *instructions[right_index].result);
        const auto address = Operand::local(Type::Ptr, *instructions[target_index].result);
        std::vector<std::size_t> loads, stores, operations;
        for (std::size_t index = 0; index < instructions.size(); ++index) {
            if (const auto *load = std::get_if<Load>(&instructions[index].operation);
                load && load->storage_type == Type::I32)
                loads.push_back(index);
            if (const auto *store = std::get_if<Store>(&instructions[index].operation);
                store && store->storage_type == Type::I32)
                stores.push_back(index);
            if (std::holds_alternative<BinaryOperation>(instructions[index].operation))
                operations.push_back(index);
        }
        check(stores.size() == 1 && target_index < stores[0] &&
                  std::get<Store>(instructions[stores[0]].operation).address == address,
              "赋值必须只写一次目标调用返回的原地址");
        const auto &store = std::get<Store>(instructions[stores[0]].operation);
        if (kind == 0) {
            check(loads.empty() && operations.empty() && store.value == right,
                  "普通赋值直接写入已固定的 RHS，不读取旧值或执行复合运算");
            continue;
        }
        check(loads.size() == (kind == 2 ? 2u : 1u) && operations.size() == 1 &&
                  target_index < loads[0] && loads.back() < operations[0] &&
                  operations[0] < stores[0],
              "旧值和共享引用内容只能在目标调用后读取，再运算并写回");
        const auto &old = std::get<Load>(instructions[loads[0]].operation);
        auto right_scalar = right;
        if (kind == 2) {
            const auto &read_right = std::get<Load>(instructions[loads[1]].operation);
            check(read_right.address == right,
                  "共享引用 RHS 必须先保留调用返回的指针，运算时才读取其独立数据");
            right_scalar = Operand::local(Type::I32, *instructions[loads[1]].result);
        }
        const auto &operation = std::get<BinaryOperation>(instructions[operations[0]].operation);
        check(old.address == address && operation.operation == BinaryOp::Add &&
                  operation.left == Operand::local(Type::I32, *instructions[loads[0]].result) &&
                  operation.right == right_scalar &&
                  store.value == Operand::local(Type::I32, *instructions[operations[0]].result),
              "加法必须使用目标改变后的旧值及已准备的 RHS，计算结果写回同一目标");
    }
}

// 普通引用赋值先完成 AssignmentValue 的指针调整，不能等目标调用之后再转换。
void check_assignment_coercion_order() {
    const auto crate = parse_crate(R"(
        fn source(p: &&i32) -> &&i32 { println_i32(1); p }
        fn slot(p: &mut &i32) -> &mut &i32 { println_i32(2); p }
        fn main() {
            let first = 7;
            let other = 9;
            let mut r = &first;
            let rr = &other;
            *slot(&mut r) = source(&rr);
        }
    )");
    const auto sema = analyze(*crate);
    std::size_t assignments = 0;
    for (const auto &[site, plan] : sema.coercion_plans) {
        if (site.kind != rx::UseKind::AssignmentValue)
            continue;
        ++assignments;
        check(plan.steps.size() == 1 && plan.steps[0].kind == rx::CoercionStepKind::Reference,
              "赋值位置必须记录从 &&i32 到 &i32 的一次引用调整");
    }
    check(assignments == 1, "必须实际覆盖赋值 RHS 的 CoercionPlan");
    const auto artifact = generate(sema);
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics) && diagnostics.str().empty(),
          "赋值引用转换模块必须通过 IR 自检");
    const auto names = rx::assign_function_names(sema);
    const auto &module = artifact.module();
    const auto &instructions = module.find_function(names[2])->blocks[0].instructions;
    const auto source = unique_call_index(instructions, module.find_function(names[0])->id);
    const auto target = unique_call_index(instructions, module.find_function(names[1])->id);
    check(source + 1 < target && target + 1 < instructions.size(),
          "RHS 调用后必须先调整指针，再调用目标并保存");
    const auto &adjust = std::get<Load>(instructions[source + 1].operation);
    const auto &store = std::get<Store>(instructions[target + 1].operation);
    check(adjust.storage_type == Type::Ptr &&
              adjust.address == Operand::local(Type::Ptr, *instructions[source].result) &&
              store.storage_type == Type::Ptr &&
              store.value == Operand::local(Type::Ptr, *instructions[source + 1].result) &&
              store.address == Operand::local(Type::Ptr, *instructions[target].result),
          "目标必须保存目标求值前已转换好的引用，而不是 RHS 的外层引用");
    for (const auto &instruction : instructions) {
        const auto *load = std::get_if<Load>(&instruction.operation);
        check(!load || load->storage_type == Type::Ptr,
              "引用赋值不得读取被引用的整数内容");
    }
}

// 分别在 RHS 和地址阶段 return，保留此前副作用，但跳过旧值、运算、写回及后续语句。
void check_assignment_early_return() {
    for (const std::string operation : {"=", "+="}) {
        const auto crate = parse_crate(
            "fn target(p: &mut i32) -> &mut i32 { println_i32(2); p } "
            "fn right() -> i32 { println_i32(1); 3 } "
            "fn leave_rhs(p: &mut i32) -> i32 { *target(p) " + operation +
            " { right(); return 7; 1 }; println_i32(99); 0 } "
            "fn leave_target(p: &mut i32) -> i32 { *{ target(p); return 8; p } " +
            operation + " right(); println_i32(99); 0 } fn main() {}");
        const auto sema = analyze(*crate);
        const auto artifact = generate(sema);
        std::ostringstream text, diagnostics;
        check(artifact.verify_and_print(text, diagnostics) && diagnostics.str().empty(),
              "赋值中提前 return 的模块必须通过 IR 自检");
        const auto names = rx::assign_function_names(sema);
        const auto &module = artifact.module();
        for (std::size_t stage = 0; stage < 2; ++stage) {
            const auto &function = *module.find_function(names[2 + stage]);
            check(function.blocks.size() == 1 && function.blocks[0].terminator &&
                      function.blocks[0].terminator->value == Operand::constant(Type::I32, 7 + stage),
                  "赋值中的 return 必须直接返回对应阶段的值，不能生成后续尾值");
            const auto &instructions = function.blocks[0].instructions;
            const auto right = unique_call_index(instructions, module.find_function(names[1])->id);
            std::size_t target_calls = 0;
            for (std::size_t index = 0; index < instructions.size(); ++index) {
                const auto &instruction = instructions[index];
                if (const auto *call = std::get_if<Call>(&instruction.operation)) {
                    if (call->function == module.find_function(names[0])->id) {
                        ++target_calls;
                        check(right < index, "地址阶段 return 前的目标调用仍必须晚于 RHS");
                    } else {
                        check(call->function == module.find_function(names[1])->id,
                              "赋值路径结束后不得执行后续打印");
                    }
                } else if (const auto *store = std::get_if<Store>(&instruction.operation)) {
                    check(store->storage_type == Type::Ptr && store->value == function.argument(0),
                          "提前 return 只能保留参数保存，不得执行本次赋值的写回");
                } else {
                    const auto *load = std::get_if<Load>(&instruction.operation);
                    check(load && load->storage_type == Type::Ptr,
                          "提前 return 不得读取旧整数值或执行复合运算");
                }
            }
            check(target_calls == stage,
                  "RHS return 跳过目标；地址 return 保留一次目标副作用但不再赋值");
        }
    }
}

// 赋值产生 unit，覆盖语句、函数尾、嵌套块尾和显式 unit return 的分派。
void check_assignment_unit_tails() {
    const auto crate = parse_crate(R"(
        fn plain(p: &mut i32) { *p = 2 }
        fn compound(p: &mut i32) { *p += 3 }
        fn nested(p: &mut i32) { { *p = 4 } }
        fn explicit(p: &mut i32) { return { *p += 5 }; }
        fn main() {
            let mut x = 1;
            x = 2;
            { x += 3 }
            plain(&mut x); compound(&mut x); nested(&mut x); explicit(&mut x);
        }
    )");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics) && diagnostics.str().empty(),
          "赋值的 unit 函数尾、块尾和 return 必须全部成功生成并通过 IR 自检");
    const auto names = rx::assign_function_names(sema);
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto &function = *artifact.module().find_function(names[index]);
        check(function.signature.result == Type::Void && function.blocks.size() == 1 &&
                  function.blocks[0].terminator && !function.blocks[0].terminator->value,
              "赋值完成后只能返回 void，不能产生虚构的 unit SSA 值");
        std::vector<const Store *> stores;
        for (const auto &instruction : function.blocks[0].instructions) {
            if (const auto *store = std::get_if<Store>(&instruction.operation);
                store && store->storage_type == Type::I32)
                stores.push_back(store);
        }
        check(stores.size() == (index == 4 ? 3u : 1u),
              "每个函数尾赋值写回一次；main 保留初始化、普通赋值和复合赋值");
        if (index == 4) {
            check(stores[0]->value == Operand::constant(Type::I32, 1) &&
                      stores[1]->value == Operand::constant(Type::I32, 2) &&
                      stores[0]->address == stores[1]->address &&
                      stores[1]->address == stores[2]->address,
                  "原赋值负例必须成为正例，后续赋值继续更新同一个局部 slot");
        }
    }
}

// 沿终结指令找块，不依赖 then/else/merge 的编号或打印顺序。
void check_unit_if() {
    for (const bool has_else : {false, true}) {
        const auto crate = parse_crate(std::string(
            "fn flag() -> bool { true } "
            "fn main() { if flag() { println_i32(1); }") +
            (has_else ? " else { println_i32(2); }" : "") +
            " println_i32(3); }");
        const auto sema = analyze(*crate);
        const auto artifact = generate(sema);
        std::ostringstream text, diagnostics;
        check(artifact.verify_and_print(text, diagnostics) && diagnostics.str().empty(),
              "unit if 必须通过 IR 自检");

        const auto names = rx::assign_function_names(sema);
        const auto &module = artifact.module();
        const auto &main = *module.find_function(names[sema.main_function->index]);
        check(main.blocks.size() == (has_else ? 4u : 3u) &&
                  main.entry_allocas.empty() && main.value_types == std::vector{Type::I1},
              "unit if 不产生结果值或 slot，无 else 时也不需要空分支块");
        const auto &entry = main.blocks.front();
        check(entry.instructions.size() == 1, "条件只能求值一次");
        const auto &instruction = entry.instructions.front();
        const auto &call = std::get<Call>(instruction.operation);
        const auto &branch = std::get<CondBranch>(entry.terminator->instruction);
        check(call.function == module.find_function(names[0])->id && instruction.result &&
                  branch.condition == Operand::local(Type::I1, *instruction.result),
              "condbr 必须直接使用一次条件调用返回的 i1");

        const auto print = module.find_function("println_i32")->id;
        const auto prints = [print](const BasicBlock &block, unsigned marker) {
            if (block.instructions.size() != 1)
                return false;
            const auto *call = std::get_if<Call>(&block.instructions.front().operation);
            return call && call->function == print &&
                   call->arguments == std::vector{Operand::constant(Type::I32, marker)};
        };
        const auto &then_block = main.block(branch.when_true);
        const auto merge = std::get<Branch>(then_block.terminator->instruction).target;
        check(prints(then_block, 1), "true 边必须进入 then 块");
        if (has_else) {
            const auto &else_block = main.block(branch.when_false);
            check(prints(else_block, 2) &&
                      std::get<Branch>(else_block.terminator->instruction).target == merge,
                  "false 边必须进入 else 块，再跳到同一合流块");
        } else {
            check(branch.when_false == merge, "没有 else 时 false 边应直达合流块");
        }
        const auto &merge_block = main.block(merge);
        check(prints(merge_block, 3) &&
                  !std::get<Return>(merge_block.terminator->instruction).value,
              "if 后的代码必须从合流块继续，最后正常 ret void");
    }
}

// 两条正常边先写同一个入口 slot；合流时用新 load，而不是直接使用分支的 SSA 值。
void check_integer_if_merge() {
    const auto crate = parse_crate(R"(
        fn flag() -> bool { get_i32() != 0 }
        fn main() {
            println_i32(get_i32() + if flag() {
                println_i32(1);
                10
            } else {
                println_i32(2);
                20
            });
        }
    )");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics) && diagnostics.str().empty(),
          "整数 if 和合法的跨块操作数必须通过 IR 自检");

    const auto names = rx::assign_function_names(sema);
    const auto &module = artifact.module();
    const auto &main = *module.find_function(names[sema.main_function->index]);
    check(main.blocks.size() == 4 && main.entry_allocas.size() == 1,
          "整数 if 必须在入口分配一个结果 slot");
    const auto &allocation = main.entry_allocas.front();
    const auto &storage = std::get<Alloca>(allocation.operation);
    check(allocation.result && storage.storage_type == Type::I32 && storage.alignment == 4,
          "整数 if 的结果 slot 必须能存放 i32");
    const auto slot = Operand::local(Type::Ptr, *allocation.result);

    const auto &entry = main.blocks.front();
    const auto input_index = unique_call_index(entry.instructions, module.find_function("get_i32")->id);
    const auto flag_index = unique_call_index(entry.instructions, module.find_function(names[0])->id);
    const auto &branch = std::get<CondBranch>(entry.terminator->instruction);
    check(entry.instructions.size() == 2 && input_index < flag_index &&
              branch.condition == Operand::local(Type::I1, *entry.instructions[flag_index].result),
          "左操作数先求值，if 的条件只求值一次并直接用于 condbr");

    const auto print = module.find_function("println_i32")->id;
    const auto check_arm = [&](BlockId id, unsigned marker, unsigned value) {
        const auto &arm = main.block(id);
        check(arm.instructions.size() == 2, "分支应先打印，再写入整数尾值");
        const auto &call = std::get<Call>(arm.instructions.front().operation);
        const auto &store = std::get<Store>(arm.instructions.back().operation);
        check(call.function == print &&
                  call.arguments == std::vector{Operand::constant(Type::I32, marker)} &&
                  store.storage_type == Type::I32 && store.address == slot &&
                  store.value == Operand::constant(Type::I32, value),
              "各分支必须将自己的尾值写到同一个入口 slot");
        return std::get<Branch>(arm.terminator->instruction).target;
    };
    const auto merge_id = check_arm(branch.when_true, 1, 10);
    check(check_arm(branch.when_false, 2, 20) == merge_id,
          "两条正常结果边必须都写完再跳到同一个合流块");

    const auto &merge = main.block(merge_id);
    check(merge.instructions.size() == 3, "合流块先读取结果，再完成外层加法和打印");
    const auto &load = std::get<Load>(merge.instructions[0].operation);
    const auto &add = std::get<BinaryOperation>(merge.instructions[1].operation);
    const auto &call = std::get<Call>(merge.instructions[2].operation);
    check(load.storage_type == Type::I32 && load.address == slot &&
              add.operation == BinaryOp::Add &&
              add.left == Operand::local(Type::I32, *entry.instructions[input_index].result) &&
              add.right == Operand::local(Type::I32, *merge.instructions[0].result) &&
              call.function == print &&
              call.arguments == std::vector{Operand::local(Type::I32, *merge.instructions[1].result)},
          "加法必须使用入口的 SSA 值和合流块新读出的整数");
}

// 提前返回只结束当前路径：检查写槽、合流及后续代码只出现在正常路径中。
void check_if_early_return() {
    const auto crate = parse_crate(R"(
        fn single(flag: bool) -> i32 {
            if flag { println_i32(1); return 7; println_i32(99); 10 }
            else { 8 }
        }
        fn both(flag: bool) -> i32 {
            if flag { println_i32(2); return 11; }
            else { println_i32(3); return 12; };
            println_i32(99);
            13
        }
        fn both_typed(flag: bool) -> i32 {
            if flag { println_i32(2); return 11; 1 }
            else { println_i32(3); return 12; 2 }
        }
        fn condition() -> i32 {
            if { println_i32(4); return 9; true } { 1 } else { 2 }
        }
        fn main() {}
    )");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics) && diagnostics.str().empty(),
          "含提前返回的 if 必须成功生成并通过 IR 自检");

    const auto names = rx::assign_function_names(sema);
    const auto &module = artifact.module();
    const auto print = module.find_function("println_i32")->id;
    const auto check_return = [print](const BasicBlock &block, unsigned marker, unsigned value) {
        check(block.instructions.size() == 1,
              "提前返回的块只保留返回前的打印，不得写结果或生成后续指令");
        const auto &call = std::get<Call>(block.instructions.front().operation);
        check(call.function == print &&
                  call.arguments == std::vector{Operand::constant(Type::I32, marker)} &&
                  std::get<Return>(block.terminator->instruction).value ==
                      Operand::constant(Type::I32, value),
              "这条边必须保留原来的副作用，并以指定值直接返回");
    };

    const auto &single = *module.find_function(names[0]);
    check(single.blocks.size() == 4 && single.entry_allocas.size() == 2,
          "单支正常完成的整数 if 需要一个合流块和一个结果 slot");
    const auto &branch = std::get<CondBranch>(single.blocks.front().terminator->instruction);
    check_return(single.block(branch.when_true), 1, 7);
    const auto &normal = single.block(branch.when_false);
    check(normal.instructions.size() == 1, "只有正常分支写结果槽");
    const auto &store = std::get<Store>(normal.instructions.front().operation);
    const auto &result_slot = single.entry_allocas.back();
    const auto &merge = single.block(std::get<Branch>(normal.terminator->instruction).target);
    check(merge.instructions.size() == 1, "合流块只读出正常分支的结果");
    const auto &load = std::get<Load>(merge.instructions.front().operation);
    check(store.storage_type == Type::I32 && store.value == Operand::constant(Type::I32, 8) &&
              store.address == Operand::local(Type::Ptr, *result_slot.result) &&
              load.storage_type == Type::I32 && load.address == store.address &&
              std::get<Return>(merge.terminator->instruction).value ==
                  Operand::local(Type::I32, *merge.instructions.front().result),
          "合流块必须读回已写入的 slot，并返回这次读取的结果");

    // 即使分支的不可达尾值让静态类型是 i32，也要按实际的返回路径判断能否合流。
    for (std::size_t index : {1u, 2u}) {
        const auto &both = *module.find_function(names[index]);
        check(both.blocks.size() == 3, "两支都返回时不能生成没有正常前驱的合流块");
        if (index == 1)
            check(both.entry_allocas.size() == 1, "never if 不需要额外的结果槽");
        const auto &choice = std::get<CondBranch>(both.blocks.front().terminator->instruction);
        check_return(both.block(choice.when_true), 2, 11);
        check_return(both.block(choice.when_false), 3, 12);
    }

    const auto &condition = *module.find_function(names[3]);
    check(condition.blocks.size() == 1 && condition.entry_allocas.empty(),
          "条件提前返回时，不能建立外层 if 的分支或结果槽");
    check_return(condition.blocks.front(), 4, 9);
}

// 按条件跳转的目标检查短路、右侧求值和提前返回，不依赖基本块编号。
void check_short_circuit() {
    const auto crate = parse_crate(R"(
        fn left() -> bool { get_i32() != 0 }
        fn right() -> bool { get_i32() != 0 }
        fn and_value() -> bool { left() && right() }
        fn or_value() -> bool { left() || right() }
        fn and_left_exit() -> bool { ({ println_i32(1); return true; false }) && right() }
        fn or_left_exit() -> bool { ({ println_i32(1); return true; false }) || right() }
        fn and_right_exit() -> bool { left() && { println_i32(2); return false; true } }
        fn or_right_exit() -> bool { left() || { println_i32(2); return false; true } }
        fn main() {}
    )");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics) && diagnostics.str().empty(),
          "含短路和提前返回的模块必须通过 IR 自检");

    const auto names = rx::assign_function_names(sema);
    const auto &module = artifact.module();
    const auto left_function = module.find_function(names[0])->id;
    const auto right_function = module.find_function(names[1])->id;
    const auto print = module.find_function("println_i32")->id;
    const auto prints = [print](const Instruction &instruction, unsigned marker) {
        const auto &call = std::get<Call>(instruction.operation);
        return call.function == print &&
               call.arguments == std::vector{Operand::constant(Type::I32, marker)};
    };
    const auto check_paths = [&](const Function &function, bool is_and, bool right_exits) {
        check(function.blocks.size() == 4 && function.entry_allocas.size() == 1,
              "短路表达式需要入口结果槽和两个分支及合流块");
        const auto &allocation = function.entry_allocas.front();
        const auto &storage = std::get<Alloca>(allocation.operation);
        check(storage.storage_type == Type::I8 && storage.alignment == 1,
              "短路结果必须使用 bool 的 i8 存储形式");
        const auto slot = Operand::local(Type::Ptr, *allocation.result);
        const auto &entry = function.blocks.front();
        check(entry.instructions.size() == 1 &&
                  std::get<Call>(entry.instructions.front().operation).function == left_function,
              "条件块只调用一次左侧，不能提前求值右侧");
        const auto &choice = std::get<CondBranch>(entry.terminator->instruction);
        check(choice.condition == Operand::local(Type::I1, *entry.instructions.front().result),
              "条件跳转必须使用左侧返回的 i1");

        const auto &short_path = function.block(is_and ? choice.when_false : choice.when_true);
        const auto &right_path = function.block(is_and ? choice.when_true : choice.when_false);
        check(short_path.instructions.size() == 1, "短路分支只写入已决定的结果");
        const auto &short_store = std::get<Store>(short_path.instructions.front().operation);
        check(short_store.address == slot &&
                  short_store.value == Operand::constant(Type::I8, is_and ? 0 : 1),
              "&& 的 false 边或 || 的 true 边应写入各自的短路结果");
        const auto merge_id = std::get<Branch>(short_path.terminator->instruction).target;
        if (right_exits) {
            check(right_path.instructions.size() == 1 && prints(right_path.instructions.front(), 2) &&
                      std::get<Return>(right_path.terminator->instruction).value ==
                          Operand::constant(Type::I1, 0),
                  "右侧提前返回时保留副作用，但不能写槽或跳到合流块");
        } else {
            check(right_path.instructions.size() == 3 &&
                      std::get<Call>(right_path.instructions[0].operation).function == right_function,
                  "右侧分支应调用一次右侧，再将结果扩展并写槽");
            const auto &extend = std::get<ZExt>(right_path.instructions[1].operation);
            const auto &store = std::get<Store>(right_path.instructions[2].operation);
            check(extend.value == Operand::local(Type::I1, *right_path.instructions[0].result) &&
                      store.address == slot &&
                      store.value == Operand::local(Type::I8, *right_path.instructions[1].result) &&
                      std::get<Branch>(right_path.terminator->instruction).target == merge_id,
                  "右侧正常完成时必须用实际结果初始化同一个槽再合流");
        }
        const auto &merge = function.block(merge_id);
        check(merge.instructions.size() == 2, "合流块应该读槽并转换回 i1");
        const auto &load = std::get<Load>(merge.instructions[0].operation);
        const auto &trunc = std::get<Trunc>(merge.instructions[1].operation);
        check(load.address == slot &&
                  trunc.value == Operand::local(Type::I8, *merge.instructions[0].result) &&
                  std::get<Return>(merge.terminator->instruction).value ==
                      Operand::local(Type::I1, *merge.instructions[1].result),
              "合流块应把已写入的值作为短路表达式的 bool 结果");
    };

    for (std::size_t index : {0u, 1u}) {
        check_paths(*module.find_function(names[2 + index]), index == 0, false);
        check_paths(*module.find_function(names[6 + index]), index == 0, true);
        const auto &left_exit = *module.find_function(names[4 + index]);
        check(left_exit.blocks.size() == 1 && left_exit.entry_allocas.empty(),
              "左侧提前返回时不应创建右侧块、结果槽或合流块");
        const auto &entry = left_exit.blocks.front();
        check(entry.instructions.size() == 1 && prints(entry.instructions.front(), 1) &&
                  std::get<Return>(entry.terminator->instruction).value ==
                      Operand::constant(Type::I1, 1),
              "左侧提前返回时只保留原有副作用和返回值");
    }
}

// 检查 while 的回边、入口分配和提前返回，沿跳转找块而不依赖块编号。
void check_while() {
    const auto crate = parse_crate(R"(
        fn condition(x: i32) -> bool { x < 2 }
        fn count() {
            let mut x = 0;
            while condition(x) { let next = x + 1; x = next; }
            println_i32(x);
        }
        fn return_in_body(flag: bool) -> i32 {
            while flag { return 7; }
            8
        }
        fn return_in_condition() -> i32 {
            while ({ println_i32(1); return 9; true }) { println_i32(99); }
            10
        }
        fn main() {}
    )");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics) && diagnostics.str().empty(),
          "源码 while 及其回边必须通过 IR 自检");

    const auto names = rx::assign_function_names(sema);
    const auto &module = artifact.module();
    const auto print = module.find_function("println_i32")->id;
    const auto &count = *module.find_function(names[1]);
    check(count.blocks.size() == 4 && count.entry_allocas.size() == 2,
          "while 只需条件、循环体和退出块，x 和 next 的槽都应位于入口");
    const auto x = Operand::local(Type::Ptr, *count.entry_allocas[0].result);
    const auto next = Operand::local(Type::Ptr, *count.entry_allocas[1].result);
    const auto &entry = count.blocks.front();
    check(entry.instructions.size() == 1, "进入循环前只初始化 x，不能提前初始化 next");
    const auto &initial = std::get<Store>(entry.instructions.front().operation);
    check(initial.address == x && initial.value == Operand::constant(Type::I32, 0),
          "入口必须在第一次判断前初始化 x");

    const auto condition_id = std::get<Branch>(entry.terminator->instruction).target;
    const auto &condition = count.block(condition_id);
    check(condition.instructions.size() == 2, "条件块应重新读取 x 并调用条件函数");
    const auto &read = std::get<Load>(condition.instructions[0].operation);
    const auto &call = std::get<Call>(condition.instructions[1].operation);
    const auto &choice = std::get<CondBranch>(condition.terminator->instruction);
    check(read.address == x && call.function == module.find_function(names[0])->id &&
              call.arguments == std::vector{Operand::local(Type::I32, *condition.instructions[0].result)} &&
              choice.condition == Operand::local(Type::I1, *condition.instructions[1].result),
          "每轮必须用当前的 x 重新求条件，并用这次结果选择路径");

    const auto &body = count.block(choice.when_true);
    std::vector<Operand> written_addresses;
    for (const auto &instruction : body.instructions) {
        check(!std::holds_alternative<Alloca>(instruction.operation), "循环体不能动态分配槽");
        if (const auto *store = std::get_if<Store>(&instruction.operation))
            written_addresses.push_back(store->address);
    }
    check(written_addresses == std::vector{next, x} &&
              std::get<Branch>(body.terminator->instruction).target == condition_id,
          "每轮在体内初始化 next、写回 x，再跳回条件块");
    const auto &exit = count.block(choice.when_false);
    check(exit.instructions.size() == 2 &&
              std::get<Load>(exit.instructions[0].operation).address == x &&
              std::get<Call>(exit.instructions[1].operation).function == print &&
              !std::get<Return>(exit.terminator->instruction).value,
          "false 边必须继续执行循环后的代码，while 自身不产生返回值");

    const auto &body_exit = *module.find_function(names[2]);
    const auto body_test = std::get<Branch>(body_exit.blocks.front().terminator->instruction).target;
    const auto &body_choice = std::get<CondBranch>(body_exit.block(body_test).terminator->instruction);
    const auto &early = body_exit.block(body_choice.when_true);
    const auto &after = body_exit.block(body_choice.when_false);
    check(std::get<Return>(early.terminator->instruction).value == Operand::constant(Type::I32, 7) &&
              std::get<Return>(after.terminator->instruction).value == Operand::constant(Type::I32, 8),
          "循环体返回时不发回边，但条件为 false 时仍能正常退出");

    const auto &condition_exit = *module.find_function(names[3]);
    check(condition_exit.blocks.size() == 2, "条件提前返回时不能生成循环体或退出块");
    const auto test = std::get<Branch>(condition_exit.blocks.front().terminator->instruction).target;
    const auto &returning = condition_exit.block(test);
    check(returning.instructions.size() == 1 &&
              std::get<Call>(returning.instructions.front().operation).arguments ==
                  std::vector{Operand::constant(Type::I32, 1)} &&
              std::get<Return>(returning.terminator->instruction).value == Operand::constant(Type::I32, 9),
          "条件提前返回时只保留返回前的副作用，不发条件分支");
}

// 分别核对语义身份和实际跳转：退出内层 loop 后，外层 while 的两个目标仍然有效。
void check_loop_targets() {
    const auto crate = parse_crate(R"(
        fn main() {
            let again = get_i32() != 0;
            while again {
                loop {
                    if again { continue; }
                    break;
                }
                if again { continue; }
                break;
            }
            println_i32(7);
        }
    )");
    const auto sema = analyze(*crate);
    check(sema.loops.size() == 2 && sema.while_expressions.size() == 1 &&
              sema.loop_expressions.size() == 1,
          "内外层循环必须各有一个语义身份");
    const auto while_id = sema.while_expressions.begin()->second;
    const auto loop_id = sema.loop_expressions.begin()->second;
    check(while_id != loop_id && sema.loops.at(while_id.index).kind == rx::LoopFact::Kind::While &&
              sema.loops.at(loop_id.index).kind == rx::LoopFact::Kind::Loop,
          "内外层的 LoopId 不能混用");
    const auto has_both_targets = [while_id, loop_id](const auto &targets) {
        std::set<rx::LoopId> ids;
        for (const auto &entry : targets)
            ids.insert(entry.second);
        return targets.size() == 2 && ids == std::set{while_id, loop_id};
    };
    check(has_both_targets(sema.break_targets) && has_both_targets(sema.continue_targets),
          "内外层各自的 break 和 continue 必须记录到不同的循环");

    const auto artifact = generate(sema);
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics) && diagnostics.str().empty(),
          "嵌套循环的跳转必须通过 IR 自检");
    const auto names = rx::assign_function_names(sema);
    const auto &module = artifact.module();
    const auto &main = *module.find_function(names[sema.main_function->index]);
    check(main.entry_allocas.size() == 1, "unit 循环不能分配结果槽");

    const auto jump_target = [](const BasicBlock &block) {
        return std::get<Branch>(block.terminator->instruction).target;
    };
    const auto while_cond = jump_target(main.blocks.front());
    const auto &while_branch = std::get<CondBranch>(main.block(while_cond).terminator->instruction);
    const auto loop_body = jump_target(main.block(while_branch.when_true));
    const auto &inner = std::get<CondBranch>(main.block(loop_body).terminator->instruction);
    check(jump_target(main.block(inner.when_true)) == loop_body,
          "loop 的 continue 必须跳回自己的体");

    // 内层 break 落在外层体的后半段，而不是 while 的出口。
    const auto loop_exit = jump_target(main.block(inner.when_false));
    check(loop_exit != while_branch.when_false, "内层 break 不能退出外层 while");
    const auto &outer = std::get<CondBranch>(main.block(loop_exit).terminator->instruction);
    check(jump_target(main.block(outer.when_true)) == while_cond &&
              jump_target(main.block(outer.when_false)) == while_branch.when_false,
          "外层 continue 回到条件，外层 break 和条件 false 使用同一出口");
}

// 无出口的 loop 不制造合流块，也不生成循环之后的代码。
void check_loop_without_exit() {
    const auto crate = parse_crate(R"(
        fn forever() -> i32 {
            let x = loop {};
            println_i32(99);
            8
        }
        fn return_before_break() -> i32 {
            loop { return 7; break; }
            8
        }
        fn main() {}
    )");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics) && diagnostics.str().empty(),
          "无出口循环必须通过 IR 自检");
    const auto names = rx::assign_function_names(sema);
    const auto &module = artifact.module();
    const auto &forever = *module.find_function(names[0]);
    check(forever.blocks.size() == 2 && forever.entry_allocas.empty(),
          "空 loop 只有入口和循环体，不给不可达的 let 分配槽");
    const auto body = std::get<Branch>(forever.blocks.front().terminator->instruction).target;
    check(body != forever.blocks.front().id && forever.block(body).instructions.empty() &&
              std::get<Branch>(forever.block(body).terminator->instruction).target == body,
          "无 break 的空 loop 必须跳回自身，不能生成后续打印或返回");
    const auto &early = *module.find_function(names[1]);
    check(early.blocks.size() == 2, "return 后未发射的 break 不能创建循环出口");
    const auto target = std::get<Branch>(early.blocks.front().terminator->instruction).target;
    check(std::get<Return>(early.block(target).terminator->instruction).value ==
              Operand::constant(Type::I32, 7),
          "循环体已经 return 时不能补回边或继续执行循环外的尾值");
}

// 沿实际退出边检查写槽和读回；操作数发散时不能发出 break 边。
void check_loop_results() {
    const auto crate = parse_crate(R"(
        fn choose(flag: bool) -> i32 {
            loop { if flag { break get_i32(); } break 8; }
        }
        fn no_result_edge() -> i32 {
            loop { break { println_i32(1); return 9; 7 }; }
        }
        fn never_result() -> i32 {
            loop { break { return 10; }; }
        }
        fn main() {}
    )");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics) && diagnostics.str().empty(),
          "带值 break 和发散的 break 必须通过 IR 自检");
    const auto names = rx::assign_function_names(sema);
    const auto &module = artifact.module();
    const auto target = [](const BasicBlock &block) {
        return std::get<Branch>(block.terminator->instruction).target;
    };

    const auto &choose = *module.find_function(names[0]);
    check(choose.entry_allocas.size() == 2, "形参槽之外只分配一个整数结果槽");
    const auto &allocation = choose.entry_allocas.back();
    check(std::get<Alloca>(allocation.operation).storage_type == Type::I32,
          "两个整数结果共用一个入口 i32 槽");
    const auto slot = Operand::local(Type::Ptr, *allocation.result);
    const auto &body = choose.block(target(choose.blocks.front()));
    const auto &branch = std::get<CondBranch>(body.terminator->instruction);
    const auto &first = choose.block(branch.when_true);
    const auto &second = choose.block(branch.when_false);
    check(first.instructions.size() == 2 && second.instructions.size() == 1 &&
              std::get<Call>(first.instructions.front().operation).function ==
                  module.find_function("get_i32")->id,
          "只在实际选中的退出边执行 break 操作数，且只求值一次");
    const auto &first_store = std::get<Store>(first.instructions.back().operation);
    const auto &second_store = std::get<Store>(second.instructions.back().operation);
    const auto exit_id = target(first);
    check(first_store.address == slot && second_store.address == slot &&
              first_store.value == Operand::local(Type::I32, *first.instructions.front().result) &&
              second_store.value == Operand::constant(Type::I32, 8) && target(second) == exit_id,
          "两条 break 边必须先写入各自的结果，再跳向同一个出口");
    const auto &exit = choose.block(exit_id);
    check(exit.instructions.size() == 1 &&
              std::get<Load>(exit.instructions.front().operation).address == slot &&
              std::get<Return>(exit.terminator->instruction).value ==
                  Operand::local(Type::I32, *exit.instructions.front().result),
          "循环正常结束后才能读出结果并交给外层函数返回");

    const auto &no_result_edge = *module.find_function(names[1]);
    const auto &early = no_result_edge.block(target(no_result_edge.blocks.front()));
    check(no_result_edge.blocks.size() == 2 && early.instructions.size() == 1 &&
              std::get<Call>(early.instructions.front().operation).function ==
                  module.find_function("println_i32")->id &&
              std::get<Return>(early.terminator->instruction).value == Operand::constant(Type::I32, 9),
          "整数操作数先 return 时，只保留副作用和 ret，不写槽或创建出口");
    const auto &never = *module.find_function(names[2]);
    const auto &never_body = never.block(target(never.blocks.front()));
    check(never.entry_allocas.empty() && never.blocks.size() == 2 && never_body.instructions.empty() &&
              std::get<Return>(never_body.terminator->instruction).value == Operand::constant(Type::I32, 10),
          "所有 break 操作数都是 never 时不能分配数据槽或伪造结果");
}

// 从实际跳转目标检查组合控制流，并区分递归调用的源 main 和机器入口。
void check_control_flow_integration() {
    const auto crate = parse_crate(R"(
        fn sum_to(limit: i32) -> i32 {
            let mut i = 0;
            let mut sum = 0;
            while i < limit {
                i += 1;
                if i == 2 { continue; println_i32(99); }
                sum += i;
            }
            loop {
                if sum > 0 { break sum; println_i32(99); }
                return 0;
                println_i32(99);
            }
        }
        fn main() {
            let depth = get_i32();
            if depth > 0 { main(); }
            println_i32(depth);
        }
    )");
    const auto sema = analyze(*crate);
    const auto artifact = generate(sema);
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics) && diagnostics.str().empty(),
          "组合循环和递归调用必须通过 IR 自检");
    const auto names = rx::assign_function_names(sema);
    const auto &module = artifact.module();
    const auto target = [](const BasicBlock &block) {
        return std::get<Branch>(block.terminator->instruction).target;
    };
    const auto &sum = *module.find_function(names[0]);
    const auto condition = target(sum.blocks.front());
    const auto &while_branch = std::get<CondBranch>(sum.block(condition).terminator->instruction);
    const auto &skip = std::get<CondBranch>(sum.block(while_branch.when_true).terminator->instruction);
    const auto &continue_block = sum.block(skip.when_true);
    const auto &update_block = sum.block(skip.when_false);
    check(continue_block.instructions.empty() && target(continue_block) == condition &&
              !update_block.instructions.empty() && target(update_block) == condition,
          "continue 跳过累加和后续打印；两条回边都重新计算 while 条件");

    const auto loop_body = target(sum.block(while_branch.when_false));
    const auto &stop = std::get<CondBranch>(sum.block(loop_body).terminator->instruction);
    const auto &break_block = sum.block(stop.when_true);
    check(break_block.instructions.size() == 2, "break 只读 sum 并写结果，不生成后续打印");
    const auto &store = std::get<Store>(break_block.instructions.back().operation);
    const auto exit_id = target(break_block);
    const auto &exit = sum.block(exit_id);
    for (const auto &block : sum.blocks) {
        const auto &terminator = block.terminator->instruction;
        if (const auto *jump = std::get_if<Branch>(&terminator))
            check(jump->target != exit_id || block.id == break_block.id,
                  "只有写入结果的 break 块能跳到 loop 出口");
        if (const auto *choice = std::get_if<CondBranch>(&terminator))
            check(choice->when_true != exit_id && choice->when_false != exit_id,
                  "条件分支不能绕过 break 的写槽直接进入 loop 出口");
    }
    check(exit.instructions.size() == 1 &&
              std::get<Load>(exit.instructions.front().operation).address == store.address &&
              std::get<Return>(exit.terminator->instruction).value ==
                  Operand::local(Type::I32, *exit.instructions.front().result),
          "loop 的正常出口读出 break 写入的结果，交给函数尾部返回");
    const auto &return_block = sum.block(stop.when_false);
    check(return_block.instructions.empty() &&
              std::get<Return>(return_block.terminator->instruction).value ==
                  Operand::constant(Type::I32, 0),
          "提前 return 不写入结果槽、不跳向 loop 出口，也不生成后续打印");

    const auto &source_main = *module.find_function(names[sema.main_function->index]);
    const auto &recurse = std::get<CondBranch>(source_main.blocks.front().terminator->instruction);
    const auto &recursive_block = source_main.block(recurse.when_true);
    check(source_main.linkage == Linkage::Internal &&
              source_main.signature == Signature{Type::Void, {}} &&
              recursive_block.instructions.size() == 1 &&
              std::get<Call>(recursive_block.instructions.front().operation).function == source_main.id &&
              target(recursive_block) == recurse.when_false,
          "递归调用指向内部源 main，返回后继续执行外层源 main");
    const auto &entry = *module.find_function("main");
    const auto &wrapper = entry.blocks.front();
    check(entry.linkage == Linkage::External && entry.signature == Signature{Type::I32, {}} &&
              wrapper.instructions.size() == 1 &&
              std::get<Call>(wrapper.instructions.front().operation).function == source_main.id &&
              std::get<Return>(wrapper.terminator->instruction).value == Operand::constant(Type::I32, 0),
          "只有独立的机器入口负责调用源 main 并返回状态 0");
}

void run_tests() {
    check_function_names();
    check_named_storage_layouts();
    check_println();
    check_locals();
    check_scalar_constants();
    check_unit_calls_and_blocks();
    check_integer_calls_and_returns();
    check_early_return();
    check_bool_boundary();
    check_mixed_scalar_storage();
    check_scalar_operations();
    check_shifts_and_casts();
    check_reference_places();
    check_reference_temporaries();
    check_reference_early_return();
    check_reference_coercions();
    check_coercion_argument_order();
    check_reference_binary_order();
    check_mixed_reference_operand_order();
    check_reference_unary_operations();
    check_reference_ordering_chains();
    check_reference_operation_early_return();
    check_reference_operation_boundaries();
    check_assignment_order();
    check_assignment_coercion_order();
    check_assignment_early_return();
    check_assignment_unit_tails();
    check_unit_if();
    check_integer_if_merge();
    check_if_early_return();
    check_short_circuit();
    check_while();
    check_loop_targets();
    check_loop_without_exit();
    check_loop_results();
    check_control_flow_integration();
    check_entry("fn main() {}");
    check_entry("fn main() -> () {}");
    check_entry("fn __rx_fn_1() {} fn main() {} fn helper() {}");
    check_entry("const LEN: usize = 2; struct Data { values: [i32; LEN] } impl Data { const VALUE: i32 = 7; } fn main() {}");
    for (const auto &source : {
             "fn main() { Box::<i32>::new(1); }",
             "fn main() { Vec::<i32>::new(); }",
             "fn main() { let x = { (); }; }",
             "fn main() { let x = { [1, 2] }; }",
             "fn main() { let mut x = [1]; x = [2]; }",
             "struct Data { value: i32 } fn main() { let mut x = Data { value: 1 }; x = Data { value: 2 }; }",
             "fn unused(value: &[i32; 1]) {} fn main() {}",
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
        if (argc == 2 && std::string_view(argv[1]) == "--emit-operations") {
            const auto artifact = make_scalar_operation_driver();
            return artifact.verify_and_print(std::cout, std::cerr) ? 0 : 1;
        }
        return 2;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
