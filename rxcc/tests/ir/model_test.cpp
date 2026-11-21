// P2/P3 独立验证：模型所有权、call/ret/存储/转换、结构检查和确定的文本输出。
#include "ir/artifact.hpp"
#include "ir/builder.hpp"
#include <iomanip>
#include <iostream>
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

// 构造一个完整的 bool 往返函数，覆盖 i1 计算、i8 存取和 zext/trunc。
void add_bool_scalar(Module &module) {
    IRBuilder builder(module);
    const auto id = module.add_function("bool_roundtrip", {Type::I1, {Type::I1}}, Linkage::Internal);
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
        "operations", {Type::I32, {Type::I32, Type::I32}}, Linkage::Internal);
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
        "bool_operations", {Type::I1, {Type::I1, Type::I1}},
        Linkage::Internal);
    auto &bool_function = module.function(bool_id);
    builder.set_insert_point(bool_function.add_block());
    const auto bool_left = bool_function.argument(0);
    const auto bool_right = bool_function.argument(1);
    const auto both = builder.binary(BinaryOp::And, bool_left, bool_right);
    builder.compare(Comparison::UnsignedLessThan, bool_left, bool_right);
    builder.ret(both);
}

// 手工构造 bool 真值驱动：把往返结果零扩展成 i32 后打印。
// 这是模型层的观察手段，不表示源码已经支持把 bool 直接传给 println_i32。
rx::IRArtifact make_bool_driver() {
    rx::IRArtifact artifact;
    auto &module = artifact.module();
    IRBuilder builder(module);
    module.add_function("println_i32", {Type::Void, {Type::I32}}, Linkage::External);
    const auto roundtrip =
        module.add_function("bool_roundtrip", {Type::I1, {Type::I1}}, Linkage::Internal);
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
    reject_model([](Module &m) { std::get<Alloca>(m.find_function("storage")->entry_allocas[0].operation).alignment = 3; });
    reject_model([](Module &m) { m.find_function("storage")->entry_allocas[0].type = Type::I32; });
    reject_model([](Module &m) { m.find_function("storage")->entry_allocas[1].result = m.find_function("storage")->entry_allocas[0].result; });
    reject_model([](Module &m) { std::get<Store>(m.find_function("storage")->blocks[0].instructions[0].operation).address = Operand::constant(Type::I32, 0); });
    reject_model([](Module &m) { std::get<Load>(m.find_function("storage")->blocks[0].instructions[1].operation).storage_type = Type::Ptr; });
    reject_model([](Module &m) { std::get<Store>(m.find_function("storage")->blocks[0].instructions[0].operation).alignment = 0; });
    reject_model([](Module &m) { m.find_function("storage")->blocks[0].instructions.push_back(m.find_function("storage")->entry_allocas[0]); });
    reject_model([](Module &m) { m.find_function("external_io")->entry_allocas.push_back(m.find_function("storage")->entry_allocas[0]); });
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
    rejects([&] { module.add_function("bad", {Type::Void, {Type::Void}}, Linkage::External); });
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
    check_invalid_models();
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
    rejects([&] { memory.load(Type::Ptr, address, 4); });
    rejects([&] { memory.load(Type::I32, Operand::constant(Type::I32, 0), 4); });
    rejects([&] { memory.store(Type::I32, address, address, 4); });
    rejects([&] { memory.store(Type::I32, Operand::constant(Type::I32, 1), address, 0); });
    check(memory_function.value_types.size() == values && memory_function.entry_allocas.size() == 1 &&
              memory_function.blocks[0].instructions.empty() && memory.insert_point() == block,
          "无效存储请求不得产生部分指令或改变状态");
    memory.ret();
    rejects([&] { memory.store(Type::I32, Operand::constant(Type::I32, 1), address, 4); });
    memory.set_insert_point(block);
    rejects([&] { memory.entry_alloca(Type::I32, 4); });
    rejects([&] { memory.load(Type::I32, address, 4); });
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
            add_bool_scalar(artifact.module());
            add_operations(artifact.module());
        }
        return artifact.verify_and_print(std::cout, std::cerr) ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
