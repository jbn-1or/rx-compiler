#pragma once
#include "ast/ast.hpp"
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace rx {

// 一个结构体字段的声明。
// 存什么：字段名 + 指向 AST 类型节点的指针 + 源码位置；Phase 1 不解析类型。
// `struct Point { x: i32 }` → {name="x", type=指向 i32 的 PathType, span=该字段区间}
struct FieldDecl {
    std::string name;           // 字段名，"x"
    const Type *type = nullptr; // 字段的语法类型节点（非拥有），`items: Vec<S>` 时指向 Vec<S>
    Span span{};
};

// func_const_namespace 里的符号基类：函数、常量、内置 I/O 都用这条记录。
// 存：名字 + 类别 + 声明位置；子类再补充各自的 AST 节点。
// `fn main() {}` → {kind=Fn, name="main"}；内置 `get_i32` → {kind=Builtin, name="get_i32"}
struct Symbol {
    // 符号类别，用户函数 → Fn；常量 → Const；内置 I/O → Builtin
    enum class Kind { Fn, Const, Builtin };
    Kind kind = Kind::Builtin; // 当前类别，main 是 Kind::Fn
    std::string name;     // 值名（函数名/常量名/内置函数名），"println_i32"
    Span span{};
};

struct StructSymbol;

// 一个函数或方法的档案（impl 内带/不带 self 的都算）。
// Symbol 部分 + 指向 AST Function 节点 + 所属结构体。
// `fn add(a: i32) -> i32` → {name="add", node=Function(add), owner=nullptr}
// `impl S { fn get(&self) -> i32 }` → {name="get", node=Function(get), owner=S}
struct FnSymbol : Symbol {
    const Function *node = nullptr; // 指向 AST 函数节点（非拥有），可取 node->params、node->ret
    StructSymbol *owner = nullptr;  // 所属结构体；顶层函数为 nullptr，impl S 内方法指向 S
};

// 一个常量的档案（顶层 const 或 impl 内的关联 const）。
// Symbol 部分 + 指向 AST Const 节点 + 所属结构体。
// `const MAX: i32 = 10;` → {name="MAX", node=Const(MAX), owner=nullptr}
//     `impl S { const K: i32 = 1; }` → {name="K", owner=S}
struct ConstSymbol : Symbol {
    const Const *node = nullptr;   // 指向 AST 常量节点（非拥有），可取 node->type、node->value
    StructSymbol *owner = nullptr;
};

// 一个结构体的档案：后续按类型名查结构体、挂 impl 成员都靠它。
// 名字 + AST 节点 + 字段列表 + derive 集合 + impl 关联项表。
// `struct Point { x: i32, y: i32 }`
//     → {name="Point", fields={{x,i32},{y,i32}}, derives={}, assoc_items={}}
//   再配 `impl Point { fn len(&self) -> i32 }`
//     → assoc_items={"len" → FnSymbol(len)}
struct StructSymbol {
    std::string name;             // 结构体名
    Span span{};
    const Struct *node = nullptr; 
    std::vector<FieldDecl> fields; // 按声明顺序的字段，Point → [{x,i32},{y,i32}]
    std::set<Derive> derives{};    // 合并去重后的 derive 请求，`#[derive(Copy, Clone)]` → {Copy, Clone}
    // impl 里的成员：名字 → FnSymbol/ConstSymbol（非拥有），"len" → FnSymbol(len)
    std::unordered_map<std::string, Symbol *> assoc_items;
};

} // namespace rx
