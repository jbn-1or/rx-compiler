// 提供内置 I/O 与容器构造器的固定调用签名。
// 这里只描述参数和返回类型，不解析调用路径，也不检查实参。

#include "sema/call.hpp"

namespace rx {

// 返回内置 I/O 的固定签名；未知名字返回 nullopt。
// get_i32() -> i32；print_i32(i32) -> ()；println_i32(i32) -> ()。
std::optional<CallableSignature>
builtin_signature(const std::string &name) {
    if (name == "get_i32") {
        return CallableSignature{
            {},
            SemanticType::int_(SemanticType::IntKind::I32),
        };
    }
    if (name == "print_i32" || name == "println_i32") {
        return CallableSignature{
            {SemanticType::int_(SemanticType::IntKind::I32)},
            SemanticType::unit(),
        };
    }
    return std::nullopt;
}

// 5.2：容器构造器的签名。
// Box::<T>::new 恰好接收一个 T；Vec::<T>::new 不接收普通实参。
// T 已由 5.1 从路径上的具体类型实参归一化，不能从外层期望反推。
std::optional<CallableSignature>
container_constructor_signature(const ResolvedCallee &target) {
    if (target.kind != ResolvedCallee::Kind::ContainerConstructor ||
        !target.container.inner)
        return std::nullopt;

    const SemanticType &container = target.container;
    switch (container.kind) {
        case SemanticType::Kind::Box:
            return CallableSignature{{*container.inner}, container};
        case SemanticType::Kind::Vec:
            return CallableSignature{{}, container};
        default:
            return std::nullopt;
    }
}

// 5.3：Vec 显式方法的签名。
// 接收者与其他参数一样走普通一对一转换，不在这里自动借用。
std::optional<CallableSignature>
container_method_signature(const ResolvedCallee &target) {
    if (target.kind != ResolvedCallee::Kind::ContainerMethod ||
        target.container.kind != SemanticType::Kind::Vec ||
        !target.container.inner)
        return std::nullopt;

    const SemanticType &vector = target.container;
    const SemanticType shared_vector = SemanticType::reference(false, vector);
    const SemanticType mutable_vector = SemanticType::reference(true, vector);
    const SemanticType element = *vector.inner;

    if (target.member == "len")
        return CallableSignature{{shared_vector},
                                 SemanticType::int_(
                                     SemanticType::IntKind::Usize)};
    if (target.member == "is_empty")
        return CallableSignature{{shared_vector}, SemanticType::bool_()};
    if (target.member == "push")
        return CallableSignature{{mutable_vector, element},
                                 SemanticType::unit()};
    if (target.member == "remove")
        return CallableSignature{{mutable_vector,
                                  SemanticType::int_(
                                      SemanticType::IntKind::Usize)},
                                 element};
    return std::nullopt;
}

// 5.3：内置/派生 Clone 的显式调用签名。
// U 的 Clone 能力由调用方先通过 has_trait 检查；这里只建立 &U -> U。
std::optional<CallableSignature>
builtin_clone_signature(const ResolvedCallee &target) {
    if (target.kind != ResolvedCallee::Kind::BuiltinClone ||
        target.clone_type.is_error())
        return std::nullopt;
    return CallableSignature{
        {SemanticType::reference(false, target.clone_type)},
        target.clone_type};
}

} // namespace rx
