// 独立验证 IR 模型、构建约束、分支结构检查和确定的文本输出。
#include "ir/artifact.hpp"
#include "ir/builder.hpp"
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
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
    rx::IRArtifact artifact;
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
    const auto id = module.add_function("identity", {Type::I32, {{Type::I32}}}, Linkage::Internal);
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
    module.add_function("external_io", {Type::Void, {{Type::I32}}}, Linkage::External);
    const auto bits = module.add_function("all_bits", {Type::I32, {}}, Linkage::Internal);
    builder.set_insert_point(module.function(bits).add_block());
    builder.ret(Operand::constant(Type::I32, 0xffffffffu));
}

// 参数操作数由签名和序号构造；指令结果仍从全部参数之后开始编号。
void check_parameter_identity() {
    auto artifact = make_module();
    auto &module = artifact.module();
    const auto id = module.add_function(
        "mixed_parameters", {Type::Ptr, {{Type::I1}, {Type::I32}, {Type::Ptr}}}, Linkage::Internal);
    auto &function = module.function(id);
    check(function.argument(0) == Operand::local(Type::I1, {id, 0}) &&
              function.argument(1) == Operand::local(Type::I32, {id, 1}) &&
              function.argument(2) == Operand::local(Type::Ptr, {id, 2}),
          "参数操作数必须保持签名类型和固定身份");
    rejects([&] { function.argument(3); });
    IRBuilder builder(module);
    builder.set_insert_point(function.add_block());
    const auto byte = builder.zext(Type::I8, function.argument(0));
    check(byte == Operand::local(Type::I8, {id, 3}), "指令结果不得占用参数编号");
    builder.ret(function.argument(2));
    std::ostringstream output, diagnostics;
    check(artifact.verify_and_print(output, diagnostics), "派生参数操作数必须通过完整检查");
}

void add_storage(Module &module) {
    IRBuilder builder(module);
    const auto id = module.add_function("storage", {Type::I32, {}}, Linkage::Internal);
    auto &function = module.function(id);
    builder.set_insert_point(function.add_block());
    const auto first = builder.entry_alloca(Type::I32, 4);
    builder.store(Type::I32, Operand::constant(Type::I32, 1), first, 4);
    const auto loaded = builder.load(Type::I32, first, 4);
    const auto insertion = builder.insert_point();
    const auto second = builder.entry_alloca(Type::I32, 4);
    check(builder.insert_point() == insertion, "入口分配不得改变插入点");
    builder.store(Type::I32, loaded, second, 4);
    builder.ret(builder.load(Type::I32, second, 4));
}

// 把指针存入 slot 后读回，经调用返回原地址，再显式读取目标整数。
void add_pointer_storage(Module &module) {
    IRBuilder builder(module);
    const auto id = module.add_function(
        "pointer_roundtrip", {Type::Ptr, {{Type::Ptr}}}, Linkage::Internal);
    auto &roundtrip = module.function(id);
    builder.set_insert_point(roundtrip.add_block());
    const auto slot = builder.entry_alloca(Type::Ptr, 4);
    builder.store(Type::Ptr, roundtrip.argument(0), slot, 4);
    const auto loaded = builder.load(Type::Ptr, slot, 4);
    check(loaded.type == Type::Ptr, "读取 ptr slot 必须产生指针值");
    builder.ret(loaded);

    const auto caller = module.add_function(
        "pointer_use", {Type::I32, {}}, Linkage::Internal);
    auto &function = module.function(caller);
    builder.set_insert_point(function.add_block());
    const auto address = builder.entry_alloca(Type::I32, 4);
    builder.store(Type::I32, Operand::constant(Type::I32, 42), address, 4);
    const auto returned = builder.call(id, {address});
    check(returned && returned->type == Type::Ptr, "ptr call 必须产生指针结果");
    const auto value = builder.load(Type::I32, *returned, 4);
    builder.ret(value);

    const auto &pointer_block = roundtrip.blocks[0];
    const auto &store = std::get<Store>(pointer_block.instructions[0].operation);
    check(store.value == roundtrip.argument(0) && store.address == slot &&
              std::get<Load>(pointer_block.instructions[1].operation).address == slot &&
              std::get<Return>(*pointer_block.terminator).value == loaded,
          "ptr 往返必须保存形参、读取同一 slot 并返回读取结果");
    const auto &caller_block = function.blocks[0];
    const auto &call = std::get<Call>(caller_block.instructions[1].operation);
    check(std::get<Store>(caller_block.instructions[0].operation).address == address &&
              call.function == id && call.arguments.size() == 1 && call.arguments[0] == address &&
              std::get<Load>(caller_block.instructions[2].operation).address == *returned &&
              std::get<Return>(*caller_block.terminator).value == value,
          "调用必须传入原地址，用返回的指针读取整数，再返回该整数");
}

// 构造一个完整的 bool 往返函数，覆盖 i1 计算、i8 存取和 zext/trunc。
void add_bool_scalar(Module &module) {
    IRBuilder builder(module);
    const auto id = module.add_function("bool_roundtrip", {Type::I1, {{Type::I1}}}, Linkage::Internal);
    auto &function = module.function(id);
    builder.set_insert_point(function.add_block());
    const auto slot = builder.entry_alloca(Type::I8, 1);
    const auto byte = builder.zext(Type::I8, function.argument(0));
    builder.store(Type::I8, byte, slot, 1);
    const auto loaded = builder.load(Type::I8, slot, 1);
    const auto bit = builder.trunc(Type::I1, loaded);
    builder.ret(bit);
    const auto constant_id = module.add_function("bool_constant", {Type::I1, {}}, Linkage::Internal);
    builder.set_insert_point(module.function(constant_id).add_block());
    builder.ret(Operand::constant(Type::I1, 1));
}

// 构造 i32 和 bool 的二元运算与比较，覆盖 builder/checker/printer 的类型规则。
void add_operations(Module &module) {
    IRBuilder builder(module);
    const auto id = module.add_function(
        "operations", {Type::I32, {{Type::I32}, {Type::I32}}}, Linkage::Internal);
    auto &function = module.function(id);
    builder.set_insert_point(function.add_block());
    const auto left = function.argument(0);
    const auto right = function.argument(1);
    // 依次生成全部整数指令；结果可以暂时不被使用，检查器只验证类型和定义顺序。
    builder.binary(BinaryOp::Add, left, right);
    builder.binary(BinaryOp::Subtract, left, right);
    builder.binary(BinaryOp::Multiply, left, right);
    builder.binary(BinaryOp::SignedDivide, left, right);
    builder.binary(BinaryOp::SignedRemainder, left, right);
    builder.binary(BinaryOp::UnsignedDivide, left, right);
    builder.binary(BinaryOp::UnsignedRemainder, left, right);
    builder.binary(BinaryOp::And, left, right);
    builder.binary(BinaryOp::Or, left, right);
    builder.binary(BinaryOp::Xor, left, right);
    builder.binary(BinaryOp::ShiftLeft, left, right);
    builder.binary(BinaryOp::ArithmeticShiftRight, left, right);
    builder.binary(BinaryOp::LogicalShiftRight, left, right);
    builder.compare(Comparison::SignedLessThan, left, right);
    builder.ret(left);
    // bool 只能使用 i1 位运算和相等/无符号比较。
    const auto bool_id = module.add_function(
        "bool_operations", {Type::I1, {{Type::I1}, {Type::I1}}},
        Linkage::Internal);
    auto &bool_function = module.function(bool_id);
    builder.set_insert_point(bool_function.add_block());
    const auto bool_left = bool_function.argument(0);
    const auto bool_right = bool_function.argument(1);
    const auto both = builder.binary(BinaryOp::And, bool_left, bool_right);
    builder.compare(Comparison::UnsignedLessThan, bool_left, bool_right);
    builder.ret(both);
}

// 用常量条件构造两个分支汇入同一个 ret 块，不依赖源码 lowering。
Function &add_branches(Module &module) {
    IRBuilder builder(module);
    auto &function = module.function(module.add_function(
        "branches", {Type::Void, {}}, Linkage::Internal));
    const auto entry = function.add_block();
    const auto then_block = function.add_block();
    const auto else_block = function.add_block();
    const auto merge = function.add_block();
    builder.set_insert_point(entry);
    builder.cond_branch(Operand::constant(Type::I1, 1), then_block, else_block);
    builder.set_insert_point(then_block);
    builder.branch(merge);
    builder.set_insert_point(else_block);
    builder.branch(merge);
    builder.set_insert_point(merge);
    builder.ret();
    return function;
}

// 无效的分支不得改变插入点；有效的分支必须关闭插入点，且不能再终结同一块。
void check_branch_builder() {
    auto artifact = make_module();
    auto &module = artifact.module();
    auto &function = module.function(module.add_function(
        "branch_builder", {Type::Void, {}}, Linkage::Internal));
    const auto entry = function.add_block();
    const auto then_block = function.add_block();
    const auto merge = function.add_block();
    const auto foreign = module.find_function("main")->blocks.front().id;
    const auto condition = Operand::constant(Type::I1, 1);
    IRBuilder builder(module);
    builder.set_insert_point(entry);
    for (const auto target : {foreign, BlockId{function.id, invalid_index}, entry}) {
        rejects([&] { builder.branch(target); });
        rejects([&] { builder.cond_branch(condition, target, merge); });
        rejects([&] { builder.cond_branch(condition, then_block, target); });
    }
    rejects([&] { builder.cond_branch(Operand::constant(Type::I32, 1), then_block, merge); });
    check(builder.insert_point() == entry && !function.block(entry).terminator &&
              function.block(entry).instructions.empty() && function.value_types.empty(),
          "无效跳转不能留下指令、结果值或终结指令，也不能移动插入点");

    builder.cond_branch(condition, then_block, merge);
    check(!builder.insert_point(), "condbr 后必须关闭插入点");
    rejects([&] { builder.call(module.find_function("__rx_fn_0")->id); });
    builder.set_insert_point(then_block);
    builder.branch(merge);
    check(!builder.insert_point(), "br 后必须关闭插入点");
    builder.set_insert_point(then_block);
    rejects([&] { builder.branch(merge); });
    rejects([&] { builder.ret(); });
    builder.set_insert_point(merge);
    builder.ret();
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics), "失败的构建请求不能破坏合法的分支模块");
}

// 绕过 Builder 破坏单项约束，确认检查器独立拒绝损坏的模型。
void check_branch_models() {
    const auto reject = [](auto corrupt, const char *message) {
        auto artifact = make_module();
        auto &module = artifact.module();
        auto &function = add_branches(module);
        corrupt(module, function);
        std::ostringstream text, diagnostics;
        check(!artifact.verify_and_print(text, diagnostics) &&
                  text.str().empty() && !diagnostics.str().empty(), message);
    };

    auto artifact = make_module();
    add_branches(artifact.module());
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics), "合法的分支模型必须通过自检");
    check(text.str().find("bb_0:\n  br i1 true, label %bb_1, label %bb_2\n"
                          "bb_1:\n  br label %bb_3\n"
                          "bb_2:\n  br label %bb_3\n"
                          "bb_3:\n  ret void\n") != std::string::npos,
          "条件跳转必须按真假顺序打印，两个分支都必须显式跳到合流块");

    reject([](Module &, Function &f) { f.blocks.back().terminator.reset(); },
           "合流块缺少终结指令必须被拒绝");
    reject([](Module &m, Function &f) {
        f.blocks.at(1).terminator = Branch{m.find_function("main")->blocks.front().id};
    }, "跨函数的 br 必须被拒绝");
    reject([](Module &, Function &f) { f.blocks.at(1).terminator = Branch{f.blocks.front().id}; },
           "跳回入口块的 br 必须被拒绝");
    reject([](Module &, Function &f) {
        const auto dead = f.add_block();
        f.block(dead).terminator = Branch{BlockId{f.id, invalid_index}};
    }, "不可达块的非法边也必须在建图时被拒绝");
    reject([](Module &, Function &f) {
        auto &branch = std::get<CondBranch>(*f.blocks.front().terminator);
        branch.when_false = BlockId{f.id, invalid_index};
    }, "condbr 的 false 目标不存在时必须被拒绝");
    reject([](Module &, Function &f) {
        auto &branch = std::get<CondBranch>(*f.blocks.front().terminator);
        branch.when_true = f.blocks.front().id;
    }, "condbr 的 true 边不能为入口块增加前驱");
    reject([](Module &, Function &f) {
        auto &branch = std::get<CondBranch>(*f.blocks.front().terminator);
        branch.condition = Operand::constant(Type::I32, 1);
    }, "condbr 的条件必须为 i1");
    reject([](Module &, Function &f) {
        const auto value = f.new_value(Type::I1);
        const auto zero = Operand::constant(Type::I32, 0);
        f.blocks.at(1).instructions.push_back({
            *value.value_id(), Type::I1, Compare{Comparison::Equal, zero, zero}});
        std::get<CondBranch>(*f.blocks.front().terminator).condition = value;
    }, "即使条件为 i1，也不能在定义它的分支执行前使用");
}

// 构造带回边的 CFG：准备块的 load 支配循环体和出口，虽然后者更早打印。
void add_dominance_loop(Module &module) {
    IRBuilder builder(module);
    auto &function = module.function(module.add_function(
        "dominance_loop", {Type::I32, {{Type::I32}}}, Linkage::Internal));
    const auto entry = function.add_block();
    const auto header = function.add_block();
    const auto body = function.add_block();
    const auto exit = function.add_block();
    const auto setup = function.add_block();
    const auto unreachable = function.add_block();
    const auto zero = Operand::constant(Type::I32, 0);
    const auto input = function.argument(0);

    builder.set_insert_point(entry);
    const auto slot = builder.entry_alloca(Type::I32, 4);
    builder.store(input, slot, 4);
    builder.branch(setup);
    builder.set_insert_point(setup);
    const auto saved = builder.load(Type::I32, slot, 4);
    builder.branch(header);
    builder.set_insert_point(header);
    const auto current = builder.load(Type::I32, slot, 4);
    const auto go = builder.compare(Comparison::NotEqual, current, zero);
    builder.cond_branch(go, body, exit);
    builder.set_insert_point(body);
    builder.store(builder.binary(BinaryOp::Subtract, saved, input), slot, 4);
    builder.branch(header);
    builder.set_insert_point(exit);
    builder.ret(saved);
    // 不可达的前驱不能抹掉 setup 对 header 及其后继的支配关系。
    builder.set_insert_point(unreachable);
    builder.branch(header);
}

// 用菱形图验证：值已登记且类型正确，不代表所有到达使用点的路径都会定义它。
void check_dominance() {
    auto artifact = make_module();
    add_dominance_loop(artifact.module());
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics) && diagnostics.str().empty(),
          "跨块使用应按 CFG 支配关系检查，不应受块顺序、回边或不可达前驱影响");

    const auto reject = [](auto add_use, const char *message) {
        auto artifact = make_module();
        auto &function = add_branches(artifact.module());
        const auto value = function.new_value(Type::I32);
        const auto one = Operand::constant(Type::I32, 1);
        function.blocks.at(1).instructions.push_back({
            *value.value_id(), Type::I32, BinaryOperation{BinaryOp::Add, one, one}});
        add_use(function, value, one);
        std::ostringstream text, diagnostics;
        check(!artifact.verify_and_print(text, diagnostics) &&
                  text.str().empty() && !diagnostics.str().empty(), message);
    };
    reject([](Function &function, Operand value, Operand) {
        function.abi.result = Type::I32;
        function.blocks.back().terminator = Return{value};
    }, "merge 的 ret 不能读取只在 then 中定义的值");
    reject([](Function &function, Operand value, Operand one) {
        const auto result = function.new_value(Type::I32);
        function.blocks.at(2).instructions.push_back({
            *result.value_id(), Type::I32, BinaryOperation{BinaryOp::Add, value, one}});
    }, "else 的算术不能读取 then 中定义的值");
    reject([](Function &function, Operand value, Operand one) {
        const auto result = function.new_value(Type::I32);
        auto &instructions = function.blocks.at(1).instructions;
        instructions.insert(instructions.begin(), Instruction{
            *result.value_id(), Type::I32, BinaryOperation{BinaryOp::Add, value, one}});
    }, "定义表完整后仍要拒绝同块内先使用、后定义");
}

// 手工构造 bool 真值驱动：把往返结果零扩展成 i32 后打印。
// 这是模型层的观察手段，不表示源码已经支持把 bool 直接传给 println_i32。
rx::IRArtifact make_bool_driver() {
    rx::IRArtifact artifact;
    auto &module = artifact.module();
    IRBuilder builder(module);
    module.add_function("println_i32", {Type::Void, {{Type::I32}}}, Linkage::External);
    const auto roundtrip =
        module.add_function("bool_roundtrip", {Type::I1, {{Type::I1}}}, Linkage::Internal);
    auto &function = module.function(roundtrip);
    builder.set_insert_point(function.add_block());
    const auto slot = builder.entry_alloca(Type::I8, 1);
    builder.store(Type::I8, builder.zext(Type::I8, function.argument(0)), slot, 1);
    builder.ret(builder.trunc(Type::I1, builder.load(Type::I8, slot, 1)));
    const auto source =
        module.add_function("__rx_fn_0", {Type::Void, {}}, Linkage::Internal);
    builder.set_insert_point(module.function(source).add_block());
    const auto print = module.find_function("println_i32")->id;
    // 先传 true 再传 false，期望打印 1、0，能发现两种真值被混同。
    for (const unsigned bits : {1u, 0u}) {
        const auto value =
            builder.call(roundtrip, {Operand::constant(Type::I1, bits)});
        builder.call(print, {builder.zext(Type::I32, *value)});
    }
    builder.ret();
    const auto entry =
        module.add_function("main", {Type::I32, {}}, Linkage::External);
    builder.set_insert_point(module.function(entry).add_block());
    builder.call(source);
    builder.ret(Operand::constant(Type::I32, 0));
    return artifact;
}

// 在 bool 正例上制造一处损坏，确认检查器拒绝且不输出部分文本。
template <class F> void reject_scalar_model(F corrupt) {
    auto artifact = make_module();
    add_bool_scalar(artifact.module());
    corrupt(artifact.module());
    std::ostringstream text, diagnostics;
    check(!artifact.verify_and_print(text, diagnostics) && text.str().empty() && !diagnostics.str().empty(),
          "损坏的 bool 模型必须诊断且不得输出部分文本");
}

// 每次只破坏指针正例的一项约束，确认检查失败时不交付部分文本。
template <class F> void reject_pointer_model(F corrupt) {
    auto artifact = make_module();
    add_pointer_storage(artifact.module());
    corrupt(artifact.module());
    std::ostringstream text, diagnostics;
    check(!artifact.verify_and_print(text, diagnostics) && text.str().empty() && !diagnostics.str().empty(),
          "损坏的 ptr 模型必须诊断且不得输出部分文本");
}

template <class F> void reject_model(F corrupt) {
    auto artifact = make_module();
    add_typed_calls(artifact.module());
    add_storage(artifact.module());
    add_operations(artifact.module());
    corrupt(artifact.module());
    std::ostringstream text, diagnostics;
    check(!artifact.verify_and_print(text, diagnostics) && text.str().empty() && !diagnostics.str().empty(),
          "损坏的模块必须诊断且不得输出部分文本");
}

// 直接查询目标布局，覆盖空存储和接近 32 位上限的对象，不实际分配它们。
void check_layout_limits() {
    Module module;
    const auto byte = StorageType::scalar(Type::I8);
    const auto word = StorageType::scalar(Type::I32);
    const auto pointer = StorageType::scalar(Type::Ptr);
    const auto max_size = std::numeric_limits<uint32_t>::max();
    const auto largest = StorageType::array_of(byte, max_size);
    const auto field_sum = module.add_named_type("field_sum");
    const auto field_align = module.add_named_type("field_align");
    const auto tail_align = module.add_named_type("tail_align");
    module.define_named_type(field_sum, {largest, byte});
    module.define_named_type(field_align, {largest, word});
    module.define_named_type(tail_align, {word, StorageType::array_of(byte, max_size - 4)});

    LayoutService layouts(module);
    check(layouts.layout(StorageType{}) == Layout{0, 1, {}}, "unit 大小为 0、对齐为 1");
    check(layouts.layout(byte) == Layout{1, 1, {}} &&
              layouts.layout(word) == Layout{4, 4, {}} &&
              layouts.layout(pointer) == Layout{4, 4, {}}, "标量布局必须取 RV32 大小和对齐");
    check(layouts.layout(StorageType::vec()) == Layout{12, 4, {0, 4, 8}},
          "Vec 头应连续存放 RV32 地址、长度和容量");
    check(layouts.layout(StorageType::array_of(word, 0)) == Layout{0, 4, {}},
          "零长度数组仍保留元素对齐");
    check(layouts.layout(largest) == Layout{max_size, 1, {}} &&
              layouts.layout(StorageType::array_of(word, max_size / 4)) == Layout{max_size - 3, 4, {}},
          "布局上限内的数组不能被误拒绝");
    rejects([&] { layouts.layout(StorageType::array_of(word, uint64_t{max_size} / 4 + 1)); });
    // 2^62 乘 4 连宿主 uint64_t 也装不下，必须在乘法前拒绝。
    rejects([&] { layouts.layout(StorageType::array_of(word, uint64_t{1} << 62)); });

    // 同一个结束位置分别触发下一字段对齐和结构体尾部对齐溢出。
    rejects([&] { layouts.layout(StorageType::struct_of(field_sum)); });
    rejects([&] { layouts.layout(StorageType::struct_of(field_align)); });
    rejects([&] { layouts.layout(StorageType::struct_of(tail_align)); });
}

// 前端不会产生这些类型表；手工模型需要被自有 verifier 拒绝，且不能吐出部分 LLVM 文本。
void check_invalid_named_layouts() {
    reject_model([](Module &m) { m.add_named_type("unfinished"); });
    reject_model([](Module &m) {
        const auto type = m.add_named_type("dangling");
        m.define_named_type(type, {StorageType::struct_of(type + 1)});
    });
    reject_model([](Module &m) {
        const auto first = m.add_named_type("first");
        const auto second = m.add_named_type("second");
        m.define_named_type(first, {StorageType::array_of(StorageType::struct_of(second), 1)});
        m.define_named_type(second, {StorageType::struct_of(first)});
    });
    reject_model([](Module &m) {
        const auto type = m.add_named_type("oversized");
        m.define_named_type(type, {StorageType::array_of(StorageType::scalar(Type::I32), 1073741824)});
    });
}

// 验证聚合分配、动态数组下标和字段投影的类型；非法操作数不能进入指令表。
void check_aggregate_gep() {
    auto artifact = make_module();
    auto &module = artifact.module();
    const auto record = module.add_named_type("record");
    module.define_named_type(record, {StorageType::scalar(Type::I8), StorageType::scalar(Type::I32)});
    const auto element = StorageType::struct_of(record);
    const auto array = StorageType::array_of(element, 2);
    const auto id = module.add_function("project", {Type::I32, {{Type::I32}}}, Linkage::Internal);
    auto &function = module.function(id);
    IRBuilder builder(module);
    builder.set_insert_point(function.add_block());
    const auto base = builder.entry_alloca(array, 4);
    const auto zero = Operand::constant(Type::I32, 0);
    const auto one = Operand::constant(Type::I32, 1);
    const auto index = function.argument(0);
    rejects([&] { builder.get_element_ptr(array, zero, {zero, index}); });
    const auto boolean = Operand::constant(Type::I1, 1);
    rejects([&] { builder.get_element_ptr(array, base, {boolean, index}); });
    rejects([&] { builder.get_element_ptr(array, base, {zero, boolean}); });
    rejects([&] { builder.get_element_ptr(StorageType::scalar(Type::I32), base, {zero, one}); });
    check(function.blocks.front().instructions.empty(), "非法 GEP 不得发指令");

    const auto item = builder.get_element_ptr(array, base, {zero, index});
    const auto out_of_range = Operand::constant(Type::I32, 2);
    rejects([&] { builder.get_element_ptr(element, item, {zero, out_of_range}); });
    rejects([&] { builder.get_element_ptr(element, item, {zero, index}); });
    const auto field = builder.get_element_ptr(element, item, {zero, one});
    builder.store(Type::I32, one, field, 4);
    builder.ret(builder.load(Type::I32, field, 4));
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics) && diagnostics.str().empty(),
          "合法的两级投影必须通过自有检查");
    const auto printed = text.str();
    check(printed.find("alloca [2 x %record], align 4") != std::string::npos &&
              printed.find("getelementptr [2 x %record], ptr %v_1, i32 0, i32 %v_0") != std::string::npos &&
              printed.find("getelementptr %record, ptr %v_2, i32 0, i32 1") != std::string::npos,
          "GEP 必须保留显式存储类型，数组使用动态下标，字段使用声明序号");

    // 手工损坏已生成的指令，确认模块检查不依赖 builder 的检查。
    auto &gep = std::get<GetElementPtr>(function.blocks.front().instructions.front().operation);
    gep.indices.back() = boolean;
    std::ostringstream invalid_text, invalid_diagnostics;
    check(!artifact.verify_and_print(invalid_text, invalid_diagnostics) &&
              invalid_text.str().empty() && !invalid_diagnostics.str().empty(),
          "检查器必须拒绝错误类型的 GEP 下标，不输出部分 IR");
    gep.indices.back() = index;
    // 首下标和数组投影都允许动态 i32，但不能读取后面的 load 结果。
    const auto later = Operand::local(Type::I32, *function.blocks.front().instructions.back().result);
    for (auto &operand : gep.indices) {
        const auto original = operand;
        operand = later;
        std::ostringstream output, errors;
        check(!artifact.verify_and_print(output, errors) && output.str().empty() && !errors.str().empty(),
              "检查器必须独立拒绝在使用点尚未定义的 GEP 下标");
        operand = original;
    }
    auto &field_gep = std::get<GetElementPtr>(function.blocks.front().instructions[1].operation);
    field_gep.indices.back() = out_of_range;
    std::ostringstream invalid_field_text, invalid_field_diagnostics;
    check(!artifact.verify_and_print(invalid_field_text, invalid_field_diagnostics) &&
              invalid_field_text.str().empty() && !invalid_field_diagnostics.str().empty(),
          "检查器必须独立拒绝越界的结构体字段序号");
}

// 聚合 ABI 的隐藏位置和实参数量由构建器检查；输出前还要独立检查模块元数据。
void check_aggregate_abi_validation() {
    const auto word = StorageType::scalar(Type::I32);
    const auto array = StorageType::array_of(word, 2);
    const FunctionABI valid{Type::Void, {{Type::Ptr}, {Type::I32}}, array};
    auto artifact = make_module();
    auto &module = artifact.module();
    for (const FunctionABI &invalid : {
             FunctionABI{Type::Void, {{Type::I32}, {Type::Ptr}}, array},
             FunctionABI{Type::Void, {{Type::Ptr, array}}, array},
             FunctionABI{Type::I32, {{Type::Ptr}}, array},
             FunctionABI{Type::Void, {{Type::I32, array}}},
             // 没有首参数时必须直接拒绝，不能继续访问 front()。
             FunctionABI{Type::Void, {}, array},
             FunctionABI{Type::Void, {{Type::Ptr}}, StorageType{}},
             FunctionABI{Type::Void, {{Type::Ptr}}, word},
             // 显式 Unit 或标量存储即使配上 ptr，也不是合法的按值聚合参数。
             FunctionABI{Type::Void, {{Type::Ptr, StorageType{}}}},
             FunctionABI{Type::Void, {{Type::Ptr, word}}},
         }) {
        rejects([&] { module.add_function("rejected_abi", invalid, Linkage::External); });
        check(!module.find_function("rejected_abi"), "无效 ABI 不能登记到模块");
    }

    const auto mixed = module.add_function(
        "mixed_pointers", {Type::Void, {{Type::Ptr}, {Type::Ptr, array}}}, Linkage::External);
    const auto &parameters = module.function(mixed).abi.parameters;
    check(parameters.size() == 2 && parameters[0] == ParameterABI{Type::Ptr} &&
              parameters[1] == ParameterABI{Type::Ptr, array},
          "普通 ptr 无按值存储，按值聚合 ptr 必须保留自己的存储类型");

    const auto callee = module.add_function("fill", valid, Linkage::External);
    const auto caller = module.add_function("invoke", {Type::Void, {}}, Linkage::Internal);
    auto &function = module.function(caller);
    IRBuilder builder(module);
    builder.set_insert_point(function.add_block());
    const auto slot = builder.entry_alloca(array, 4);
    const auto number = Operand::constant(Type::I32, 17);
    rejects([&] { builder.call(callee, {number}); });
    rejects([&] { builder.call(callee, {number, slot}); });
    check(function.blocks.front().instructions.empty(), "无效调用不能进入指令表");
    check(!builder.call(callee, {slot, number}), "通过地址返回的调用不产生 SSA 结果");
    builder.ret();
    std::ostringstream text, diagnostics;
    check(artifact.verify_and_print(text, diagnostics), "合法的聚合返回调用必须通过校验");

    // 绕过 builder 后修改 Call，确认 check_module 也会检查隐藏参数及其位置。
    auto &call = std::get<Call>(function.blocks.front().instructions.front().operation);
    for (const auto &invalid : {std::vector{number}, std::vector{number, slot}}) {
        call.arguments = invalid;
        std::ostringstream output, errors;
        check(!artifact.verify_and_print(output, errors) && output.str().empty() && !errors.str().empty(),
              "错误数量或位置的隐藏参数必须被模块校验拒绝");
    }
    reject_model([&](Module &m) {
        const auto id = m.add_function("wrong_result", valid, Linkage::External);
        m.function(id).abi.parameters.front().type = Type::I32;
    });
    reject_model([&](Module &m) {
        const auto id = m.add_function("wrong_layout", valid, Linkage::External);
        m.function(id).abi.result_storage = StorageType::struct_of(999);
    });
    reject_model([](Module &m) {
        m.add_function("wrong_parameter_layout",
                       {Type::Void, {{Type::Ptr, StorageType::struct_of(999)}}},
                       Linkage::External);
    });
    reject_model([&](Module &m) {
        const auto id = m.add_function("stale_parameter_type", valid, Linkage::External);
        m.function(id).abi.parameters.back().type = Type::I1;
    });
}

// 构造可独立输出的聚合调用：命名字段与数组投影、按值参数和隐藏返回地址一起交给 LLVM 验证。
void add_aggregate_calls(Module &module) {
    const auto record = module.add_named_type("aggregate_record");
    module.define_named_type(record, {StorageType::scalar(Type::I8), StorageType::scalar(Type::I32)});
    const auto element = StorageType::struct_of(record);
    const auto array = StorageType::array_of(element, 2);
    const FunctionABI abi{Type::Void, {{Type::Ptr}, {Type::Ptr, array}}, array};
    const auto callee = module.add_function("aggregate_copy", abi, Linkage::Internal);
    auto &copy = module.function(callee);
    IRBuilder builder(module);
    builder.set_insert_point(copy.add_block());
    const auto zero = Operand::constant(Type::I32, 0);
    for (uint32_t index = 0; index < 2; ++index) {
        const auto position = Operand::constant(Type::I32, index);
        const auto source = builder.get_element_ptr(array, copy.argument(1), {zero, position});
        const auto destination = builder.get_element_ptr(array, copy.argument(0), {zero, position});
        // 只搬运两个实际字段，不把尾填充当作源值内容读取。
        for (uint32_t field = 0; field < 2; ++field) {
            const auto member = Operand::constant(Type::I32, field);
            const auto type = field == 0 ? Type::I8 : Type::I32;
            const uint32_t alignment = field == 0 ? 1 : 4;
            const auto from = builder.get_element_ptr(element, source, {zero, member});
            const auto to = builder.get_element_ptr(element, destination, {zero, member});
            builder.store(type, builder.load(type, from, alignment), to, alignment);
        }
    }
    builder.ret();

    const auto caller = module.add_function("aggregate_invoke", {Type::I32, {}}, Linkage::Internal);
    builder.set_insert_point(module.function(caller).add_block());
    const auto source = builder.entry_alloca(array, 4);
    const auto destination = builder.entry_alloca(array, 4);
    const auto one = Operand::constant(Type::I32, 1);
    for (uint32_t index = 0; index < 2; ++index) {
        const auto item = builder.get_element_ptr(array, source,
            {zero, Operand::constant(Type::I32, index)});
        builder.store(Type::I8, Operand::constant(Type::I8, index),
                      builder.get_element_ptr(element, item, {zero, zero}), 1);
        builder.store(Type::I32, Operand::constant(Type::I32, 41 + index),
                      builder.get_element_ptr(element, item, {zero, one}), 4);
    }
    builder.call(callee, {destination, source});
    const auto item = builder.get_element_ptr(array, destination, {zero, one});
    const auto field = builder.get_element_ptr(element, item, {zero, one});
    builder.ret(builder.load(Type::I32, field, 4));
}

void check_invalid_models() {
    reject_model([](Module &m) { m.find_function("main")->blocks[0].terminator.reset(); });
    reject_model([](Module &m) { m.find_function("main")->blocks[0].terminator = Return{}; });
    reject_model([](Module &m) {
        m.find_function("__rx_fn_0")->blocks[0].terminator = Return{Operand::constant(Type::I32, 1)};
    });
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
        Module foreign;
        const auto id = foreign.add_function("other", {Type::Void, {{Type::I32}}}, Linkage::External);
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
        m.find_function("external_io")->abi.parameters[0].type = Type::Void;
    });
    // ABI 参数和值类型表之间的缺项、多项和类型失配仍必须拒绝。
    reject_model([](Module &m) { m.find_function("identity")->value_types.clear(); });
    reject_model([](Module &m) { m.find_function("identity")->value_types[0] = Type::I1; });
    reject_model([](Module &m) {
        auto &function = *m.find_function("identity");
        function.abi.parameters.push_back({Type::I32});
    });
    reject_model([](Module &m) {
        auto &function = *m.find_function("external_io");
        function.abi.parameters.clear();
    });
    reject_model([](Module &m) { m.find_function("__rx_fn_0")->blocks.clear(); });
    reject_model([](Module &m) { std::get<Alloca>(m.find_function("storage")->entry_allocas[0].operation).alignment = 3; });
    reject_model([](Module &m) { m.find_function("storage")->entry_allocas[0].type = Type::I32; });
    reject_model([](Module &m) { m.find_function("storage")->entry_allocas[1].result = m.find_function("storage")->entry_allocas[0].result; });
    reject_model([](Module &m) { std::get<Store>(m.find_function("storage")->blocks[0].instructions[0].operation).address = Operand::constant(Type::I32, 0); });
    // ptr 本身是合法存储类型；这里拒绝的是显式读取类型与 i32 结果不一致。
    reject_model([](Module &m) { std::get<Load>(m.find_function("storage")->blocks[0].instructions[1].operation).storage_type = Type::Ptr; });
    reject_model([](Module &m) { std::get<Store>(m.find_function("storage")->blocks[0].instructions[0].operation).alignment = 0; });
    reject_model([](Module &m) { m.find_function("storage")->blocks[0].instructions.push_back(m.find_function("storage")->entry_allocas[0]); });
    reject_model([](Module &m) { m.find_function("external_io")->entry_allocas.push_back(m.find_function("storage")->entry_allocas[0]); });
    // 只核对显式存取类型和值归属，不从 opaque ptr 推断目标类型。
    reject_pointer_model([](Module &m) {
        std::get<Load>(m.find_function("pointer_roundtrip")->blocks[0].instructions[1].operation)
            .storage_type = Type::I32;
    });
    reject_pointer_model([](Module &m) {
        std::get<Store>(m.find_function("pointer_roundtrip")->blocks[0].instructions[0].operation)
            .value = Operand::constant(Type::I1, 1);
    });
    // 同模块另一函数的 ptr 也不能作为当前函数的值或地址使用。
    reject_pointer_model([](Module &m) {
        std::get<Store>(m.find_function("pointer_roundtrip")->blocks[0].instructions[0].operation)
            .value = Operand::local(Type::Ptr, *m.find_function("pointer_use")->entry_allocas[0].result);
    });
    reject_pointer_model([](Module &m) {
        std::get<Load>(m.find_function("pointer_use")->blocks[0].instructions[2].operation)
            .address = m.find_function("pointer_roundtrip")->argument(0);
    });
    reject_pointer_model([](Module &m) {
        std::get<Call>(m.find_function("pointer_use")->blocks[0].instructions[1].operation)
            .arguments[0] = m.find_function("pointer_roundtrip")->argument(0);
    });
    reject_pointer_model([](Module &m) {
        std::get<Call>(m.find_function("pointer_use")->blocks[0].instructions[1].operation)
            .arguments[0] = Operand::constant(Type::I32, 42);
    });
    reject_pointer_model([](Module &m) {
        auto &function = *m.find_function("pointer_use");
        auto &call = function.blocks[0].instructions[1];
        // 同步值表，避免仅因结果登记不一致而拒绝，真正检查 call 的返回签名。
        call.type = Type::I32;
        function.value_types[call.result->index] = Type::I32;
    });
    reject_pointer_model([](Module &m) {
        m.find_function("pointer_roundtrip")->blocks[0].terminator =
            Return{Operand::constant(Type::I32, 42)};
    });
    reject_pointer_model([](Module &m) {
        m.find_function("pointer_roundtrip")->blocks[0].terminator = Return{};
    });
    reject_pointer_model([](Module &m) {
        m.find_function("pointer_roundtrip")->blocks[0].terminator = Return{
            Operand::local(Type::Ptr, *m.find_function("pointer_use")->entry_allocas[0].result)};
    });
    // bool 模型：i1 常量范围、zext/trunc 的源/目标/结果类型都必须一致。
    reject_scalar_model([](Module &m) {
        m.find_function("bool_constant")->blocks[0].terminator = Return{Operand::constant(Type::I1, 2)};
    });
    reject_scalar_model([](Module &m) {
        auto &extension = std::get<ZExt>(
            m.find_function("bool_roundtrip")->blocks[0].instructions[0].operation);
        extension.target_type = Type::I1;
    });
    reject_scalar_model([](Module &m) {
        auto &truncation = std::get<Trunc>(
            m.find_function("bool_roundtrip")->blocks[0].instructions[3].operation);
        truncation.value.type = Type::I1;
    });
    // 指针不是较窄整数；即使结果类型和值表一致，也必须拒绝这条 trunc。
    reject_scalar_model([](Module &m) {
        auto &function = *m.find_function("bool_roundtrip");
        const auto result = function.new_value(Type::Ptr);
        function.blocks[0].instructions.push_back({
            *result.value_id(), Type::Ptr,
            Trunc{Operand::constant(Type::I32, 1), Type::Ptr}});
    });
    reject_scalar_model([](Module &m) {
        m.find_function("bool_roundtrip")->blocks[0].instructions[2].type =
            Type::I32;
    });
    // 二元/比较：结果类型、操作数类型和 bool 谓词都必须符合指令规则。
    reject_model([](Module &m) {
        m.find_function("operations")->blocks[0].instructions[0].type = Type::I1;
    });
    reject_model([](Module &m) {
        auto &binary = std::get<BinaryOperation>(
            m.find_function("operations")->blocks[0].instructions[0].operation);
        binary.right.type = Type::I1;
    });
    reject_model([](Module &m) {
        m.find_function("operations")->blocks[0].instructions[13].type =
            Type::I32;
    });
    reject_model([](Module &m) {
        auto &comparison = std::get<Compare>(
            m.find_function("bool_operations")->blocks[0].instructions[1].operation);
        comparison.predicate = Comparison::SignedLessThan;
    });
    // 未知谓词必须在文本输出前拒绝，不能打印缺少谓词的 icmp。
    for (const auto *name : {"operations", "bool_operations"}) {
        reject_model([&](Module &m) {
            const auto index = std::string_view(name) == "operations" ? 13u : 1u;
            auto &comparison = std::get<Compare>(
                m.find_function(name)->blocks[0].instructions[index].operation);
            comparison.predicate = static_cast<Comparison>(999);
        });
    }
    reject_model([](Module &m) {
        auto &binary = std::get<BinaryOperation>(
            m.find_function("bool_operations")->blocks[0].instructions[0].operation);
        binary.operation = BinaryOp::Add;
    });
    // 移位只允许 i32，bool 上的 shl/ashr/lshr 必须在发射前拒绝。
    reject_model([](Module &m) {
        auto &binary = std::get<BinaryOperation>(
            m.find_function("bool_operations")->blocks[0].instructions[0].operation);
        binary.operation = BinaryOp::ShiftLeft;
    });
}

struct GroupedDigits : std::numpunct<char> {
    std::string do_grouping() const override { return "\3"; }
};

void run_tests() {
    static_assert(std::is_nothrow_move_constructible_v<rx::IRArtifact>);
    static_assert(!std::is_copy_constructible_v<rx::IRArtifact>);
    auto artifact = make_module();
    const auto *original = &artifact.module();
    const auto main_id = original->find_function("main")->id;
    rx::IRArtifact moved(std::move(artifact));
    check(&moved.module() == original && moved.module().contains(main_id), "移动 artifact 不改变模块或身份");
    add_typed_calls(moved.module());
    add_operations(moved.module());
    std::ostringstream text, again, diagnostics;
    check(moved.verify_and_print(text, diagnostics) && diagnostics.str().empty(), "合法模型必须通过检查");
    check(moved.verify_and_print(again, diagnostics) && text.str() == again.str(), "重复输出必须确定");
    std::ostringstream formatted;
    const std::locale locale(std::locale::classic(), new GroupedDigits);
    formatted.imbue(locale);
    formatted << std::hex << std::showbase << std::setfill('#') << std::setw(40);
    const auto flags = formatted.flags();
    check(moved.verify_and_print(formatted, diagnostics) && formatted.str() == text.str(),
          "数字进制、分组和字段宽度不得改变 IR 文本");
    check(formatted.flags() == flags && formatted.width() == 40 && formatted.fill() == '#' &&
              formatted.getloc() == locale && formatted.rdbuf()->getloc() == locale,
          "输出后应保留调用方的流和缓冲区格式");
    check(text.str().find("add i32") != std::string::npos &&
              text.str().find("sdiv i32") != std::string::npos &&
              text.str().find("udiv i32") != std::string::npos &&
              text.str().find("icmp slt i32") != std::string::npos &&
              text.str().find("and i1") != std::string::npos &&
              text.str().find("icmp ult i1") != std::string::npos &&
              text.str().find("shl i32") != std::string::npos &&
              text.str().find("ashr i32") != std::string::npos &&
              text.str().find("lshr i32") != std::string::npos,
          "二元/比较指令必须按类型正确输出");
    check(text.str().find(" nsw ") == std::string::npos &&
              text.str().find(" nuw ") == std::string::npos &&
              text.str().find(" exact") == std::string::npos &&
              text.str().find("noalias") == std::string::npos &&
              text.str().find("inbounds") == std::string::npos &&
              text.str().find("nonnull") == std::string::npos,
          "二元指令不能附加不溢出、精确性或别名承诺");
    check(text.str().find("ret i32 -2147483648") == std::string::npos &&
              text.str().find("i32 -2147483648") != std::string::npos &&
              text.str().find("ret i32 -1") != std::string::npos,
          "32 位常量位模式必须正确输出，调用结果不能被替换为常量");
    auto independent = make_module();
    add_typed_calls(independent.module());
    add_operations(independent.module());
    std::ostringstream other;
    check(independent.verify_and_print(other, diagnostics) && other.str() == text.str(),
          "模块归属标记不参与文本命名");
    auto &module = moved.module();
    IRBuilder builder(module);
    rejects([&] { builder.ret(); });
    rejects([&] { builder.set_insert_point(independent.module().find_function("main")->blocks[0].id); });
    rejects([&] { module.add_function("main", {Type::I32, {}}, Linkage::External); });
    rejects([&] { module.add_function("unsafe\"symbol", {Type::Void, {}}, Linkage::External); });
    rejects([&] { module.add_function("bad", {Type::Void, {{Type::Void}}}, Linkage::External); });
    check(!module.find_function("bad") && module.find_function("main")->id == main_id,
          "登记失败不能破坏函数名称索引");
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
    struct RejectWrites : std::streambuf {} unwritable;
    std::ostream failing_output(&unwritable);
    check(!moved.verify_and_print(failing_output, diagnostics) && failing_output.bad(),
          "缓冲区写入失败应传播到目标流");
    std::ostream throwing_output(&unwritable);
    const auto exceptions = std::ios::badbit | std::ios::failbit;
    throwing_output.exceptions(exceptions);
    std::ostringstream write_diagnostics;
    check(!moved.verify_and_print(throwing_output, write_diagnostics) &&
              throwing_output.bad() && throwing_output.exceptions() == exceptions &&
              write_diagnostics.str().find("IR 输出流写入失败") != std::string::npos,
          "启用异常的写入失败也必须返回 false，保留失败位和异常设置并输出诊断");
    check_parameter_identity();
    check_invalid_models();
    check_branch_builder();
    check_branch_models();
    check_dominance();
    check_layout_limits();
    check_invalid_named_layouts();
    check_aggregate_gep();
    check_aggregate_abi_validation();
    auto aggregates = make_module();
    add_aggregate_calls(aggregates.module());
    check(aggregates.verified_text(diagnostics).has_value(),
          "独立聚合调用模型必须通过结构校验和序列化");
    auto storage = make_module();
    add_storage(storage.module());
    std::ostringstream storage_text;
    check(storage.verify_and_print(storage_text, diagnostics), "存储模型必须通过检查");
    const auto printed = storage_text.str();
    check(printed.find("%v_0 = alloca i32, align 4\n  %v_2 = alloca i32, align 4\n  store i32 1, ptr %v_0, align 4\n  %v_1 = load i32, ptr %v_0, align 4\n  store i32 %v_1, ptr %v_2, align 4") != std::string::npos,
          "迟分配必须移到入口，保持原值编号和正文顺序");
    IRBuilder memory(storage.module());
    rejects([&] { memory.entry_alloca(Type::I32, 4); });
    rejects([&] { memory.load(Type::I32, Operand::constant(Type::I32, 0), 4); });
    const auto memory_id = storage.module().add_function("memory_errors", {Type::Void, {}}, Linkage::Internal);
    auto &memory_function = storage.module().function(memory_id);
    const auto block = memory_function.add_block();
    memory.set_insert_point(block);
    const auto address = memory.entry_alloca(Type::I32, 4);
    const auto values = memory_function.value_types.size();
    rejects([&] { memory.entry_alloca(Type::Void, 4); });
    rejects([&] { memory.entry_alloca(Type::I32, 3); });
    const auto nested = StorageType::array_of(StorageType::array_of(Type::I32, 2), 3);
    check(valid_storage(nested, 1), "嵌套数组仍可使用合法的非自然对齐");
    rejects([&] { memory.entry_alloca(nested, 3); });
    rejects([&] { memory.entry_alloca(StorageType::array_of(StorageType::array_of(Type::I1, 2), 3), 4); });
    rejects([&] { memory.load(Type::Void, address, 4); });
    rejects([&] { memory.load(Type::I32, Operand::constant(Type::I32, 0), 4); });
    rejects([&] { memory.store(Type::I32, address, address, 4); });
    rejects([&] { memory.store(Type::I32, Operand::constant(Type::I32, 1), address, 0); });
    check(memory_function.value_types.size() == values && memory_function.entry_allocas.size() == 1 &&
              memory_function.blocks[0].instructions.empty() &&
              !memory_function.blocks[0].terminator && memory.insert_point() == block,
          "无效存储请求不得产生部分指令或改变状态");
    memory.ret();
    rejects([&] { memory.store(Type::I32, Operand::constant(Type::I32, 1), address, 4); });
    memory.set_insert_point(block);
    rejects([&] { memory.entry_alloca(Type::I32, 4); });
    rejects([&] { memory.load(Type::I32, address, 4); });
    auto pointers = make_module();
    add_pointer_storage(pointers.module());
    std::ostringstream pointer_text;
    check(pointers.verify_and_print(pointer_text, diagnostics), "ptr 存取和调用模型必须通过检查");
    const auto pointer_ir = pointer_text.str();
    check(pointer_ir.find("define internal ptr @pointer_roundtrip(ptr ") != std::string::npos &&
              pointer_ir.find("alloca ptr, align 4") != std::string::npos &&
              pointer_ir.find("store ptr ") != std::string::npos &&
              pointer_ir.find("load ptr, ptr ") != std::string::npos &&
              pointer_ir.find("call ptr @pointer_roundtrip(ptr ") != std::string::npos &&
              pointer_ir.find("ret ptr ") != std::string::npos &&
              pointer_ir.find("load i32, ptr ") != std::string::npos,
          "ptr 分配、存取、参数、call/ret 和目标读取必须打印各自的显式类型");
    auto scalars = make_module();
    add_bool_scalar(scalars.module());
    std::ostringstream scalar_text;
    check(scalars.verify_and_print(scalar_text, diagnostics), "bool 标量模型必须通过检查");
    const auto scalar_ir = scalar_text.str();
    check(scalar_ir.find("define internal i1 @bool_roundtrip(i1 %v_0)") != std::string::npos &&
              scalar_ir.find("alloca i8, align 1") != std::string::npos &&
              scalar_ir.find("= zext i1 %v_0 to i8") != std::string::npos &&
              scalar_ir.find("= trunc i8 %v_") != std::string::npos &&
              scalar_ir.find("ret i1 true") != std::string::npos,
          "bool 计算/参数/返回使用 i1，slot 使用 i8，并显式 zext/trunc");
    IRBuilder scalar_builder(scalars.module());
    const auto bool_errors_id = scalars.module().add_function(
        "bool_errors", {Type::Void, {}}, Linkage::Internal);
    auto &bool_errors = scalars.module().function(bool_errors_id);
    const auto bool_block = bool_errors.add_block();
    scalar_builder.set_insert_point(bool_block);
    const auto scalar_values = bool_errors.value_types.size();
    // builder 拒绝未知谓词时，也不能新增结果值或部分指令。
    for (const auto type : {Type::I1, Type::I32}) {
        rejects([&] {
            scalar_builder.compare(static_cast<Comparison>(999),
                                   Operand::constant(type, 0),
                                   Operand::constant(type, 1));
        });
    }
    rejects([&] { scalar_builder.zext(Type::I8, Operand::constant(Type::I32, 1)); });
    rejects([&] { scalar_builder.zext(Type::I1, Operand::constant(Type::I1, 1)); });
    // 非整数目标和相等位宽都不能构造扩展/截断。
    rejects([&] { scalar_builder.zext(Type::Ptr, Operand::constant(Type::I1, 1)); });
    for (const auto target : {Type::Void, Type::Ptr, static_cast<Type>(999)}) {
        rejects([&] {
            scalar_builder.trunc(target, Operand::constant(Type::I32, 1));
        });
    }
    rejects([&] { scalar_builder.trunc(Type::I1, Operand::constant(Type::I1, 0)); });
    rejects([&] { scalar_builder.trunc(Type::I8, Operand::constant(Type::I1, 0)); });
    rejects([&] { scalar_builder.trunc(Type::I32, Operand::constant(Type::I32, 0)); });
    rejects([&] { scalar_builder.entry_alloca(Type::I1, 1); });
    rejects([&] { scalar_builder.entry_alloca(Type::I8, 3); });
    check(bool_errors.value_types.size() == scalar_values &&
              bool_errors.entry_allocas.empty() &&
              bool_errors.blocks[0].instructions.empty() &&
              scalar_builder.insert_point() == bool_block,
          "无效 bool 转换或对齐不得产生部分指令");
    scalar_builder.ret();
    // 手工驱动必须把 bool 返回值扩展成 i32，再交给打印函数。
    auto driver = make_bool_driver();
    std::ostringstream driver_text;
    check(driver.verify_and_print(driver_text, diagnostics), "bool 真值驱动必须通过检查");
    check(driver_text.str().find("call i1 @bool_roundtrip(i1 true)") != std::string::npos &&
              driver_text.str().find("call i1 @bool_roundtrip(i1 false)") != std::string::npos &&
              driver_text.str().find("= zext i1 %v_") != std::string::npos,
          "bool 真值驱动必须分别传递 true/false 并零扩展后打印");
}
}

int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--test") {
            run_tests();
            return 0;
        }
        const auto mode = argc == 2 ? std::string_view(argv[1]) : std::string_view{};
        if (argc > 2 || (!mode.empty() && mode != "--invalid" &&
                         mode != "--emit-model" && mode != "--emit-bool"))
            return 2;
        auto artifact = mode == "--emit-bool" ? make_bool_driver()
                                              : make_module(mode == "--invalid");
        if (mode == "--emit-model") {
            add_typed_calls(artifact.module());
            add_storage(artifact.module());
            add_pointer_storage(artifact.module());
            add_bool_scalar(artifact.module());
            add_operations(artifact.module());
            add_branches(artifact.module());
            add_dominance_loop(artifact.module());
            add_aggregate_calls(artifact.module());
        }
        return artifact.verify_and_print(std::cout, std::cerr) ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
