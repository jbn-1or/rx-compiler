// 用 API 验证已求值标量接口；--emit 输出可由 Clang/REIMU 执行的自检模块。
#include "ast/ast.hpp"
#include "ir/artifact.hpp"
#include "ir/scalar.hpp"
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

rx::IRArtifact make_module() {
    rx::IRArtifact artifact("scalar-api");
    auto &context = artifact.context();
    auto &module = artifact.module();
    llvm::IRBuilder<> builder(context);
    const auto i32 = rx::SemanticType::int_(rx::SemanticType::IntKind::I32);
    const auto u32 = rx::SemanticType::int_(rx::SemanticType::IntKind::U32);
    const auto usize = rx::SemanticType::int_(rx::SemanticType::IntKind::Usize);
    const auto boolean = rx::SemanticType::bool_();
    struct Case {
        rx::BinaryOperator operation;
        rx::SemanticType type;
        uint32_t left, right, expected;
    };
    const Case cases[] = {
        {rx::BinaryOperator::LessThan, u32, 0x80000000u, 1, 0},
        {rx::BinaryOperator::GreaterThan, u32, 0xffffffffu, 1, 1},
        {rx::BinaryOperator::LessThan, usize, 0xffffffffu, 1, 0},
        {rx::BinaryOperator::LessThan, boolean, 0, 1, 1},
        {rx::BinaryOperator::GreaterThan, boolean, 1, 0, 1},
        {rx::BinaryOperator::LessThanOrEqual, boolean, 1, 0, 0},
        {rx::BinaryOperator::GreaterThanOrEqual, boolean, 0, 1, 0},
        {rx::BinaryOperator::BitwiseAnd, boolean, 1, 0, 0},
        {rx::BinaryOperator::BitwiseOr, boolean, 1, 0, 1},
        {rx::BinaryOperator::BitwiseXor, boolean, 1, 1, 0},
        {rx::BinaryOperator::Add, u32, 0xffffffffu, 1, 0},
        {rx::BinaryOperator::Add, u32, 0x7fffffffu, 1, 0x80000000u},
        {rx::BinaryOperator::Subtract, u32, 0, 1, 0xffffffffu},
        {rx::BinaryOperator::Multiply, u32, 0x80000000u, 2, 0},
        {rx::BinaryOperator::Add, usize, 0xffffffffu, 2, 1},
        {rx::BinaryOperator::Divide, u32, 0xffffffffu, 2, 0x7fffffffu},
        {rx::BinaryOperator::Remainder, u32, 0xffffffffu, 2, 1},
    };
    auto *main = llvm::Function::Create(
        llvm::FunctionType::get(builder.getInt32Ty(), false),
        llvm::Function::ExternalLinkage, "main", module);
    auto *entry = llvm::BasicBlock::Create(context, "entry", main);
    llvm::IRBuilder<> main_builder(context);
    main_builder.SetInsertPoint(entry);
    llvm::Value *passed = main_builder.getTrue();
    std::size_t index = 0;
    for (const auto &test : cases) {
        auto *type = rx::ir::value_type(context, test.type, llvm::errs());
        const bool comparison = test.operation == rx::BinaryOperator::LessThan ||
                                test.operation == rx::BinaryOperator::GreaterThan ||
                                test.operation == rx::BinaryOperator::LessThanOrEqual ||
                                test.operation == rx::BinaryOperator::GreaterThanOrEqual;
        auto *function = llvm::Function::Create(
            llvm::FunctionType::get(comparison ? builder.getInt1Ty() : type,
                                    {type, type}, false),
            llvm::Function::InternalLinkage, "scalar." + std::to_string(index++), module);
        builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", function));
        auto result = rx::ir::emit_scalar_binary(
            builder, test.operation, {function->getArg(0), test.type},
            {function->getArg(1), test.type}, llvm::errs());
        check(result && result->source_type.same_type(comparison ? boolean : test.type),
              "标量 API 必须独立于 AST 推导结果源类型");
        builder.CreateRet(result->value);
        // 参数形式避免在构造阶段折叠；O0 实测这些函数，O2 也必须保持相同结果。
        auto *actual = main_builder.CreateCall(function, {
            llvm::ConstantInt::get(type, test.left), llvm::ConstantInt::get(type, test.right)});
        auto *expected = llvm::ConstantInt::get(actual->getType(), test.expected);
        passed = main_builder.CreateAnd(passed, main_builder.CreateICmpEQ(actual, expected));
    }

    struct ShiftCase {
        rx::BinaryOperator operation;
        rx::SemanticType left_type;
        rx::SemanticType right_type;
        uint32_t left;
        uint32_t count;
        uint32_t expected;
    };
    const ShiftCase shifts[] = {
        {rx::BinaryOperator::ShiftLeft, i32, u32, 1, 0, 1},
        {rx::BinaryOperator::ShiftLeft, i32, u32, 1, 31, 0x80000000u},
        {rx::BinaryOperator::ShiftLeft, i32, i32, 1, 32, 1},
        {rx::BinaryOperator::ShiftLeft, i32, usize, 1, 33, 2},
        {rx::BinaryOperator::ShiftLeft, i32, u32, 1, 0xffffffffu, 0x80000000u},
        {rx::BinaryOperator::ShiftRight, i32, u32, 0xfffffff8u, 1, 0xfffffffcu},
        {rx::BinaryOperator::ShiftRight, i32, usize, 0xfffffff8u, 32, 0xfffffff8u},
        {rx::BinaryOperator::ShiftRight, i32, i32, 0xfffffff8u, 0xffffffffu, 0xffffffffu},
        {rx::BinaryOperator::ShiftRight, u32, i32, 0x80000000u, 1, 0x40000000u},
        {rx::BinaryOperator::ShiftRight, u32, usize, 0xffffffffu, 31, 1},
    };
    index = 0;
    for (const auto &test : shifts) {
        auto *function = llvm::Function::Create(
            llvm::FunctionType::get(builder.getInt32Ty(),
                                    {builder.getInt32Ty(), builder.getInt32Ty()}, false),
            llvm::Function::ExternalLinkage, "shift." + std::to_string(index++), module);
        builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", function));
        const rx::ir::IRValue left{function->getArg(0), test.left_type};
        const rx::ir::IRValue right{function->getArg(1), test.right_type};
        auto result = rx::ir::emit_scalar_binary(builder, test.operation,
                                                  left, right, llvm::errs());
        check(result && result->source_type.same_type(test.left_type) &&
                  result->value->getType()->isIntegerTy(32),
              "移位结果必须保留左操作数的源类型");
        auto *masked = llvm::dyn_cast<llvm::BinaryOperator>(result->value);
        check(masked && (masked->getOpcode() == llvm::Instruction::Shl ||
                         masked->getOpcode() == llvm::Instruction::AShr ||
                         masked->getOpcode() == llvm::Instruction::LShr),
              "移位必须生成对应的 LLVM 指令");
        const auto *count_mask = llvm::dyn_cast<llvm::BinaryOperator>(
            masked->getOperand(1));
        check(count_mask && count_mask->getOpcode() == llvm::Instruction::And &&
                  llvm::isa<llvm::ConstantInt>(count_mask->getOperand(1)) &&
                  llvm::cast<llvm::ConstantInt>(count_mask->getOperand(1))->getZExtValue() == 31 &&
                  !masked->hasNoSignedWrap() && !masked->hasNoUnsignedWrap(),
              "LLVM shift 必须掩码计数且不附加溢出承诺");
        builder.CreateRet(result->value);
        auto *actual = main_builder.CreateCall(function, {
            llvm::ConstantInt::get(builder.getInt32Ty(), test.left),
            llvm::ConstantInt::get(builder.getInt32Ty(), test.count)});
        passed = main_builder.CreateAnd(
            passed, main_builder.CreateICmpEQ(
                        actual, llvm::ConstantInt::get(builder.getInt32Ty(), test.expected)));
    }

    const rx::SemanticType integer_types[] = {
        rx::SemanticType::int_(rx::SemanticType::IntKind::I32),
        u32,
        rx::SemanticType::int_(rx::SemanticType::IntKind::Isize),
        usize,
    };
    index = 0;
    for (const auto &source_type : integer_types) {
        for (const auto &target_type : integer_types) {
            auto *function = llvm::Function::Create(
                llvm::FunctionType::get(builder.getInt32Ty(), {builder.getInt32Ty()}, false),
                llvm::Function::InternalLinkage, "cast.int." + std::to_string(index++), module);
            builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", function));
            const rx::ir::IRValue source{function->getArg(0), source_type};
            auto result = rx::ir::emit_scalar_cast(
                builder, source, target_type, llvm::errs());
            check(result && result->source_type.same_type(target_type) &&
                      result->value == function->getArg(0),
                  "整数之间的 cast 必须只改源类型并保留同一 i32 位模式");
            builder.CreateRet(result->value);
            auto *actual = main_builder.CreateCall(
                function, {llvm::ConstantInt::get(builder.getInt32Ty(), 0x80000000u)});
            passed = main_builder.CreateAnd(
                passed, main_builder.CreateICmpEQ(
                            actual, llvm::ConstantInt::get(builder.getInt32Ty(), 0x80000000u)));
        }
    }
    index = 0;
    for (const auto &target_type : integer_types) {
        auto *function = llvm::Function::Create(
            llvm::FunctionType::get(builder.getInt32Ty(), {builder.getInt1Ty()}, false),
            llvm::Function::InternalLinkage, "cast.bool." + std::to_string(index++), module);
        builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", function));
        const rx::ir::IRValue source{function->getArg(0), boolean};
        auto result = rx::ir::emit_scalar_cast(builder, source, target_type, llvm::errs());
        check(result && result->source_type.same_type(target_type) &&
                  llvm::isa<llvm::ZExtInst>(result->value),
              "bool 到整数的 cast 必须执行 i1 到 i32 的零扩展");
        builder.CreateRet(result->value);
        for (const bool value : {false, true}) {
            auto *actual = main_builder.CreateCall(function, {main_builder.getInt1(value)});
            passed = main_builder.CreateAnd(
                passed, main_builder.CreateICmpEQ(actual, main_builder.getInt32(value ? 1 : 0)));
        }
    }
    main_builder.CreateRet(main_builder.CreateSelect(
        passed, main_builder.getInt32(0), main_builder.getInt32(1)));

    // 下列 check 在宿主构造模块时立即执行；这个 LLVM 函数仅提供插入块。
    auto *errors_function = llvm::Function::Create(
        llvm::FunctionType::get(builder.getVoidTy(), false),
        llvm::Function::InternalLinkage, "invalid_requests", module);
    auto *errors_block = llvm::BasicBlock::Create(context, "entry", errors_function);
    builder.SetInsertPoint(errors_block);
    const rx::ir::IRValue one{builder.getInt32(1), i32};
    const rx::ir::IRValue flag{builder.getTrue(), boolean};
    std::string errors;
    llvm::raw_string_ostream diagnostics(errors);
    const auto rejects = [&](rx::BinaryOperator op, const rx::ir::IRValue &left,
                             const rx::ir::IRValue &right) {
        const auto count = errors_block->size();
        errors.clear();
        check(!rx::ir::emit_scalar_binary(builder, op, left, right, diagnostics) &&
                  !errors.empty() && errors_block->size() == count,
              "无效请求必须诊断失败且不发射指令");
    };
    rejects(rx::BinaryOperator::Add, one, {one.value, u32});
    rejects(rx::BinaryOperator::Add, one, {builder.getInt8(1), i32});
    rejects(rx::BinaryOperator::Add, one, {nullptr, i32});
    rejects(rx::BinaryOperator::Add, flag, flag);
    rejects(rx::BinaryOperator::LogicalAnd, flag, flag);
    rejects(rx::BinaryOperator::ShiftLeft, one, flag);
    for (const auto &target : {boolean, rx::SemanticType::unit()}) {
        const auto count = errors_block->size();
        errors.clear();
        check(!rx::ir::emit_scalar_cast(builder, one, target, diagnostics) &&
                  !errors.empty() && errors_block->size() == count,
              "不支持的 cast 目标必须在发射指令前拒绝");
    }
    check(!rx::ir::emit_scalar_unary(builder, rx::UnaryOperator::Negative,
                                     {one.value, u32}, diagnostics),
          "无符号整数负号必须拒绝");
    auto folded = rx::ir::emit_scalar_binary(builder, rx::BinaryOperator::Add,
                                              one, one, diagnostics);
    check(folded && llvm::isa<llvm::ConstantInt>(folded->value),
          "接口不得假设 IRBuilder 总是返回指令");
    builder.CreateRetVoid();
    rejects(rx::BinaryOperator::Add, one, one);
    check(!rx::ir::emit_scalar_unary(builder, rx::UnaryOperator::Not, flag, diagnostics),
          "已终结块中的一元运算必须拒绝");
    builder.ClearInsertionPoint();
    rejects(rx::BinaryOperator::Add, one, one);
    check(!rx::ir::emit_scalar_unary(builder, rx::UnaryOperator::Not, flag, diagnostics),
          "无插入块的一元运算必须拒绝");
    return artifact;
}

} // namespace

int main(int argc, char **argv) {
    try {
        auto artifact = make_module();
        if (argc == 2 && std::string_view(argv[1]) == "--emit")
            return artifact.verify_and_print(llvm::outs(), llvm::errs()) ? 0 : 1;
        if (argc != 1)
            return 2;
        std::string text;
        llvm::raw_string_ostream output(text);
        return artifact.verify_and_print(output, llvm::errs()) ? 0 : 1;
    } catch (const std::exception &error) {
        llvm::errs() << error.what() << '\n';
        return 1;
    }
}
