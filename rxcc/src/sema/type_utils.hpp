// 提供基本类型名、整数字面量后缀到 SemanticType 的转换工具。
// 由类型解析、常量求值和函数体中的字面量/路径检查共用。
// 函数不依赖分析器状态，无法识别的输入返回 Error，由调用方决定如何处理。

#pragma once

#include "ast/ast.hpp"
#include "sema/type.hpp"
#include <string>

namespace rx {

// AST 后缀/基本类型名到语义类型的无状态转换，供常量、类型和表达式检查复用。
// 输入：整数字面量的后缀 IntSuffix。
// 输出：对应的 SemanticType 整数类型；没有后缀时返回 Error。
// 作用：把语法后缀（如 `1u32` 的 u32）转成语义类型。
// 例：IntSuffix::U32 → u32；IntSuffix::None → Error。
inline SemanticType type_from_suffix(IntSuffix suffix) {
    switch (suffix) {
        case IntSuffix::I32:
            return SemanticType::int_(SemanticType::IntKind::I32);
        case IntSuffix::U32:
            return SemanticType::int_(SemanticType::IntKind::U32);
        case IntSuffix::Isize:
            return SemanticType::int_(SemanticType::IntKind::Isize);
        case IntSuffix::Usize:
            return SemanticType::int_(SemanticType::IntKind::Usize);
        case IntSuffix::None:
            break;
    }
    return SemanticType::error();
}

// 在类型解析和常量路径中识别 i32/u32/isize/usize/bool。输出对应的 SemanticType
// 例："usize" → SemanticType::int_(Usize)；"S" 或 "Box" → Error。
inline SemanticType basic_type_from_name(const std::string &name) {
    if (name == "i32")
        return SemanticType::int_(SemanticType::IntKind::I32);
    if (name == "u32")
        return SemanticType::int_(SemanticType::IntKind::U32);
    if (name == "isize")
        return SemanticType::int_(SemanticType::IntKind::Isize);
    if (name == "usize")
        return SemanticType::int_(SemanticType::IntKind::Usize);
    if (name == "bool")
        return SemanticType::bool_();
    return SemanticType::error();
}

} // namespace rx
