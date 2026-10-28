// 候选选择只看类型和声明顺序；选中后再沿保存的路径检查接收者权限。
#include "sema/body/checker.hpp"
#include "sema/sema.hpp"
#include <array>
#include <cassert>
#include <utility>

namespace rx {

// 判断某个候选接收者类型是否提供名为 name 的方法，并返回其签名。
// 先查结构体声明里的方法（接收者类型必须与候选完全一致），
// 再查内置方法；clone 还要求类型实现 Clone。
std::optional<CallableSignature>
BodyChecker::lookup_method_signature(const SemanticType &candidate,
                                     const std::string &name) {
    // 剥掉一层引用，找到声明方法所属的结构体类型。
    const SemanticType *owner = &candidate;
    if (owner->kind == SemanticType::Kind::Reference && owner->inner)
        owner = owner->inner.get();
    if (owner->kind == SemanticType::Kind::Struct && owner->struct_symbol) {
        const auto &items = owner->struct_symbol->associated_items;
        const auto item = items.find(name);
        if (item != items.end() && item->second->kind == Symbol::Kind::Function) {
            const auto &function = static_cast<const FunctionSymbol &>(*item->second);
            // 声明的接收者必须与候选类型完全相同，这里不做额外调整。
            if (function.receiver && function.receiver->type.same_type(candidate))
                return function_signature(function);
        }
    }
    // 内置方法只按引用接收者提供，先排除值类型候选。
    if (candidate.kind != SemanticType::Kind::Reference || !candidate.inner)
        return std::nullopt;
    auto signature = builtin_method_signature(*candidate.inner, name);
    // 内置签名可能声明 &mut self，必须与当前候选的引用形式一致。
    if (!signature || !signature->receiver->same_type(candidate))
        return std::nullopt;
    // clone 的接收者规则之外，还要求可派生的 Clone 能力。
    if (name == "clone" && !analyzer_.has_trait(*candidate.inner, Derive::Clone))
        return std::nullopt;
    return signature;
}

// 按候选顺序解析方法：从接收者类型开始，每层依次尝试 T、&T、&mut T，
// 未命中就自动解一层引用或 Box 继续。这里只决定选中哪个方法和后续的
// 调整路径，不检查权限与实参，保证选择不受它们影响。
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
            // 命中后记录解引用层数和借用方式，供接收者权限检查复用。
            if (auto signature = lookup_method_signature(candidate, name))
                return ResolvedMethod{std::move(*signature), {dereferences, borrow}};
        }
        // 只有引用和 Box 能继续向内解，其他类型到此为止。
        if (layer->kind != SemanticType::Kind::Reference &&
            layer->kind != SemanticType::Kind::Box)
            break;
        layer = layer->inner.get();
    }
    // 所有层都没有匹配的方法。
    return std::nullopt;
}

// 点方法调用 value.f(...)
// 按已选中的调整路径检查接收者权限：先执行相应次数的解引用，
// 再确认需要可变接收者时目标 place 可写且没有跨过共享访问。
bool BodyChecker::check_method_receiver(const ExpressionResult &receiver,
                                        const ResolvedMethod &method, Span at) {
    assert(method.signature.receiver);
    ExpressionResult reached = receiver;
    // 沿保存的路径逐层解引用；任何一层失败都终止权限检查。
    for (std::size_t step = 0; step < method.receiver.dereferences; ++step) {
        reached = dereference_place(reached.type, reached.place, at);
        if (reached.type.is_error())
            return false;
    }
    const auto &declared = *method.signature.receiver;
    using Borrow = ReceiverAdjustment::Borrow;
    // 接收者本身已是 &mut U、方法也声明 &mut self 时，候选按 None 命中，
    // 但仍要解一层验证重借用权限。
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
        // 值表达式先物化为临时 place；已有 place 保留原权限。
        const auto place = materialize_place(reached.place);
        // 可变接收者要求 place 可写，且不能来自共享引用之后。
        if (!place.is_mutable || place.crossed_shared) {
            diagnostics_.error(
                at, "cannot mutably borrow an immutable or shared place");
            return false;
        }
    }
    return true;
}

// 检查点方法调用：先检查接收者，再解析方法、核对接收者权限，
// 最后按签名检查实参。方法调用不接收外层期望类型；
// 只有接收者类型和方法名参与选择，实参不参与。
ExpressionResult BodyChecker::check_method_call(const MethodCallExpression &call) {
    if (!call.receiver)
        return ExpressionResult::error();
    // 接收者独立定型，外层期望不向它传播。
    const auto receiver = check_expression(*call.receiver, nullptr);
    // 方法名不支持类型实参。
    if (!call.method.generics.empty()) {
        diagnostics_.error(call.span,
                           "methods have no type parameters: " + call.method.name);
        return ExpressionResult::error();
    }
    if (receiver.type.is_error())
        return ExpressionResult::error();
    // 只用接收者类型和方法名选目标；权限与实参稍后再核对。
    const auto method = resolve_method(receiver.type, call.method.name);
    if (!method) {
        diagnostics_.error(call.span, "no method named " + call.method.name +
                                         " for type " + receiver.type.toString());
        return ExpressionResult::error();
    }
    // 接收者和实参都检查，不因前者失败跳过后者，避免丢失诊断。
    const bool receiver_ok =
        check_method_receiver(receiver, *method, call.receiver->span);
    const bool arguments_ok =
        check_arguments(method->signature.parameters, call.arguments, call.span);
    if (!receiver_ok || !arguments_ok)
        return ExpressionResult::error();
    return ExpressionResult::value(method->signature.return_type);
}

} // namespace rx
