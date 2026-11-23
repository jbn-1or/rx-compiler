// 实现标量操作数读取、二元和一元运算以及 as 转换的 lowering。
#include "ir/lowering.hpp"
#include "ast/ast.hpp"
#include "ir/builder.hpp"

namespace rx {

// 把已经求出的操作数变成最终标量。
// 直接标量在自己的求值时刻就已经读出；共享引用只保留指针，
// 要等所有操作数都求值后，真正执行运算前才读取目标。
ir::Operand
GenerationContext::finish_scalar_operand(ir::Operand value,
                                         const SemanticType &type) {
    // sema 已保证运算操作数最多是一层共享引用，且其目标是直接标量。
    if (is_reference(type))
        return load_value(value, *type.inner);
    return value;
}

// 先求值左操作数，再求值右操作数；引用目标要等两侧都求值后才读取。
std::optional<EmitResult>
GenerationContext::emit_binary(const BinaryExpression &expression,
                               LocalPlaces &locals) {
    // 短路运算不能先求值两侧，必须在控制流中按左侧结果选择是否执行右侧。
    if (expression.operation == BinaryOperator::LogicalAnd ||
        expression.operation == BinaryOperator::LogicalOr)
        return emit_logical(expression, locals);
    const auto &left_type =
        semantics.expressions.at(expression.left.get()).type;
    const auto &right_type =
        semantics.expressions.at(expression.right.get()).type;
    // 非短路运算先完成两侧求值，引用操作数在这里只得到指针。
    auto left = emit_value(*expression.left, locals);
    if (!left)
        return std::nullopt;
    // 左操作数提前 return 时，右操作数和运算本身都不能再生成。
    if (path_ended(*left))
        return EmitResult{NoContinuation{}};
    auto right = emit_value(*expression.right, locals);
    if (!right)
        return std::nullopt;
    if (path_ended(*right))
        return EmitResult{NoContinuation{}};
    auto left_value = std::get<ir::Operand>(*left);
    auto right_value = std::get<ir::Operand>(*right);

    // 相等需要按任意类型结构化读取；引用相等留到 P10，不能误比较地址。
    if (expression.operation == BinaryOperator::Equal ||
        expression.operation == BinaryOperator::NotEqual) {
        if (is_reference(left_type) || is_reference(right_type)) {
            diagnostics << "IR 生成尚未支持引用相等比较（P10）\n";
            return std::nullopt;
        }
        auto result = emit_binary_operation(expression.operation, left_type,
                                            left_value, right_value);
        if (!result)
            return std::nullopt;
        return EmitResult{*result};
    }

    // 排序比较按源类型记录的引用层级逐层读取，最后比较目标标量。
    if (expression.operation == BinaryOperator::LessThan ||
        expression.operation == BinaryOperator::LessThanOrEqual ||
        expression.operation == BinaryOperator::GreaterThan ||
        expression.operation == BinaryOperator::GreaterThanOrEqual) {
        // 沿着引用层走到最终标量类型；sema 已保证两侧层级和基础类型匹配。
        const SemanticType *base_type = &left_type;
        while (is_reference(*base_type))
            base_type = base_type->inner.get();
        const auto load_reference_chain =
            [&](ir::Operand value, const SemanticType &type) {
                std::size_t depth = 0;
                const SemanticType *current = &type;
                while (is_reference(*current)) {
                    ++depth;
                    current = current->inner.get();
                }
                // 除最后一层外，每层读取的是下一层引用指针。
                for (std::size_t index = 0; index + 1 < depth; ++index)
                    value = load_value(value, ir::Type::Ptr);
                // 最后一层读取最终标量；depth 为 0 时值已经就是标量。
                if (depth > 0)
                    value = load_value(value, *base_type);
                return value;
            };
        auto left_scalar = load_reference_chain(left_value, left_type);
        auto right_scalar = load_reference_chain(right_value, right_type);
        auto result = emit_binary_operation(expression.operation, *base_type,
                                            left_scalar, right_scalar);
        if (!result)
            return std::nullopt;
        return EmitResult{*result};
    }

    // 算术、位运算和移位只允许一层共享引用；sema 已完成兼容性判断。
    const auto &operand_type =
        is_reference(left_type) ? *left_type.inner : left_type;
    auto left_scalar = finish_scalar_operand(left_value, left_type);
    auto right_scalar = finish_scalar_operand(right_value, right_type);
    auto result = emit_binary_operation(expression.operation, operand_type,
                                        left_scalar, right_scalar);
    if (!result)
        return std::nullopt;
    return EmitResult{*result};
}

// 只接收已求好的标量操作数，按源类型选择具体指令。
std::optional<ir::Operand> GenerationContext::emit_binary_operation(
    BinaryOperator operation, const SemanticType &operand_type,
    ir::Operand left, ir::Operand right) {
    // 只有 i32 和 isize 是有符号整数；bool 不是整数，不能沿用默认的 int_kind。
    const bool is_signed =
        operand_type.is_int() &&
        (operand_type.int_kind == SemanticType::IntKind::I32 ||
         operand_type.int_kind == SemanticType::IntKind::Isize);
    // 有符号比较只用于有符号整数；bool 和常见无符号整数都走无符号谓词。
    const auto ordered_comparison = [&](ir::Comparison signed_comparison,
                                        ir::Comparison unsigned_comparison) {
        return is_signed ? signed_comparison : unsigned_comparison;
    };
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
    case BinaryOperator::Equal:
        return builder.compare(ir::Comparison::Equal, left, right);
    case BinaryOperator::NotEqual:
        return builder.compare(ir::Comparison::NotEqual, left, right);
    // bool 只能用无符号比较，避免把 i1 的 true 当成有符号的 -1。
    case BinaryOperator::LessThan:
        return builder.compare(
            ordered_comparison(ir::Comparison::SignedLessThan,
                               ir::Comparison::UnsignedLessThan),
            left, right);
    case BinaryOperator::LessThanOrEqual:
        return builder.compare(
            ordered_comparison(ir::Comparison::SignedLessThanOrEqual,
                               ir::Comparison::UnsignedLessThanOrEqual),
            left, right);
    case BinaryOperator::GreaterThan:
        return builder.compare(
            ordered_comparison(ir::Comparison::SignedGreaterThan,
                               ir::Comparison::UnsignedGreaterThan),
            left, right);
    case BinaryOperator::GreaterThanOrEqual:
        return builder.compare(
            ordered_comparison(ir::Comparison::SignedGreaterThanOrEqual,
                               ir::Comparison::UnsignedGreaterThanOrEqual),
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
    // 短路逻辑由 emit_logical 生成控制流，不经过标量运算发射接口。
    case BinaryOperator::LogicalAnd:
    case BinaryOperator::LogicalOr:
        break;
    }
    diagnostics << "IR 生成尚未支持此二元运算\n";
    return std::nullopt;
}

// 生成一元运算：借用和解引用走 place 路径，取负和取反读取标量或一层共享引用的目标。
std::optional<EmitResult>
GenerationContext::emit_unary(const UnaryExpression &expression,
                              LocalPlaces &locals) {
    // 借用必须先决定取 place 地址还是物化值，不能先把操作数读成值。
    if (expression.operation == UnaryOperator::Borrow ||
        expression.operation == UnaryOperator::MutableBorrow)
        return emit_borrow(expression, locals);
    // 显式解引用先得到指针值，再按目标类型读取；不能复用普通值求值。
    if (expression.operation == UnaryOperator::Dereference)
        return emit_dereference(expression, locals);

    // 一元运算只有一个操作数：先求值它，再读取共享引用的目标。
    const auto &operand_type =
        semantics.expressions.at(expression.operand.get()).type;
    auto operand = emit_value(*expression.operand, locals);
    if (!operand)
        return std::nullopt;
    // 操作数提前结束路径时，外层运算也不能再生成。
    if (path_ended(*operand))
        return EmitResult{NoContinuation{}};
    const auto &type = semantics.expressions.at(&expression).type;
    const auto value = finish_scalar_operand(
        std::get<ir::Operand>(*operand), operand_type);
    switch (expression.operation) {
    // 语义阶段已保证取负结果为整数；按 32 位回绕用 0 减操作数，不附加不溢出承诺。
    case UnaryOperator::Negative:
        return EmitResult{
            builder.binary(ir::BinaryOp::Subtract,
                           ir::Operand::constant(ir::Type::I32, 0), value)};
    // bool 与 true 异或；整数与全 1 位模式异或，不套用布尔真值规则。
    case UnaryOperator::Not:
        if (type.kind == SemanticType::Kind::Bool)
            return EmitResult{builder.binary(
                ir::BinaryOp::Xor, value,
                ir::Operand::constant(ir::Type::I1, 1))};
        // 语义阶段已保证取反结果只能是 bool 或整数，排除 bool 后直接生成整数取反。
        return EmitResult{builder.binary(
            ir::BinaryOp::Xor, value,
            ir::Operand::constant(ir::Type::I32, 0xffffffffu))};
    case UnaryOperator::Dereference:
    case UnaryOperator::Borrow:
    case UnaryOperator::MutableBorrow:
        break;
    }
    diagnostics << "IR 生成尚未支持此一元运算\n";
    return std::nullopt;
}

// 生成 as 转换：整数之间只换源类型，bool 到整数先零扩展成 i32。
std::optional<EmitResult>
GenerationContext::emit_cast(const CastExpression &expression,
                             LocalPlaces &locals) {
    auto operand = emit_value(*expression.operand, locals);
    if (!operand)
        return std::nullopt;
    // 操作数提前结束路径时，cast 也不能再生成。
    if (path_ended(*operand))
        return EmitResult{NoContinuation{}};
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
