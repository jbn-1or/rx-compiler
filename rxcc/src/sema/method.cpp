// 实现点方法调用（MethodCallExpression）的目标查找。
// 5.4：先检查接收者一次，再从接收者类型沿引用/Box 解引用得到候选层；
// 每层按 Ti、&Ti、&mut Ti 的顺序匹配声明接收者类型，找到第一个就停止。
// Vec 不解引用；参数类型、结果期望和 place 可变性都不参与候选过滤。
// 5.5 再按保存的调整路径检查接收者权限，并用同一签名检查普通实参。
// 本文件不重复检查接收者子表达式。

#include "sema/body.hpp"
#include "sema/sema.hpp"
#include <set>
#include <utility>
#include <vector>

namespace rx {

namespace {

// 从接收者类型构造内置解引用链：T0, T1, ...。
// 引用和 Box 各解一层，Vec 与数组等不参与解引用。
std::vector<SemanticType> receiver_layers(const SemanticType &type) {
    std::vector<SemanticType> layers;
    layers.push_back(type);

    SemanticType current = type;
    while ((current.kind == SemanticType::Kind::Reference ||
            current.kind == SemanticType::Kind::Box) &&
           current.inner) {
        current = *current.inner;
        layers.push_back(current);
    }
    return layers;
}

// 候选类型若由用户结构体提供方法，返回该结构体。
// S 与 &S/&mut S 都能找到 S；&&S 不展开，避免用户方法跨过额外引用。
const StructSymbol *candidate_owner(const SemanticType &candidate) {
    const SemanticType *type = &candidate;
    if (type->kind == SemanticType::Kind::Reference) {
        if (!type->inner)
            return nullptr;
        type = type->inner.get();
    }
    return type->kind == SemanticType::Kind::Struct ? type->struct_symbol
                                                    : nullptr;
}

// 诊断中给结构体打印真实名字；其余类型沿用 SemanticType::toString。
std::string type_name(const SemanticType &type) {
    if (type.kind == SemanticType::Kind::Struct && type.struct_symbol)
        return type.struct_symbol->name;
    return type.toString();
}

} // namespace

// 5.4：按候选类型顺序查找名字相同、声明接收者类型精确等于候选的方法。
// 返回第一个匹配；没有匹配返回 nullopt，由调用方报告“没有该方法”。
std::optional<ResolvedMethod>
BodyChecker::resolve_method(const SemanticType &receiver_type,
                            const std::string &name) {
    const std::vector<SemanticType> layers = receiver_layers(receiver_type);

    const auto try_candidate =
        [&](const SemanticType &layer, const SemanticType &candidate,
            std::size_t dereferences,
            bool borrowed) -> std::optional<ResolvedMethod> {
        // 用户方法：只匹配声明接收者类型与候选完全相等的方法。
        // 无 receiver 的关联函数不参与点调用（ME5）。
        if (const StructSymbol *owner = candidate_owner(candidate)) {
            const auto item = owner->associated_items.find(name);
            if (item != owner->associated_items.end() &&
                item->second->kind == Symbol::Kind::Function) {
                const auto *function =
                    static_cast<const FunctionSymbol *>(item->second);
                if (function->self_parameter.present && function->owner) {
                    const SemanticType owner_type =
                        SemanticType::struct_(function->owner);
                    const SemanticType declared =
                        function->self_parameter.by_reference
                            ? SemanticType::reference(
                                  function->self_parameter.is_mut, owner_type)
                            : owner_type;
                    if (declared.equals(candidate)) {
                        ResolvedMethod method;
                        method.kind = ResolvedMethod::Kind::User;
                        method.function = function;
                        method.declared_receiver = candidate;
                        method.receiver = ReceiverAdjustment{
                            dereferences, borrowed,
                            borrowed && candidate.is_mut, layer};
                        method.return_type = function->return_type;
                        for (const ResolvedParameter &parameter :
                             function->parameters)
                            method.parameters.push_back(parameter.type);
                        return method;
                    }
                }
            }
        }

        // Vec 内置方法：&Vec<T> 提供 len/is_empty，&mut Vec<T> 提供 push/remove。
        if (candidate.kind == SemanticType::Kind::Reference &&
            candidate.inner && candidate.inner->kind == SemanticType::Kind::Vec &&
            candidate.inner->inner) {
            const SemanticType vector = *candidate.inner;
            const SemanticType element = *vector.inner;

            ResolvedMethod method;
            method.kind = ResolvedMethod::Kind::VecMethod;
            method.declared_receiver = candidate;
            method.receiver = ReceiverAdjustment{
                dereferences, borrowed, borrowed && candidate.is_mut, layer};
            method.container = vector;

            if (!candidate.is_mut && name == "len") {
                method.parameters = {};
                method.return_type =
                    SemanticType::int_(SemanticType::IntKind::Usize);
                return method;
            }
            if (!candidate.is_mut && name == "is_empty") {
                method.parameters = {};
                method.return_type = SemanticType::bool_();
                return method;
            }
            if (candidate.is_mut && name == "push") {
                method.parameters = {element};
                method.return_type = SemanticType::unit();
                return method;
            }
            if (candidate.is_mut && name == "remove") {
                method.parameters = {SemanticType::int_(
                    SemanticType::IntKind::Usize)};
                method.return_type = element;
                return method;
            }
        }

        // 固定数组的 len 是 &self 内置方法（B9）。
        if (name == "len" && candidate.kind == SemanticType::Kind::Reference &&
            !candidate.is_mut && candidate.inner &&
            candidate.inner->kind == SemanticType::Kind::Array) {
            ResolvedMethod method;
            method.kind = ResolvedMethod::Kind::ArrayMethod;
            method.declared_receiver = candidate;
            method.receiver = ReceiverAdjustment{
                dereferences, borrowed, borrowed && candidate.is_mut, layer};
            method.return_type =
                SemanticType::int_(SemanticType::IntKind::Usize);
            return method;
        }

        // Clone：对每个 U: Clone 提供声明接收者 &U、返回 U 的方法（ME6）。
        // 候选本身可能就是已有的 &U，也可能是本层新增的 &Ti。
        if (name == "clone" && candidate.kind == SemanticType::Kind::Reference &&
            !candidate.is_mut && candidate.inner) {
            const SemanticType clone_type = *candidate.inner;
            std::set<std::pair<const StructSymbol *, Derive>> visiting;
            if (analyzer_.has_trait(clone_type, Derive::Clone, visiting)) {
                ResolvedMethod method;
                method.kind = ResolvedMethod::Kind::BuiltinClone;
                method.declared_receiver = candidate;
                method.receiver = ReceiverAdjustment{
                    dereferences, borrowed, false, layer};
                method.clone_type = clone_type;
                method.return_type = clone_type;
                return method;
            }
        }

        return std::nullopt;
    };

    for (std::size_t index = 0; index < layers.size(); ++index) {
        const SemanticType &layer = layers[index];
        if (auto method = try_candidate(layer, layer, index, false))
            return method;
        if (auto method = try_candidate(
                layer, SemanticType::reference(false, layer), index, true))
            return method;
        if (auto method = try_candidate(
                layer, SemanticType::reference(true, layer), index, true))
            return method;
    }
    return std::nullopt;
}

// 5.5：沿 5.4 保存的路径应用接收者调整并检查访问权限。
// 返回 false 表示已诊断；成功时只做判定，接收者不属于普通实参表。
bool BodyChecker::check_method_receiver(const ExpressionResult &receiver,
                                        const ResolvedMethod &method,
                                        Span at) {
    // 先按同一路径执行内置解引用，得到方法接收者所在的 place。
    ExpressionResult reached = receiver;
    for (std::size_t step = 0; step < method.receiver.dereferences; ++step) {
        reached = dereference_place(reached.type, reached.place, at);
        if (reached.type.is_error())
            return false;
    }

    // &mut self 命中原本就有的 &mut U 候选时，不做新的自动借用，
    // 但仍要把 U 当作可变重借用检查；保存该引用的绑定本身不必可赋值。
    if (!method.receiver.borrow &&
        method.declared_receiver.kind == SemanticType::Kind::Reference &&
        method.declared_receiver.is_mut) {
        const ExpressionResult target =
            dereference_place(reached.type, reached.place, at);
        if (target.type.is_error())
            return false;
        if (!target.place.is_mutable || target.place.crossed_shared) {
            diagnostics_.error(
                at, "cannot mutably borrow an immutable or shared place");
            return false;
        }
        return true;
    }

    // 自动 &mut Ti：到达的 Ti 必须是可变且未跨共享访问的 place；
    // 值接收者可以物化成可变临时 place（如 Vec::new().push(1)）。
    if (method.receiver.borrow && method.receiver.mutable_borrow) {
        const PlaceAccessInformation place = materialize_place(reached.place);
        if (!place.is_mutable || place.crossed_shared) {
            diagnostics_.error(
                at, "cannot mutably borrow an immutable or shared place");
            return false;
        }
    }

    // &self/Clone/len 与按值接收者不要求调用方绑定可变。
    return true;
}

// 5.4/5.5：检查一次点调用。接收者只检查一次；方法片段带具体类型实参时报 G6。
// 选定目标后先应用接收者调整，再用同一签名检查普通实参；失败不回退其他方法。
ExpressionResult
BodyChecker::check_method_call(const MethodCallExpression &call) {
    if (!call.receiver)
        return ExpressionResult{SemanticType::error(),
                                PlaceAccessInformation{}};

    // 接收者表达式只检查一次，查找和后续调整共用这个结果。
    const ExpressionResult receiver = check_expression(*call.receiver, nullptr);

    // G6：方法片段不能带具体类型参数。
    if (!call.method.generics.empty()) {
        diagnostics_.error(call.span,
                           "methods have no type parameters: " +
                               call.method.name);
        return ExpressionResult{SemanticType::error(),
                                PlaceAccessInformation{}};
    }

    // 接收者已出错时不再追加“没有方法”的连锁诊断。
    if (receiver.type.is_error())
        return ExpressionResult{SemanticType::error(),
                                PlaceAccessInformation{}};

    const std::optional<ResolvedMethod> method =
        resolve_method(receiver.type, call.method.name);
    if (!method) {
        diagnostics_.error(call.span,
                           "no method named " + call.method.name +
                               " for type " + type_name(receiver.type));
        return ExpressionResult{SemanticType::error(),
                                PlaceAccessInformation{}};
    }

    // 5.5：先按保存的路径检查接收者权限，再检查不含接收者的普通实参。
    if (!check_method_receiver(receiver, *method, call.receiver->span))
        return ExpressionResult{SemanticType::error(),
                                PlaceAccessInformation{}};
    if (!check_arguments(method->parameters, call.arguments, call.span))
        return ExpressionResult{SemanticType::error(),
                                PlaceAccessInformation{}};

    // 方法调用产生值，不继承接收者的 place。
    return ExpressionResult{method->return_type, PlaceAccessInformation{}};
}

} // namespace rx
