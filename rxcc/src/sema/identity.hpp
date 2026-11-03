// 定义结构体、函数、常量和局部绑定的编号，以及内置输入输出的种类。
// 语义检查和后续阶段用这些编号找到同一份声明或绑定。

#pragma once

#include <compare>
#include <cstddef>
#include <limits>

namespace rx {

// 编号对应一份 SemanticResult 中的表下标；不同类型的编号不能混用。
// 默认编号不指向任何有效下标，避免未赋值时误指向第一个元素。
template <class Tag> struct SemanticId {
    std::size_t index = std::numeric_limits<std::size_t>::max();
    auto operator<=>(const SemanticId &) const = default;
};

using StructId = SemanticId<struct StructIdentity>;
using FunctionId = SemanticId<struct FunctionIdentity>;
using ConstantId = SemanticId<struct ConstantIdentity>;
using LocalId = SemanticId<struct LocalIdentity>;

enum class BuiltinIO { GetI32, PrintI32, PrintlnI32 };

} // namespace rx
