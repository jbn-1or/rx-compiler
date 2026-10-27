// 实现表达式检查分派、字面量规则和 return。
// 统一返回 ExpressionResult（类型 + place），expected 由具体处理器决定是否使用。
//
// 具体检查：
// - 整数文字：有后缀用后缀；无后缀且期望是整数则取期望；否则 i32。
//   四种整数都是 32 位；这里只定型，不做数值范围检查。
// - bool/unit 字面量直接得到 bool/()，都不是 place。
// - 路径交给 value_path.cpp；普通调用与多段关联调用交给 call.cpp（5.1 先
//   选定目标）；点方法调用交给 method.cpp（5.4 先选定目标）。
// - 二元/一元/赋值/cast 交给 operators.cpp；操作数默认不吸收外层期望。
// - 结构体/字段/数组/下标交给 aggregate.cpp。
// - if/loop/while/break/continue 交给 control_flow.cpp。
// - 块交给 body.cpp；块尾期望继续下传，块结果不继承尾表达式的 place。
// - return 以当前函数返回类型为期望检查值；裸 return 等价于 ()；
//   return 表达式自身为 !。
// - 点方法由 method.cpp 查找目标、调整接收者并检查普通实参。

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

    // 普通函数调用 / 内置 I/O / 显式关联调用：4.4/5.1–5.3。
    if (const auto *call = dynamic_cast<const CallExpression *>(&expression))
        return check_call(*call);

    // 点方法调用：5.4 先按接收者候选顺序选定目标。
    if (const auto *method_call =
            dynamic_cast<const MethodCallExpression *>(&expression))
        return check_method_call(*method_call);

    // 运算、赋值、借用、解引用和 cast：4.5 的节点不吸收外层期望类型。
    if (const auto *binary = dynamic_cast<const BinaryExpression *>(&expression))
        return check_binary(*binary);
    if (const auto *unary = dynamic_cast<const UnaryExpression *>(&expression))
        return check_unary(*unary);
    if (const auto *assignment =
            dynamic_cast<const AssignmentExpression *>(&expression))
        return check_assignment(*assignment);
    if (const auto *cast = dynamic_cast<const CastExpression *>(&expression))
        return check_cast(*cast);

    // 4.6 聚合：结构体构造与数组表达式是值；字段和索引返回 place。
    if (const auto *structure =
            dynamic_cast<const StructExpression *>(&expression))
        return check_struct_expression(*structure);
    if (const auto *field = dynamic_cast<const FieldExpression *>(&expression))
        return check_field(*field);
    if (const auto *array = dynamic_cast<const ArrayExpression *>(&expression))
        return check_array(*array, expected);
    if (const auto *index = dynamic_cast<const IndexExpression *>(&expression))
        return check_index(*index);

    // 4.7 控制流：if/loop/while 是值；break/continue 是发散表达式。
    if (const auto *if_expression =
            dynamic_cast<const IfExpression *>(&expression))
        return check_if(*if_expression, expected);
    if (const auto *loop = dynamic_cast<const LoopExpression *>(&expression))
        return check_loop(*loop, expected);
    if (const auto *while_expression =
            dynamic_cast<const WhileExpression *>(&expression))
        return check_while(*while_expression);
    if (const auto *break_expression =
            dynamic_cast<const BreakExpression *>(&expression))
        return check_break(*break_expression);
    if (const auto *continue_expression =
            dynamic_cast<const ContinueExpression *>(&expression))
        return check_continue(*continue_expression);

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
