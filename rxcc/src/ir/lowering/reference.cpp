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
        // 引用和 Box 的计算值都是指向目标的指针，一次 * 只投影一层。
        if (type.kind != SemanticType::Kind::Reference &&
            type.kind != SemanticType::Kind::Box) {
            diagnostics << "IR 生成遇到不可解引用的操作数\n";
            return std::nullopt;
        }
        return Place{std::get<ir::Operand>(*reference), type.inner.get()};
    }
    diagnostics << "IR 生成尚未支持此 place 表达式\n";
    return std::nullopt;
}

// 引用/Box 基址先求值为指针，再沿剩余引用和 Box 层读取；
// 聚合 place 不得先复制。Box 的存储也是指针，自动解引用读取同一层指针。
std::optional<PlaceResult>
GenerationContext::emit_base_place(const Expression &expression,
                                   LocalPlaces &locals) {
    const auto &base = semantics.expressions.at(&expression);
    const bool indirect = is_reference(base.type) ||
                          base.type.kind == SemanticType::Kind::Box;
    if (!indirect && base.place.is_place)
        return emit_place(expression, locals);

    auto value = emit_value(expression, locals);
    if (!value)
        return std::nullopt;
    if (path_ended(*value))
        return PlaceResult{NoContinuation{}};
    if (const auto *aggregate = std::get_if<AggregateValue>(&*value))
        return PlaceResult{Place{aggregate->address, aggregate->type}};
    if (indirect) {
        Place target{std::get<ir::Operand>(*value), base.type.inner.get()};
        while (is_reference(*target.type) ||
               target.type->kind == SemanticType::Kind::Box) {
            target = Place{load_value(target.address, ir::Type::Ptr),
                           target.type->inner.get()};
        }
        return target;
    }
    diagnostics << "IR 生成遇到不可投影的基址\n";
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
        diagnostics << "IR 生成遇到非结构体字段基址\n";
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
    // 先固定容器位置，再求一次下标；只有两者都完成后才投影元素地址。
    auto base = emit_base_place(*expression.base, locals);
    if (!base || path_ended(*base))
        return base;
    const auto &current = std::get<Place>(*base);
    auto index = emit_value(*expression.index, locals);
    if (!index)
        return std::nullopt;
    if (path_ended(*index))
        return PlaceResult{NoContinuation{}};
    const auto index_operand = std::get<ir::Operand>(*index);
    const auto zero = ir::Operand::constant(ir::Type::I32, 0);
    if (current.type->kind == SemanticType::Kind::Array) {
        const auto storage = ir::storage_type_of(*current.type);
        const auto address = builder.get_element_ptr(
            storage, current.address, {zero, index_operand});
        return PlaceResult{Place{address, current.type->inner.get()}};
    }
    if (current.type->kind == SemanticType::Kind::Vec) {
        // Vec 头保存缓冲区指针；元素地址必须由该指针加下标得到，
        // 不能把头本身的地址当作元素缓冲区。
        const auto header_storage = ir::StorageType::vec();
        const auto data_address = builder.get_element_ptr(
            header_storage, current.address, {zero, zero});
        const auto data = builder.load(ir::Type::Ptr, data_address, 4);
        // 缓冲区长度由运行时头决定；零长度数组只用来计算元素步长。
        const auto buffer_storage = ir::StorageType::array_of(
            ir::storage_type_of(*current.type->inner), 0);
        const auto address = builder.get_element_ptr(
            buffer_storage, data, {zero, index_operand});
        return PlaceResult{Place{address, current.type->inner.get()}};
    }
    diagnostics << "IR 生成遇到非数组或 Vec 的索引基址\n";
    return std::nullopt;
}

// 生成 & 或 &mut：已有 place 直接返回其地址；
// 值表达式先求值一次，再写入固定临时 slot 并返回该地址。
std::optional<EmitResult>
GenerationContext::emit_borrow(const UnaryExpression &expression,
                               LocalPlaces &locals) {
    const auto &result_type = semantics.expressions.at(&expression).type;
    // 引用目标可以是标量、Box 或可按布局保存的聚合。
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

// 按 sema 已选定的调整路径执行引用/Box 转换。
// 每个步骤只读取当前层保存的指针：引用保存目标地址，Box 保存拥有者指针，
// 因此两者都执行一次 load ptr；不读取目标中保存的值。
std::optional<ir::Operand>
GenerationContext::emit_coercion(ir::Operand value,
                                 const CoercionPlan &plan) {
    for (std::size_t step = 0; step < plan.steps.size(); ++step)
        value = load_value(value, ir::Type::Ptr);
    return value;
}

// 直接读取固定使用位置已经保存的调整路径；缺失记录不能当作无转换处理。
std::optional<ir::Operand>
GenerationContext::emit_coercion_at(const UseSite &site, ir::Operand value) {
    return emit_coercion(value, semantics.coercion_plans.at(site));
}

} // namespace rx
