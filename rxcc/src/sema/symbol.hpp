// 定义结构体、函数、常量和内置项的符号数据，以及字段和定型后的签名信息。
// 常量符号额外保存求值状态与缓存，结构体符号保存字段、derive 和关联项索引。
// 符号由 SemanticAnalyzer 持有，各检查阶段共享同一份声明信息。

#pragma once
#include "ast/ast.hpp"
#include "sema/type.hpp"
#include <cstdint>
#include <set>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace rx {

// 一个结构体字段的声明。
// 保存字段名、指向 AST 类型节点的非拥有指针和诊断位置；声明收集时不解析类型。
// `struct Point { x: i32 }` → {name="x", type=指向 i32 的 PathType, span=i32 的区间}
struct FieldDeclaration {
    std::string name;           // 字段名，"x"
    const Type *type = nullptr; // 字段的语法类型节点（非拥有），`items: Vec<S>` 时指向 Vec<S>
    Span span{};
};

// function_constant_namespace 里的符号基类：函数、常量、内置 I/O 都用这条记录。
// 存：名字 + 类别 + 声明位置；子类再补充各自的 AST 节点。
// `fn main() {}` → {kind=Function, name="main"}；内置 `get_i32` → {kind=Builtin, name="get_i32"}
struct Symbol {
    // 符号类别，用户函数 → Function；常量 → Constant；内置 I/O → Builtin
    enum class Kind { Function, Constant, Builtin };
    Kind kind = Kind::Builtin; // 当前类别，main 是 Kind::Function
    std::string name; // 值名（函数名/常量名/内置函数名），"println_i32"
    Span span{};
};

struct StructSymbol;

// 常量求值结果：类型 + 值（有符号/无符号整数或 bool）+ 是否成功。
// 失败时 type 为 Error、value 为 monostate、ok 为 false，供上层传播失败并抑制连锁诊断。
// 例：`3u32` → {type=u32, value=uint64_t{3}, ok=true}；`true` → {type=bool, value=true, ok=true}
struct EvaluatedConstant {
    SemanticType type;
    std::variant<std::monostate, int64_t, uint64_t, bool> value;
    bool ok = false;
};

// ConstantSymbol 的三色求值状态
enum class ConstantEvaluationState { Unvisited, Visiting, Visited };

// 定型后的普通参数：语义类型 + 绑定可变性；对应 AST 的 Parameter。
// `mut x: i32` → {type=i32, is_mut=true}。
struct ResolvedParameter {
    SemanticType type;
    bool is_mut = false;
};

// 接收者在签名阶段定型；类型可变性与绑定可变性分别保存。
struct ResolvedReceiver {
    SemanticType type;
    bool binding_is_mut = false;
};

// 一个函数或方法的档案（impl 内带/不带 self 的都算）。
// Symbol 部分 + 指向 AST Function 节点 + 所属结构体。
// `fn add(a: i32) -> i32` → {name="add", node=Function(add), owner=nullptr}
// `impl S { fn get(&self) -> i32 }` → {name="get", node=Function(get), owner=S}
struct FunctionSymbol : Symbol {
    const Function *node = nullptr;    // 指向 AST 函数节点（非拥有），可取 node->parameters、node->return_type
    StructSymbol *owner = nullptr;     // 所属结构体；顶层函数为 nullptr，impl S 内方法指向 S
    std::vector<ResolvedParameter> parameters; // 定型后的参数，按声明顺序
    SemanticType return_type; // 返回类型；省略时为 Unit
    std::optional<ResolvedReceiver> receiver; // 无 self 时为空。
};

// 一个常量的档案（顶层 const 或 impl 内的关联 const）。
// Symbol 部分 + 指向 AST Const 节点 + 所属结构体。
// `const MAX: i32 = 10;` → {name="MAX", node=Const(MAX), owner=nullptr}
//     `impl S { const K: i32 = 1; }` → {name="K", owner=S}
struct ConstantSymbol : Symbol {
    const Const *node = nullptr;   // 指向 AST 常量节点（非拥有），可取 node->type、node->value
    StructSymbol *owner = nullptr;
    // 求值三色状态
    ConstantEvaluationState state = ConstantEvaluationState::Unvisited;
    EvaluatedConstant evaluated; // 求值缓存
};

// 一个结构体的档案：后续按类型名查结构体、挂 impl 成员都靠它。
// 名字 + AST 节点 + 字段列表 + derive 集合 + impl 关联项表。
// `struct Point { x: i32, y: i32 }`
//     → {name="Point", fields={{x,i32},{y,i32}}, derives={}, associated_items={}}
//   再配 `impl Point { fn len(&self) -> i32 }`
//     → associated_items={"len" → FunctionSymbol(len)}
struct StructSymbol {
    std::string name;             // 结构体名
    Span span{};
    const Struct *node = nullptr; // 保留声明节点，供后续语义结果和 IR 映射使用。
    std::vector<FieldDeclaration> fields; // 按声明顺序的字段，Point → [{x,i32},{y,i32}]
    std::unordered_map<std::string, SemanticType> field_types; // 字段名 → 定型后的语义类型
    std::set<Derive> derives{};    // 合并去重后的 derive ，`#[derive(Copy, Clone)]` → {Copy, Clone}
    // impl 里的成员：名字 → FunctionSymbol/ConstantSymbol（非拥有），"len" → FunctionSymbol(len)
    std::unordered_map<std::string, Symbol *> associated_items;
};

} // namespace rx
