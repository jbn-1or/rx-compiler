// 实现位置地址计算、借用、解引用以及按语义记录执行引用转换的 lowering。
#include "ir/lowering.hpp"
#include "ast/ast.hpp"
#include "ir/builder.hpp"

namespace rx {

// 计算位置表达式的地址：局部路径直接给出 slot 地址，
// 显式解引用先读出引用保存的指针，再把该指针当作目标位置。
std::optional<PlaceResult>
GenerationContext::emit_place(const Expression &expression,
                              LocalPlaces &locals) {
    if (const auto *path = dynamic_cast<const PathExpression *>(&expression)) {
        const auto &target = semantics.paths.at(path);
        // sema 只会把局部变量路径标记为 place，直接取 LocalId。
        const auto local_id = std::get<LocalId>(target);
        // 路径变量的地址就是它的 slot；引用变量的 slot 中保存的是目标指针。
        return PlaceResult{Place{
            locals.at(local_id), &semantics.locals[local_id.index].type}};
    }
    if (const auto *field = dynamic_cast<const FieldExpression *>(&expression))
        return emit_field_place(*field, locals);
    if (const auto *index = dynamic_cast<const IndexExpression *>(&expression))
        return emit_index_place(*index, locals);
    if (const auto *unary =
            dynamic_cast<const UnaryExpression *>(&expression)) {
        // sema 只会把显式解引用标记为 place，不再重复检查操作符。
        // 求值被解引用的引用本身；每个显式 * 只解一层。
        auto reference = emit_value(*unary->operand, locals);
        if (!reference)
            return std::nullopt;
        if (path_ended(*reference))
            return PlaceResult{NoContinuation{}};
        const auto &type = semantics.expressions.at(unary->operand.get()).type;
        if (!is_reference(type)) {
            diagnostics << "IR 生成尚未支持 Box 解引用（P9）\n";
            return std::nullopt;
        }
        return Place{std::get<ir::Operand>(*reference), type.inner.get()};
    }
    diagnostics << "IR 生成尚未支持此 place 表达式\n";
    return std::nullopt;
}

// 引用基址先求值为指针，再沿剩余引用层读取；聚合 place 不得先复制。
std::optional<PlaceResult>
GenerationContext::emit_base_place(const Expression &expression,
                                   LocalPlaces &locals) {
    const auto &base = semantics.expressions.at(&expression);
    if (!is_reference(base.type) && base.place.is_place)
        return emit_place(expression, locals);

    auto value = emit_value(expression, locals);
    if (!value)
        return std::nullopt;
    if (path_ended(*value))
        return PlaceResult{NoContinuation{}};
    if (const auto *aggregate = std::get_if<AggregateValue>(&*value))
        return PlaceResult{Place{aggregate->address, aggregate->type}};
    if (is_reference(base.type)) {
        Place target{std::get<ir::Operand>(*value), base.type.inner.get()};
        while (is_reference(*target.type)) {
            target = Place{load_value(target.address, ir::Type::Ptr),
                           target.type->inner.get()};
        }
        return target;
    }
    diagnostics << "IR 生成尚未支持此投影基址（P9）\n";
    return std::nullopt;
}

// 在底层结构体位置上按声明序号投影字段。
std::optional<PlaceResult>
GenerationContext::emit_field_place(const FieldExpression &expression,
                                    LocalPlaces &locals) {
    auto base = emit_base_place(*expression.base, locals);
    if (!base || path_ended(*base))
        return base;
    const auto &current = std::get<Place>(*base);
    if (current.type->kind != SemanticType::Kind::Struct) {
        diagnostics << "IR 生成尚未支持 Box 字段访问（P9）\n";
        return std::nullopt;
    }
    const StructSymbol &structure = *current.type->struct_symbol;
    const auto &field_type = structure.field_types.at(expression.field);
    const auto storage = ir::storage_type_of(*current.type);
    const auto address = builder.get_element_ptr(
        storage, current.address,
        {ir::Operand::constant(ir::Type::I32, 0),
         ir::Operand::constant(
             ir::Type::I32,
             static_cast<uint32_t>(
                 field_declaration_index(structure, expression.field)))});
    return PlaceResult{Place{address, &field_type}};
}

// 先求基表达式再求一次下标；数组元素地址由显式数组存储类型的 GEP 得到。
std::optional<PlaceResult>
GenerationContext::emit_index_place(const IndexExpression &expression,
                                    LocalPlaces &locals) {
    auto base = emit_base_place(*expression.base, locals);
    if (!base || path_ended(*base))
        return base;
    const auto &current = std::get<Place>(*base);
    if (current.type->kind != SemanticType::Kind::Array) {
        diagnostics << "IR 生成尚未支持此容器索引（P9）\n";
        return std::nullopt;
    }
    auto index = emit_value(*expression.index, locals);
    if (!index)
        return std::nullopt;
    if (path_ended(*index))
        return PlaceResult{NoContinuation{}};
    const auto storage = ir::storage_type_of(*current.type);
    const auto address = builder.get_element_ptr(
        storage, current.address,
        {ir::Operand::constant(ir::Type::I32, 0),
         std::get<ir::Operand>(*index)});
    return PlaceResult{Place{address, current.type->inner.get()}};
}

// 生成 & 或 &mut：已有 place 直接返回其地址；
// 值表达式先求值一次，再写入固定临时 slot 并返回该地址。
std::optional<EmitResult>
GenerationContext::emit_borrow(const UnaryExpression &expression,
                               LocalPlaces &locals) {
    const auto &result_type = semantics.expressions.at(&expression).type;
    // 引用目标可以是标量或可按布局保存的聚合；Box 引用仍随 P9。
    if (!is_storable_value(result_type)) {
        diagnostics << "IR 生成尚未支持此引用类型（P9）\n";
        return std::nullopt;
    }
    const auto &operand = *expression.operand;
    const auto &operand_record = semantics.expressions.at(&operand);
    // 操作数本身是 place 时借用原位置，不能先复制内容，否则会破坏别名关系。
    if (operand_record.place.is_place) {
        auto place = emit_place(operand, locals);
        if (!place)
            return std::nullopt;
        if (path_ended(*place))
            return EmitResult{NoContinuation{}};
        return EmitResult{std::get<Place>(*place).address};
    }
    // 值表达式没有固定位置，只在借用发生处物化一次。
    auto value = emit_value(operand, locals);
    if (!value)
        return std::nullopt;
    if (path_ended(*value))
        return EmitResult{NoContinuation{}};
    // 聚合值已经位于独立临时存储，借用直接返回该存储地址。
    if (const auto *aggregate = std::get_if<AggregateValue>(&*value))
        return EmitResult{aggregate->address};
    return EmitResult{materialize_value(std::get<ir::Operand>(*value))};
}

// 按 sema 已选定的调整路径执行引用转换。
// 每个 Reference 步骤只读取当前引用保存的指针，最后仍是指向目标位置的引用；
// 不读取目标中保存的值，也不重新推导整数或引用可变性。
std::optional<ir::Operand>
GenerationContext::emit_coercion(ir::Operand value,
                                 const CoercionPlan &plan) {
    for (const auto &step : plan.steps) {
        // Box 的指针读取随 P9 的容器布局实现；当前不能假装它是普通引用。
        if (step.kind == CoercionStepKind::Box) {
            diagnostics << "IR 生成尚未支持 Box 引用转换（P9）\n";
            return std::nullopt;
        }
        value = load_value(value, ir::Type::Ptr);
    }
    return value;
}

// 直接读取固定使用位置已经保存的调整路径；缺失记录不能当作无转换处理。
std::optional<ir::Operand>
GenerationContext::emit_coercion_at(const UseSite &site, ir::Operand value) {
    return emit_coercion(value, semantics.coercion_plans.at(site));
}

} // namespace rx
