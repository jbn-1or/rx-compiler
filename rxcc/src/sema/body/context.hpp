// 函数体检查的临时状态；可持久化的表达式结果与绑定定义在 facts.hpp。

#pragma once

#include "sema/facts.hpp"
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

namespace rx {

struct BreakExpression;

// 单个块的名字表；内层块通过作用域栈覆盖外层同名绑定。
using Scope = std::unordered_map<std::string, Binding>;

// 一条 break 的值检查结果；循环结束后据此记录最终结果边的转换路径。
struct BreakValueRecord {
    const BreakExpression *expression = nullptr; // 这条 break 的 AST 位置。
    ExpressionResult result; // 操作数的类型、place 权限和发散事实。
};

// 一个循环帧：while、loop 或 while 条件屏障。
// loop 帧保存外层期望类型和直接指向它的 break 操作数检查结果。
struct LoopFrame {
    enum class Kind { While, Loop, WhileConditionBarrier };

    Kind kind = Kind::While;
    std::optional<LoopId> id; // 当前循环的身份；条件屏障没有对应循环。
    std::optional<SemanticType> expected;
    std::vector<BreakValueRecord> break_values;
};

} // namespace rx
