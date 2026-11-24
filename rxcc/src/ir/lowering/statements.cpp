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
    // 先按旧绑定求值；只有显式标注才有固定目标转换。
    auto prepared = statement.type_annotation
        ? emit_prepared_value(*statement.initializer, locals,
                              {&statement, UseKind::Initializer, 0})
        : emit_value(*statement.initializer, locals);
    if (!prepared || path_ended(*prepared))
        return prepared;
    const auto binding = semantics.let_bindings.at(&statement);
    const auto &type = semantics.locals[binding.index].type;
    // 局部存储支持标量、引用链和按布局搬运的聚合值。
    if (!is_storable_value(type)) {
        diagnostics << "IR 生成尚未支持局部绑定类型 " << type.toString() << '\n';
        return std::nullopt;
    }
    // 标量写入自己的存储单元，聚合值按完整布局搬运到新 slot。
    auto address = allocate_storage_slot(ir::storage_type_of(type));
    store_prepared_value(address, *prepared, type);
    // 初始化已生成后才登记新绑定，支持 `let x = x` 读取旧 x。
    locals.emplace(binding, address);
    return EmitResult{UnitResult{}};
}

// 函数尾部和显式 return 只在转换记录的位置上有别。
std::optional<EmitResult>
GenerationContext::emit_return(const ReturnExpression &expression,
                                LocalPlaces &locals) {
    return emit_return_value(expression.value.get(),
                             {&expression, UseKind::ReturnValue, 0}, locals);
}

// unit 只执行副作用；聚合写入调用者提供的地址，其他值直接返回。
std::optional<EmitResult>
GenerationContext::emit_return_value(const Expression *value, const UseSite &site,
                                     LocalPlaces &locals) {
    if (!value) {
        builder.ret();
        return EmitResult{NoContinuation{}};
    }
    auto result = current_return_type_->is_unit()
        ? emit_discarded_expression(*value, locals)
        : emit_prepared_value(*value, locals, site);
    if (!result || path_ended(*result))
        return result;

    if (function_result_pointer_)
        store_prepared_value(*function_result_pointer_, *result, *current_return_type_);
    if (const auto *operand = std::get_if<ir::Operand>(&*result))
        builder.ret(*operand);
    else
        builder.ret();
    return EmitResult{NoContinuation{}};
}

// 生成普通或复合赋值。两种形式都先固定 RHS，再只求一次目标地址；
// 复合赋值在目标确定后才读取旧值，目标函数改变旧值时会读取改变后的内容。
std::optional<EmitResult>
GenerationContext::emit_assignment(const AssignmentExpression &expression,
                                   LocalPlaces &locals) {
    // 普通赋值先完成目标类型转换；复合赋值只固定 RHS，不提前读取引用目标。
    const auto operation = compound_operator(expression.operation);
    auto right = operation ? emit_value(*expression.right, locals)
                           : emit_prepared_value(*expression.right, locals,
                                                 {&expression, UseKind::AssignmentValue, 0});
    if (!right || path_ended(*right))
        return right;

    auto target_result = emit_place(*expression.left, locals);
    if (!target_result)
        return std::nullopt;
    if (path_ended(*target_result))
        return EmitResult{NoContinuation{}};
    const auto &target = std::get<Place>(*target_result);
    if (!operation) {
        if (!is_storable_value(*target.type)) {
            diagnostics << "IR 生成尚未支持赋值目标类型 " << target.type->toString() << '\n';
            return std::nullopt;
        }
        store_prepared_value(target.address, *right, *target.type);
        return EmitResult{UnitResult{}};
    }
    // 旧值必须在目标地址求值之后读取，不能复用检查阶段的任何缓存。
    const auto old_value = load_value(target.address, *target.type);
    // 直接标量 RHS 在求值时就已固定；共享引用此时才读取其目标。
    const auto &right_type = semantics.expressions.at(expression.right.get()).type;
    const auto right_scalar = finish_scalar_operand(std::get<ir::Operand>(*right), right_type);
    const auto result = emit_binary_operation(*operation, *target.type, old_value,
                                              right_scalar);
    store_value(result, target.address);
    return EmitResult{UnitResult{}};
}

} // namespace rx
