#pragma once

#include "ast/ast.hpp"
#include "diagnostics.hpp"
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "sema/type.hpp"
#include "sema/symbol.hpp"

namespace rx {

// 访问一个 place 时的路径信息：是否真的是 place、能否写、路径上是否经过共享引用。
// 例：`let p = &mut x; *p = 1;` 中 `p` 是 place 但绑定本身不可重新赋值，
//     而 `*p` 是可写 place。
struct PlaceAccessInformation {
    bool is_place = false;       // 表达式本身是否指向存储位置
    bool is_mutable = false;     // 该位置当前是否可写
    bool crossed_shared = false; // 访问路径是否经过共享引用或不可变 Vec 索引
};

// 一次表达式检查的结果：表达式类型 + place 访问信息。
// 例：`p`（p: &mut i32）→ {type=&mut i32, place={true,false,false}}；
//     `*p` → {type=i32, place={true,true,false}}。
struct ExpressionResult {
    SemanticType type;
    PlaceAccessInformation place;
};

// SEMANTIC 阶段分析器。
// 遍历 Crate，建立符号表并做语义检查；错误写进构造时传入的诊断收集器。
// 例：输入 `struct S { x: i32 } fn main() {}`
//     → type_namespace 里有 "S"，function_constant_namespace 里有 "main"（以及三个内置 I/O 名）。
class SemanticAnalyzer {
public:
    // 构造：接入外部诊断收集器。
    // 输入：diagnostics（main.cpp 创建，整个阶段共用）
    explicit SemanticAnalyzer(Diagnostics &diagnostics)
        : diagnostics_(diagnostics) {}

    // 对整个 crate 做语义检查。
    void run(const Crate &crate);

private:
    Diagnostics &diagnostics_; // 诊断收集器，例：N2 重名时 → diagnostics_.error(span, "duplicate struct")

    // 名字表
    // 结构体名 → 档案，例："Point" → Point 的 StructSymbol
    std::unordered_map<std::string, StructSymbol *> type_namespace;
    // 值名 → 档案，例："main" → FunctionSymbol；"MAX" → ConstantSymbol；"get_i32" → Builtin Symbol
    std::unordered_map<std::string, Symbol *> function_constant_namespace;

    // —— 所有权：表里只存裸指针，这些容器保证对象在分析期间不被释放 ——
    // 所有结构体档案，例：每个 struct 一个元素
    std::vector<std::unique_ptr<StructSymbol>> structs_;
    // 所有函数/方法档案，例：main、S::get 各一个
    std::vector<std::unique_ptr<FunctionSymbol>> functions_;
    // 所有常量/关联常量档案，例：MAX、S::K 各一个
    std::vector<std::unique_ptr<ConstantSymbol>> constants_;
    // 内置 I/O 档案，例：get_i32/print_i32/println_i32 三条
    std::vector<std::unique_ptr<Symbol>> builtins_;

    void initialize_builtins();
    void collect_struct(const Struct &s);
    void collect_const(const Const &c);
    void collect_function(const Function &f);
    void check_parameter_names(const std::vector<Parameter> &parameters,
                               Span fallback);
    StructSymbol *resolve_impl_target(const Type &t);
    void collect_impl(const InherentImpl &impl);
    void check_main_declaration();
    // 把字段、参数、返回类型从 AST 定型为 SemanticType。
    void resolve_declared_types();
    void resolve_function_signature(FunctionSymbol &fn, StructSymbol *owner);
    // 入口检查的最后一块：main 的返回类型必须是 unit（E4）。
    void check_main_return_type();
    // 按字段类型检测结构体是否存在按值内联的无限布局环（T5）。
    enum class LayoutVisitState { Unvisited, Visiting, Visited };
    using LayoutVisitStates = std::map<const StructSymbol *, LayoutVisitState>;
    void check_inline_layout_cycles();
    void visit_layout(const StructSymbol *sym, LayoutVisitStates &states);
    void visit_layout_type(const SemanticType &type, Span at,
                           LayoutVisitStates &states);
    // 判断类型是否具备指定 derive 能力（D6/D7）；visiting 记录递归假设。
    bool has_trait(
        const SemanticType &type, Derive trait,
        std::set<std::pair<const StructSymbol *, Derive>> &visiting) const;
    // 检查结构体已请求的 derive 是否合法（D2–D5）。
    void check_derives();
    void check_one_derive(StructSymbol &sym, Derive trait);
    // 函数体检查的统一入口。expected == nullptr 表示没有期望类型；
    // 返回表达式按自身规则得到的类型与 place，不在这里做外层强制转换。
    ExpressionResult check_expression(const Expression &expression,
                                      const SemanticType *expected);
    ExpressionResult check_block(const BlockExpression &block,
                                 const SemanticType *expected);
    // 4.2：place 物化与一次内置解引用。
    PlaceAccessInformation
    materialize_place(const PlaceAccessInformation &place) const;
    ExpressionResult dereference_place(const SemanticType &base_type,
                                       const PlaceAccessInformation &base_place,
                                       Span at);
    // 4.2：一对一强制转换判定、固定目标报错与多分支 LUB。
    bool can_coerce(const SemanticType &from,
                    const PlaceAccessInformation &place,
                    const SemanticType &to) const;
    void require_coerce(const ExpressionResult &from, const SemanticType &to,
                        Span at);
    SemanticType compute_least_upper_bound(
        const std::vector<ExpressionResult> &results) const;
    // 扫一遍所有顶层/关联常量，逐个求值并落缓存。
    void evaluate_all_constants();
    // 求值不依赖名字的常量值（整数/布尔/负数）。
    EvaluatedConstant evaluate_constant_value(const ConstantValue &value,
                                              const SemanticType *expected,
                                              const StructSymbol *current);
    // 常量路径解析 + 三色递归求值。
    ConstantSymbol *resolve_constant_path(
        const std::vector<PathSegment> &path, Span span,
        const StructSymbol *current);
    EvaluatedConstant evaluate_constant(ConstantSymbol &sym);
    // 把语法类型归一化为 SemanticType：基本类型/Unit/引用/数组/Box/Vec/Self，
    // 数组长度在这里按 usize 常量求值。
    SemanticType resolve_type(const Type &type, const StructSymbol *current);
};

} // namespace rx
