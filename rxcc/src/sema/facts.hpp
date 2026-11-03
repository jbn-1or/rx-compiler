// 定义语义检查需要保存的表达式结果、访问权限和局部绑定信息。
// 这些信息既供函数体检查使用，也随语义结果交给后续阶段。

#pragma once

#include "sema/identity.hpp"
#include "sema/type.hpp"
#include <utility>

namespace rx {

// 表达式是否表示存储位置、能否写入，以及访问时是否受只读限制。
struct PlaceAccessInformation {
    bool is_place = false;       // 是否表示存储位置
    bool is_mutable = false;     // 该位置是否可写
    bool crossed_shared = false; // 是否经过共享引用或不可写的 Vec 索引
};

// 表达式自身的类型和访问权限，不包含使用它时可能发生的类型调整。
struct ExpressionResult {
    SemanticType type;
    PlaceAccessInformation place;
    // 无法正常执行结束不一定意味着类型是 !，如 { loop {}; 42 } 的类型是 i32。
    bool diverges = false;

    static ExpressionResult value(SemanticType type, bool diverges = false) {
        diverges = diverges || type.is_never();
        return {std::move(type), {}, diverges};
    }
    static ExpressionResult error() {
        return value(SemanticType::error());
    }
    static ExpressionResult from_place(SemanticType type,
                                       PlaceAccessInformation access,
                                       bool diverges = false) {
        diverges = diverges || type.is_never();
        return {std::move(type), access, diverges};
    }
};

// 局部绑定的固定类型、能否重新赋值，以及区分同名绑定的编号。
struct Binding {
    SemanticType type;
    bool is_mut = false;
    LocalId id;
};

} // namespace rx
