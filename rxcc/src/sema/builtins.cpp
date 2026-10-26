// 提供内置 I/O 的固定调用签名。
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

} // namespace rx
