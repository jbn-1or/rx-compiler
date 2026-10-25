// 声明语义分析器，持有全局名字表、符号对象及各声明检查阶段的接口。
// 函数体检查通过 BodyChecker 复用类型解析、常量求值和能力查询等服务。
// 具体实现按职责分布在同目录的各个 .cpp 文件中。

#pragma once

#include "ast/ast.hpp"
#include "diagnostics.hpp"
#include "sema/symbol.hpp"
#include "sema/type.hpp"
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rx {

class BodyChecker;

// SEMANTIC 阶段分析器。
// 持有 crate 级符号和声明检查服务；sema.cpp 只编排 Pass。
// 函数体上下文和表达式检查由 BodyChecker 负责；错误进入同一诊断收集器。
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
    // 函数体检查复用本分析器的名字表、类型解析和 trait 查询。
    friend class BodyChecker;

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

    // collect.cpp：声明收集与入口形态。
    void initialize_builtins();
    void collect_struct(const Struct &s);
    void collect_const(const Const &c);
    void collect_function(const Function &f);
    void check_parameter_names(const std::vector<Parameter> &parameters,
                               Span fallback);
    StructSymbol *resolve_impl_target(const Type &t);
    void collect_impl(const InherentImpl &impl);
    void check_main_declaration();

    // signatures.cpp：字段与函数签名定型。
    // 把字段、参数、返回类型从 AST 定型为 SemanticType。
    void resolve_declared_types();
    void resolve_function_signature(FunctionSymbol &fn, StructSymbol *owner);
    // 入口检查的最后一块：main 的返回类型必须是 unit（E4）。
    void check_main_return_type();

    // layout.cpp：按值内联布局。
    // 按字段类型检测结构体是否存在按值内联的无限布局环（T5）。
    enum class LayoutVisitState { Unvisited, Visiting, Visited };
    using LayoutVisitStates = std::map<const StructSymbol *, LayoutVisitState>;
    void check_inline_layout_cycles();
    void visit_layout(const StructSymbol *sym, LayoutVisitStates &states);
    void visit_layout_type(const SemanticType &type, Span at,
                           LayoutVisitStates &states);

    // traits.cpp：能力查询与 derive 验证。
    // 判断类型是否具备指定 derive 能力（D6/D7）；visiting 记录递归假设。
    bool has_trait(
        const SemanticType &type, Derive trait,
        std::set<std::pair<const StructSymbol *, Derive>> &visiting) const;
    // 检查结构体已请求的 derive 是否合法（D2–D5）。
    void check_derives();
    void check_one_derive(StructSymbol &sym, Derive trait);

    // constants.cpp：常量路径、三色求值与缓存。
    // 扫一遍所有顶层/关联常量，逐个求值并落缓存。
    void evaluate_all_constants();
    // 按常量上下文规则求值字面量、负数与路径。
    EvaluatedConstant evaluate_constant_value(const ConstantValue &value,
                                              const SemanticType *expected,
                                              const StructSymbol *current);
    // 常量路径解析 + 三色递归求值。
    ConstantSymbol *resolve_constant_path(
        const std::vector<PathSegment> &path, Span span,
        const StructSymbol *current);
    EvaluatedConstant evaluate_constant(ConstantSymbol &sym);

    // resolve_type.cpp：语法类型归一化。
    // 把语法类型归一化为 SemanticType：基本类型/Unit/引用/数组/Box/Vec/Self，
    // 数组长度在这里按 usize 常量求值。
    SemanticType resolve_type(const Type &type, const StructSymbol *current);
};

} // namespace rx
