// 实现函数体中的二元/一元运算、赋值、借用、解引用和 as 显式转换。
// 具体检查：
// - 算术 + - * / %：T 或一层共享引用 &T，两侧同一种整数 T，结果 T。
// - 位运算 & | ^：T 或 &T，两侧同一种整数或 bool，结果 T。
// - 移位 << >>：左右都是整数或 &整数，允许整数类型不同，结果取左侧标量类型。
// - 相等 == !=：两侧源类型完全相同且实现 PartialEq，结果 bool。
// - 排序 < <= > >=：同种标量，或层数和可变性匹配的引用；只允许最外层
//   左 &T / 右 &mut T 的调整，结果 bool。
// - 逻辑 && ||：两侧都必须是直接的 bool，结果 bool。
// - 一元 -：有符号整数或 &有符号整数；!：整数/bool 或 &整数/&bool。
// - 解引用 *：只接受 &T、&mut T、Box<T>，返回带权限的 place。
// - 借用 &/&mut：值物化临时 place，已有 place 保留权限；&mut 要求可写且未跨共享。
// - 赋值：左值必须是可变 place；普通 = 的右侧按左值类型检查并转换；复合赋值
//   左侧必须是直接标量且不自动解引用，右侧独立定型后按对应二元运算规则检查；
//   两者结果都是 ()。左值无效时仍遍历右侧，避免丢失独立诊断。
// - as：只允许整数→整数和 bool→整数；目标类型不向操作数传播期望，结果为目标整数类型。
// - 除 * 返回 place 外，其余运算结果都是值；每个操作数只检查一次。

#include "sema/body/checker.hpp"
#include "sema/sema.hpp"
#include <optional>
#include <utility>
#include <vector>

namespace rx {

namespace {

// 判断类型是否为不带引用层的直接标量（整数或 bool）。
bool is_direct_scalar(const SemanticType &type) {
    return type.kind == SemanticType::Kind::Int ||
           type.kind == SemanticType::Kind::Bool;
}

// 运算操作数只允许一层共享引用：T 或 &T。
// &mut、&& 以及 Box 都必须拒绝；这里不做通用的引用解引用。
std::optional<SemanticType> scalar_operand(const SemanticType &type) {
    if (is_direct_scalar(type))
        return type;
    if (type.kind == SemanticType::Kind::Reference && !type.is_mut &&
        type.inner && is_direct_scalar(*type.inner))
        return *type.inner;
    return std::nullopt;
}

// 判断整数类型是否为有符号整数（i32 或 isize）。
bool is_signed_integer(const SemanticType &type) {
    return type.kind == SemanticType::Kind::Int &&
           (type.int_kind == SemanticType::IntKind::I32 ||
            type.int_kind == SemanticType::IntKind::Isize);
}

// 排序比较保留完整引用层级：外层到内层的可变性 + 最终标量类型。
struct OrderingShape {
    std::vector<bool> mutable_layers;
    SemanticType base;
};

// 提取排序比较所需的完整引用层级和最终标量类型；遇到 Box 等非引用类型停止。
std::optional<OrderingShape> ordering_shape(const SemanticType &type) {
    OrderingShape shape;
    const SemanticType *current = &type;
    while (current->kind == SemanticType::Kind::Reference) {
        if (!current->inner)
            return std::nullopt;
        shape.mutable_layers.push_back(current->is_mut);
        current = current->inner.get();
    }
    if (!is_direct_scalar(*current))
        return std::nullopt;
    shape.base = *current;
    return shape;
}

// 判断两侧排序操作数的引用层级是否可匹配。
// 只允许最外层把右侧 &mut T 调整为 &T；更内层的可变性必须完全一致。
bool ordering_layers_match(const std::vector<bool> &left,
                           const std::vector<bool> &right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        // 唯一特例：左 &T、右 &mut T 可以把最外层调整为 &T。
        if (index == 0 && !left[index] && right[index])
            continue;
        if (left[index] != right[index])
            return false;
    }
    return true;
}

// 判断复合赋值是否属于算术类（+=、-=、*=、/=、%=）。
bool is_arithmetic_assignment(AssignmentOperator operation) {
    return operation == AssignmentOperator::AddAssignment ||
           operation == AssignmentOperator::SubtractAssignment ||
           operation == AssignmentOperator::MultiplyAssignment ||
           operation == AssignmentOperator::DivideAssignment ||
           operation == AssignmentOperator::RemainderAssignment;
}

// 判断复合赋值是否属于位运算类（&=、|=、^=）。
bool is_bitwise_assignment(AssignmentOperator operation) {
    return operation == AssignmentOperator::BitwiseAndAssignment ||
           operation == AssignmentOperator::BitwiseXorAssignment ||
           operation == AssignmentOperator::BitwiseOrAssignment;
}

// 判断复合赋值是否属于移位类（<<=、>>=）。
bool is_shift_assignment(AssignmentOperator operation) {
    return operation == AssignmentOperator::ShiftLeftAssignment ||
           operation == AssignmentOperator::ShiftRightAssignment;
}

} // namespace

// 检查二元表达式：算术、位运算、移位、相等、排序和惰性逻辑。
// 两侧操作数独立定型，不接收外层期望类型；结果除比较/逻辑为 bool 外，
// 按运算规则返回对应标量类型，且不产生 place。
ExpressionResult
BodyChecker::check_binary(const BinaryExpression &expression) {
    if (!expression.left || !expression.right)
        return ExpressionResult::error();

    // 二元运算的操作数都独立定型；外层期望类型不向它们传播（CV3）。
    const ExpressionResult left =
        check_expression(*expression.left, nullptr);
    const ExpressionResult right =
        check_expression(*expression.right, nullptr);
    if (left.type.is_error() || right.type.is_error())
        return ExpressionResult::error();

    switch (expression.operation) {
        case BinaryOperator::Add:
        case BinaryOperator::Subtract:
        case BinaryOperator::Multiply:
        case BinaryOperator::Divide:
        case BinaryOperator::Remainder: {
            const auto left_base = scalar_operand(left.type);
            const auto right_base = scalar_operand(right.type);
            if (!left_base || !right_base ||
                left_base->kind != SemanticType::Kind::Int ||
                right_base->kind != SemanticType::Kind::Int ||
                !left_base->same_type(*right_base)) {
                diagnostics_.error(
                    expression.span,
                    "arithmetic operands must have the same integer type");
                return ExpressionResult::error();
            }
            return ExpressionResult::value(*left_base);
        }

        case BinaryOperator::BitwiseAnd:
        case BinaryOperator::BitwiseXor:
        case BinaryOperator::BitwiseOr: {
            const auto left_base = scalar_operand(left.type);
            const auto right_base = scalar_operand(right.type);
            if (!left_base || !right_base ||
                !left_base->same_type(*right_base)) {
                diagnostics_.error(
                    expression.span,
                    "bitwise operands must have the same integer or bool type");
                return ExpressionResult::error();
            }
            return ExpressionResult::value(*left_base);
        }

        case BinaryOperator::ShiftLeft:
        case BinaryOperator::ShiftRight: {
            const auto left_base = scalar_operand(left.type);
            const auto right_base = scalar_operand(right.type);
            if (!left_base || !right_base ||
                left_base->kind != SemanticType::Kind::Int ||
                right_base->kind != SemanticType::Kind::Int) {
                diagnostics_.error(expression.span,
                                   "shift operands must be integers");
                return ExpressionResult::error();
            }
            return ExpressionResult::value(*left_base);
        }

        case BinaryOperator::Equal:
        case BinaryOperator::NotEqual: {
            if (!left.type.same_type(right.type)) {
                diagnostics_.error(
                    expression.span,
                    "equality operands must have exactly the same type");
                return ExpressionResult::error();
            }

            if (!analyzer_.has_trait(left.type, Derive::PartialEq)) {
                diagnostics_.error(
                    expression.span,
                    "equality requires PartialEq on the operand type");
                return ExpressionResult::error();
            }
            return ExpressionResult::value(SemanticType::bool_());
        }

        case BinaryOperator::LessThan:
        case BinaryOperator::LessThanOrEqual:
        case BinaryOperator::GreaterThan:
        case BinaryOperator::GreaterThanOrEqual: {
            const auto left_shape = ordering_shape(left.type);
            const auto right_shape = ordering_shape(right.type);
            if (!left_shape || !right_shape ||
                !left_shape->base.same_type(right_shape->base) ||
                !ordering_layers_match(left_shape->mutable_layers,
                                       right_shape->mutable_layers)) {
                diagnostics_.error(
                    expression.span,
                    "ordering operands must be matching scalars or references");
                return ExpressionResult::error();
            }
            return ExpressionResult::value(SemanticType::bool_());
        }

        case BinaryOperator::LogicalAnd:
        case BinaryOperator::LogicalOr: {
            if (left.type.kind != SemanticType::Kind::Bool ||
                right.type.kind != SemanticType::Kind::Bool) {
                diagnostics_.error(
                    expression.span,
                    "logical operators require bool operands");
                return ExpressionResult::error();
            }
            return ExpressionResult::value(SemanticType::bool_());
        }
    }

    return ExpressionResult::error();
}

// 检查一元表达式：取负、按位/逻辑取反、解引用、共享借用和可变借用。
// 解引用复用 place 工具并返回 place；借用返回引用值；其余返回标量值。
ExpressionResult BodyChecker::check_unary(const UnaryExpression &expression) {
    if (!expression.operand)
        return ExpressionResult::error();

    // 一元运算的操作数独立定型，不吸收外层期望类型。
    const ExpressionResult operand =
        check_expression(*expression.operand, nullptr);
    if (operand.type.is_error())
        return ExpressionResult::error();

    // 解引用 * → dereference_place
    // 借用 & / &mut → 必要时 materialize_place，检查可变借用权限
    // 其他一元运算 → scalar_operand / is_signed_integer 等类型判断
    switch (expression.operation) {
        case UnaryOperator::Negative: {
            const auto base = scalar_operand(operand.type);
            if (!base || !is_signed_integer(*base)) {
                diagnostics_.error(
                    expression.span,
                    "unary minus requires a signed integer");
                return ExpressionResult::error();
            }
            return ExpressionResult::value(*base);
        }

        case UnaryOperator::Not: {
            const auto base = scalar_operand(operand.type);
            if (!base) {
                diagnostics_.error(
                    expression.span,
                    "logical not requires an integer or bool");
                return ExpressionResult::error();
            }
            return ExpressionResult::value(*base);
        }

        case UnaryOperator::Dereference:
            return dereference_place(operand.type, operand.place,
                                     expression.span);

        case UnaryOperator::Borrow:
        case UnaryOperator::MutableBorrow: {
            if (operand.type.is_never()) {
                diagnostics_.error(expression.span,
                                   "cannot borrow a diverging expression");
                return ExpressionResult::error();
            }

            // 值表达式物化为临时 place；已有 place 保持原访问权限。
            const auto place = materialize_place(operand.place);

            const bool mutable_borrow =
                expression.operation == UnaryOperator::MutableBorrow;
            if (mutable_borrow &&
                (!place.is_mutable || place.crossed_shared)) {
                diagnostics_.error(
                    expression.span,
                    "cannot mutably borrow an immutable or shared place");
                return ExpressionResult::error();
            }

            return ExpressionResult::value(
                SemanticType::reference(mutable_borrow, operand.type));
        }
    }

    return ExpressionResult::error();
}

// 检查普通赋值和复合赋值。
// 左值必须是可变 place；普通赋值的右侧接收左值类型期望，
// 复合赋值右侧独立定型并按对应二元运算规则检查，结果均为 unit。
ExpressionResult
BodyChecker::check_assignment(const AssignmentExpression &expression) {
    if (!expression.left || !expression.right)
        return ExpressionResult::error();

    // 左值已经无效时，右侧仍是独立表达式；继续遍历它，避免错误恢复阶段
    // 漏掉右侧的名字/类型/发散诊断。
    auto check_right_after_invalid_left = [&]() {
        check_expression(*expression.right, nullptr);
        return ExpressionResult::error();
    };
    
    // 检查左运算数
    const ExpressionResult left =
        check_expression(*expression.left, nullptr);
    if (left.type.is_error())
        return check_right_after_invalid_left();
    if (!left.place.is_place) {
        diagnostics_.error(expression.left->span,
                           "assignment target is not a place");
        return check_right_after_invalid_left();
    }
    if (!left.place.is_mutable) {
        diagnostics_.error(expression.left->span,
                           "assignment target is not mutable");
        return check_right_after_invalid_left();
    }

    // // 普通赋值
    if (expression.operation == AssignmentOperator::Assign) {
        // 普通赋值是固定目标：右侧以左侧类型为期望，再统一做一对一转换。
        const ExpressionResult right =
            check_expression(*expression.right, &left.type);
        if (!require_coerce(right, left.type, expression.right->span))
            return ExpressionResult::error();
        return ExpressionResult::value(SemanticType::unit());
    }

    // 复合赋值
    const bool arithmetic = is_arithmetic_assignment(expression.operation);
    const bool bitwise = is_bitwise_assignment(expression.operation);
    const bool shift = is_shift_assignment(expression.operation);
    if (!arithmetic && !bitwise && !shift)
        return check_right_after_invalid_left();

    // 复合赋值左侧必须是原始标量 place，引用目标不自动解引用（M7）。
    const SemanticType *left_scalar = &left.type;
    if (!is_direct_scalar(*left_scalar)) {
        diagnostics_.error(
            expression.left->span,
            "compound assignment target must have scalar type");
        return check_right_after_invalid_left();
    }
    if ((arithmetic || shift) &&
        left_scalar->kind != SemanticType::Kind::Int) {
        diagnostics_.error(
            expression.left->span,
            "compound assignment requires an integer target");
        return check_right_after_invalid_left();
    }

    // 复合赋值右侧不接收左侧类型作为期望类型（CV3/M7）。
    const ExpressionResult right =
        check_expression(*expression.right, nullptr);
    if (right.type.is_error())
        return ExpressionResult::error();

    const auto right_scalar = scalar_operand(right.type);
    if (!right_scalar) {
        diagnostics_.error(
            expression.span,
            "compound assignment right operand must be a scalar or one shared reference");
        return ExpressionResult::error();
    }

    if (shift) {
        if (right_scalar->kind != SemanticType::Kind::Int) {
            diagnostics_.error(expression.span,
                               "shift operands must be integers");
            return ExpressionResult::error();
        }
    } else if (!left_scalar->same_type(*right_scalar)) {
        diagnostics_.error(
            expression.span,
            arithmetic
                ? "arithmetic operands must have the same integer type"
                : "bitwise operands must have the same integer or bool type");
        return ExpressionResult::error();
    }

    return ExpressionResult::value(SemanticType::unit());
}

// 检查 as 显式转换：只允许整数→整数和 bool→整数。
// 目标类型不向操作数传播期望类型，结果为目标整数类型且不是 place。
ExpressionResult BodyChecker::check_cast(const CastExpression &expression) {
    if (!expression.operand || !expression.target)
        return ExpressionResult::error();

    const SemanticType target =
        analyzer_.resolve_type(*expression.target, current_owner_);
    // 目标类型已经出错时，操作数仍要遍历，保留其中独立的名字/类型诊断。
    if (target.is_error()) {
        check_expression(*expression.operand, nullptr);
        return ExpressionResult::error();
    }

    // 检查源和目标类型，返回目标类型的值
    const bool target_is_integer = target.kind == SemanticType::Kind::Int;
    if (!target_is_integer) {
        diagnostics_.error(expression.span,
                           "cast target must be an integer type");
    }

    // as 的目标类型不向操作数提供期望类型（CV3/CV8）。
    const ExpressionResult operand =
        check_expression(*expression.operand, nullptr);
    if (operand.type.is_error())
        return ExpressionResult::error();

    const bool operand_is_scalar =
        operand.type.kind == SemanticType::Kind::Int ||
        operand.type.kind == SemanticType::Kind::Bool;
    if (!operand_is_scalar) {
        diagnostics_.error(
            expression.span,
            "casts require an integer or bool operand");
    }

    if (!target_is_integer || !operand_is_scalar)
        return ExpressionResult::error();
    return ExpressionResult::value(target);
}

} // namespace rx
