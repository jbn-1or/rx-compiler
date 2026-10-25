// 定义表达式检查结果，统一保存语义类型与存储位置的访问信息。
// 访问信息包括是否为 place、是否可写，以及访问路径是否经过共享引用或受限的 Vec 索引。
// 供表达式、place 和类型转换检查共同使用。

#pragma once

#include "sema/type.hpp"

namespace rx {

// 访问一个 place 时的路径信息：是否真的是 place、能否写、路径上是否经过共享引用。
// 例：`let p = &mut x; *p = 1;` 中 `p` 是 place 但绑定本身不可重新赋值，
//     而 `*p` 是可写 place。
struct PlaceAccessInformation {
    bool is_place = false;       // 表达式本身是否指向存储位置
    bool is_mutable = false;     // 该位置当前是否可写
    bool crossed_shared = false; // 访问路径是否经过共享引用或不可变 Vec 索引
};

// 一次表达式检查的结果：表达式类型 + place 访问信息。
// 例：`p`（p: &mut i32）→ {type=&mut i32, place={true,false,false}}；
//     `*p` → {type=i32, place={true,true,false}}。
struct ExpressionResult {
    SemanticType type;
    PlaceAccessInformation place;
};

} // namespace rx
