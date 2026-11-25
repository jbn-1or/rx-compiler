// 实现标量操作数读取、二元和一元运算以及 as 转换的 lowering。
#include "ir/lowering.hpp"
#include "ast/ast.hpp"
#include "ir/builder.hpp"
#include <stdexcept>

namespace rx {

// 把已经求出的操作数变成最终标量。
// 直接标量在自己的求值时刻就已经读出；共享引用只保留指针，
// 要等所有操作数都求值后，真正执行运算前才读取目标。
ir::Operand
GenerationContext::finish_scalar_operand(ir::Operand value,
                                         const SemanticType &type) {
    // 排序允许多层引用；其他标量运算的层数限制已由 sema 检查。
    const SemanticType *current = &type;
    while (is_reference(*current)) {
        current = current->inner.get();
        value = load_value(value, *current);
    }
    return value;
}

// 先求值左操作数，再求值右操作数；引用目标要等两侧都求值后才读取。
std::optional<EmitResult>
GenerationContext::emit_binary(const BinaryExpression &expression,
                               LocalPlaces &locals, ResultUse use) {
    // 短路运算不能先求值两侧，必须在控制流中按左侧结果选择是否执行右侧。
    if (expression.operation == BinaryOperator::LogicalAnd ||
        expression.operation == BinaryOperator::LogicalOr)
        return emit_logical(expression, locals, use);
    if (expression.operation == BinaryOperator::Equal ||
        expression.operation == BinaryOperator::NotEqual) {
        // 比较本身没有副作用；丢弃结果时仍需依次执行两侧表达式。
        if (use == ResultUse::Discard) {
            auto left = emit_discarded_expression(*expression.left, locals);
            if (!left || path_ended(*left))
                return left;
            return emit_discarded_expression(*expression.right, locals);
        }
        return emit_equality(expression, locals);
    }
    const auto &left_type =
        semantics.expressions.at(expression.left.get()).type;
    const auto &right_type =
        semantics.expressions.at(expression.right.get()).type;
    // 非短路运算先完成两侧求值，引用操作数在这里只得到指针。
    auto left = emit_value(*expression.left, locals);
    if (!left || path_ended(*left))
        return left;
    auto right = emit_value(*expression.right, locals);
    if (!right || path_ended(*right))
        return right;

    const auto left_scalar =
        finish_scalar_operand(std::get<ir::Operand>(*left), left_type);
    const auto right_scalar =
        finish_scalar_operand(std::get<ir::Operand>(*right), right_type);
    const SemanticType *operand_type = &left_type;
    while (is_reference(*operand_type))
        operand_type = operand_type->inner.get();
    return emit_binary_operation(expression.operation, *operand_type,
                                 left_scalar, right_scalar);
}

// 只接收已求好的标量操作数，按源类型选择具体指令。
ir::Operand GenerationContext::emit_binary_operation(
    BinaryOperator operation, const SemanticType &operand_type,
    ir::Operand left, ir::Operand right) {
    // 只有 i32 和 isize 是有符号整数；bool 不是整数，不能沿用默认的 int_kind。
    const bool is_signed =
        operand_type.is_int() &&
        (operand_type.int_kind == SemanticType::IntKind::I32 ||
         operand_type.int_kind == SemanticType::IntKind::Isize);
    switch (operation) {
    case BinaryOperator::Add:
        return builder.binary(ir::BinaryOp::Add, left, right);
    case BinaryOperator::Subtract:
        return builder.binary(ir::BinaryOp::Subtract, left, right);
    case BinaryOperator::Multiply:
        return builder.binary(ir::BinaryOp::Multiply, left, right);
    case BinaryOperator::Divide:
        return builder.binary(is_signed ? ir::BinaryOp::SignedDivide
                                        : ir::BinaryOp::UnsignedDivide,
                              left, right);
    case BinaryOperator::Remainder:
        return builder.binary(is_signed ? ir::BinaryOp::SignedRemainder
                                        : ir::BinaryOp::UnsignedRemainder,
                              left, right);
    case BinaryOperator::BitwiseAnd:
        return builder.binary(ir::BinaryOp::And, left, right);
    case BinaryOperator::BitwiseOr:
        return builder.binary(ir::BinaryOp::Or, left, right);
    case BinaryOperator::BitwiseXor:
        return builder.binary(ir::BinaryOp::Xor, left, right);
    // 有符号整数用有符号谓词；bool 和无符号整数用无符号谓词，避免把 true 当成 -1。
    case BinaryOperator::LessThan:
        return builder.compare(
            is_signed ? ir::Comparison::SignedLessThan
                      : ir::Comparison::UnsignedLessThan,
            left, right);
    case BinaryOperator::LessThanOrEqual:
        return builder.compare(
            is_signed ? ir::Comparison::SignedLessThanOrEqual
                      : ir::Comparison::UnsignedLessThanOrEqual,
            left, right);
    case BinaryOperator::GreaterThan:
        return builder.compare(
            is_signed ? ir::Comparison::SignedGreaterThan
                      : ir::Comparison::UnsignedGreaterThan,
            left, right);
    case BinaryOperator::GreaterThanOrEqual:
        return builder.compare(
            is_signed ? ir::Comparison::SignedGreaterThanOrEqual
                      : ir::Comparison::UnsignedGreaterThanOrEqual,
            left, right);
    // 移位量只保留低 5 位；即使右侧是字面量也保留这条掩码指令。
    case BinaryOperator::ShiftLeft:
    case BinaryOperator::ShiftRight: {
        const auto count = builder.binary(
            ir::BinaryOp::And, right,
            ir::Operand::constant(ir::Type::I32, 31));
        const auto shift =
            operation == BinaryOperator::ShiftLeft
                ? ir::BinaryOp::ShiftLeft
                : is_signed ? ir::BinaryOp::ArithmeticShiftRight
                            : ir::BinaryOp::LogicalShiftRight;
        return builder.binary(shift, left, count);
    }
    // 这些运算已由 emit_binary 单独分派。
    case BinaryOperator::Equal:
    case BinaryOperator::NotEqual:
    case BinaryOperator::LogicalAnd:
    case BinaryOperator::LogicalOr:
        break;
    }
    throw std::logic_error("不是标量二元运算");
}

// 生成借用、取负和取反；显式解引用由表达式分派按 place 读取。
std::optional<EmitResult>
GenerationContext::emit_unary(const UnaryExpression &expression,
                              LocalPlaces &locals) {
    // 借用必须先决定取 place 地址还是物化值，不能先把操作数读成值。
    if (expression.operation == UnaryOperator::Borrow ||
        expression.operation == UnaryOperator::MutableBorrow)
        return emit_borrow(expression, locals);
    // 一元运算只有一个操作数：先求值它，再读取共享引用的目标。
    const auto &operand_type =
        semantics.expressions.at(expression.operand.get()).type;
    auto operand = emit_value(*expression.operand, locals);
    if (!operand || path_ended(*operand))
        return operand;
    const auto value = finish_scalar_operand(
        std::get<ir::Operand>(*operand), operand_type);
    // 语义阶段已保证取负结果为整数；按 32 位回绕用 0 减操作数，不附加不溢出承诺。
    if (expression.operation == UnaryOperator::Negative)
        return EmitResult{
            builder.binary(ir::BinaryOp::Subtract,
                           ir::Operand::constant(ir::Type::I32, 0), value)};
    // 剩下的是取反：bool 与 true 异或，整数与全 1 位模式异或。
    const auto mask = semantics.expressions.at(&expression).type.kind == SemanticType::Kind::Bool
                          ? ir::Operand::constant(ir::Type::I1, 1)
                          : ir::Operand::constant(ir::Type::I32, 0xffffffffu);
    return builder.binary(ir::BinaryOp::Xor, value, mask);
}

// 生成 as 转换：整数之间只换源类型，bool 到整数先零扩展成 i32。
std::optional<EmitResult>
GenerationContext::emit_cast(const CastExpression &expression,
                             LocalPlaces &locals) {
    auto operand = emit_value(*expression.operand, locals);
    if (!operand || path_ended(*operand))
        return operand;
    const auto value = std::get<ir::Operand>(*operand);
    // 语义阶段只允许整数到整数和 bool 到整数；整数 cast 保留 i32 位模式，
    // 因此不生成指令。目标源类型已记录在 cast 表达式的语义结果中，
    // 后续运算会按该类型选择有符号或无符号指令。
    const auto &source_type =
        semantics.expressions.at(expression.operand.get()).type;
    if (source_type.kind == SemanticType::Kind::Bool)
        return EmitResult{builder.zext(ir::Type::I32, value)};
    return EmitResult{value};
}

} // namespace rx
