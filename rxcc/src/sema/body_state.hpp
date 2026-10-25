// 定义函数体检查期间使用的局部绑定和作用域数据。
// 作用域按块嵌套保存，BodyChecker 负责压入和弹出；不保存全局符号所有权。

#pragma once

#include "sema/type.hpp"
#include <string>
#include <unordered_map>

namespace rx {

// 一个局部绑定：定型后的类型 + 绑定本身是否允许重新赋值。
// 例：`let mut x: i32 = 1;` → {type=i32, is_mut=true}。
struct Binding {
    SemanticType type;
    bool is_mut = false;
};

// 单个块的名字表；内层块通过作用域栈覆盖外层同名绑定。
using Scope = std::unordered_map<std::string, Binding>;

} // namespace rx
