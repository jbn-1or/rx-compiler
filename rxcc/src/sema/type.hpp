#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace rx {

// 结构体符号
// 这里前向声明：SemaType 以指针引用它
struct StructSymbol;

struct SemaType;

// 子类型（引用/数组元素/Box/Vec 内层）共享且不可变，用 shared_ptr<const> 承载，
// 递归类型（如 struct Node { next: Box<Node> }）也能安全表示。
using SemaTypePtr = std::shared_ptr<const SemaType>;

// 归一化语义类型：把 ast::Type（语法树）解析成可判等、可判能力的规范形式。
// 与 ast::Type 的关键差别（见实现计划 §B.0.2）：
//   - `Self` 在 struct/impl 内先展开为 Struct(S)，故 `Self` 与 `S` 天然相等；
//   - Array 的长度已常量求值成 uint64_t，故 `[i32; COUNT]` 与 `[i32; 3]` 按值相等；
//   - 生命周期不进入 Ref 的判等（只比较 is_mut 与内层类型）。
// 例：`i32` → Int(I32)；`&mut S` → Ref(is_mut=true, Struct(S))；`[u32; 3]` → Array(Int(U32), 3)。
struct SemaType {
    enum class Kind {
        Unit,   // ()，函数/控制流结果
        Never,  // !，发散表达式（return/break/continue/loop 无 break）
        Error,  // 错误恢复哨兵：可与任何类型兼容，防止一处失败级联成多报
        Int,    // 四种整数之一，具体见 int_kind
        Bool,   // bool
        Struct, // 用户定义结构体（名义类型）
        Ref,    // &T / &mut T，可变性见 is_mut
        Array,  // [T; N]，N 已求值为 len（uint64_t）
        Box,    // Box<T>
        Vec,    // Vec<T>
    };

    // 四种整数类型互不相同：i32 ≠ isize、u32 ≠ usize。
    enum class IntKind { I32, U32, Isize, Usize };

    Kind kind = Kind::Error;
    IntKind int_kind = IntKind::I32;          // 仅 Kind::Int 有意义，例：I32
    const StructSymbol *struct_sym = nullptr; // 仅 Kind::Struct 有意义，例：S
    bool is_mut = false;                      // 仅 Kind::Ref 有意义，例：`&mut T` 时 = true
    SemaTypePtr inner;                        // Ref/Box/Vec 的内层；Array 的元素；例：&S 时 = S
    uint64_t len = 0;                         // 仅 Kind::Array 有意义，例：`[i32; 3]` 时 = 3

    // —— 构造原语（替代裸露字段赋值，保证 kind 与字段一致）——
    // 例：int_(IntKind::U32) → `u32`；ref(false, int_(IntKind::I32)) → `&i32`。
    static SemaType unit() {
        SemaType t;
        t.kind = Kind::Unit;
        return t;
    }
    static SemaType never() {
        SemaType t;
        t.kind = Kind::Never;
        return t;
    }
    static SemaType error() {
        SemaType t;
        t.kind = Kind::Error;
        return t;
    }
    static SemaType int_(IntKind k) {
        SemaType t;
        t.kind = Kind::Int;
        t.int_kind = k;
        return t;
    }
    static SemaType bool_() {
        SemaType t;
        t.kind = Kind::Bool;
        return t;
    }
    static SemaType struct_(const StructSymbol *s) {
        SemaType t;
        t.kind = Kind::Struct;
        t.struct_sym = s;
        return t;
    }
    static SemaType ref(bool is_mut, const SemaType &inner) {
        SemaType t;
        t.kind = Kind::Ref;
        t.is_mut = is_mut;
        t.inner = std::make_shared<const SemaType>(inner);
        return t;
    }
    static SemaType array(const SemaType &elem, uint64_t len) {
        SemaType t;
        t.kind = Kind::Array;
        t.inner = std::make_shared<const SemaType>(elem);
        t.len = len;
        return t;
    }
    static SemaType boxed(const SemaType &inner) {
        SemaType t;
        t.kind = Kind::Box;
        t.inner = std::make_shared<const SemaType>(inner);
        return t;
    }
    static SemaType vec(const SemaType &inner) {
        SemaType t;
        t.kind = Kind::Vec;
        t.inner = std::make_shared<const SemaType>(inner);
        return t;
    }

    // —— 便捷判定 ——
    bool is_error() const { return kind == Kind::Error; }
    bool is_never() const { return kind == Kind::Never; }
    bool is_unit() const { return kind == Kind::Unit; }
    bool is_int() const { return kind == Kind::Int; }

    // 语义类型判等。
    // 规则（规范 §10 支持的类型）：
    //   - 四种整数按 int_kind 区分；Bool/Unit/Never 各自只等于自身；
    //   - Struct 名义判等：比较 StructSymbol 指针（同形状不同名判不等）；
    //   - Ref 比较 is_mut + 内层，忽略生命周期；
    //   - Array 比较元素类型 + 求值后的 len 值；
    //   - Box/Vec 比较内层。
    // 特例：任一侧为 Error 一律返回 true——Error 是错误恢复哨兵，用于在已报过
    // 错后静默放行后续检查，避免级联报错（见实现计划 §B.0.2 / §C.2）。
    bool equals(const SemaType &other) const {
        if (kind == Kind::Error || other.kind == Kind::Error)
            return true;
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
            return struct_sym == other.struct_sym;
        case Kind::Ref:
            return is_mut == other.is_mut && inner_equals(inner, other.inner);
        case Kind::Array:
            return len == other.len && inner_equals(inner, other.inner);
        case Kind::Box:
        case Kind::Vec:
            return inner_equals(inner, other.inner);
        case Kind::Error:
            return true; // 不可达（上面已处理），保留以覆盖全部枚举值
        }
        return false;
    }

    // 打印与报错用，例："i32"、"&mut S"、"[u32; 3]"、"Box<i32>"、"()"、"!"。
    // 注意：Struct 目前打印 "struct"（StructSymbol 尚未定义，无法取名字，
    // Phase 3 符号表落地后可改为打印其名字）。
    std::string toString() const {
        switch (kind) {
        case Kind::Unit:
            return "()";
        case Kind::Never:
            return "!";
        case Kind::Error:
            return "<error>";
        case Kind::Int:
            switch (int_kind) {
            case IntKind::I32:
                return "i32";
            case IntKind::U32:
                return "u32";
            case IntKind::Isize:
                return "isize";
            case IntKind::Usize:
                return "usize";
            }
            return "i32";
        case Kind::Bool:
            return "bool";
        case Kind::Struct:
            return "struct";
        case Kind::Ref:
            return (is_mut ? "&mut " : "&") + inner_str(inner);
        case Kind::Array:
            return "[" + inner_str(inner) + "; " + std::to_string(len) + "]";
        case Kind::Box:
            return "Box<" + inner_str(inner) + ">";
        case Kind::Vec:
            return "Vec<" + inner_str(inner) + ">";
        }
        return "<error>";
    }

private:
    // 递归比较两个子类型指针：都为空视为相等（如 Unit 无 inner），否则解引用比较。
    static bool inner_equals(const SemaTypePtr &a, const SemaTypePtr &b) {
        if (!a || !b)
            return a == b;
        return a->equals(*b);
    }
    static std::string inner_str(const SemaTypePtr &t) {
        return t ? t->toString() : std::string();
    }
};

// 实现计划 §B.0.2 书写的自由函数形式 `equals(a, b)`；等价于 a.equals(b)。
inline bool equals(const SemaType &a, const SemaType &b) { return a.equals(b); }

} // namespace rx
