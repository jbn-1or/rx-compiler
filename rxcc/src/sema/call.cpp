// 实现普通函数调用、内置 I/O 和多段被调用者解析。
// 4.4 处理单段 PathExpression callee：局部绑定优先（N13），再查值名字空间，
// 未命中值名才算同名结构体不可调用（N16）。
// 5.1 处理两段类型前缀路径（S::f、Self::f、Box::<T>::new、Vec::<T>::len、
// Type::clone）：第一段始终按类型解析，不当作局部值；这里只选定目标并校验
// 路径上的类型实参，实参检查在 5.2/5.3 建立签名后统一进行。
// 点方法调用（MethodCallExpression）的候选查找属于 5.4/5.5，不经过这里。
// 实参检查集中在 check_arguments，供普通调用与关联调用复用。

#include "sema/body.hpp"
#include "sema/call.hpp"
#include "sema/sema.hpp"
#include "sema/type_utils.hpp"
#include <set>
#include <utility>

namespace rx {

namespace {

ExpressionResult failed_call() {
    return ExpressionResult{SemanticType::error(), PlaceAccessInformation{}};
}

// 5.3：显式用户关联调用的参数表。
// 无接收者时就是声明的普通参数；有接收者时把接收者放在最前：
// self/mut self -> U，&self -> &U，&mut self -> &mut U。
std::optional<CallableSignature>
associated_call_signature(const ResolvedCallee &target) {
    const FunctionSymbol *function = target.function;
    if (!function)
        return std::nullopt;

    CallableSignature signature;
    signature.return_type = function->return_type;

    if (function->self_parameter.present) {
        if (!function->owner)
            return std::nullopt;
        const SemanticType owner = SemanticType::struct_(function->owner);
        if (function->self_parameter.by_reference) {
            signature.parameters.push_back(SemanticType::reference(
                function->self_parameter.is_mut, owner));
        } else {
            signature.parameters.push_back(owner);
        }
    }
    for (const ResolvedParameter &parameter : function->parameters)
        signature.parameters.push_back(parameter.type);
    return signature;
}

} // namespace

// 按声明参数类型逐个检查实参。
// 数量不等时只报一次数量错误，不越界访问；类型不兼容时由 require_coerce 定位到实参。
bool BodyChecker::check_arguments(
    const std::vector<SemanticType> &parameter_types,
    const std::vector<std::shared_ptr<Expression>> &arguments, Span at) {
    // 形参和实参数量对不上
    if (parameter_types.size() != arguments.size()) {
        diagnostics_.error(at, "call expects " +
                                   std::to_string(parameter_types.size()) +
                                   " argument(s), found " +
                                   std::to_string(arguments.size()));
        return false;
    }
    // 一次核对每个实参
    bool ok = true;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (!arguments[index]) {
            diagnostics_.error(at, "missing argument expression");
            ok = false;
            continue;
        }
        // 实参的期望类型来自被调声明，外层调用位置的期望不改变签名。
        const ExpressionResult argument =
            check_expression(*arguments[index], &parameter_types[index]);
        if (!can_coerce(argument.type, argument.place,
                        parameter_types[index])) {
            require_coerce(argument, parameter_types[index],
                           arguments[index]->span);
            ok = false;
        }
    }
    return ok;
}

// 解析并检查一次调用表达式：先取出被调用者路径，再按段数分派。
// 非路径 callee 不能解析为函数；空路径同样无法确定目标。
ExpressionResult BodyChecker::check_call(const CallExpression &call) {
    if (!call.callee)
        return failed_call();

    const auto *path = dynamic_cast<const PathExpression *>(call.callee.get());
    if (!path) {
        // Rx 没有函数指针/函数值调用；非路径 callee 不能解析为函数。
        diagnostics_.error(call.callee->span, "expression is not callable");
        return failed_call();
    }
    if (path->segments.empty())
        return failed_call();

    // 单段沿用 4.4；两段按类型前缀解析（5.1），超过两段在解析时报错。
    if (path->segments.size() == 1)
        return check_ordinary_call(*path, call);

    // 5.1：先选定目标并校验路径上的类型实参，第一段不查局部值名。
    const std::optional<ResolvedCallee> target =
        resolve_qualified_callee(*path);
    if (!target)
        return failed_call();

    // 目标已选定；5.2 的容器构造器和 5.3 的关联调用/容器方法/Clone
    // 都建立普通参数表，再统一交给 check_arguments。
    const auto check_signature =
        [&](const std::optional<CallableSignature> &signature,
            const char *what) -> ExpressionResult {
        if (!signature) {
            diagnostics_.error(call.span, std::string("unsupported ") + what);
            return failed_call();
        }
        if (!check_arguments(signature->parameters, call.arguments, call.span))
            return failed_call();
        return ExpressionResult{signature->return_type,
                                PlaceAccessInformation{}};
    };

    switch (target->kind) {
        case ResolvedCallee::Kind::AssociatedFunction:
            return check_signature(associated_call_signature(*target),
                                   "associated call");
        case ResolvedCallee::Kind::ContainerConstructor:
            return check_signature(container_constructor_signature(*target),
                                   "container constructor");
        case ResolvedCallee::Kind::ContainerMethod:
            return check_signature(container_method_signature(*target),
                                   "container method");
        case ResolvedCallee::Kind::BuiltinClone: {
            // B12：Box<T>/Vec<T> 的 Clone 递归要求元素具备 Clone。
            std::set<std::pair<const StructSymbol *, Derive>> visiting;
            if (!analyzer_.has_trait(target->clone_type, Derive::Clone,
                                     visiting)) {
                diagnostics_.error(call.span,
                                   "type " + target->clone_type.toString() +
                                       " does not support clone");
                return failed_call();
            }
            return check_signature(builtin_clone_signature(*target), "clone");
        }
    }
    return failed_call();
}

// 4.4：单段被调用者。局部名字优先，命中后不再回退外层函数；
// 值名字空间里函数/内置 I/O 可调用，常量不可调用；
// 值名字空间未命中时，同名结构体判为不可调用（N16），保证 struct S 与
// fn S() 并存时调用按值名字空间解析。
ExpressionResult
BodyChecker::check_ordinary_call(const PathExpression &path,
                                 const CallExpression &call) {
    const PathSegment &segment = path.segments[0];

    // N13：局部绑定优先；命中局部就不再回退到同名函数/内置函数。
    if (lookup_local(segment.name)) {
        diagnostics_.error(path.span,
                           "local binding " + segment.name +
                               " is not callable");
        return failed_call();
    }

    // 值名字空间：函数、常量、内置 I/O 共用。
    const auto value = analyzer_.function_constant_namespace.find(segment.name);
    if (value != analyzer_.function_constant_namespace.end()) {
        const Symbol *symbol = value->second;

        // 顶层/关联常量都不是可调用对象。
        if (symbol->kind == Symbol::Kind::Constant) {
            diagnostics_.error(path.span,
                               "constant " + segment.name +
                                   " is not callable");
            return failed_call();
        }

        if (symbol->kind == Symbol::Kind::Function) {
            // G2：当前语言没有泛型函数。
            if (!segment.generics.empty()) {
                diagnostics_.error(path.span,
                                   "functions have no type parameters: " +
                                       segment.name);
                return failed_call();
            }
            const auto *function =
                static_cast<const FunctionSymbol *>(symbol);
            std::vector<SemanticType> parameter_types;
            parameter_types.reserve(function->parameters.size());
            for (const auto &parameter : function->parameters)
                parameter_types.push_back(parameter.type);

            if (!check_arguments(parameter_types, call.arguments, call.span))
                return failed_call();
            // 调用结果是声明的返回类型；函数体是否发散不改变签名。
            return ExpressionResult{function->return_type,
                                    PlaceAccessInformation{}};
        }

        if (symbol->kind == Symbol::Kind::Builtin) {
            // 内置 I/O 同样没有类型参数。
            if (!segment.generics.empty()) {
                diagnostics_.error(path.span,
                                   "builtin functions have no type parameters: " +
                                       segment.name);
                return failed_call();
            }
            const auto signature = builtin_signature(segment.name);
            if (!signature) {
                diagnostics_.error(
                    path.span,
                    "unknown built-in function: " + segment.name);
                return failed_call();
            }
            if (!check_arguments(signature->parameters, call.arguments,
                                 call.span))
                return failed_call();
            return ExpressionResult{signature->return_type,
                                    PlaceAccessInformation{}};
        }
    }

    // 上方已经判断值名字空间未命中，同名类型名判为不可调用结构体。
    // 这样 struct S 与 fn S() 可以并存，调用按值名字空间解析。
    if (analyzer_.type_namespace.count(segment.name)) {
        diagnostics_.error(path.span,
                           "struct " + segment.name +
                               " is not callable; use struct literal syntax");
        return failed_call();
    }

    // N14：既不是值，也不是可调用结构体。
    diagnostics_.error(path.span,
                       "unresolved value name: " + segment.name);
    return failed_call();
}

// 5.1：解析 `Type::member` 形式的两段被调用者。
// 第一段始终按类型前缀解析（不查局部值名，也不存在“回退”）；第二段是成员。
// 只选定目标并校验路径上的类型实参：
//   - 所有路径段都检查具体类型实参（G1/G2/G3，Box/Vec 恰好一个，G4/G5）；
//   - Self 用当前 owner，没有则报 N10；
//   - S::f / Self::f 查结构体 associated_items，常量不可调用（C2）；
//   - 类型前缀无法解析、成员不存在或路径超过两段报错。
// 返回 nullopt 表示已诊断，调用方直接按失败恢复。
std::optional<ResolvedCallee>
BodyChecker::resolve_qualified_callee(const PathExpression &path) {
    const std::vector<PathSegment> &segments = path.segments;
    if (segments.size() > 2) {
        diagnostics_.error(path.span,
                           "call path must have at most two segments");
        return std::nullopt;
    }

    const PathSegment &prefix = segments[0];
    const PathSegment &member = segments[1];

    // 成员片段不接受具体类型实参；该检查与成员是否存在无关，先做。
    if (!member.generics.empty()) {
        diagnostics_.error(path.span,
                           "functions have no type parameters: " +
                               member.name);
        return std::nullopt;
    }

    // self 是值不是类型，不能作为路径前缀。
    if (prefix.name == "self") {
        diagnostics_.error(path.span, "self is not a type name");
        return std::nullopt;
    }

    // Self 展开为当前 impl 的结构体；没有归属报 N10。
    if (prefix.name == "Self") {
        if (!current_owner_) {
            diagnostics_.error(
                path.span, "Self is not available outside a struct or impl");
            return std::nullopt;
        }
        // G1：Self 与用户结构体一样不接受类型实参。
        if (!prefix.generics.empty()) {
            diagnostics_.error(path.span,
                               "structs have no type parameters: Self");
            return std::nullopt;
        }
        return resolve_struct_member(*current_owner_, member, path.span);
    }

    // 用户结构体：查关联项；路径段不接受类型实参（G1）。
    const auto structure = analyzer_.type_namespace.find(prefix.name);
    if (structure != analyzer_.type_namespace.end()) {
        if (!prefix.generics.empty()) {
            diagnostics_.error(path.span,
                               "structs have no type parameters: " +
                                   prefix.name);
            return std::nullopt;
        }
        return resolve_struct_member(*structure->second, member, path.span);
    }

    // Box / Vec：恰好一个具体类型实体，成员限于构造器、Clone 与 Vec 方法。
    if (prefix.name == "Box" || prefix.name == "Vec") {
        // G4/G5：具体类型实参数必须等于 1；builder 丢弃了生命周期实参，
        // 这里只数保留的具体类型，零个与多个都拒绝。
        if (prefix.generics.size() != 1) {
            diagnostics_.error(
                path.span,
                prefix.name + " requires exactly one concrete type argument");
            return std::nullopt;
        }
        if (!prefix.generics[0])
            return std::nullopt;
        const SemanticType inner =
            analyzer_.resolve_type(*prefix.generics[0], current_owner_);
        if (inner.is_error())
            return std::nullopt;

        const bool is_vec = prefix.name == "Vec";
        const bool supported =
            member.name == "new" || member.name == "clone" ||
            (is_vec && (member.name == "len" || member.name == "is_empty" ||
                        member.name == "push" || member.name == "remove"));
        if (!supported) {
            diagnostics_.error(path.span,
                               prefix.name + " has no member named " +
                                   member.name);
            return std::nullopt;
        }

        ResolvedCallee target;
        if (member.name == "new")
            target.kind = ResolvedCallee::Kind::ContainerConstructor;
        else if (member.name == "clone")
            target.kind = ResolvedCallee::Kind::BuiltinClone;
        else
            target.kind = ResolvedCallee::Kind::ContainerMethod;
        target.container =
            is_vec ? SemanticType::vec(inner) : SemanticType::boxed(inner);
        if (target.kind == ResolvedCallee::Kind::BuiltinClone)
            target.clone_type = target.container;
        target.member = member.name;
        return target;
    }

    // 基本类型：只有内置 Clone 能作为显式成员，签名在 5.3 建立。
    const SemanticType primitive = basic_type_from_name(prefix.name);
    if (!primitive.is_error()) {
        // G3：基本类型不接受类型实参。
        if (!prefix.generics.empty()) {
            diagnostics_.error(path.span,
                               "primitives take no type parameters: " +
                                   prefix.name);
            return std::nullopt;
        }
        if (member.name != "clone") {
            diagnostics_.error(path.span,
                               "primitive type " + prefix.name +
                                   " has no member named " + member.name);
            return std::nullopt;
        }
        ResolvedCallee target;
        target.kind = ResolvedCallee::Kind::BuiltinClone;
        target.clone_type = primitive;
        target.member = member.name;
        return target;
    }

    // 两段路径的第一段不是任何可解析的类型（含“它是局部值”的情况：
    // 类型前缀不按局部值解析，也不回退）。
    diagnostics_.error(path.span, "unresolved type: " + prefix.name);
    return std::nullopt;
}

// 5.1：在结构体的关联项里找成员。
// 关联函数/方法可调用；关联常量不是可调用目标（C2）。
// 找不到成员且名字是 clone 时，若结构体具备 Clone 能力，显式 `S::clone`
// 命中内置/派生 Clone（B12/ME6：先查 inherent，未找到才用派生），签名在
// 5.3 建立；其余情况报成员不存在。
std::optional<ResolvedCallee>
BodyChecker::resolve_struct_member(const StructSymbol &structure,
                                   const PathSegment &member, Span at) {
    const auto item = structure.associated_items.find(member.name);
    if (item == structure.associated_items.end()) {
        if (member.name == "clone") {
            std::set<std::pair<const StructSymbol *, Derive>> visiting;
            if (analyzer_.has_trait(SemanticType::struct_(&structure),
                                    Derive::Clone, visiting)) {
                ResolvedCallee target;
                target.kind = ResolvedCallee::Kind::BuiltinClone;
                target.clone_type = SemanticType::struct_(&structure);
                target.member = member.name;
                return target;
            }
        }
        diagnostics_.error(
            at, "struct " + structure.name + " has no associated item named " +
                    member.name);
        return std::nullopt;
    }

    const Symbol *symbol = item->second;
    if (symbol->kind == Symbol::Kind::Constant) {
        diagnostics_.error(at, "constant " + structure.name + "::" +
                                   member.name + " is not callable");
        return std::nullopt;
    }

    ResolvedCallee target;
    target.kind = ResolvedCallee::Kind::AssociatedFunction;
    target.function = static_cast<const FunctionSymbol *>(symbol);
    return target;
}

} // namespace rx
