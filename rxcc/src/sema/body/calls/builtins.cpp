// 内置签名不执行名称解析或表达式检查，显式调用与点调用共用这里的规则。
#include "sema/body/calls/call.hpp"

namespace rx {

std::optional<CallableSignature> builtin_signature(const std::string &name) {
    const auto i32 = SemanticType::int_(SemanticType::IntKind::I32);
    if (name == "get_i32")
        return CallableSignature{{}, i32};
    if (name == "print_i32" || name == "println_i32")
        return CallableSignature{{i32}, SemanticType::unit()};
    return std::nullopt;
}

std::optional<CallableSignature>
container_constructor_signature(const SemanticType &container) {
    if (!container.inner)
        return std::nullopt;
    if (container.kind == SemanticType::Kind::Box)
        return CallableSignature{{*container.inner}, container};
    if (container.kind == SemanticType::Kind::Vec)
        return CallableSignature{{}, container};
    return std::nullopt;
}

std::optional<CallableSignature>
builtin_method_signature(const SemanticType &owner, const std::string &name) {
    const auto usize = SemanticType::int_(SemanticType::IntKind::Usize);
    const auto shared = SemanticType::reference(false, owner);
    if (name == "clone")
        return CallableSignature{{}, owner, shared};
    if (name == "len" && (owner.kind == SemanticType::Kind::Array ||
                          owner.kind == SemanticType::Kind::Vec))
        return CallableSignature{{}, usize, shared};
    if (owner.kind != SemanticType::Kind::Vec || !owner.inner)
        return std::nullopt;
    if (name == "is_empty")
        return CallableSignature{{}, SemanticType::bool_(), shared};
    const auto mutable_receiver = SemanticType::reference(true, owner);
    if (name == "push")
        return CallableSignature{
            {*owner.inner}, SemanticType::unit(), mutable_receiver};
    if (name == "remove")
        return CallableSignature{{usize}, *owner.inner, mutable_receiver};
    return std::nullopt;
}

} // namespace rx
