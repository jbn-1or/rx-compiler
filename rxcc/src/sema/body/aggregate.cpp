// 检查结构体构造、字段访问、数组列表/重复和下标访问。
// 构造检查字段/元素是否匹配目标类型；字段和下标访问返回带权限的 place。
// 数组重复次数按 usize 常量求值；字段和下标可沿引用、Box 逐层解引用。

#include "sema/body/checker.hpp"
#include "sema/sema.hpp"
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rx {

namespace {

// 下标和数组重复次数都使用 usize 类型。
SemanticType usize_type() {
    return SemanticType::int_(SemanticType::IntKind::Usize);
}

} // namespace

// 检查 `S { x: ..., y: ... }` 或 `Self { ... }`
// 先确认花括号前写的是一个真正的结构体，再逐个核对字段：
ExpressionResult BodyChecker::check_struct_expression(
    const StructExpression &expression) {
    // 路径已经出错时，字段值仍要遍历，避免错误恢复阶段丢掉后续名字/类型错误。
    auto check_fields_without_target = [&]() {
        for (const StructExpressionField &field : expression.fields) {
            if (field.value)
                check_expression(*field.value, nullptr);
        }
    };

    // 没有写出结构体名字，无法继续。
    if (!expression.path) {
        check_fields_without_target();
        return ExpressionResult::error();
    }

    const std::vector<PathSegment> &segments = expression.path->segments;
    // 这里只接受一段名字，比如 `S` 或 `Self`；多段名字不是结构体写法。
    if (segments.size() != 1) {
        diagnostics_.error(expression.path->span,
                           "struct literal path must name exactly one type");
        check_fields_without_target();
        return ExpressionResult::error();
    }
    // 取出目标的语义类型
    const SemanticType target = analyzer_.resolve_type_segment(
        segments[0], current_owner_, expression.path->span);   
    if (target.is_error()) {
        check_fields_without_target();
        return ExpressionResult::error();
    }
    if (target.kind != SemanticType::Kind::Struct) {
        diagnostics_.error(expression.path->span,
                           "struct literal requires a user-defined struct");
        check_fields_without_target();
        return ExpressionResult::error();
    }
    const StructSymbol *structure = target.struct_symbol;
    
    // 查 StructSymbol::field_types，检查字段是否存在、重复或缺失 
    // 每个字段值 → check_expression(value, &field_type) → require_coerce
    bool ok = true;
    std::unordered_set<std::string> initialized;
    for (const StructExpressionField &field : expression.fields) {
        const auto type = structure->field_types.find(field.name);
        // 写了一个结构体里没有的字段名。
        if (type == structure->field_types.end()) {
            diagnostics_.error(expression.span,
                               "unknown struct field: " + field.name);
            ok = false;
            if (field.value)
                check_expression(*field.value, nullptr);
            continue;
        }

        // 同一个字段写了不止一次。
        if (!initialized.insert(field.name).second) {
            diagnostics_.error(expression.span,
                               "duplicate struct field initializer: " +
                                   field.name);
            ok = false;
        }
        // 字段名后面没有给值，写法不完整。
        if (!field.value) {
            diagnostics_.error(expression.span,
                               "missing struct field initializer: " +
                                   field.name);
            ok = false;
            continue;
        }

        const ExpressionResult value =
            check_expression(*field.value, &type->second);
        if (!require_coerce(value, type->second, field.value->span))
            ok = false;
    }

    // 按声明顺序找出一直没写过的字段。
    for (const FieldDeclaration &field : structure->fields) {
        if (!initialized.count(field.name)) {
            diagnostics_.error(expression.span,
                               "missing struct field initializer: " +
                                   field.name);
            ok = false;
        }
    }

    // 有任何一个字段不对，这次结构体写法就没有正常结果。
    if (!ok)
        return ExpressionResult::error();
    return ExpressionResult::value(SemanticType::struct_(structure));
}

// 检查 `对象.字段`
// 对象可以是结构体本身，也可以是指向结构体的引用或 Box。
// 找到真正的结构体，再取出字段。保留对象原来的读写状态
ExpressionResult BodyChecker::check_field(const FieldExpression &expression) {
    // 没有对象可以取字段。
    if (!expression.base)
        return ExpressionResult::error();

    ExpressionResult base = check_expression(*expression.base, nullptr);
    // 对象本身已经出错，不再继续查找字段。
    if (base.type.is_error())
        return ExpressionResult::error();
    // 值表达式物化为临时 place；已有 place 保留原权限，再据此确定字段权限。
    base.place = materialize_place(base.place);

    // 引用和 Box 都只是包在结构体外面的一层，逐层打开，直到看到结构体。
    while (base.type.kind == SemanticType::Kind::Reference ||
           base.type.kind == SemanticType::Kind::Box) {
        // 包装层里没有写清里面是什么类型，结构不完整。
        if (!base.type.inner)
            return ExpressionResult::error();
        base = dereference_place(base.type, base.place, expression.base->span);
        // 这一层打不开，后面的字段也就不存在了。
        if (base.type.is_error())
            return ExpressionResult::error();
    }

    // 打开后看到的不是结构体，不能点字段。
    if (base.type.kind != SemanticType::Kind::Struct ||
        !base.type.struct_symbol) {
        diagnostics_.error(expression.span,
                           "field access requires a struct base");
        return ExpressionResult::error();
    }

    const StructSymbol *structure = base.type.struct_symbol;
    const auto field = structure->field_types.find(expression.field);
    // 结构体里没有这个名字的字段。
    if (field == structure->field_types.end()) {
        diagnostics_.error(expression.span,
                           "unknown struct field: " + expression.field);
        return ExpressionResult::error();
    }
    // 字段类型前面已经出过错，不再返回一个不可用的位置。
    if (field->second.is_error())
        return ExpressionResult::error();

    return ExpressionResult::from_place(field->second, base.place);
}

// 检查 `[a, b, c]`和 `[值; 次数]`
// 已说明这里要一个固定数组 `[T; N]`，就按元素类型 T 和长度 N 逐个核对；
// 未说明数组类型，就先分别看每个元素，再从这些元素里找一个共同类型。
// 重复写法的“次数”必须是一个 usize 常量；次数大于 1 时，元素要能被复制（Copy）。
ExpressionResult
BodyChecker::check_array(const ArrayExpression &expression,
                         const SemanticType *expected) {
    const SemanticType *expected_element = nullptr;
    uint64_t expected_length = 0;
    bool has_fixed_target = false;

    // 目标类型已经出错时，仍遍历元素/重复值，保留后续名字和类型诊断。
    auto check_elements_without_target = [&]() {
        if (expression.repeat_value) {
            check_expression(*expression.repeat_value, nullptr);
            return;
        }
        for (const auto &element : expression.elements) {
            if (element)
                check_expression(*element, nullptr);
        }
    };

    // expected 为数组时，使用其元素类型检查各元素，并核对结果长度。
    if (expected) {
        // 期望类型已经是 Error，仍检查元素以保留独立诊断。
        if (expected->is_error()) {
            check_elements_without_target();
            return ExpressionResult::error();
        }
        if (expected->kind == SemanticType::Kind::Array) {
            // 固定数组必须写清里面装什么元素，否则不知道该怎么检查每个元素。
            if (!expected->inner) {
                diagnostics_.error(expression.span,
                                   "array target is missing its element type");
                check_elements_without_target();
                return ExpressionResult::error();
            }
            has_fixed_target = true;
            expected_element = expected->inner.get();
            expected_length = expected->length;
        }
    }

    // `[值; 次数]`：值本身是普通表达式，只有“次数”按常量来处理。
    if (expression.repeat_value) {
        const auto element =
            check_expression(*expression.repeat_value, expected_element);
        bool ok = !element.type.is_error();

        if (has_fixed_target &&
            !require_coerce(element, *expected_element, expression.repeat_value->span))
            ok = false;

        // 次数必须是能算出来的 usize 常量，不能用普通变量或别的类型。
        const SemanticType usize = usize_type();
        const EvaluatedConstant count =
            analyzer_.evaluate_constant_value(expression.repeat_count, &usize,
                                              current_owner_);
        const auto *count_value = std::get_if<uint64_t>(&count.value);
        const bool count_ok = count.ok && !count.type.is_error() &&
                              count.type.same_type(usize) && count_value;
        if (!count_ok) {
            // 求值失败已有诊断；只有求值成功但类型不对时才报告次数错误。
            if (count.ok && !count.type.is_error())
                diagnostics_.error(expression.repeat_count.span,
                                   "array repeat count must be a usize constant");
            ok = false;
        }

        if (count_ok) {
            // 外面指定了数组长度时，重复次数必须和它一样。
            if (has_fixed_target && *count_value != expected_length) {
                diagnostics_.error(expression.span,
                                   "array repeat count does not match the "
                                   "fixed array length");
                ok = false;
            }
            // 同一个值要重复两次以上，就必须能安全复制；重复 0 次或 1 次不需要。
            if (*count_value > 1) {
                const SemanticType &element_type =
                    has_fixed_target ? *expected_element : element.type;
                if (!element_type.is_error()) {

                    if (!analyzer_.has_trait(element_type, Derive::Copy)) {
                        diagnostics_.error(
                            expression.span,
                            "array repeat with count greater than one "
                            "requires a Copy element");
                        ok = false;
                    }
                } else {
                    // 元素类型已经出错，复制检查没有意义。
                    ok = false;
                }
            }
        }

        // 前面任何一步失败，这次数组写法都没有正常结果。
        if (!ok)
            return ExpressionResult::error();
        // 有固定目标时，结果就是外面要求的那个数组类型。
        if (has_fixed_target)
            return ExpressionResult::value(*expected);
        // 没有固定目标时，元素类型就是重复值本身的类型。
        return ExpressionResult::value(
            SemanticType::array(element.type, *count_value));
    }

    // 外面要求固定数组：每个元素都按同一个元素类型检查，最后核对数量。
    if (has_fixed_target) {
        bool ok = true;
        // 元素类型本身已经出错，后面不再重复报错。
        if (expected_element->is_error())
            ok = false;
        for (const auto &element : expression.elements) {
            // 数组里少了一个元素表达式。
            if (!element) {
                diagnostics_.error(expression.span,
                                   "missing array element expression");
                ok = false;
                continue;
            }
            const ExpressionResult value =
                check_expression(*element, expected_element);
            if (!require_coerce(value, *expected_element, element->span))
                ok = false;
        }
        // 写的元素个数和外面要求的长度不一样。
        if (static_cast<uint64_t>(expression.elements.size()) !=
            expected_length) {
            diagnostics_.error(expression.span,
                               "array length does not match the fixed target");
            ok = false;
        }
        if (!ok)
            return ExpressionResult::error();
        return ExpressionResult::value(*expected);
    }

    // 外面没有要求数组类型：每个元素先各看各的，再看它们能不能用同一个类型装下。
    std::vector<ExpressionResult> elements;
    elements.reserve(expression.elements.size());
    bool ok = true;
    for (const auto &element : expression.elements) {
        // 数组里少了一个元素表达式。
        if (!element) {
            diagnostics_.error(expression.span,
                               "missing array element expression");
            ok = false;
            continue;
        }
        const ExpressionResult value = check_expression(*element, nullptr);
        // 这个元素本身已经出错。
        if (value.type.is_error())
            ok = false;
        elements.push_back(value);
    }
    if (!ok)
        return ExpressionResult::error();

    // 从前到后找一个能同时容纳这些元素的共同类型。
    const SemanticType element_type = compute_least_upper_bound(elements);
    // 找不到共同类型，这些元素就不能放在同一个数组里。
    if (element_type.is_error()) {
        diagnostics_.error(expression.span,
                           "array elements do not have a common type");
        return ExpressionResult::error();
    }
    return ExpressionResult::value(
        SemanticType::array(element_type, elements.size()));
}

// 下标具有 usize 期望；Never 下标不在规范要求支持的执行范围内。
bool BodyChecker::check_index_operand(const Expression &expression) {
    const auto usize = usize_type();
    const auto index = check_expression(expression, &usize);
    if (index.type.is_never()) {
        diagnostics_.error(expression.span,
                           "a diverging index expression is not supported");
        return false;
    }
    return require_coerce(index, usize, expression.span);
}

// 检查 `容器[下标]`
// 取出的元素保留容器的读写状态：
// 固定数组里，容器原来能写元素就能写，原来只能读元素也只能读；
// Vec 在容器不可写、或中间经过只能读的引用时，取出的元素也只能读。
ExpressionResult BodyChecker::check_index(const IndexExpression &expression) {
    // 缺少容器或下标，写法不完整。
    if (!expression.base || !expression.index)
        return ExpressionResult::error();

    // base 已经出错时，下标仍是独立表达式；继续检查它，保证错误恢复阶段
    // 不会漏掉下标里的名字/类型/发散诊断。
    auto check_index_after_invalid_base = [&]() {
        check_index_operand(*expression.index);
        return ExpressionResult::error();
    };

    ExpressionResult base = check_expression(*expression.base, nullptr);
    // 容器本身已经出错；仍检查下标表达式。
    if (base.type.is_error())
        return check_index_after_invalid_base();
    // 值容器物化为临时 place；已有 place 保留原权限，再据此确定元素权限。
    base.place = materialize_place(base.place);

    // 引用和 Box 只是包在容器外面的一层，逐层打开，直到看到数组或 Vec。
    while (base.type.kind == SemanticType::Kind::Reference ||
           base.type.kind == SemanticType::Kind::Box) {
        // 包装层里没有写清里面是什么类型，结构不完整。
        if (!base.type.inner)
            return check_index_after_invalid_base();
        base = dereference_place(base.type, base.place, expression.base->span);
        // 这一层打不开，后面的下标访问也就不成立。
        if (base.type.is_error())
            return check_index_after_invalid_base();
    }

    // 不是固定数组，也不是 Vec，就不能用下标取元素。
    if (base.type.kind != SemanticType::Kind::Array &&
        base.type.kind != SemanticType::Kind::Vec) {
        diagnostics_.error(expression.span,
                           "only arrays and Vec values can be indexed");
        return check_index_after_invalid_base();
    }
    // 数组或 Vec 没有写清元素类型，结构不完整。
    if (!base.type.inner)
        return check_index_after_invalid_base();

    if (!check_index_operand(*expression.index))
        return ExpressionResult::error();

    PlaceAccessInformation element_place;
    // 固定数组不额外加“只能读”的限制，容器原来能写，元素就能写。
    if (base.type.kind == SemanticType::Kind::Array) {
        element_place = base.place;
    } else if (base.place.is_mutable && !base.place.crossed_shared) {
        // Vec 本身可写，而且中间没有经过只能读的引用，元素才可写。
        element_place = PlaceAccessInformation{true, true, false};
    } else {
        // 否则这次取元素相当于透过一次只能读的引用：元素只能读，后面也不能再变成可写。
        element_place = PlaceAccessInformation{true, false, true};
    }

    return ExpressionResult::from_place(*base.type.inner, element_place);
}

} // namespace rx
