// 函数体检查共享的数据：表达式结果、place 权限、局部绑定与循环帧。

#pragma once

#include "sema/type.hpp"
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

namespace rx {

// 是否真的是 place、能否写、路径上是否经过共享引用。
// 例：`let p = &mut x; *p = 1;` 中 `p` 是 place 但绑定本身不可重新赋值，
//     而 `*p` 是可写 place。
struct PlaceAccessInformation {
    bool is_place = false;       // 表达式本身是否指向存储位置
    bool is_mutable = false;     // 该位置当前是否可写
    bool crossed_shared = false; // 访问路径是否经过共享引用或不可变 Vec 索引
};

// 一次表达式检查的结果：表达式类型、place 访问信息及是否发散。
// 例：`p`（p: &mut i32）→ {type=&mut i32, place={true,false,false}}；
//     `*p` → {type=i32, place={true,true,false}}。
struct ExpressionResult {
    SemanticType type;
    PlaceAccessInformation place;
    // 发散不等于类型为 !：{ loop {}; 42 } 的类型为 i32，但不会正常完成。
    bool diverges = false;

    static ExpressionResult value(SemanticType type, bool diverges = false) {
        diverges = diverges || type.is_never();
        return {std::move(type), {}, diverges};
    }
    // 只传播已经记录的错误；发现新问题的检查器必须先记录诊断。
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

// 一个局部绑定：定型后的类型 + 绑定本身是否允许重新赋值。
// 例：`let mut x: i32 = 1;` → {type=i32, is_mut=true}。
struct Binding {
    SemanticType type;
    bool is_mut = false;
};

// 单个块的名字表；内层块通过作用域栈覆盖外层同名绑定。
using Scope = std::unordered_map<std::string, Binding>;

// 一个循环帧：while、loop 或 while 条件屏障。
// loop 帧保存外层期望类型和直接指向它的 break 操作数检查结果。
struct LoopFrame {
    enum class Kind { While, Loop, WhileConditionBarrier };

    Kind kind = Kind::While;
    std::optional<SemanticType> expected;
    std::vector<ExpressionResult> break_values;
};

} // namespace rx
