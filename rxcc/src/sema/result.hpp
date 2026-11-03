// 保存语义分析得到的声明、绑定、表达式类型和名称查找结果。
// 分析器结束后，驱动和后续阶段仍可通过这份结果查询这些信息。

#pragma once

#include "sema/body/calls/call.hpp"
#include "sema/facts.hpp"
#include "sema/symbol.hpp"
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

namespace rx {

struct FunctionLocals {
    std::optional<LocalId> self;
    std::vector<LocalId> parameters; // 按声明顺序保存普通参数，不包括 self。
};

struct IntegerLiteralFact {
    SemanticType type;
    uint32_t bits;
};

using PathTarget = std::variant<LocalId, ConstantId, FunctionId>;
using CallTarget = std::variant<FunctionId, BuiltinIO>;

// 用父节点、用途和序号区分表达式的不同使用位置。
// 序号从零开始；只有一个位置时也填 0。节点与其他结果表一样由 Crate 持有。
enum class UseKind {
    Initializer, // let 初始化器
    FunctionBody, // 函数体结果
    ReturnValue, // return 操作数
    AssignmentValue, // 赋值右侧
    Argument, // 普通调用、关联调用或方法调用的参数，序号不包括接收者
    Receiver, // 方法接收者
    Field, // 结构体构造的字段值，按源码中的字段顺序编号
    ArrayElement, // 数组元素，列表按源码顺序编号；重复形式使用 0
    BlockTail, // 块尾表达式
    IfBranch, // then 为 0，else 为 1
    BreakValue, // break 操作数
};

struct UseSite {
    const Node *parent;
    UseKind kind;
    std::size_t index;

    bool operator<(const UseSite &other) const {
        const std::less<const Node *> less;
        if (parent != other.parent)
            return less(parent, other.parent);
        if (kind != other.kind)
            return kind < other.kind;
        return index < other.index;
    }
};

// 这里只预留目标类型，不表示已经知道需要哪些转换操作。
// 后续实现具体转换时再扩展；没有记录也不表示无需转换。
struct UseAdjustment {
    SemanticType target_type;
};

// 结果独立持有符号，移动结果不会改变符号地址，类型和关联项中的指针仍然有效。
// 结果中的 AST 指针不拥有节点；调用方使用结果期间必须保持 Crate 存活。
// LLVM 对象及生成期间的映射不放在这里。
struct SemanticResult {
    SemanticResult() = default;
    SemanticResult(const SemanticResult &) = delete;
    SemanticResult &operator=(const SemanticResult &) = delete;
    SemanticResult(SemanticResult &&) noexcept = default;
    SemanticResult &operator=(SemanticResult &&) noexcept = default;

    std::vector<std::unique_ptr<StructSymbol>> structs;
    std::vector<std::unique_ptr<FunctionSymbol>> functions;
    std::vector<std::unique_ptr<ConstantSymbol>> constants;
    std::vector<std::unique_ptr<Symbol>> builtins;

    std::optional<FunctionId> main_function;
    std::unordered_map<const Struct *, StructId> struct_declarations;
    std::unordered_map<const Function *, FunctionId> function_declarations;
    std::unordered_map<const Const *, ConstantId> constant_declarations;
    std::map<BuiltinIO, CallableSignature> builtin_signatures;

    std::vector<Binding> locals;
    std::map<FunctionId, FunctionLocals> function_locals;
    std::unordered_map<const LetStatement *, LocalId> let_bindings;
    // 保存作为值检查的表达式；被调用的函数路径和类型路径不在此列。
    // 查询时必须检查记录是否存在，缺失时不能用默认值代替。
    std::unordered_map<const Expression *, ExpressionResult> expressions;
    // 当前语义检查不会填写此表；后续阶段应在确定调整后按使用位置记录。
    std::map<UseSite, UseAdjustment> use_adjustments;
    std::unordered_map<const PathExpression *, PathTarget> paths;
    std::unordered_map<const IntegerLiteral *, IntegerLiteralFact> integer_literals;
    std::unordered_map<const CallExpression *, CallTarget> ordinary_calls;
    // 保存常量初始化器、数组长度、重复次数及其操作数的求值结果。
    // ConstantValue 不是 AST 节点，直接用它自身的地址查询。
    std::unordered_map<const ConstantValue *, EvaluatedConstant> constant_values;
    std::unordered_map<const ConstantValue *, ConstantId> constant_paths;
};

} // namespace rx
