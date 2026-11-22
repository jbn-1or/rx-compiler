// 实现 let 绑定、return 以及普通和复合赋值的 lowering。
#include "ir/lowering.hpp"
#include "ast/ast.hpp"
#include "ir/builder.hpp"
#include <optional>
#include <variant>

namespace rx {
namespace {

// 把复合赋值运算符映射到对应的标量二元运算；普通 = 不在这里处理。
std::optional<BinaryOperator> compound_operator(AssignmentOperator operation) {
    switch (operation) {
    case AssignmentOperator::AddAssignment:
        return BinaryOperator::Add;
    case AssignmentOperator::SubtractAssignment:
        return BinaryOperator::Subtract;
    case AssignmentOperator::MultiplyAssignment:
        return BinaryOperator::Multiply;
    case AssignmentOperator::DivideAssignment:
        return BinaryOperator::Divide;
    case AssignmentOperator::RemainderAssignment:
        return BinaryOperator::Remainder;
    case AssignmentOperator::BitwiseAndAssignment:
        return BinaryOperator::BitwiseAnd;
    case AssignmentOperator::BitwiseXorAssignment:
        return BinaryOperator::BitwiseXor;
    case AssignmentOperator::BitwiseOrAssignment:
        return BinaryOperator::BitwiseOr;
    case AssignmentOperator::ShiftLeftAssignment:
        return BinaryOperator::ShiftLeft;
    case AssignmentOperator::ShiftRightAssignment:
        return BinaryOperator::ShiftRight;
    case AssignmentOperator::Assign:
        break;
    }
    return std::nullopt;
}

} // namespace

// 先求值 let 初始化器，再为其 LocalId 分配并登记存储位置。
std::optional<EmitResult>
GenerationContext::emit_let(const LetStatement &statement,
                            LocalPlaces &locals) {
    const auto binding = semantics.let_bindings.at(&statement);
    // 在新绑定进入映射前先生成初始化器。
    auto initializer = emit_value(*statement.initializer, locals);
    // 初始化器不支持时立即失败，不创建未初始化的存储位置。
    if (!initializer)
        return std::nullopt;
    // 初始化器提前 return 时不分配存储，也不执行 store。
    if (path_ended(*initializer))
        return EmitResult{NoContinuation{}};
    const auto &type = semantics.locals[binding.index].type;
    // 当前局部存储支持标量和标量引用；聚合绑定随 P7 实现。
    if (!is_supported_value(type)) {
        diagnostics << "IR 生成尚未支持聚合局部绑定（P7）\n";
        return std::nullopt;
    }
    auto value = std::get<ir::Operand>(*initializer);
    // 有类型标注时，初始化器是固定使用位置，按 sema 选定路径调整；
    // 没有标注时，绑定类型就是初始化器自身的类型。
    if (statement.type_annotation) {
        auto coerced = emit_coercion_at(
            UseSite{&statement, UseKind::Initializer, 0}, value);
        if (!coerced)
            return std::nullopt;
        value = *coerced;
    }
    // bool 使用 i8 存储单元，整数使用 i32，引用保存 ptr；
    // 写入统一经过 store_value。
    auto address = builder.entry_alloca(slot_type(type), slot_alignment(type));
    store_value(value, address);
    // 初始化已生成后才登记新绑定，支持 `let x = x` 读取旧 x。
    locals.emplace(binding, address);
    return EmitResult{UnitResult{}};
}

// 生成 return：先按操作数的源类型求值，再发 ret 并结束当前路径。
std::optional<EmitResult>
GenerationContext::emit_return(const ReturnExpression &expression,
                               LocalPlaces &locals) {
    if (!expression.value) {
        builder.ret();
        return EmitResult{NoContinuation{}};
    }
    const auto &value_record = semantics.expressions.at(expression.value.get());
    // 返回 () 的路径只发 ret void，不产生数据操作数。
    if (value_record.type.is_unit()) {
        auto result = emit_unit_expression(*expression.value, locals);
        if (!result)
            return std::nullopt;
        // 操作数内部的 return 已经发过 ret，外层不能再补一条。
        if (path_ended(*result))
            return result;
        builder.ret();
        return EmitResult{NoContinuation{}};
    }
    auto result = emit_value(*expression.value, locals);
    if (!result)
        return std::nullopt;
    // 操作数本身提前结束路径时，由它负责发 ret。
    if (path_ended(*result))
        return result;
    // 返回值是固定使用位置，按 sema 记录的计划调整到声明返回类型。
    auto coerced = emit_coercion_at(
        UseSite{&expression, UseKind::ReturnValue, 0},
        std::get<ir::Operand>(*result));
    if (!coerced)
        return std::nullopt;
    builder.ret(*coerced);
    return EmitResult{NoContinuation{}};
}

// 生成普通或复合赋值。两种形式都先固定 RHS，再只求一次目标地址；
// 复合赋值在目标确定后才读取旧值，目标函数改变旧值时会读取改变后的内容。
std::optional<EmitResult>
GenerationContext::emit_assignment(const AssignmentExpression &expression,
                                   LocalPlaces &locals) {
    // —— 普通赋值：右侧先求值并实施已选定的转换，然后计算目标地址。——
    if (expression.operation == AssignmentOperator::Assign) {
        auto right = emit_value(*expression.right, locals);
        if (!right)
            return std::nullopt;
        // 右侧提前 return 时，不能计算目标地址，更不能写入。
        if (path_ended(*right))
            return EmitResult{NoContinuation{}};
        // 赋值右侧是固定使用位置；按 sema 已记录的路径调整到左值类型。
        auto prepared = emit_coercion_at(
            UseSite{&expression, UseKind::AssignmentValue, 0},
            std::get<ir::Operand>(*right));
        if (!prepared)
            return std::nullopt;
        // 目标只计算一次；*target(...) 这类 place 的副作用也在这里发生。
        auto target_result = emit_place(*expression.left, locals);
        if (!target_result)
            return std::nullopt;
        if (path_ended(*target_result))
            return EmitResult{NoContinuation{}};
        const auto &target = std::get<Place>(*target_result);
        // 聚合赋值的读写随 P7 的布局实现；当前只写入标量或引用。
        if (!is_supported_value(*target.type)) {
            diagnostics << "IR 生成尚未支持聚合赋值（P7）\n";
            return std::nullopt;
        }
        store_value(*prepared, target.address);
        return EmitResult{UnitResult{}};
    }

    // —— 复合赋值：先固定 RHS 的标量或共享引用，再求目标并读取旧值。——
    const auto operation = compound_operator(expression.operation);
    if (!operation) {
        diagnostics << "IR 生成尚未支持此赋值运算\n";
        return std::nullopt;
    }
    auto right = emit_value(*expression.right, locals);
    if (!right)
        return std::nullopt;
    if (path_ended(*right))
        return EmitResult{NoContinuation{}};
    const auto right_value = std::get<ir::Operand>(*right);
    // 共享引用右侧只固定指针；目标内容要等旧值读取之后才读取。
    const auto &right_type =
        semantics.expressions.at(expression.right.get()).type;

    auto target_result = emit_place(*expression.left, locals);
    if (!target_result)
        return std::nullopt;
    if (path_ended(*target_result))
        return EmitResult{NoContinuation{}};
    const auto &target = std::get<Place>(*target_result);
    // 旧值必须在目标地址求值之后读取，不能复用检查阶段的任何缓存。
    const auto old_value = load_value(target.address, *target.type);
    // 直接标量 RHS 在求值时就已固定；共享引用此时才读取其目标。
    const auto right_scalar =
        finish_scalar_operand(right_value, right_type);
    const auto &left_type =
        semantics.expressions.at(expression.left.get()).type;
    auto result = emit_binary_operation(*operation, left_type, old_value,
                                        right_scalar);
    if (!result)
        return std::nullopt;
    store_value(*result, target.address);
    return EmitResult{UnitResult{}};
}

} // namespace rx
