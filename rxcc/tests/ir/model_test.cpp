// P2 独立验证：模型所有权、call/ret、结构检查与确定的文本输出。
#include "ir/artifact.hpp"
#include "ir/builder.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace {
using namespace rx::ir;

void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <class F> void rejects(F operation) {
    bool rejected = false;
    try { operation(); } catch (const std::logic_error &) { rejected = true; }
    check(rejected, "无效构建请求必须抛出 logic_error");
}

rx::IRArtifact make_module(bool invalid = false) {
    rx::IRArtifact artifact("rx-p2-smoke");
    auto &module = artifact.module();
    IRBuilder builder(module);
    const auto source = module.add_function("__rx_fn_0", {Type::Void, {}}, Linkage::Internal);
    builder.set_insert_point(module.function(source).add_block());
    if (!invalid)
        builder.ret();
    const auto entry = module.add_function("main", {Type::I32, {}}, Linkage::External);
    builder.set_insert_point(module.function(entry).add_block());
    check(!builder.call(source), "void call 不产生结果值");
    builder.ret(Operand::constant(Type::I32, 0));
    return artifact;
}

void add_typed_calls(Module &module) {
    IRBuilder builder(module);
    const auto id = module.add_function("identity", {Type::I32, {Type::I32}}, Linkage::Internal);
    auto &identity = module.function(id);
    builder.set_insert_point(identity.add_block());
    builder.ret(identity.argument(0));
    const auto caller = module.add_function("caller", {Type::I32, {}}, Linkage::Internal);
    builder.set_insert_point(module.function(caller).add_block());
    const auto first = builder.call(id, {Operand::constant(Type::I32, 0x80000000u)});
    check(first && first->type == Type::I32, "有返回值的 call 必须产生 i32 值");
    const auto second = builder.call(id, {*first});
    builder.ret(*second);
    // 测试声明的打印和 32 位位模式；这是手动模型，不代表源码参数/调用已接通。
    module.add_function("external_io", {Type::Void, {Type::I32}}, Linkage::External);
    const auto bits = module.add_function("all_bits", {Type::I32, {}}, Linkage::Internal);
    builder.set_insert_point(module.function(bits).add_block());
    builder.ret(Operand::constant(Type::I32, 0xffffffffu));
}

template <class F> void reject_model(F corrupt) {
    auto artifact = make_module();
    add_typed_calls(artifact.module());
    corrupt(artifact.module());
    std::ostringstream text, diagnostics;
    check(!artifact.verify_and_print(text, diagnostics) && text.str().empty() && !diagnostics.str().empty(),
          "损坏的模块必须诊断且不得输出部分文本");
}

void check_invalid_models() {
    reject_model([](Module &m) { m.find_function("main")->blocks[0].terminator.reset(); });
    reject_model([](Module &m) { m.find_function("main")->blocks[0].terminator = Return{}; });
    reject_model([](Module &m) {
        m.find_function("__rx_fn_0")->blocks[0].terminator = Return{Operand::constant(Type::I32, 1)};
    });
    reject_model([](Module &m) { m.find_function("__rx_fn_0")->name = "main"; });
    reject_model([](Module &m) { m.find_function("main")->name = "unsafe\"symbol"; });
    reject_model([](Module &m) { m.target_triple = "x86_64-unknown-linux-gnu"; });
    reject_model([](Module &m) { m.find_function("main")->new_value(Type::I32); });
    reject_model([](Module &m) { m.find_function("main")->blocks[0].id.index = 99; });
    reject_model([](Module &m) {
        std::get<Call>(m.find_function("main")->blocks[0].instructions[0].operation)
            .arguments.push_back(Operand::constant(Type::I32, 1));
    });
    reject_model([](Module &m) { m.find_function("main")->blocks[0].instructions[0].type = Type::I32; });
    reject_model([](Module &m) {
        auto &call = std::get<Call>(m.find_function("caller")->blocks[0].instructions[0].operation);
        call.arguments[0] = Operand::constant(Type::Void, 0);
    });
    reject_model([](Module &m) {
        auto &call = std::get<Call>(m.find_function("caller")->blocks[0].instructions[0].operation);
        call.function.index = invalid_index;
    });
    reject_model([](Module &m) {
        Module foreign("foreign");
        const auto id = foreign.add_function("other", {Type::Void, {Type::I32}}, Linkage::External);
        m.find_function("main")->blocks[0].terminator = Return{foreign.function(id).argument(0)};
    });
    reject_model([](Module &m) {
        auto &block = m.find_function("caller")->blocks[0];
        std::swap(block.instructions[0], block.instructions[1]);
    });
    reject_model([](Module &m) {
        auto &block = m.find_function("caller")->blocks[0];
        block.instructions.push_back(block.instructions[0]);
    });
    reject_model([](Module &m) {
        m.find_function("external_io")->signature.parameters[0] = Type::Void;
    });
    reject_model([](Module &m) { m.find_function("__rx_fn_0")->blocks.clear(); });
}

void run_tests() {
    static_assert(std::is_nothrow_move_constructible_v<rx::IRArtifact>);
    static_assert(!std::is_copy_constructible_v<rx::IRArtifact>);
    auto artifact = make_module();
    const auto *original = &artifact.module();
    const auto main_id = original->find_function("main")->id;
    rx::IRArtifact moved(std::move(artifact));
    check(&moved.module() == original && moved.module().contains(main_id), "移动 artifact 不改变模块或身份");
    add_typed_calls(moved.module());
    std::ostringstream text, again, diagnostics;
    check(moved.verify_and_print(text, diagnostics) && diagnostics.str().empty(), "合法模型必须通过检查");
    check(moved.verify_and_print(again, diagnostics) && text.str() == again.str(), "重复输出必须确定");
    check(text.str().find("ret i32 -2147483648") == std::string::npos &&
              text.str().find("i32 -2147483648") != std::string::npos &&
              text.str().find("ret i32 -1") != std::string::npos,
          "32 位常量位模式必须正确输出，调用结果不能被替换为常量");
    auto independent = make_module();
    add_typed_calls(independent.module());
    std::ostringstream other;
    check(independent.verify_and_print(other, diagnostics) && other.str() == text.str(),
          "模块归属标记不参与文本命名");
    auto &module = moved.module();
    IRBuilder builder(module);
    rejects([&] { builder.ret(); });
    rejects([&] { builder.set_insert_point(independent.module().find_function("main")->blocks[0].id); });
    rejects([&] { module.add_function("main", {Type::I32, {}}, Linkage::External); });
    rejects([&] { module.add_function("bad", {Type::Void, {Type::Void}}, Linkage::External); });
    builder.set_insert_point(module.find_function("main")->blocks[0].id);
    rejects([&] { builder.ret(Operand::constant(Type::I32, 0)); });
    const auto open_id = module.add_function("open", {Type::Void, {}}, Linkage::Internal);
    auto &open = module.function(open_id);
    builder.set_insert_point(open.add_block());
    const auto insertion = builder.insert_point();
    const auto identity = module.find_function("identity")->id;
    const auto count = open.value_types.size();
    rejects([&] { builder.call(identity); });
    rejects([&] { builder.call(identity, {Operand::constant(Type::Void, 0)}); });
    rejects([&] { builder.call(identity, {module.function(identity).argument(0)}); });
    rejects([&] { builder.ret(Operand::constant(Type::I32, 1)); });
    check(open.blocks[0].instructions.empty() && !open.blocks[0].terminator &&
              open.value_types.size() == count && builder.insert_point() == insertion,
          "无效请求不得改写指令、值表、终结指令或插入点");
    builder.ret();
    check(!builder.insert_point(), "发出 ret 后关闭插入位置");
    std::ostream failed(nullptr);
    check(!moved.verify_and_print(failed, diagnostics), "写入失败必须传播");
    check_invalid_models();
}
}

int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--test") {
            run_tests();
            return 0;
        }
        const auto mode = argc == 2 ? std::string_view(argv[1]) : std::string_view{};
        if (argc > 2 || (!mode.empty() && mode != "--invalid" && mode != "--emit-model"))
            return 2;
        auto artifact = make_module(mode == "--invalid");
        if (mode == "--emit-model")
            add_typed_calls(artifact.module());
        return artifact.verify_and_print(std::cout, std::cerr) ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
