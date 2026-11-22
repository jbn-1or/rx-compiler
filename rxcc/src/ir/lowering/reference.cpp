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
    if (const auto *unary =
            dynamic_cast<const UnaryExpression *>(&expression)) {
        // sema 只会把显式解引用标记为 place，不再重复检查操作符。
        // 求值被解引用的引用本身；每个显式 * 只解一层。
        auto reference = emit_value(*unary->operand, locals);
        if (!reference)
            return std::nullopt;
        if (path_ended(*reference))
            return PlaceResult{NoContinuation{}};
        // 语义阶段已保证该值是可解引用的引用；这里只把指针当作目标地址，
        // 不读取目标内容，借用 &*p 因而仍是同一个位置。
        const auto &type = semantics.expressions.at(&expression).type;
        return PlaceResult{Place{std::get<ir::Operand>(*reference), &type}};
    }
    // 字段、下标等 place 投影留到 P7/P8 随聚合布局实现。
    diagnostics << "IR 生成尚未支持此 place 表达式\n";
    return std::nullopt;
}

// 生成 & 或 &mut：已有 place 直接返回其地址；
// 值表达式先求值一次，再写入固定临时 slot 并返回该地址。
std::optional<EmitResult>
GenerationContext::emit_borrow(const UnaryExpression &expression,
                               LocalPlaces &locals) {
    const auto &result_type = semantics.expressions.at(&expression).type;
    // 当前只支持引用到标量或引用链；聚合引用随 P7 的布局一起实现。
    if (!is_supported_value(result_type)) {
        diagnostics << "IR 生成尚未支持聚合引用（P7）\n";
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
    return EmitResult{
        materialize_once(operand, std::get<ir::Operand>(*value))};
}

// 生成一次内置解引用：先得到引用保存的地址，再读取该位置的值。
// 目标本身仍是引用时，读取出的就是下一层指针，不会继续自动解引用。
std::optional<EmitResult>
GenerationContext::emit_dereference(const UnaryExpression &expression,
                                    LocalPlaces &locals) {
    auto place = emit_place(expression, locals);
    if (!place)
        return std::nullopt;
    if (path_ended(*place))
        return EmitResult{NoContinuation{}};
    const auto &target = std::get<Place>(*place);
    // 解引用结果也是普通值：标量或引用链；聚合目标留到 P7。
    if (!is_supported_value(*target.type)) {
        diagnostics << "IR 生成尚未支持解引用聚合类型（P7）\n";
        return std::nullopt;
    }
    return EmitResult{load_value(target.address, *target.type)};
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
        value = builder.load(ir::Type::Ptr, value, 4);
    }
    return value;
}

// 直接读取固定使用位置已经保存的调整路径；缺失记录不能当作无转换处理。
std::optional<ir::Operand>
GenerationContext::emit_coercion_at(const UseSite &site, ir::Operand value) {
    return emit_coercion(value, semantics.coercion_plans.at(site));
}

} // namespace rx
