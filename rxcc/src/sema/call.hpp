// 定义调用检查共享的数据：已解析目标的签名，以及内置 I/O 的签名查询。
// 参数类型按声明顺序保存；返回类型来自函数/内置声明，不因调用位置改变。
// 具体调用解析和实参检查在 call.cpp，内置签名表在 builtins.cpp。

#pragma once

#include "sema/type.hpp"
#include <optional>
#include <string>
#include <vector>

namespace rx {

// 一次调用可用的签名。
// 例：`fn f(a: i32) -> bool` → {parameters={i32}, return_type=bool}；
//     `println_i32` → {parameters={i32}, return_type=()}。
struct CallableSignature {
    std::vector<SemanticType> parameters;
    SemanticType return_type;
};

// 查询内置 I/O 的固定签名；不是内置 I/O 时返回 nullopt。
// 例："get_i32" → {parameters={}, return_type=i32}。
std::optional<CallableSignature>
builtin_signature(const std::string &name);

} // namespace rx
