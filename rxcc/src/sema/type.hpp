// 定义统一的语义类型表示，提供类型构造、判等及诊断用的字符串输出。
// 保存已解析的结构体身份、引用可变性、容器元素类型和数组长度。
// 包含 unit、never 和错误恢复类型；AST 类型的解析由 declarations/resolve_type.cpp 负责。

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace rx {

// 结构体符号
// 这里前向声明：SemanticType 以指针引用它
struct StructSymbol;

struct SemanticType;

// 子类型（引用/数组元素/Box/Vec 内层）共享且不可变，用 shared_ptr<const> 承载，
// 递归类型（如 struct Node { next: Box<Node> }）也能安全表示。
using SemanticTypePointer = std::shared_ptr<const SemanticType>;

// 表示 AST 的 rx::Type 经解析后的语义类型，供判等、转换和能力查询使用。
// 与 AST 类型的关键差别：
//   - `Self` 在 struct/impl 内先展开为 Struct(S)，故 `Self` 与 `S` 天然相等；
//   - Array 的长度已常量求值成 uint64_t；COUNT 为 3 时，`[i32; COUNT]`
//     与 `[i32; 3]` 相等；
//   - 生命周期不进入 Reference 的判等（只比较 is_mut 与内层类型）。
// 例：`i32` → Int(I32)；`&mut S` → Reference(is_mut=true, Struct(S))；`[u32; 3]` →
// Array(Int(U32), 3)。
struct SemanticType {
    enum class Kind {
        Unit,   // ()，函数/控制流结果
        Never,  // !，发散表达式（return/break/continue/loop 无 break）
        Error,  // 已诊断错误；恢复入口跳过后续兼容性诊断
        Int,    // 四种整数之一，具体见 int_kind
        Bool,   // bool
        Struct, // 用户定义结构体（名义类型）
        Reference, // &T / &mut T，可变性见 is_mut
        Array,  // [T; N]，N 已求值为 length（uint64_t）
        Box,    // Box<T>
        Vec,    // Vec<T>
    };

    // 四种整数类型互不相同：i32 ≠ isize、u32 ≠ usize。
    enum class IntKind { I32, U32, Isize, Usize };

    Kind kind = Kind::Error;
    IntKind int_kind = IntKind::I32;          // 仅 Kind::Int 有意义，例：I32
    // 仅 Kind::Struct 有意义，例：S
    const StructSymbol *struct_symbol = nullptr;
    bool is_mut = false; // 仅 Kind::Reference 有意义，例：`&mut T` 时 = true
    // Reference/Box/Vec 的内层；Array 的元素；例：&S 时 = S
    SemanticTypePointer inner;
    uint64_t length = 0; // 仅 Kind::Array 有意义，例：`[i32; 3]` 时 = 3

    // 构造辅助函数：同时设置 kind 和对应字段。
    // 例：int_(IntKind::U32) → `u32`；reference(false, int_(IntKind::I32)) → `&i32`。
    static SemanticType unit() {
        SemanticType t;
        t.kind = Kind::Unit;
        return t;
    }
    static SemanticType never() {
        SemanticType t;
        t.kind = Kind::Never;
        return t;
    }
    static SemanticType error() {
        SemanticType t;
        t.kind = Kind::Error;
        return t;
    }
    static SemanticType int_(IntKind k) {
        SemanticType t;
        t.kind = Kind::Int;
        t.int_kind = k;
        return t;
    }
    static SemanticType bool_() {
        SemanticType t;
        t.kind = Kind::Bool;
        return t;
    }
    static SemanticType struct_(const StructSymbol *s) {
        SemanticType t;
        t.kind = Kind::Struct;
        t.struct_symbol = s;
        return t;
    }
    static SemanticType reference(bool is_mut, const SemanticType &inner) {
        SemanticType t;
        t.kind = Kind::Reference;
        t.is_mut = is_mut;
        t.inner = std::make_shared<const SemanticType>(inner);
        return t;
    }
    static SemanticType array(const SemanticType &element, uint64_t length) {
        SemanticType t;
        t.kind = Kind::Array;
        t.inner = std::make_shared<const SemanticType>(element);
        t.length = length;
        return t;
    }
    static SemanticType boxed(const SemanticType &inner) {
        SemanticType t;
        t.kind = Kind::Box;
        t.inner = std::make_shared<const SemanticType>(inner);
        return t;
    }
    static SemanticType vec(const SemanticType &inner) {
        SemanticType t;
        t.kind = Kind::Vec;
        t.inner = std::make_shared<const SemanticType>(inner);
        return t;
    }

    // —— 便捷判定 ——
    bool is_error() const { return kind == Kind::Error; }
    bool is_never() const { return kind == Kind::Never; }
    bool is_unit() const { return kind == Kind::Unit; }
    bool is_int() const { return kind == Kind::Int; }

    // 语义类型判等。
    //   - 四种整数按 int_kind 区分；Bool/Unit/Never 各自只等于自身；
    //   - Struct 名义判等：比较 StructSymbol 指针（同形状不同名判不等）；
    //   - Reference 比较 is_mut + 内层，忽略生命周期；
    //   - Array 比较元素类型 + 求值后的 length 值；
    //   - Box/Vec 比较内层。
    // Error 仅与 Error 相等；错误恢复由转换检查等调用入口处理。
    bool same_type(const SemanticType &other) const {
        if (kind != other.kind)
            return false;
        switch (kind) {
            case Kind::Unit:
            case Kind::Never:
            case Kind::Bool:
                return true;
            case Kind::Int:
                return int_kind == other.int_kind;
            case Kind::Struct:
                return struct_symbol == other.struct_symbol;
            case Kind::Reference:
                return is_mut == other.is_mut &&
                       inner_equals(inner, other.inner);
            case Kind::Array:
                return length == other.length && inner_equals(inner, other.inner);
            case Kind::Box:
            case Kind::Vec:
                return inner_equals(inner, other.inner);
            case Kind::Error:
                return true;
        }
        return false;
    }

    // 诊断名称递归保留结构体名，例如 &mut Point、Vec<Box<Node>>。
    std::string toString() const;

private:
    // 递归比较两个子类型指针：
    // 都为空视为相等，仅一侧为空则不等；两侧非空时递归比较内层类型。
    static bool inner_equals(const SemanticTypePointer &a,
                             const SemanticTypePointer &b) {
        if (!a || !b)
            return a == b;
        return a->same_type(*b);
    }

};

} // namespace rx
