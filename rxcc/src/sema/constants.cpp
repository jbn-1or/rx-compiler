// 实现常量路径解析、递归求值、声明类型检查及求值结果缓存。
// 通过访问状态检测直接和间接的常量依赖环，支持前向引用与关联常量。
// 数组长度复用此处的求值服务；名称查找仅使用常量声明，不涉及局部绑定。

#include "sema/sema.hpp"
#include "sema/type_utils.hpp"

namespace rx {

namespace {

// 统一构造求值失败哨兵，避免每个失败分支重复写空结果。
EvaluatedConstant failed_evaluation() {
    return EvaluatedConstant{};
}

// true = 该类型是 i32 或 isize。
// 用于判断负数常量 `-magnitude` 的操作数是否为有符号整数（C5）。
bool is_signed_int(const SemanticType &type) {
    return type.kind == SemanticType::Kind::Int &&
           (type.int_kind == SemanticType::IntKind::I32 ||
            type.int_kind == SemanticType::IntKind::Isize);
}

} // namespace

// 驱动所有常量求值：constants_ 同时包含顶层常量和 impl 关联常量。
// evaluate_constant 内部负责三色环检测、C1 比对与缓存，这里只负责遍历。
// 无引用者、只声明不求值的常量也会在这里被检查。
void SemanticAnalyzer::evaluate_all_constants() {
    for (auto &sym : constants_)
        evaluate_constant(*sym);
}

// 输入：path（常量路径段列表）、span（报错位置）、current（当前 impl 的
//       结构体；顶层常量为 nullptr）。解析到 ConstantSymbol*
// 作用：把 `N`、`Type::N`、`Self::N` 解析成常量符号，只查名字不求值；
//       关联常量只查 associated_items，不进入顶层值名空间（C7）。
// 例：`N` → 顶层 const N；`Config::N` → Config 的关联常量；
//     `Self::B` 在 impl S 内 → S::B；`fn_name` 或 `S::method` → C2。
ConstantSymbol *
SemanticAnalyzer::resolve_constant_path(
    const std::vector<PathSegment> &path, Span span,
    const StructSymbol *current) {
    // 常量路径只允许 1 段或 2 段；0/>2 段都不可能是常量项。
    if (path.empty() || path.size() > 2) {
        diagnostics_.error(span, "constant path must resolve to a constant");
        return nullptr;
    }

    const PathSegment &first = path[0];
    if (path.size() == 1) {
        // 顶层常量不能带类型实参；带实参说明不是常量项。
        if (!first.generics.empty()) {
            diagnostics_.error(span, "constant path must resolve to a constant");
            return nullptr;
        }
        const auto it = function_constant_namespace.find(first.name);
        // 名字不存在，或名字存在但是函数/内置 I/O
        if (it == function_constant_namespace.end() ||
            it->second->kind != Symbol::Kind::Constant) {
            // C2：未找到，或找到函数/内置 I/O（都不是常量）。
            diagnostics_.error(span, "constant path must resolve to a constant");
            return nullptr;
        }
        return static_cast<ConstantSymbol *>(it->second);
    }

    // 2 段：先解析前缀结构体，再查 associated const。
    const StructSymbol *owner = nullptr;
    if (first.name == "Self") {
        // N10：Self 只在结构体/impl 的上下文中有目标。
        if (!current) {
            diagnostics_.error(span,
                         "Self is not available outside a struct or impl");
            return nullptr;
        }
        if (!first.generics.empty()) {
            // G1：Self不接受类型实参。
            diagnostics_.error(span, "structs have no type parameters: Self");
            return nullptr;
        }
        owner = current;
    } else {  // 前缀不是 `Self`
        // 已登记的用户结构体才能有 associated const。
        const auto it = type_namespace.find(first.name);
        if (it != type_namespace.end()) {  // 在 type_namespace 中找到
            if (!first.generics.empty()) {
                // G1：用户结构体不接受类型实参。
                diagnostics_.error(span,
                             "structs have no type parameters: " + first.name);
                return nullptr;
            }
            owner = it->second;
        } else {
            // 带类型实参的基本类型（如 i32<bool>::X）是 G3；
            if (!first.generics.empty() &&
                !basic_type_from_name(first.name).is_error()) {
                diagnostics_.error(span, "primitives take no type parameters: " +
                                       first.name);
            } else { // Box/Vec 或其他非结构体前缀没有 associated const， C2。
                diagnostics_.error(span, "constant path must resolve to a constant");
            }
            return nullptr;
        }
    }

    // associated constant 本身不接受类型实参。
    const PathSegment &second = path[1];
    if (!second.generics.empty()) {
        diagnostics_.error(span, "constant path must resolve to a constant");
        return nullptr;
    }
    const auto it = owner->associated_items.find(second.name);
    // 联合条件：成员不存在，或成员存在但它是方法/关联函数而不是常量。
    if (it == owner->associated_items.end() ||
        it->second->kind != Symbol::Kind::Constant) {
        // C2：不存在，或成员是方法/关联函数。
        diagnostics_.error(span, "constant path must resolve to a constant");
        return nullptr;
    }
    return static_cast<ConstantSymbol *>(it->second);
}

// 根据ConstantSymbol输出EvaluatedConstant
// 成功时 type/value 为初始化器结果，失败时type=Error、value=monostate、ok=false。
// 作用：DFS求值常量并写缓存；求值时使用目标自己的声明类型，引用方的 expected 不传入。
// 例：`const A: i32 = B; const B: i32 = 3;` 求 A 时递归求 B；
//     `const A: i32 = A;` → C4 并把 A 缓存为 Error。
EvaluatedConstant SemanticAnalyzer::evaluate_constant(ConstantSymbol &sym) {
    // 已求过直接取缓存
    if (sym.state == ConstantEvaluationState::Visited)
        return sym.evaluated;
    // 还在当前递归栈上又被访问 → 形成常量依赖环（C4）。
    if (sym.state == ConstantEvaluationState::Visiting) {
        diagnostics_.error(sym.span, "constant dependency cycle");
        return failed_evaluation();
    }

    // 先置 Visiting，再解析声明类型；这样数组长度里的自引用
    // （如 `const A: [i32; A]`）也会被 C4 捕获。
    sym.state = ConstantEvaluationState::Visiting;
    // AST 正常情况下由 builder 保证 node/type 非空；这里防御性缓存失败。
    if (!sym.node || !sym.node->type) {
        sym.state = ConstantEvaluationState::Visited;
        sym.evaluated = failed_evaluation();
        return sym.evaluated;
    }

    const SemanticType declared = resolve_type(*sym.node->type, sym.owner);
    // 声明类型解析失败时已经报过 N10/N15/G1/G3/G4/G5 等诊断；
    // 统一缓存 Error，后续引用直接静默放行。
    if (declared.is_error()) {
        sym.state = ConstantEvaluationState::Visited;
        sym.evaluated = failed_evaluation();
        return sym.evaluated;
    }

    EvaluatedConstant result =
        evaluate_constant_value(sym.node->value, &declared, sym.owner);
    // 求值器没报错但结果类型与声明不一致（例如去掉路径期望传播后的
    // `const N: isize = -1;`）时，在单个常量内部补 C1，避免把错误结果
    // 发布给后续引用者。
    if (result.ok && !result.type.is_error() && !result.type.equals(declared)) {
        diagnostics_.error(sym.node->value.span,
                     "constant initializer does not match declared type");
        result = failed_evaluation();
    }
    // 只有“求值成功且类型不是 Error”才保留结果；C1/C5 等失败统一缓存 Error。
    sym.state = ConstantEvaluationState::Visited;
    sym.evaluated =
        (result.ok && !result.type.is_error()) ? result : failed_evaluation();
    return sym.evaluated;
}

// 输入：value（常量值 AST）、expected（期望类型，可为 nullptr）、
//       current（Self 所属结构体，供路径求值使用）。
// 输出：EvaluatedConstant；成功 = 类型 + 值 + ok，失败 = Error/monostate/false。
// 求不依赖名字的常量值（整数、布尔、负数）；
EvaluatedConstant
SemanticAnalyzer::evaluate_constant_value(const ConstantValue &value,
                                         const SemanticType *expected,
                                         const StructSymbol *current) {
    // 期望类型本身已经出错：不再追加新诊断，直接把失败向上传递。
    if (expected && expected->is_error())
        return failed_evaluation();

    switch (value.kind) {
        case ConstantValue::Kind::Int: {
            SemanticType type = SemanticType::error();
            if (value.suffix != IntSuffix::None) {
                // 有后缀时后缀优先；若与声明/上下文期望的类型不同 → C1。
                type = type_from_suffix(value.suffix);
                if (expected && !expected->equals(type)) {
                    diagnostics_.error(value.span,
                                 "integer literal suffix does not match the "
                                 "expected type");
                    return failed_evaluation();
                }
            } else if (expected) {
                // 无后缀但有期望类型：只有期望是整数时才能直接采用；
                // 期望 bool/结构体等非整数 → C1。
                if (!expected->is_int()) {
                    diagnostics_.error(value.span,
                                 "integer constant does not match the expected "
                                 "type");
                    return failed_evaluation();
                }
                type = *expected;
            } else {
                // 无后缀且无期望类型：按 §10 规则回退 i32。
                type = SemanticType::int_(SemanticType::IntKind::I32);
            }

            EvaluatedConstant result;
            result.type = type;
            // 无符号整数存 uint64_t，有符号整数存 int64_t；这样负数也能
            // 保留符号，后续 C5/数组长度检查不会混淆符号性。
            if (type.int_kind == SemanticType::IntKind::U32 ||
                type.int_kind == SemanticType::IntKind::Usize)
                result.value = static_cast<uint64_t>(value.int_value);
            else
                result.value = value.int_value;
            result.ok = true;
            return result;
        }

        case ConstantValue::Kind::Bool: {
            // 有期望类型时它必须是 bool；期望是整数/结构体等 → C1。
            if (expected && expected->kind != SemanticType::Kind::Bool) {
                diagnostics_.error(
                    value.span,
                    "boolean constant does not match the expected type");
                return failed_evaluation();
            }
            EvaluatedConstant result;
            result.type = SemanticType::bool_();
            result.value = value.bool_value;
            result.ok = true;
            return result;
        }

        case ConstantValue::Kind::Negative: {
            // 负数 AST 必须带 magnitude 子节点；缺失说明 AST 不完整。
            if (!value.operand)
                return failed_evaluation();
            // 规范 §12：期望类型不穿过一元负号，因此子节点传 nullptr。
            // 子节点会按自己的后缀/常量声明类型或 i32 回退求值。
            EvaluatedConstant operand =
                evaluate_constant_value(*value.operand, nullptr, current);
            // 子节点已经失败或类型是 Error：直接向上传递，避免重复诊断。
            if (!operand.ok || operand.type.is_error())
                return failed_evaluation();
            // 只有 i32/isize 能取负；u32/usize/bool 等 → C5。
            if (!is_signed_int(operand.type)) {
                diagnostics_.error(
                    value.span,
                    "cannot negate an unsigned or non-integer constant");
                return failed_evaluation();
            }
            // 有符号整数应存 int64_t；variant 不是 int64_t 说明内部状态不一致。
            const auto *magnitude = std::get_if<int64_t>(&operand.value);
            if (!magnitude)
                return failed_evaluation();
            EvaluatedConstant result;
            result.type = operand.type;
            result.value = -*magnitude;
            result.ok = true;
            return result;
        }

        case ConstantValue::Kind::Path: {
            // 先做名字解析；失败时 resolve_constant_path 已报 C2/N10/G1/G3。
            ConstantSymbol *target =
                resolve_constant_path(value.path, value.span, current);
            if (!target)
                return failed_evaluation();
            // 递归求目标常量
            const EvaluatedConstant target_eval = evaluate_constant(*target);
            // 目标求值失败（C1/C4/C5/C2 等）时，路径也返回失败，
            if (!target_eval.ok || target_eval.type.is_error())
                return failed_evaluation();
            // 目标常量已在 evaluate_constant 内用声明类型求值并通过 C1 比对，
            // 缓存里的 type 就是目标声明类型；直接复用，不能再用调用方的
            // expected 重新定型目标常量。
            return target_eval;
        }
    }

    return failed_evaluation();
}

} // namespace rx
