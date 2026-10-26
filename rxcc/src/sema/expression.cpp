// 实现表达式检查分派和最小字面量规则。
// 处理整数/布尔/unit 字面量、路径、块、普通调用，并识别 return 的发散类型；
// 其他表达式类别由后续步骤接入，当前返回 Error 占位，保留已有诊断并避免连锁报错。

#include "sema/body.hpp"
#include "sema/sema.hpp"
#include "sema/type_utils.hpp"

namespace rx {

// 整数宽度统一为 32 位；这里只定型，不检查数值范围。
// 有后缀时后缀优先；无后缀时吸收整数期望类型，否则回退 i32。
ExpressionResult
BodyChecker::check_integer_literal(const IntegerLiteral &literal,
                                   const SemanticType *expected) {
    // 先假设失败，下面三种情况会给出真正的类型。
    SemanticType type = SemanticType::error();

    if (literal.suffix != IntSuffix::None) {
        // 写明了后缀，就按后缀来，比如 1u32 是 u32。
        type = type_from_suffix(literal.suffix);
    } else if (expected && expected->is_int()) {
        // 没写后缀，但外面要求一个整数，就跟着外面的要求，
        // 比如 let x: usize = 1; 里的 1 按 usize 算。
        type = *expected;
    } else {
        // 没写后缀，外面也没有整数要求，默认是 i32。
        type = SemanticType::int_(SemanticType::IntKind::I32);
    }

    // 数字是值，本身不是一个可以存取的变量。
    return ExpressionResult{type, PlaceAccessInformation{}};
}

// 先判断表达式是哪一种，再分别算出它的类型和“能不能当存储位置”。
// expected 是外面希望这里得到的类型，可以为空（表示外面没有要求）。
ExpressionResult
BodyChecker::check_expression(const Expression &expression,
                              const SemanticType *expected) {
    // 数字：有后缀就按后缀；没后缀但外面要整数就跟着外面；否则按 i32。
    if (const auto *literal = dynamic_cast<const IntegerLiteral *>(&expression))
        return check_integer_literal(*literal, expected);

    // true / false：结果是 bool，本身不是一个可存的变量。
    if (dynamic_cast<const BooleanLiteral *>(&expression))
        return ExpressionResult{SemanticType::bool_(),
                                PlaceAccessInformation{}};

    // ()：结果是 ()，本身也不是一个可存的变量。
    if (dynamic_cast<const UnitExpression *>(&expression))
        return ExpressionResult{SemanticType::unit(),
                                PlaceAccessInformation{}};

    // 名字路径：可能是局部变量、常量或 self，交给专门的值路径检查。
    if (const auto *path = dynamic_cast<const PathExpression *>(&expression))
        return check_value_path(*path);

    // 普通函数调用 / 内置 I/O：4.4 只处理单段路径 callee。
    if (const auto *call = dynamic_cast<const CallExpression *>(&expression))
        return check_call(*call);

    // 块：检查里面的语句和最后的值；外面希望的类型继续往里传。
    if (const auto *block = dynamic_cast<const BlockExpression *>(&expression))
        return check_block(*block, expected);

    // return：它不产生值，执行后后面的代码也不会再跑，所以类型是 !。
    // 有返回值时按函数声明的返回类型检查；没有返回值时要求返回类型是 ()。
    if (const auto *return_expression =
            dynamic_cast<const ReturnExpression *>(&expression)) {
        if (current_function_) {
            if (return_expression->value) {
                const ExpressionResult value = check_expression(
                    *return_expression->value,
                    &current_function_->return_type);
                require_coerce(value, current_function_->return_type,
                               return_expression->value->span);
            } else {
                require_coerce(
                    ExpressionResult{SemanticType::unit(),
                                     PlaceAccessInformation{}},
                    current_function_->return_type, return_expression->span);
            }
        }
        return ExpressionResult{SemanticType::never(),
                                PlaceAccessInformation{}};
    }

    // 这一步还没实现的表达式：先返回 Error，让检查能继续；
    // 它们会在后面的步骤里逐类补上。
    return ExpressionResult{SemanticType::error(), PlaceAccessInformation{}};
}

} // namespace rx
