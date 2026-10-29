// 内置函数、Box::new/Vec::new 构造器和内置方法的签名。
#include "sema/body/calls/call.hpp"

namespace rx {

// 返回内置自由函数的签名；名字不匹配时返回空，由调用方报未解析。
// get_i32 无参数、返回 i32；print_i32/println_i32 接收一个 i32、返回 ()。
std::optional<CallableSignature> builtin_signature(const std::string &name) {
    const auto i32 = SemanticType::int_(SemanticType::IntKind::I32);
    if (name == "get_i32")
        return CallableSignature{{}, i32};
    // 两个打印函数参数和返回值相同，只在运行时是否换行上不同。
    if (name == "print_i32" || name == "println_i32")
        return CallableSignature{{i32}, SemanticType::unit()};
    return std::nullopt;
}

// 返回容器构造器的签名：Box::new(value) 接收内部类型并返回容器，
// Vec::new() 无参数并返回空容器。其他类型没有内置 new。
std::optional<CallableSignature>
container_constructor_signature(const SemanticType &container) {
    // 不是容器（没有内部类型参数）时不提供构造器。
    if (!container.inner)
        return std::nullopt;
    if (container.kind == SemanticType::Kind::Box)
        return CallableSignature{{*container.inner}, container};
    // Vec 没有初始元素，参数表为空。
    if (container.kind == SemanticType::Kind::Vec)
        return CallableSignature{{}, container};
    return std::nullopt;
}

// 返回内置点方法的签名。owner 是接收者剥掉引用后的实际类型；
// clone/len/is_empty 借用 &self，push/remove 只对 Vec 提供并使用 &mut self。
// clone 的 Clone trait 前提不在这里检查，由查找方负责。
std::optional<CallableSignature>
builtin_method_signature(const SemanticType &owner, const std::string &name) {
    const auto usize = SemanticType::int_(SemanticType::IntKind::Usize);
    // 共享接收者：方法只读取 owner。
    const auto shared = SemanticType::reference(false, owner);
    if (name == "clone")
        return CallableSignature{{}, owner, shared};
    if (name == "len" && (owner.kind == SemanticType::Kind::Array ||
                          owner.kind == SemanticType::Kind::Vec))
        return CallableSignature{{}, usize, shared};
    // 以下方法只属于 Vec；其他类型或缺少元素类型时不再匹配。
    if (owner.kind != SemanticType::Kind::Vec || !owner.inner)
        return std::nullopt;
    if (name == "is_empty")
        return CallableSignature{{}, SemanticType::bool_(), shared};
    // push/remove 会修改容器，接收者从 &self 改为 &mut self。
    const auto mutable_receiver = SemanticType::reference(true, owner);
    if (name == "push")
        return CallableSignature{
            {*owner.inner}, SemanticType::unit(), mutable_receiver};
    if (name == "remove")
        return CallableSignature{{usize}, *owner.inner, mutable_receiver};
    return std::nullopt;
}

} // namespace rx
