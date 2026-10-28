// 候选选择只看类型和声明顺序；选中后再沿保存的路径检查接收者权限。
#include "sema/body/checker.hpp"
#include "sema/sema.hpp"
#include <array>
#include <cassert>
#include <utility>

namespace rx {

std::optional<CallableSignature>
BodyChecker::lookup_method_signature(const SemanticType &candidate,
                                     const std::string &name) {
    const SemanticType *owner = &candidate;
    if (owner->kind == SemanticType::Kind::Reference && owner->inner)
        owner = owner->inner.get();
    if (owner->kind == SemanticType::Kind::Struct && owner->struct_symbol) {
        const auto &items = owner->struct_symbol->associated_items;
        const auto item = items.find(name);
        if (item != items.end() && item->second->kind == Symbol::Kind::Function) {
            const auto &function = static_cast<const FunctionSymbol &>(*item->second);
            if (function.receiver && function.receiver->type.same_type(candidate))
                return function_signature(function);
        }
    }
    if (candidate.kind != SemanticType::Kind::Reference || !candidate.inner)
        return std::nullopt;
    auto signature = builtin_method_signature(*candidate.inner, name);
    if (!signature || !signature->receiver->same_type(candidate))
        return std::nullopt;
    if (name == "clone" && !analyzer_.has_trait(*candidate.inner, Derive::Clone))
        return std::nullopt;
    return signature;
}

std::optional<ResolvedMethod>
BodyChecker::resolve_method(const SemanticType &receiver_type,
                            const std::string &name) {
    using Borrow = ReceiverAdjustment::Borrow;
    const SemanticType *layer = &receiver_type;
    for (std::size_t dereferences = 0; layer; ++dereferences) {
        // 每层固定尝试 T、&T、&mut T；权限和实参不能影响候选选择。
        const auto candidates = std::array{
            std::pair{*layer, Borrow::None},
            std::pair{SemanticType::reference(false, *layer), Borrow::Shared},
            std::pair{SemanticType::reference(true, *layer), Borrow::Mutable},
        };
        for (const auto &[candidate, borrow] : candidates) {
            if (auto signature = lookup_method_signature(candidate, name))
                return ResolvedMethod{std::move(*signature), {dereferences, borrow}};
        }
        if (layer->kind != SemanticType::Kind::Reference &&
            layer->kind != SemanticType::Kind::Box)
            break;
        layer = layer->inner.get();
    }
    return std::nullopt;
}

bool BodyChecker::check_method_receiver(const ExpressionResult &receiver,
                                        const ResolvedMethod &method, Span at) {
    assert(method.signature.receiver);
    ExpressionResult reached = receiver;
    for (std::size_t step = 0; step < method.receiver.dereferences; ++step) {
        reached = dereference_place(reached.type, reached.place, at);
        if (reached.type.is_error())
            return false;
    }
    const auto &declared = *method.signature.receiver;
    using Borrow = ReceiverAdjustment::Borrow;
    const bool reborrow_mutable = method.receiver.borrow == Borrow::None &&
                                  declared.kind == SemanticType::Kind::Reference &&
                                  declared.is_mut;
    if (reborrow_mutable) {
        // 已有的 &mut U 也要验证重借用；保存引用的绑定本身不必可赋值。
        reached = dereference_place(reached.type, reached.place, at);
        if (reached.type.is_error())
            return false;
    }
    if (reborrow_mutable || method.receiver.borrow == Borrow::Mutable) {
        const auto place = materialize_place(reached.place);
        if (!place.is_mutable || place.crossed_shared) {
            diagnostics_.error(
                at, "cannot mutably borrow an immutable or shared place");
            return false;
        }
    }
    return true;
}

ExpressionResult BodyChecker::check_method_call(const MethodCallExpression &call) {
    if (!call.receiver)
        return ExpressionResult::error();
    const auto receiver = check_expression(*call.receiver, nullptr);
    if (!call.method.generics.empty()) {
        diagnostics_.error(call.span,
                           "methods have no type parameters: " + call.method.name);
        return ExpressionResult::error();
    }
    if (receiver.type.is_error())
        return ExpressionResult::error();
    const auto method = resolve_method(receiver.type, call.method.name);
    if (!method) {
        diagnostics_.error(call.span, "no method named " + call.method.name +
                                         " for type " + receiver.type.toString());
        return ExpressionResult::error();
    }
    const bool receiver_ok =
        check_method_receiver(receiver, *method, call.receiver->span);
    const bool arguments_ok =
        check_arguments(method->signature.parameters, call.arguments, call.span);
    if (!receiver_ok || !arguments_ok)
        return ExpressionResult::error();
    return ExpressionResult::value(method->signature.return_type);
}

} // namespace rx
