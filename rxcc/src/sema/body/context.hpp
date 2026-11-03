// 函数体检查的临时状态；可持久化的表达式结果与绑定定义在 facts.hpp。

#pragma once

#include "sema/facts.hpp"
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

namespace rx {

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
