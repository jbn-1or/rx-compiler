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
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <iostream>
#include <iterator>
#include <map>
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

template <class T, class U> const T &checked_ast(const U *node) {
    const auto *typed = dynamic_cast<const T *>(node);
    check(typed != nullptr, "测试 AST 节点类型不匹配");
    return *typed;
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

void check_local_slots() {
    const auto crate = parse_crate(R"(
        fn helper(flag: bool, count: u32) { let local = false; }
        fn main() {
            let x = 1;
            let x = 2;
            let flag = true;
        }
    )");
    const auto sema = analyze(*crate);
    check(sema.main_function && sema.main_function->index == 1 &&
              sema.locals.size() == 6,
          "参数及遮蔽的 let 必须拥有各自的 LocalId");
    const auto &helper = checked_ast<rx::Function>(crate->items.at(0).get());
    const auto &main = checked_ast<rx::Function>(crate->items.at(1).get());
    const auto &helper_let = checked_ast<rx::LetStatement>(
        helper.body->statements.at(0).get());
    const auto &first_let = checked_ast<rx::LetStatement>(
        main.body->statements.at(0).get());
    const auto &second_let = checked_ast<rx::LetStatement>(
        main.body->statements.at(1).get());
    const auto &bool_let = checked_ast<rx::LetStatement>(
        main.body->statements.at(2).get());
    const auto first_id = sema.let_bindings.at(&first_let);
    const auto second_id = sema.let_bindings.at(&second_let);
    const auto helper_let_id = sema.let_bindings.at(&helper_let);
    check(first_id != second_id && helper_let_id != first_id &&
              helper_let_id != second_id,
          "不同 let（含遮蔽）必须对应不同 LocalId");

    std::string errors;
    llvm::raw_string_ostream diagnostics(errors);
    auto artifact = rx::generate_ir(*crate, sema, diagnostics);
    diagnostics.flush();
    check(artifact && errors.empty(), "常量 let 和参数应生成 IR");
    auto &module = artifact->module();
    const auto *helper_ir = module.getFunction("__rx_fn_0");
    const auto *main_ir = module.getFunction("__rx_fn_1");
    check(helper_ir && main_ir && !llvm::verifyModule(module, &diagnostics),
          "含 LocalId slot 的模块必须通过 verifier");

    const auto &helper_locals = sema.function_locals.at(sema.functions[0]->id);
    check(helper_locals.parameters.size() == 2,
          "helper 参数编号应按声明顺序记录");
    std::set<const llvm::Value *> all_slots;
    auto check_alloca_prefix = [&](const llvm::Function &function,
                                   std::size_t expected_count) {
        bool non_alloca_seen = false;
        std::size_t count = 0;
        for (const auto &instruction : function.getEntryBlock()) {
            if (const auto *allocation = llvm::dyn_cast<llvm::AllocaInst>(&instruction)) {
                check(!non_alloca_seen, "所有固定 slot 必须集中在函数入口");
                check(all_slots.insert(allocation).second,
                      "不同 LocalId 必须对应不同地址");
                ++count;
            } else {
                non_alloca_seen = true;
            }
        }
        check(count == expected_count, "入口 alloca 数量必须与函数 LocalId 数量相同");
    };
    check_alloca_prefix(*helper_ir, 3);
    check_alloca_prefix(*main_ir, 3);

    const auto slot_name = [](rx::LocalId id) {
        return "local." + std::to_string(id.index) + ".slot";
    };
    for (const auto id : helper_locals.parameters) {
        const auto name = slot_name(id);
        const auto found = std::find_if(
            helper_ir->getEntryBlock().begin(), helper_ir->getEntryBlock().end(),
            [&](const llvm::Instruction &instruction) {
                const auto *allocation = llvm::dyn_cast<llvm::AllocaInst>(&instruction);
                return allocation && allocation->getName() == name;
            });
        check(found != helper_ir->getEntryBlock().end(),
              "参数 LocalId 应能找到按 ID 命名的固定 slot");
        const auto *allocation = llvm::cast<llvm::AllocaInst>(&*found);
        const auto &binding = sema.locals.at(id.index);
        check(allocation->getAllocatedType()->isIntegerTy(
                  binding.type.kind == rx::SemanticType::Kind::Bool ? 8 : 32),
              "参数 slot 必须使用正确的存储表示");
    }
    bool saw_bool_argument_store = false;
    bool saw_integer_argument_store = false;
    for (const auto &instruction : helper_ir->getEntryBlock()) {
        const auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
        if (!store)
            continue;
        if (store->getPointerOperand()->getName() == slot_name(helper_locals.parameters[0])) {
            const auto *extension = llvm::dyn_cast<llvm::ZExtInst>(store->getValueOperand());
            saw_bool_argument_store = extension &&
                                      extension->getOperand(0) ==
                                          &*helper_ir->arg_begin();
        }
        if (store->getPointerOperand()->getName() == slot_name(helper_locals.parameters[1]))
            saw_integer_argument_store =
                store->getValueOperand() == &*std::next(helper_ir->arg_begin());
    }
    check(saw_bool_argument_store && saw_integer_argument_store,
          "函数入口必须把每个参数写入其 LocalId 对应 slot");

    std::map<uint64_t, const llvm::Value *> integer_stores;
    for (const auto &instruction : main_ir->getEntryBlock()) {
        const auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
        const auto *value = store ? llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand())
                                  : nullptr;
        if (value && value->getType()->isIntegerTy(32))
            integer_stores.emplace(value->getZExtValue(), store->getPointerOperand());
    }
    check(integer_stores.size() == 2 && integer_stores.at(1) != integer_stores.at(2),
          "遮蔽的 let 初始化值必须写入各自独立的 slot");
    check(integer_stores.at(1)->getName() == slot_name(first_id) &&
              integer_stores.at(2)->getName() == slot_name(second_id),
          "初始化 store 必须根据 let 的 LocalId 写入对应 slot");

    const auto bool_id = sema.let_bindings.at(&bool_let);
    const auto bool_name = slot_name(bool_id);
    const auto bool_allocation = std::find_if(
        main_ir->getEntryBlock().begin(), main_ir->getEntryBlock().end(),
        [&](const llvm::Instruction &instruction) {
            const auto *allocation = llvm::dyn_cast<llvm::AllocaInst>(&instruction);
            return allocation && allocation->getName() == bool_name;
        });
    check(bool_allocation != main_ir->getEntryBlock().end() &&
              llvm::cast<llvm::AllocaInst>(&*bool_allocation)->getAllocatedType()->isIntegerTy(8),
          "bool 绑定应分配 i8 slot");
    bool bool_store_found = false;
    for (const auto &instruction : main_ir->getEntryBlock()) {
        const auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
        const auto *stored = store ? llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand())
                                   : nullptr;
        bool_store_found |= store && stored && stored->getType()->isIntegerTy(8) &&
                            store->getPointerOperand()->getName() == bool_name &&
                            store->getAlign() == llvm::Align(1);
    }
    check(bool_store_found, "bool 字面量应在 let 位置 zext 后写入 i8 slot");
    const auto *bool_initializer = dynamic_cast<const rx::BooleanLiteral *>(
        bool_let.initializer.get());
    check(bool_initializer && sema.locals.at(bool_id.index)
                                  .type.kind == rx::SemanticType::Kind::Bool,
          "bool let 应保留语义绑定类型");
    check(!llvm::verifyModule(module, &diagnostics), "整数/bool slot 模块 verifier 应通过");
}

void check_calls_returns_and_blocks() {
    const auto crate = parse_crate(R"(
        const ANSWER: i32 = 7;
        fn main() {
            {
                let n = answer();
                println_i32(n);
            }
            unit_call();
            println_i32(4);
        }

        fn answer() -> i32 {
            return 7;
            println_i32(99);
        }

        fn echo(flag: bool) -> bool { flag }
        fn relay(flag: bool) -> bool { echo(flag) }
        fn unit_call() {}
        fn later() { unit_call(); }
        fn many(a: i32, b: i32, c: i32, d: i32, e: i32,
                f: i32, g: i32, h: i32, i: i32) -> i32 { a }
        fn many_call() { many(1, 2, 3, 4, 5, 6, 7, 8, 9); }
        fn constant() { println_i32(ANSWER); }
        fn first() -> i32 { 1 }
        fn second() -> i32 { 2 }
        fn pair(a: i32, b: i32) -> i32 { a }
        fn ordered() { pair(first(), second()); }
        fn consume(value: i32) {}
        fn early() -> i32 { consume(return 7); 9 }
    )");
    const auto sema = analyze(*crate);
    std::string errors;
    llvm::raw_string_ostream diagnostics(errors);
    auto artifact = rx::generate_ir(*crate, sema, diagnostics);
    diagnostics.flush();
    check(artifact && errors.empty(), "普通调用、返回和嵌套块应成功生成 IR");
    auto &module = artifact->module();
    check(!llvm::verifyModule(module, &diagnostics), "P3.3 模块必须通过 verifier");

    const auto function = [&](const std::string &name) {
        const auto found = std::find_if(
            sema.functions.begin(), sema.functions.end(),
            [&](const auto &symbol) { return symbol->name == name; });
        check(found != sema.functions.end(), "测试函数应存在于语义符号表");
        return module.getFunction("__rx_fn_" + std::to_string((*found)->id.index));
    };
    const auto *main = function("main");
    const auto *answer = function("answer");
    check(main && answer && answer->getReturnType()->isIntegerTy(32),
          "前向调用目标必须按 FunctionId 生成 i32 函数");
    const auto *answer_return = answer && !answer->empty()
                                    ? llvm::dyn_cast<llvm::ReturnInst>(
                                          &answer->getEntryBlock().front())
                                    : nullptr;
    const auto *answer_value = answer_return
                                   ? llvm::dyn_cast<llvm::ConstantInt>(
                                         answer_return->getReturnValue())
                                   : nullptr;
    check(answer && answer->getEntryBlock().size() == 1 && answer_return && answer_value &&
              answer_value->getSExtValue() == 7,
          "return 后的语句不能在当前路径继续生成");
    const auto *print = module.getFunction("println_i32");
    check(print && print->isDeclaration() && print->arg_size() == 1 &&
              print->getReturnType()->isVoidTy(),
          "println_i32 应按内置签名声明为外部函数");
    bool saw_forward_call = false;
    bool saw_local_load = false;
    bool saw_print = false;
    bool saw_unit_call = false;
    for (const auto &instruction : llvm::instructions(main)) {
        if (const auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
            saw_forward_call |= call->getCalledFunction() == answer;
            saw_print |= call->getCalledFunction() == print;
            saw_unit_call |= call->getCalledFunction() == function("unit_call") &&
                             call->getType()->isVoidTy();
        }
        saw_local_load |= llvm::isa<llvm::LoadInst>(&instruction);
    }
    check(saw_forward_call && saw_local_load && saw_print && saw_unit_call,
          "前向调用、局部读取、unit 调用后继续执行和内置调用应生成");

    const auto *relay = function("relay");
    const auto *echo = function("echo");
    check(relay && echo && relay->getReturnType()->isIntegerTy(1) &&
              echo->getReturnType()->isIntegerTy(1),
          "bool 参数和返回值应保持 i1");
    bool saw_bool_call = false;
    for (const auto &instruction : llvm::instructions(relay))
        if (const auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction))
            saw_bool_call |= call->getCalledFunction() == echo &&
                             call->getArgOperand(0)->getType()->isIntegerTy(1);
    check(saw_bool_call, "bool 调用参数必须按 i1 传递");

    const auto *many = function("many");
    check(many && many->arg_size() == 9 && many->getReturnType()->isIntegerTy(32),
          "超过八个参数的函数签名也必须完整生成");
    const auto *many_call = function("many_call");
    bool saw_nine_argument_call = false;
    for (const auto &instruction : llvm::instructions(many_call))
        if (const auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction))
            saw_nine_argument_call |= call->getCalledFunction() == many &&
                                      call->arg_size() == 9;
    check(saw_nine_argument_call, "超过八个整数实参的普通调用必须完整发射");

    const auto *constant = function("constant");
    bool saw_constant_operand = false;
    for (const auto &instruction : llvm::instructions(constant))
        if (const auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction))
            if (call->getCalledFunction() == print)
                saw_constant_operand =
                    llvm::isa<llvm::ConstantInt>(call->getArgOperand(0)) &&
                    llvm::cast<llvm::ConstantInt>(call->getArgOperand(0))->getSExtValue() == 7;
    check(saw_constant_operand, "常量路径应直接读取 sema 求值结果");

    const auto *ordered = function("ordered");
    const auto *first = function("first");
    const auto *second = function("second");
    const auto *pair = function("pair");
    std::vector<const llvm::Function *> call_order;
    for (const auto &instruction : llvm::instructions(ordered))
        if (const auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction))
            call_order.push_back(call->getCalledFunction());
    check(call_order.size() == 3 && call_order[0] == first &&
              call_order[1] == second && call_order[2] == pair &&
              call_order[2]->arg_size() == 2,
          "多个实参必须从左到右求值，然后只发出一次目标调用");

    const auto *early = function("early");
    check(early && early->getEntryBlock().size() == 1 &&
              llvm::isa<llvm::ReturnInst>(early->getEntryBlock().front()) &&
              llvm::cast<llvm::ReturnInst>(early->getEntryBlock().front())
                      .getReturnValue()->getType()->isIntegerTy(32),
          "发散实参生成 return 后不得发出调用或后续尾值");

    const auto recursive = parse_crate(R"(
        fn main() { first(); }
        fn first() { second(); }
        fn second() { first(); }
    )");
    const auto recursive_sema = analyze(*recursive);
    auto recursive_module = rx::generate_ir(*recursive, recursive_sema, diagnostics);
    check(recursive_module && !llvm::verifyModule(recursive_module->module(), &diagnostics),
          "相互递归函数应先登记签名再生成调用");
    diagnostics.flush();
    check(errors.empty(), "有效 P3.3 用例不应产生生成诊断");
}

void check_never_let() {
    for (const auto &source : {
             "fn main() { let x = return; println_i32(99); }",
             "fn main() { let x = { return; }; let y = x; println_i32(99); }",
             "fn main() {} fn early() -> i32 { let x = return 7; println_i32(99); 9 }",
             "fn main() {} fn early() -> bool { let x = return true; false }"}) {
        const auto crate = parse_crate(source);
        const auto sema = analyze(*crate);
        check(!sema.locals.empty(), "Never let 用例应保留局部身份");
        for (const auto &local : sema.locals)
            check(local.type.is_never(), "无标注的发散初始化器应推导出 Never 绑定");
        std::string errors;
        llvm::raw_string_ostream diagnostics(errors);
        auto artifact = rx::generate_ir(*crate, sema, diagnostics);
        check(artifact && errors.empty() &&
                  !llvm::verifyModule(artifact->module(), &diagnostics),
              "Never let 应生成合法模块而非尝试分配存储");
        for (const auto &symbol : sema.functions) {
            const auto *function = artifact->module().getFunction(
                "__rx_fn_" + std::to_string(symbol->id.index));
            check(function && function->getEntryBlock().size() == 1,
                  "Never 初始化器之后不得发出 slot、store、调用或尾值");
            const auto *ret = llvm::dyn_cast<llvm::ReturnInst>(
                &function->getEntryBlock().front());
            check(ret != nullptr, "源函数只能发出对应的 return");
            if (symbol->return_type.is_unit()) {
                check(!ret->getReturnValue(), "unit return 不得伪造返回数据");
            } else {
                const auto *value = llvm::dyn_cast<llvm::ConstantInt>(ret->getReturnValue());
                check(value && value->getZExtValue() ==
                          (symbol->return_type.kind == rx::SemanticType::Kind::Bool ? 1u : 7u),
                      "非 unit 初始化器中的 return 必须保留真实返回值");
            }
        }
    }

    // 标注保持绑定的具体类型，但初始化器终止后仍不应 store 或发出后续调用。
    const auto crate = parse_crate("fn main() { let x: i32 = return; println_i32(99); }");
    const auto sema = analyze(*crate);
    check(sema.locals.size() == 1 && sema.locals[0].type.kind == rx::SemanticType::Kind::Int,
          "显式类型标注不能被初始化器的 Never 类型覆盖");
    std::string errors;
    llvm::raw_string_ostream diagnostics(errors);
    auto artifact = rx::generate_ir(*crate, sema, diagnostics);
    check(artifact && errors.empty() &&
              !llvm::verifyModule(artifact->module(), &diagnostics),
          "有具体标注的发散初始化器应正常生成");
    const auto *main = artifact->module().getFunction(
        "__rx_fn_" + std::to_string(sema.main_function->index));
    check(main && main->getEntryBlock().size() == 2 &&
              llvm::isa<llvm::AllocaInst>(main->getEntryBlock().front()) &&
              llvm::isa<llvm::ReturnInst>(main->getEntryBlock().back()),
          "具体类型可以预留 slot，但不能在 return 后写入或继续执行");
}

void check_scalar_lowering() {
    const auto crate = parse_crate(R"(
        fn add(a: i32, b: i32) -> i32 { a + b }
        fn subtract(a: i32, b: i32) -> i32 { a - b }
        fn multiply(a: i32, b: i32) -> i32 { a * b }
        fn signed_divide(a: i32, b: i32) -> i32 { a / b }
        fn signed_remainder(a: isize, b: isize) -> isize { a % b }
        fn unsigned_divide(a: u32, b: u32) -> u32 { a / b }
        fn unsigned_remainder(a: usize, b: usize) -> usize { a % b }
        fn integer_and(a: u32, b: u32) -> u32 { a & b }
        fn integer_or(a: i32, b: i32) -> i32 { a | b }
        fn integer_xor(a: i32, b: i32) -> i32 { a ^ b }
        fn boolean_and(a: bool, b: bool) -> bool { a & b }
        fn boolean_or(a: bool, b: bool) -> bool { a | b }
        fn boolean_xor(a: bool, b: bool) -> bool { a ^ b }
        fn negate(a: i32) -> i32 { -a }
        fn invert_integer(a: u32) -> u32 { !a }
        fn invert_boolean(a: bool) -> bool { !a }
        fn equal(a: i32, b: i32) -> bool { a == b }
        fn not_equal(a: bool, b: bool) -> bool { a != b }
        fn signed_less(a: isize, b: isize) -> bool { a < b }
        fn unsigned_less(a: u32, b: u32) -> bool { a < b }
        fn boolean_less(a: bool, b: bool) -> bool { a < b }
        fn signed_ge(a: i32, b: i32) -> bool { a >= b }
        fn main() {}
    )");
    const auto sema = analyze(*crate);
    std::string errors;
    llvm::raw_string_ostream diagnostics(errors);
    auto artifact = rx::generate_ir(*crate, sema, diagnostics);
    diagnostics.flush();
    check(artifact && errors.empty() &&
              !llvm::verifyModule(artifact->module(), &diagnostics),
          "标量运算模块应生成成功并通过 verifier");
    const auto get_function = [&](const std::string &name) {
        const auto found = std::find_if(
            sema.functions.begin(), sema.functions.end(),
            [&](const auto &symbol) { return symbol->name == name; });
        check(found != sema.functions.end(), "标量运算测试函数必须存在");
        return artifact->module().getFunction(
            "__rx_fn_" + std::to_string((*found)->id.index));
    };
    const auto has_opcode = [&](const std::string &name, unsigned opcode) {
        const auto *function = get_function(name);
        return std::any_of(llvm::instructions(function).begin(),
                           llvm::instructions(function).end(),
                           [opcode](const llvm::Instruction &instruction) {
                               return instruction.getOpcode() == opcode;
                           });
    };
    check(has_opcode("add", llvm::Instruction::Add) &&
              has_opcode("subtract", llvm::Instruction::Sub) &&
              has_opcode("multiply", llvm::Instruction::Mul) &&
              has_opcode("negate", llvm::Instruction::Sub),
          "加减乘和取负必须使用普通 i32 算术指令");
    for (const auto &name : {"add", "subtract", "multiply", "negate"}) {
        for (const auto &instruction : llvm::instructions(get_function(name))) {
            if (const auto *operation = llvm::dyn_cast<llvm::BinaryOperator>(&instruction))
                check(!operation->hasNoSignedWrap() && !operation->hasNoUnsignedWrap(),
                      "回绕算术不得添加 nsw/nuw 承诺");
        }
    }
    check(has_opcode("signed_divide", llvm::Instruction::SDiv) &&
              has_opcode("signed_remainder", llvm::Instruction::SRem) &&
              has_opcode("unsigned_divide", llvm::Instruction::UDiv) &&
              has_opcode("unsigned_remainder", llvm::Instruction::URem),
          "除法和余数指令必须按源整数的符号性选择");
    for (const auto &name : {"signed_divide", "signed_remainder",
                             "unsigned_divide", "unsigned_remainder"}) {
        for (const auto &instruction : llvm::instructions(get_function(name)))
            if (const auto *operation = llvm::dyn_cast<llvm::BinaryOperator>(&instruction))
                check(!operation->isExact(), "除法和余数不得附加 exact 承诺");
    }
    check(has_opcode("integer_and", llvm::Instruction::And) &&
              has_opcode("integer_or", llvm::Instruction::Or) &&
              has_opcode("integer_xor", llvm::Instruction::Xor) &&
              has_opcode("boolean_and", llvm::Instruction::And) &&
              has_opcode("boolean_or", llvm::Instruction::Or) &&
              has_opcode("boolean_xor", llvm::Instruction::Xor),
          "整数和 bool 位运算都必须使用 and/or/xor");

    const auto has_predicate = [&](const std::string &name,
                                   llvm::CmpInst::Predicate predicate) {
        for (const auto &instruction : llvm::instructions(get_function(name)))
            if (const auto *compare = llvm::dyn_cast<llvm::ICmpInst>(&instruction))
                if (compare->getPredicate() == predicate &&
                    compare->getType()->isIntegerTy(1))
                    return true;
        return false;
    };
    check(has_predicate("equal", llvm::CmpInst::ICMP_EQ) &&
              has_predicate("not_equal", llvm::CmpInst::ICMP_NE) &&
              has_predicate("signed_less", llvm::CmpInst::ICMP_SLT) &&
              has_predicate("unsigned_less", llvm::CmpInst::ICMP_ULT) &&
              has_predicate("boolean_less", llvm::CmpInst::ICMP_ULT) &&
              has_predicate("signed_ge", llvm::CmpInst::ICMP_SGE),
          "比较必须按语义类型选择比较谓词并返回 i1");

    bool saw_integer_invert = false;
    bool saw_boolean_invert = false;
    for (const auto &instruction : llvm::instructions(get_function("invert_integer")))
        if (const auto *operation = llvm::dyn_cast<llvm::BinaryOperator>(&instruction))
            if (operation->getOpcode() == llvm::Instruction::Xor &&
                operation->getType()->isIntegerTy(32)) {
                const auto *mask = llvm::dyn_cast<llvm::ConstantInt>(
                    operation->getOperand(1));
                saw_integer_invert = mask && mask->isMinusOne();
            }
    for (const auto &instruction : llvm::instructions(get_function("invert_boolean")))
        if (const auto *operation = llvm::dyn_cast<llvm::BinaryOperator>(&instruction))
            if (operation->getOpcode() == llvm::Instruction::Xor &&
                operation->getType()->isIntegerTy(1)) {
                const auto *mask = llvm::dyn_cast<llvm::ConstantInt>(
                    operation->getOperand(1));
                saw_boolean_invert = mask && mask->isOne();
            }
    check(saw_integer_invert && saw_boolean_invert,
          "整数 ! 应与全 1 位模式异或，bool ! 应与 true 异或");
    diagnostics.flush();
    check(errors.empty(), "标量运算模块不应产生 verifier 诊断");
}

void run_tests() {
    check_function_names();
    check_local_slots();
    check_calls_returns_and_blocks();
    check_never_let();
    check_scalar_lowering();
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
    check_rejected("fn main() { let x = true && false; }");
    check_rejected("fn main() {} fn unused() { let x = true || false; }");
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
