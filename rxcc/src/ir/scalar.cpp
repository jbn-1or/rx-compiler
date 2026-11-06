#include "ir/scalar.hpp"
#include "ast/ast.hpp"
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/raw_ostream.h>

namespace rx::ir {
namespace {

bool is_signed_integer(const SemanticType &type) {
    return type.kind == SemanticType::Kind::Int &&
           (type.int_kind == SemanticType::IntKind::I32 ||
            type.int_kind == SemanticType::IntKind::Isize);
}

bool can_emit(const llvm::IRBuilderBase &builder, const IRValue &operand) {
    const auto *block = builder.GetInsertBlock();
    if (!block || block->getTerminator() || !operand.value ||
        &operand.value->getContext() != &builder.getContext())
        return false;
    return (operand.source_type.kind == SemanticType::Kind::Int &&
            operand.value->getType()->isIntegerTy(32)) ||
           (operand.source_type.kind == SemanticType::Kind::Bool &&
            operand.value->getType()->isIntegerTy(1));
}

} // namespace

std::optional<IRValue> emit_scalar_binary(llvm::IRBuilderBase &builder,
                                          BinaryOperator operation,
                                          const IRValue &left, const IRValue &right,
                                          llvm::raw_ostream &diagnostics) {
    const bool shift = operation == BinaryOperator::ShiftLeft ||
                       operation == BinaryOperator::ShiftRight;
    const bool same_type = left.source_type.same_type(right.source_type);
    if (!can_emit(builder, left) || !can_emit(builder, right) ||
        (shift ? (left.source_type.kind != SemanticType::Kind::Int ||
                  right.source_type.kind != SemanticType::Kind::Int)
               : !same_type)) {
        diagnostics << "IR 内部错误：标量二元运算需要合法插入块和兼容的标量值\n";
        return std::nullopt;
    }
    const bool arithmetic = operation == BinaryOperator::Add ||
                            operation == BinaryOperator::Subtract ||
                            operation == BinaryOperator::Multiply ||
                            operation == BinaryOperator::Divide ||
                            operation == BinaryOperator::Remainder;
    if (arithmetic && left.source_type.kind != SemanticType::Kind::Int) {
        diagnostics << "IR 内部错误：标量算术需要整数操作数\n";
        return std::nullopt;
    }

    const bool signed_order = is_signed_integer(left.source_type);
    auto result_type = left.source_type;
    llvm::Value *result = nullptr;
    // 回绕算术不承诺 nsw/nuw，除余不承诺 exact。
    switch (operation) {
        case BinaryOperator::Add:
            result = builder.CreateAdd(left.value, right.value); break;
        case BinaryOperator::Subtract:
            result = builder.CreateSub(left.value, right.value); break;
        case BinaryOperator::Multiply:
            result = builder.CreateMul(left.value, right.value); break;
        case BinaryOperator::Divide:
            result = signed_order ? builder.CreateSDiv(left.value, right.value)
                                  : builder.CreateUDiv(left.value, right.value); break;
        case BinaryOperator::Remainder:
            result = signed_order ? builder.CreateSRem(left.value, right.value)
                                  : builder.CreateURem(left.value, right.value); break;
        case BinaryOperator::BitwiseAnd:
            result = builder.CreateAnd(left.value, right.value); break;
        case BinaryOperator::BitwiseOr:
            result = builder.CreateOr(left.value, right.value); break;
        case BinaryOperator::BitwiseXor:
            result = builder.CreateXor(left.value, right.value); break;
        case BinaryOperator::ShiftLeft: {
            auto *count = builder.CreateAnd(right.value, builder.getInt32(31));
            result = builder.CreateShl(left.value, count);
            break;
        }
        case BinaryOperator::ShiftRight: {
            auto *count = builder.CreateAnd(right.value, builder.getInt32(31));
            result = signed_order ? builder.CreateAShr(left.value, count)
                                  : builder.CreateLShr(left.value, count);
            break;
        }
        case BinaryOperator::Equal:
        case BinaryOperator::NotEqual:
        case BinaryOperator::LessThan:
        case BinaryOperator::LessThanOrEqual:
        case BinaryOperator::GreaterThan:
        case BinaryOperator::GreaterThanOrEqual: {
            llvm::CmpInst::Predicate predicate;
            switch (operation) {
                case BinaryOperator::Equal: predicate = llvm::CmpInst::ICMP_EQ; break;
                case BinaryOperator::NotEqual: predicate = llvm::CmpInst::ICMP_NE; break;
                case BinaryOperator::LessThan:
                    predicate = signed_order ? llvm::CmpInst::ICMP_SLT
                                             : llvm::CmpInst::ICMP_ULT; break;
                case BinaryOperator::LessThanOrEqual:
                    predicate = signed_order ? llvm::CmpInst::ICMP_SLE
                                             : llvm::CmpInst::ICMP_ULE; break;
                case BinaryOperator::GreaterThan:
                    predicate = signed_order ? llvm::CmpInst::ICMP_SGT
                                             : llvm::CmpInst::ICMP_UGT; break;
                case BinaryOperator::GreaterThanOrEqual:
                    predicate = signed_order ? llvm::CmpInst::ICMP_SGE
                                             : llvm::CmpInst::ICMP_UGE; break;
                default: return std::nullopt;
            }
            result = builder.CreateICmp(predicate, left.value, right.value);
            result_type = SemanticType::bool_();
            break;
        }
        case BinaryOperator::LogicalAnd:
        case BinaryOperator::LogicalOr:
            diagnostics << "IR 生成尚未支持该标量二元运算\n";
            return std::nullopt;
    }
    return IRValue{result, result_type};
}

std::optional<IRValue> emit_scalar_unary(llvm::IRBuilderBase &builder,
                                         UnaryOperator operation, const IRValue &operand,
                                         llvm::raw_ostream &diagnostics) {
    if (!can_emit(builder, operand)) {
        diagnostics << "IR 内部错误：标量一元运算需要合法插入块和标量值\n";
        return std::nullopt;
    }
    llvm::Value *result = nullptr;
    if (operation == UnaryOperator::Negative && is_signed_integer(operand.source_type)) {
        result = builder.CreateSub(builder.getInt32(0), operand.value);
    } else if (operation == UnaryOperator::Not) {
        result = builder.CreateXor(operand.value,
                                  llvm::ConstantInt::get(operand.value->getType(), -1, true));
    } else {
        diagnostics << "IR 生成尚未支持该类型的一元标量运算\n";
        return std::nullopt;
    }
    return IRValue{result, operand.source_type};
}

std::optional<IRValue> emit_scalar_cast(llvm::IRBuilderBase &builder,
                                        const IRValue &operand,
                                        const SemanticType &target_type,
                                        llvm::raw_ostream &diagnostics) {
    if (!can_emit(builder, operand) || target_type.kind != SemanticType::Kind::Int) {
        diagnostics << "IR 内部错误：标量 cast 需要有效值和整数目标类型\n";
        return std::nullopt;
    }
    if (operand.source_type.kind == SemanticType::Kind::Int)
        return IRValue{operand.value, target_type};
    if (operand.source_type.kind == SemanticType::Kind::Bool)
        return IRValue{builder.CreateZExt(operand.value, builder.getInt32Ty()), target_type};
    diagnostics << "IR 生成尚未支持此源类型的 cast\n";
    return std::nullopt;
}

} // namespace rx::ir
