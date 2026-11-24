// 声明 lowering 共用的源类型分类和普通值 IR 类型映射。
#pragma once

#include "ir/model.hpp"
#include "sema/type.hpp"
#include <string>

namespace rx {

// 判断源类型是否是当前 lowering 支持的标量（整数或 bool）。
bool is_scalar(const SemanticType &type);

// 判断源类型是否是引用；引用只保存目标地址，不保存目标内容。
bool is_reference(const SemanticType &type);

// 判断源类型是否是可按布局存放和搬运的聚合值：结构体、固定数组或 Vec 头。
bool is_aggregate_value(const SemanticType &type);
// 局部存储和函数签名共用：标量、结构体、数组、Vec 头，以及指向这些类型的引用链。
bool is_storable_value(const SemanticType &type);

// 计算普通值使用的 IR 类型：标量沿用标量映射，引用统一为 ptr。
ir::Type lower_value_type(const SemanticType &type);

// 生成完整具体类型的稳定键：结构体用 StructId，容器和数组保留子类型与长度。
// 供 clone/相等辅助函数按类型缓存，不依赖打印名称或内存地址。
std::string type_identity_key(const SemanticType &type);

} // namespace rx
