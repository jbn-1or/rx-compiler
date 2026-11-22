// 声明 lowering 共用的源类型分类和普通值 IR 类型映射。
#pragma once

#include "ir/model.hpp"
#include "sema/type.hpp"

namespace rx {

// 判断源类型是否是当前 lowering 支持的标量（整数或 bool）。
bool is_scalar(const SemanticType &type);

// 判断源类型是否是引用；引用只保存目标地址，不保存目标内容。
bool is_reference(const SemanticType &type);

// 判断源类型是否是当前 lowering 支持的普通值：标量，或指向受支持值的引用。
bool is_supported_value(const SemanticType &type);

// 计算标量值使用的 IR 类型：bool 为 i1，整数为 i32。
ir::Type lower_scalar_type(const SemanticType &type);

// 计算普通值使用的 IR 类型：标量沿用标量映射，引用统一为 ptr。
ir::Type lower_value_type(const SemanticType &type);

} // namespace rx
