#pragma once

#include "ast/ast.hpp"
#include "diagnostics.hpp"
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "sema/type.hpp"
#include "sema/symbol.hpp"

namespace rx {

// SEMANTIC 阶段分析器。
// 遍历 Crate，建立符号表并做语义检查；错误写进构造时传入的诊断收集器。
// 例：输入 `struct S { x: i32 } fn main() {}`
//     → type_namespace 里有 "S"，func_const_namespace 里有 "main"（以及三个内置 I/O 名）。
class SemaAnalyzer {
public:
    // 构造：接入外部诊断收集器。
    // 输入：diags（main.cpp 创建，整个阶段共用）
    explicit SemaAnalyzer(Diagnostics &diags) : diags_(diags) {}

    // 对整个 crate 做语义检查。
    void run(const Crate &crate);

private:
    Diagnostics &diags_; // 诊断收集器，例：N2 重名时 → diags_.error(span, "duplicate struct")

    // 名字表
    // 结构体名 → 档案，例："Point" → Point 的 StructSymbol
    std::unordered_map<std::string, StructSymbol *> type_namespace;
    // 值名 → 档案，例："main" → FnSymbol；"MAX" → ConstSymbol；"get_i32" → Builtin Symbol
    std::unordered_map<std::string, Symbol *> func_const_namespace;

    // —— 所有权：表里只存裸指针，这些容器保证对象在分析期间不被释放 ——
    // 所有结构体档案，例：每个 struct 一个元素
    std::vector<std::unique_ptr<StructSymbol>> structs_;
    // 所有函数/方法档案，例：main、S::get 各一个
    std::vector<std::unique_ptr<FnSymbol>> fns_;
    // 所有常量/关联常量档案，例：MAX、S::K 各一个
    std::vector<std::unique_ptr<ConstSymbol>> consts_;
    // 内置 I/O 档案，例：get_i32/print_i32/println_i32 三条
    std::vector<std::unique_ptr<Symbol>> builtins_;

    void init_builtins();
    void collect_struct(const Struct &s);
    void collect_const(const Const &c);
    void collect_function(const Function &f);
    void check_param_names(const std::vector<Param> &params, Span fallback);
    StructSymbol *resolve_impl_target(const Type &t);
    void collect_impl(const InherentImpl &impl);
    void check_main_declaration();
    // 把字段、参数、返回类型从 AST 定型为 SemaType。
    void resolve_declared_types();
    void resolve_function_signature(FnSymbol &fn, StructSymbol *owner);
    // 入口检查的最后一块：main 的返回类型必须是 unit（E4）。
    void check_main_return_type();
    // 按字段类型检测结构体是否存在按值内联的无限布局环（T5）。
    enum class LayoutVisitState { Unvisited, Visiting, Visited };
    using LayoutVisitStates = std::map<const StructSymbol *, LayoutVisitState>;
    void check_inline_layout_cycles();
    void visit_layout(const StructSymbol *sym, LayoutVisitStates &states);
    void visit_layout_type(const SemaType &ty, Span at,
                           LayoutVisitStates &states);
    // 扫一遍所有顶层/关联常量，逐个求值并落缓存。
    void evaluate_all_constants();
    // 求值不依赖名字的常量值（整数/布尔/负数）。
    EvaluatedConst evaluated_const_value(const ConstValue &value,
                                         const SemaType *expected,
                                         const StructSymbol *current);
    // 常量路径解析 + 三色递归求值。
    ConstSymbol *resolve_const_path(const std::vector<PathSegment> &path,
                                    Span span, const StructSymbol *current);
    EvaluatedConst evaluated_const(ConstSymbol &sym);
    // 把语法类型归一化为 SemaType：基本类型/Unit/引用/数组/Box/Vec/Self，
    // 数组长度在这里按 usize 常量求值。
    SemaType resolve_type(const Type &type, const StructSymbol *current);
};

} // namespace rx
