// 检查整数字面量和值路径：字面量吸收整数期望类型；路径先查局部绑定，
// 再查全局常量。关联常量复用声明阶段的路径解析与求值缓存。

#include "sema/body/checker.hpp"
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
        // 写明了后缀，就按后缀来
        type = type_from_suffix(literal.suffix);
    } else if (expected && expected->is_int()) {
        // 没写后缀，跟着外面的要求，
        // 比如 let x: usize = 1; 里的 1 按 usize 算。
        type = *expected;
    } else {
        // 没写后缀，外面也没有整数要求，默认是 i32。
        type = SemanticType::int_(SemanticType::IntKind::I32);
    }

    // 数字是值，本身不是一个可以存取的变量。
    return ExpressionResult::value(type);
}

namespace {

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
// 单段路径先找局部名字，再找全局的值名；值位置的多段路径按关联常量解析。
ExpressionResult
BodyChecker::check_value_path(const PathExpression &path) {
    if (path.segments.empty()) {
        diagnostics_.error(path.span, "internal error: empty value path");
        return ExpressionResult::error();
    }

    const PathSegment &first = path.segments[0];

    // self：只能出现在有接收者的方法里。
    if (first.name == "self") {
        // 普通函数或关联函数里没有 self。
        if (!current_has_self_) {
            diagnostics_.error(path.span,
                               "self is not available outside a method");
            return ExpressionResult::error();
        }
        // self 不能写成 self::...
        if (path.segments.size() != 1) {
            diagnostics_.error(path.span, "self is not a type name");
            return ExpressionResult::error();
        }
        // self 不能带类型参数。
        if (!first.generics.empty()) {
            diagnostics_.error(path.span, "self has no type parameters");
            return ExpressionResult::error();
        }
        // 检查当前方法有接收者
        // 取出进入函数时登记好的 self，类型和“能不能改”都从这里来。
        const auto binding = lookup_local("self");
        if (!binding)
            return ExpressionResult::error();
        return ExpressionResult::from_place(binding->type,
                                PlaceAccessInformation{true, binding->is_mut,
                                                      false});
    }

    // 检查self的type
    if (first.name == "Self" && path.segments.size() == 1) {
        const auto type = analyzer_.resolve_type_segment(first, current_owner_, path.span);
        if (!type.is_error())
            diagnostics_.error(path.span, "Self is a type, not a value");
        return ExpressionResult::error();
    }

    // 只有一段的路径：局部 → 全局值名 → 类型名 → 不认识
    if (path.segments.size() == 1) {
        // 局部变量和参数优先，就近的块先找到。
        if (const auto binding = lookup_local(first.name)) {
            // 局部名字不能带类型参数，比如 x::<i32> 
            if (reject_value_type_arguments(first, "local binding",
                                            diagnostics_, path.span))
                return ExpressionResult::error();
            return ExpressionResult::from_place(binding->type,
                                    PlaceAccessInformation{true,
                                                          binding->is_mut,
                                                          false});
        }

        // 查全局值名（函数、常量、内置函数）。
        const auto value =
            analyzer_.function_constant_namespace.find(first.name);
        if (value != analyzer_.function_constant_namespace.end()) {
            const Symbol *symbol = value->second;
            if (symbol->kind == Symbol::Kind::Constant) {
                // 常量不能带类型参数；它的值在常量阶段已经算好，这里直接取。
                if (reject_value_type_arguments(first, "constant",
                                                diagnostics_, path.span))
                    return ExpressionResult::error();
                const auto *constant =
                    static_cast<const ConstantSymbol *>(symbol);
                if (!constant->evaluated.ok ||
                    constant->evaluated.type.is_error())
                    return ExpressionResult::error();
                // 常量是值，不是可以改的存储位置。
                return ExpressionResult::value(constant->evaluated.type);
            }
            // 函数/内置函数不能单独当值用；真正的调用由 calls/call.cpp 处理。
            diagnostics_.error(path.span,
                               "function " + first.name +
                                   " cannot be used as a value");
            return ExpressionResult::error();
        }

        // 类型名出现在值位置时，仍由统一解析器检查其类型实参。
        if (analyzer_.type_namespace.count(first.name) ||
            !basic_type_from_name(first.name).is_error() ||
            first.name == "Box" || first.name == "Vec") {
            const auto type = analyzer_.resolve_type_segment(first, current_owner_, path.span);
            if (!type.is_error())
                diagnostics_.error(path.span, "type " + type.toString() + " is not a value");
            return ExpressionResult::error();
        }

        // 这个值名找不到。
        diagnostics_.error(path.span,
                           "unresolved value name: " + first.name);
        return ExpressionResult::error();
    }

    // 值位置的多段路径按关联常量解析，比如 Config::N 或 Self::N。
    // S::new() 的被调用者由 calls/call.cpp 直接解析，不经过这里。
    const ConstantSymbol *constant =
        analyzer_.resolve_constant_path(path.segments, path.span,
                                        current_owner_);
    if (!constant)
        return ExpressionResult::error();
    // 常量求值失败时，错误已经在常量阶段报过，这里不再重复报。
    if (!constant->evaluated.ok || constant->evaluated.type.is_error())
        return ExpressionResult::error();
    return ExpressionResult::value(constant->evaluated.type);
}

} // namespace rx
