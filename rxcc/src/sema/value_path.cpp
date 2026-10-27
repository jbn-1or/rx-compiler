// 实现普通表达式中的值路径解析。
// 单段路径先查局部绑定，再查顶层值名；关联常量复用常量路径解析。
// 函数/内置函数作为值留给调用检查，裸类型名不能作为值。
//
// 具体检查：
// - 单段路径先查局部作用域（含参数/self），命中后返回绑定类型和 place
//   {is_place=true, is_mutable=binding.is_mut, crossed_shared=false}；
//   局部绑定遮蔽同名全局函数/常量，不再回退。
// - self 无接收者报 N9；Self 无当前结构体报 N10；裸 Self / 类型名不能当值。
// - 顶层常量使用已求值缓存，作为值返回；函数和内置 I/O 作为普通值属 UB。
// - 多段路径复用关联常量解析规则；结构体/基本类型前段带具体类型实参分别按
//   G1/G3 检查，常量/绑定不接受类型参数。
// - 找不到值名报 N14；类型参数检查在解析目标后进行，生命周期实参已由 AST 丢弃。

#include "sema/body.hpp"
#include "sema/sema.hpp"
#include "sema/type_utils.hpp"

namespace rx {

namespace {

ExpressionResult failed_value() {
    return ExpressionResult{SemanticType::error(), PlaceAccessInformation{}};
}

// 单段值名不能携带类型实参；普通绑定和常量都没有类型参数。
bool reject_value_type_arguments(const PathSegment &segment,
                                 const std::string &kind,
                                 Diagnostics &diagnostics, Span at) {
    if (segment.generics.empty())
        return false;
    diagnostics.error(at, kind + " has no type parameters: " + segment.name);
    return true;
}

} // namespace

// 解析 `x`、`self`、`N`、`S::C` 这类值路径。
// 单段路径先找局部名字，再找全局的值名；多段路径在 4.3 只可能是关联常量。
ExpressionResult
BodyChecker::check_value_path(const PathExpression &path) {
    // 正常语法至少有一段；空路径直接按失败处理。
    if (path.segments.empty())
        return failed_value();

    const PathSegment &first = path.segments[0];

    // self：只能出现在有接收者的方法里。
    if (first.name == "self") {
        // 普通函数或关联函数里没有 self。
        if (!current_has_self_) {
            diagnostics_.error(path.span,
                               "self is not available outside a method");
            return failed_value();
        }
        // self 不能写成 self::...，它不是类型名。
        if (path.segments.size() != 1) {
            diagnostics_.error(path.span, "self is not a type name");
            return failed_value();
        }
        // self 不能带类型参数。
        if (!first.generics.empty()) {
            diagnostics_.error(path.span, "self has no type parameters");
            return failed_value();
        }
        // 取出进入函数时登记好的 self，类型和“能不能改”都从这里来。
        const auto binding = lookup_local("self");
        if (!binding)
            return failed_value();
        return ExpressionResult{binding->type,
                                PlaceAccessInformation{true, binding->is_mut,
                                                      false}};
    }

    // 单段 Self：它是类型名，不是值；多段 Self::C 交给常量路径解析。
    if (first.name == "Self" && path.segments.size() == 1) {
        // 不在结构体或 impl 里就没有 Self。
        if (!current_owner_) {
            diagnostics_.error(
                path.span, "Self is not available outside a struct or impl");
            return failed_value();
        }
        // Self 不接受类型参数。
        if (!first.generics.empty()) {
            diagnostics_.error(path.span,
                               "structs have no type parameters: Self");
            return failed_value();
        }
        // 即使当前有 Self，单独写 Self 也只是类型，不能当值用。
        diagnostics_.error(path.span, "Self is a type, not a value");
        return failed_value();
    }

    // 只有一段的路径：按“局部 → 全局值名 → 类型名 → 不认识”的顺序查。
    if (path.segments.size() == 1) {
        // 第一步：局部变量和参数优先，就近的块先找到。
        if (const auto binding = lookup_local(first.name)) {
            // 局部名字不能带类型参数，比如 x::<i32> 不合法。
            if (reject_value_type_arguments(first, "local binding",
                                            diagnostics_, path.span))
                return failed_value();
            return ExpressionResult{binding->type,
                                    PlaceAccessInformation{true,
                                                          binding->is_mut,
                                                          false}};
        }

        // 第二步：查全局值名（函数、常量、内置函数）。
        const auto value =
            analyzer_.function_constant_namespace.find(first.name);
        if (value != analyzer_.function_constant_namespace.end()) {
            const Symbol *symbol = value->second;
            if (symbol->kind == Symbol::Kind::Constant) {
                // 常量不能带类型参数；它的值在常量阶段已经算好，这里直接取。
                if (reject_value_type_arguments(first, "constant",
                                                diagnostics_, path.span))
                    return failed_value();
                const auto *constant =
                    static_cast<const ConstantSymbol *>(symbol);
                if (!constant->evaluated.ok ||
                    constant->evaluated.type.is_error())
                    return failed_value();
                // 常量是值，不是可以改的存储位置。
                return ExpressionResult{constant->evaluated.type,
                                        PlaceAccessInformation{}};
            }
            // 函数/内置函数不能单独当值用；真正的调用在 4.4 处理。
            return failed_value();
        }

        // 第三步：名字可能是用户结构体，但裸结构体名不是值。
        if (analyzer_.type_namespace.count(first.name)) {
            if (!first.generics.empty()) {
                diagnostics_.error(
                    path.span,
                    "structs have no type parameters: " + first.name);
            } else {
                diagnostics_.error(path.span,
                                   "struct " + first.name +
                                       " is not a value");
            }
            return failed_value();
        }

        // 第四步：名字可能是 i32、bool 这类基本类型，也不是值。
        if (!basic_type_from_name(first.name).is_error()) {
            if (!first.generics.empty()) {
                diagnostics_.error(
                    path.span,
                    "primitives take no type parameters: " + first.name);
            } else {
                diagnostics_.error(
                    path.span,
                    first.name + " is a type, not a value");
            }
            return failed_value();
        }

        // Box / Vec 也是类型名，不能单独当值。
        if (first.name == "Box" || first.name == "Vec") {
            diagnostics_.error(path.span,
                               first.name + " is a type, not a value");
            return failed_value();
        }

        // 最后：这个值名确实找不到。
        diagnostics_.error(path.span,
                           "unresolved value name: " + first.name);
        return failed_value();
    }

    // 多段路径在 4.3 只可能是关联常量，比如 Config::N 或 Self::N。
    // 函数/方法调用（S::new()）留给 4.4 和 Phase 5。
    const ConstantSymbol *constant =
        analyzer_.resolve_constant_path(path.segments, path.span,
                                        current_owner_);
    if (!constant)
        return failed_value();
    // 常量求值失败时，错误已经在常量阶段报过，这里不再重复报。
    if (!constant->evaluated.ok || constant->evaluated.type.is_error())
        return failed_value();
    return ExpressionResult{constant->evaluated.type,
                            PlaceAccessInformation{}};
}

} // namespace rx
