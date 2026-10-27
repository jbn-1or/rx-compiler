// 检查结构体构造、字段访问、数组列表/重复和下标访问。
// 具体检查：
// - 结构体构造 S { ... } / Self { ... }：
// - 字段访问 base.field：
// - 数组列表 [a, b, ...]：
// - 数组重复 [e; N]：
// - 下标 base[index]：

#include "sema/body.hpp"
#include "sema/sema.hpp"
#include <cstdint>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rx {

namespace {

// 生成一个出错结果
ExpressionResult failed_expression() {
    return ExpressionResult{SemanticType::error(), PlaceAccessInformation{}};
}

// 普通值的结果
ExpressionResult value_expression(SemanticType type) {
    return ExpressionResult{std::move(type), PlaceAccessInformation{}};
}

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
        return failed_expression();
    }

    const std::vector<PathSegment> &segments = expression.path->segments;
    // 这里只接受一段名字，比如 `S` 或 `Self`；多段名字不是结构体写法。
    if (segments.size() != 1) {
        diagnostics_.error(expression.path->span,
                           "struct literal path must name exactly one type");
        check_fields_without_target();
        return failed_expression();
    }

    const PathSegment &segment = segments[0];
    StructSymbol *structure = nullptr;
    if (segment.name == "Self") {
        // `Self` 只能在结构体自己的实现里使用。
        if (!current_owner_) {
            diagnostics_.error(expression.path->span,
                               "Self is not available outside a struct");
            check_fields_without_target();
            return failed_expression();
        }
        structure = current_owner_;
    } else {
        const auto found = analyzer_.type_namespace.find(segment.name);
        // 名字必须对应一个已经声明的结构体；不能拿同名函数或常量顶替。
        if (found == analyzer_.type_namespace.end()) {
            diagnostics_.error(expression.path->span,
                               "unresolved struct name: " + segment.name);
            check_fields_without_target();
            return failed_expression();
        }
        structure = found->second;
    }

    // 结构体名字后面不能写 `<...>` 这样的额外说明；写了就报错。
    if (!segment.generics.empty()) {
        diagnostics_.error(expression.path->span,
                           "structs have no type parameters: " + segment.name);
        check_fields_without_target();
        return failed_expression();
    }
    // 正常情况下不会走到这里，留作兜底。
    if (!structure) {
        check_fields_without_target();
        return failed_expression();
    }

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
        // 字段类型或这个值前面已经出过错，不再重复报同一条问题。
        if (type->second.is_error() || value.type.is_error()) {
            ok = false;
            continue;
        }
        // 值放不进声明的字段类型。
        if (!can_coerce(value.type, value.place, type->second)) {
            require_coerce(value, type->second, field.value->span);
            ok = false;
        }
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
        return failed_expression();
    return value_expression(SemanticType::struct_(structure));
}

// 检查 `对象.字段`
// 对象可以是结构体本身，也可以是指向结构体的引用或 Box。
// 找到真正的结构体，再取出字段。保留对象原来的读写状态
ExpressionResult BodyChecker::check_field(const FieldExpression &expression) {
    // 没有对象可以取字段。
    if (!expression.base)
        return failed_expression();

    ExpressionResult base = check_expression(*expression.base, nullptr);
    // 对象本身已经出错，不再继续查找字段。
    if (base.type.is_error())
        return failed_expression();
    // 对象只是一个临时结果，不是变量；先给它一个临时存放位置，后面才能取字段。
    if (!base.place.is_place)
        base.place = materialize_place(base.place);

    // 引用和 Box 都只是包在结构体外面的一层，逐层打开，直到看到结构体。
    while (base.type.kind == SemanticType::Kind::Reference ||
           base.type.kind == SemanticType::Kind::Box) {
        // 包装层里没有写清里面是什么类型，结构不完整。
        if (!base.type.inner)
            return failed_expression();
        base = dereference_place(base.type, base.place, expression.base->span);
        // 这一层打不开，后面的字段也就不存在了。
        if (base.type.is_error())
            return failed_expression();
    }

    // 打开后看到的不是结构体，不能点字段。
    if (base.type.kind != SemanticType::Kind::Struct ||
        !base.type.struct_symbol) {
        diagnostics_.error(expression.span,
                           "field access requires a struct base");
        return failed_expression();
    }

    const StructSymbol *structure = base.type.struct_symbol;
    const auto field = structure->field_types.find(expression.field);
    // 结构体里没有这个名字的字段。
    if (field == structure->field_types.end()) {
        diagnostics_.error(expression.span,
                           "unknown struct field: " + expression.field);
        return failed_expression();
    }
    // 字段类型前面已经出过错，不再返回一个不可用的位置。
    if (field->second.is_error())
        return failed_expression();

    return ExpressionResult{field->second, base.place};
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

    // 外面可能已经要求这里必须是一个固定数组；只有这种情况才把元素类型和长度往里传。
    if (expected) {
        // 外面的类型前面已经出错，不再把元素当作固定目标，但仍检查元素本身。
        if (expected->is_error()) {
            check_elements_without_target();
            return failed_expression();
        }
        if (expected->kind == SemanticType::Kind::Array) {
            // 固定数组必须写清里面装什么元素，否则不知道该怎么检查每个元素。
            if (!expected->inner) {
                diagnostics_.error(expression.span,
                                   "array target is missing its element type");
                check_elements_without_target();
                return failed_expression();
            }
            has_fixed_target = true;
            expected_element = expected->inner.get();
            expected_length = expected->length;
        }
    }

    // `[值; 次数]`：值本身是普通表达式，只有“次数”按常量来处理。
    if (expression.repeat_value) {
        bool ok = true;
        ExpressionResult element =
            check_expression(*expression.repeat_value, expected_element);
        // 重复的那个值本身就出错了。
        if (element.type.is_error())
            ok = false;

        // 外面要求固定数组时，这个值必须能放进数组的元素类型。
        if (has_fixed_target) {
            // 元素类型本身已经出错，不再重复报类型不匹配。
            if (expected_element->is_error()) {
                ok = false;
            } else if (!element.type.is_error() &&
                       !can_coerce(element.type, element.place,
                                   *expected_element)) {
                require_coerce(element, *expected_element,
                               expression.repeat_value->span);
                ok = false;
            }
        }

        // 次数必须是能算出来的 usize 常量，不能用普通变量或别的类型。
        const SemanticType usize = usize_type();
        const std::size_t diagnostics_before = diagnostics_.items.size();
        const EvaluatedConstant count =
            analyzer_.evaluate_constant_value(expression.repeat_count, &usize,
                                              current_owner_);
        bool count_ok = count.ok && !count.type.is_error() &&
                        count.type.equals(usize);
        const auto *count_value = std::get_if<uint64_t>(&count.value);
        // 次数算不出来，或者算出来的不是非负的 usize。常量求值器已经报过错时
        // 不再重复追加一条泛化诊断。
        if (!count_ok || !count_value) {
            if (diagnostics_.items.size() == diagnostics_before) {
                diagnostics_.error(
                    expression.repeat_count.span,
                    "array repeat count must be a usize constant");
            }
            ok = false;
        }

        if (count_ok && count_value) {
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
                    std::set<std::pair<const StructSymbol *, Derive>> visiting;
                    if (!analyzer_.has_trait(element_type, Derive::Copy,
                                             visiting)) {
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
        if (!ok || !count_ok || !count_value)
            return failed_expression();
        // 有固定目标时，结果就是外面要求的那个数组类型。
        if (has_fixed_target)
            return value_expression(*expected);
        if (element.type.is_error())
            return failed_expression();
        // 没有固定目标时，元素类型就是重复值本身的类型。
        return value_expression(
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
            // 这个元素本身已经出错。
            if (value.type.is_error()) {
                ok = false;
                continue;
            }
            // 这个元素放不进要求的元素类型。
            if (!can_coerce(value.type, value.place, *expected_element)) {
                require_coerce(value, *expected_element, element->span);
                ok = false;
            }
        }
        // 写的元素个数和外面要求的长度不一样。
        if (static_cast<uint64_t>(expression.elements.size()) !=
            expected_length) {
            diagnostics_.error(expression.span,
                               "array length does not match the fixed target");
            ok = false;
        }
        if (!ok)
            return failed_expression();
        return value_expression(*expected);
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
        return failed_expression();

    // 从前到后找一个能同时容纳这些元素的共同类型。
    const SemanticType element_type = compute_least_upper_bound(elements);
    // 找不到共同类型，这些元素就不能放在同一个数组里。
    if (element_type.is_error()) {
        diagnostics_.error(expression.span,
                           "array elements do not have a common type");
        return failed_expression();
    }
    return value_expression(
        SemanticType::array(element_type, elements.size()));
}

// 检查 `容器[下标]`
// 取出的元素保留容器的读写状态：
// 固定数组里，容器原来能写元素就能写，原来只能读元素也只能读；
// Vec 在容器不可写、或中间经过只能读的引用时，取出的元素也只能读。
ExpressionResult BodyChecker::check_index(const IndexExpression &expression) {
    // 缺少容器或下标，写法不完整。
    if (!expression.base || !expression.index)
        return failed_expression();

    // base 已经出错时，下标仍是独立表达式；继续检查它，保证错误恢复阶段
    // 不会漏掉下标里的名字/类型/发散诊断。
    auto check_index_after_invalid_base = [&]() {
        const SemanticType usize = usize_type();
        const ExpressionResult index =
            check_expression(*expression.index, &usize);
        if (index.type.is_error())
            return failed_expression();
        if (index.type.is_never()) {
            diagnostics_.error(expression.index->span,
                               "a diverging index expression is not supported");
            return failed_expression();
        }
        if (!can_coerce(index.type, index.place, usize))
            require_coerce(index, usize, expression.index->span);
        return failed_expression();
    };

    ExpressionResult base = check_expression(*expression.base, nullptr);
    // 容器本身已经出错；仍检查下标表达式。
    if (base.type.is_error())
        return check_index_after_invalid_base();
    // 容器只是一个临时结果，不是变量；先给它一个临时存放位置，后面才能取元素。
    if (!base.place.is_place)
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

    // 下标必须能当成 usize 使用。
    const SemanticType usize = usize_type();
    const ExpressionResult index =
        check_expression(*expression.index, &usize);
    // 下标本身已经出错。
    if (index.type.is_error())
        return failed_expression();
    // 下标永远算不出来（例如里面直接 return），不能当作正常下标使用。
    if (index.type.is_never()) {
        diagnostics_.error(expression.index->span,
                           "a diverging index expression is not supported");
        return failed_expression();
    }
    // 下标类型不是 usize，或者放不进 usize。
    if (!can_coerce(index.type, index.place, usize)) {
        require_coerce(index, usize, expression.index->span);
        return failed_expression();
    }

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

    return ExpressionResult{*base.type.inner, element_place};
}

} // namespace rx
