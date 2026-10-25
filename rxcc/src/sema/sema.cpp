#include "sema/sema.hpp"

namespace rx {

namespace {

// 语言自带的类型名：结构体不能与它们重名
const std::set<std::string> kBuiltinTypes = {"i32",  "u32", "isize", "usize",
                                             "bool", "Box", "Vec"};

// 内置 I/O 函数名：不能再拿它们当顶层值名（N18）
const std::set<std::string> kBuiltinValueNames = {"get_i32", "print_i32",
                                                  "println_i32"};

// 输入：无。
// 输出：失败的 EvaluatedConstant（type=Error、value=monostate、ok=false）。
// 作用：统一构造求值失败哨兵，避免每个失败分支重复写空结果。
// 例：常量路径解析失败时 → return failed_evaluation();
EvaluatedConstant failed_evaluation() {
    return EvaluatedConstant{};
}

// 输入：整数字面量的后缀 IntSuffix。
// 输出：对应的 SemanticType 整数类型；没有后缀时返回 Error。
// 作用：把语法后缀（如 `1u32` 的 u32）转成语义类型。
// 例：IntSuffix::U32 → u32；IntSuffix::None → Error。
SemanticType type_from_suffix(IntSuffix suffix) {
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

// 输入：一个语义类型。
// 输出：true = 该类型是 i32 或 isize。
// 作用：判断负数常量 `-magnitude` 的操作数是否为有符号整数（C5）。
// 例：i32 → true；isize → true；u32/usize/bool → false。
bool is_signed_int(const SemanticType &type) {
    return type.kind == SemanticType::Kind::Int &&
           (type.int_kind == SemanticType::IntKind::I32 ||
            type.int_kind == SemanticType::IntKind::Isize);
}

// 在类型解析和常量路径中识别 i32/u32/isize/usize/bool。输出对应的 SemanticType
// 例："usize" → SemanticType::int_(Usize)；"S" 或 "Box" → Error。
SemanticType basic_type_from_name(const std::string &name) {
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

} // namespace

// 先登记内置 I/O 名，再逐条登记顶层声明，最后检查 main；错误进 diagnostics_。
// `struct S { x: i32 } fn main() {}`
//   → type_namespace = {"S" → S}，function_constant_namespace = {kBuiltinValueNames,
//   "main"}
void SemanticAnalyzer::run(const Crate &crate) {
    initialize_builtins();
    // 第一遍只登记结构体：规范要求顶层名称查找与声明顺序无关，故 impl 可以
    // 出现在 struct 之前；先建好 type_namespace，1.5 解析 impl 目标时才能命中。
    for (const auto &item : crate.items) {
        if (auto *s = dynamic_cast<Struct *>(item.get()))
            collect_struct(*s);
    }
    // 第二遍登记值名与 impl（结构体已在上一步处理）。
    for (const auto &item : crate.items) {
        if (auto *c = dynamic_cast<Const *>(item.get()))
            collect_const(*c);
        else if (auto *f = dynamic_cast<Function *>(item.get()))
            collect_function(*f);
        else if (auto *i = dynamic_cast<InherentImpl *>(item.get()))
            collect_impl(*i);
    }
    check_main_declaration();
    evaluate_all_constants();
    resolve_declared_types();
    check_main_return_type();
    check_inline_layout_cycles();
    check_derives();
}

// 把内置 I/O 名 get_i32/print_i32/println_i32 以 Kind::Builtin
// 放进 function_constant_namespace；对象由 builtins_ 持有
// （function_constant_namespace 里只存指针）。
void SemanticAnalyzer::initialize_builtins() {
    for (const char *n : {"get_i32", "print_i32", "println_i32"}) {
        auto sym = std::make_unique<Symbol>();
        sym->kind = Symbol::Kind::Builtin;
        sym->name = n;
        function_constant_namespace[n] = sym.get();
        builtins_.push_back(std::move(sym));
    }
}

// 输入：`struct Point { x: i32 }` 的 AST 节点
// 成功 → type_namespace["Point"] = 新 StructSymbol；名字非法 →
// 追加诊断，不登记。
void SemanticAnalyzer::collect_struct(const Struct &s) {
    // N17：结构体名不能占用语言自带的类型名。报错后不登记，避免污染
    // type_namespace。
    if (kBuiltinTypes.count(s.name)) {
        diagnostics_.error(s.span, "protected type name: " + s.name);
        return;
    }
    // N2：结构体名在类型名空间里必须唯一。保留先登记的那个，后一个不覆盖。
    if (type_namespace.count(s.name)) {
        diagnostics_.error(s.span, "duplicate struct: " + s.name);
        return;
    }
    // N4：同一结构体内字段名必须唯一。StructField
    // 没有 span，用字段类型节点的位置。
    std::set<std::string> seen_fields;
    for (const auto &f : s.fields) {
        if (!seen_fields.insert(f.name).second) {
            const Span at = f.type ? f.type->span : s.span;
            diagnostics_.error(at, "duplicate field: " + f.name);
        }
    }
    // D1：derive 列表去重后若长度变小，说明同一 trait 被请求了多次。
    // AstBuilder 已把多个 #[derive(..)] 合并进 s.derives
    std::set<Derive> derives;
    for (Derive d : s.derives) {
        if (!derives.insert(d).second)
            diagnostics_.error(
                s.span, std::string("repeated derive: ") + name(d));
    }
    // 建档案并登记。字段/derive 有错时仍登记，让后续 Phase 能继续解析。
    auto sym = std::make_unique<StructSymbol>();
    sym->name = s.name;
    sym->span = s.span;
    sym->node = &s;
    for (const auto &f : s.fields) {
        sym->fields.push_back(FieldDeclaration{
            f.name, f.type.get(), f.type ? f.type->span : s.span});
    }
    sym->derives = std::move(derives);
    type_namespace[sym->name] = sym.get();
    structs_.push_back(std::move(sym));
}

// 输入：`const MAX: i32 = 10;` 的 AST 节点
// 效果：成功 → function_constant_namespace["MAX"] = 新 ConstantSymbol；名字非法 →
// 只追加诊断，不登记。 例：第二个 `const MAX` → 报 N1；`const get_i32` → 报
// N18。
void SemanticAnalyzer::collect_const(const Const &c) {
    // N18：内置 I/O 名是受保护值名，用户常量不能占用。
    // 必须先于 N1 检查：这三个名字已经预登记在 function_constant_namespace 中。
    if (kBuiltinValueNames.count(c.name)) {
        diagnostics_.error(c.span, "protected value name: " + c.name);
        return;
    }
    // N1：函数与常量共用值名空间，名字必须唯一；保留先登记的那个。
    if (function_constant_namespace.count(c.name)) {
        diagnostics_.error(c.span, "duplicate value name: " + c.name);
        return;
    }
    // 登记档案：owner = nullptr 表示顶层常量。
    auto sym = std::make_unique<ConstantSymbol>();
    sym->kind = Symbol::Kind::Constant;
    sym->name = c.name;
    sym->span = c.span;
    sym->node = &c;
    sym->owner = nullptr;
    function_constant_namespace[sym->name] = sym.get();
    constants_.push_back(std::move(sym));
}

// 输入：`fn f(a: i32) {}` 的 AST 节点
// 效果：成功 → function_constant_namespace["f"] = 新 FunctionSymbol；名字非法 →
// 只追加诊断，不登记。 例：`fn f(a: i32, a: i32)` → 报 N5；顶层 `fn f(&self)` →
// 报 N8；
void SemanticAnalyzer::collect_function(const Function &f) {
    // N18：内置 I/O 名是受保护值名，用户函数不能占用。
    if (kBuiltinValueNames.count(f.name)) {
        diagnostics_.error(f.span, "protected value name: " + f.name);
        return;
    }
    // N1：函数与常量共用值名空间，名字必须唯一。
    if (function_constant_namespace.count(f.name)) {
        diagnostics_.error(f.span, "duplicate value name: " + f.name);
        return;
    }
    // N5：参数名必须唯一。
    check_parameter_names(f.parameters, f.span);
    // N8：顶层函数不能带接收者（self 只允许出现在 impl 方法里）。
    if (f.self_parameter)
        diagnostics_.error(f.span,
                     "top-level function cannot have a receiver: " + f.name);

    // 参数/接收者有错也登记，避免后续按名字找不到函数而连锁报错。
    auto sym = std::make_unique<FunctionSymbol>();
    sym->kind = Symbol::Kind::Function;
    sym->name = f.name;
    sym->span = f.span;
    sym->node = &f;
    sym->owner = nullptr;
    function_constant_namespace[sym->name] = sym.get();
    functions_.push_back(std::move(sym));
}

// 输入：函数参数列表 + 出错时回退的位置
// 参数名重复时追加 N5 诊断。Parameter 没有 span，用参数类型节点的位置。
void SemanticAnalyzer::check_parameter_names(
    const std::vector<Parameter> &parameters, Span fallback) {
    std::set<std::string> seen;
    for (const auto &p : parameters) {
        if (!seen.insert(p.name).second) {
            const Span at = p.type ? p.type->span : fallback;
            diagnostics_.error(at, "duplicate parameter: " + p.name);
        }
    }
}

// 输入：`impl S` 的 self_type 节点
// 输出：S 对应的 StructSymbol；不是用户结构体或带类型实参时追加诊断并返回
// nullptr。 例：`impl S` → S；`impl Box<i32>``impl [i32; 2]``impl &S` →
// G7；`impl S<i32>` → G1。
StructSymbol *SemanticAnalyzer::resolve_impl_target(const Type &t) {
    const auto *pt = dynamic_cast<const PathType *>(&t);
    if (!pt || pt->segments.size() != 1) { // impl对象不唯一
        diagnostics_.error(t.span, "impl target must be a user-defined struct");
        return nullptr;
    }
    const PathSegment &seg = pt->segments[0];
    const auto it = type_namespace.find(seg.name);
    if (it == type_namespace.end()) { // impl对象还未定义或者是自留类名
        diagnostics_.error(t.span,
                     "impl target must be a user-defined struct: " + seg.name);
        return nullptr;
    }
    if (!seg.generics.empty()) { // impl对象带类型实参
        diagnostics_.error(
            t.span, "structs have no type parameters: " + seg.name);
        return nullptr;
    }
    return it->second;
}

// 把 impl 的目标类型解析成已登记的结构体。
// 输入：`impl S { fn get(&self) {} }` 的 AST 节点
// 效果：成功 → S 的 associated_items["get"] = FunctionSymbol；
// 目标不是用户结构体 → 报 G7，目标带类型实参 → G1，成员重名 → N6，方法参数重名
// → N5。 例：两个 impl 都定义 get → 报 N6；`impl i32 {}` → 报 G7。
void SemanticAnalyzer::collect_impl(const InherentImpl &impl) {
    StructSymbol *target = resolve_impl_target(*impl.self_type);
    if (!target)
        return;

    for (const auto &item : impl.items) {
        // impl成员是函数还是常量
        if (const auto *fn = dynamic_cast<const Function *>(item.get())) {
            // N6：跨 impl 方法/关联函数/关联常量也不能重名。
            if (target->associated_items.count(fn->name)) {
                diagnostics_.error(fn->span,
                             "duplicate associated item: " + fn->name);
                continue; // 保留先登记的那个
            }
            // N5：方法参数名必须唯一。
            check_parameter_names(fn->parameters, fn->span);
            // 创建函数symbol
            auto sym = std::make_unique<FunctionSymbol>();
            sym->kind = Symbol::Kind::Function;
            sym->name = fn->name;
            sym->span = fn->span;
            sym->node = fn;
            sym->owner = target;
            target->associated_items[fn->name] = sym.get();
            functions_.push_back(std::move(sym));
        } else if (const auto *c = dynamic_cast<const Const *>(item.get())) {
            // N6：关联常量与同名方法/关联函数冲突。
            if (target->associated_items.count(c->name)) {
                diagnostics_.error(
                    c->span, "duplicate associated item: " + c->name);
                continue;
            }
            // 创建常量symbol
            auto sym = std::make_unique<ConstantSymbol>();
            sym->kind = Symbol::Kind::Constant;
            sym->name = c->name;
            sym->span = c->span;
            sym->node = c;
            sym->owner = target;
            target->associated_items[c->name] = sym.get();
            constants_.push_back(std::move(sym));
        }
    }
}

// 检查填好的 function_constant_namespace；
// 例：没有 main → E1；`const main: i32 = 1;` → E1；`fn main(x: i32)` → E2；
//     `fn main<'a>()` → E3；`fn main() {}` → 无诊断。
void SemanticAnalyzer::check_main_declaration() {
    const auto it = function_constant_namespace.find("main");
    const bool has_fn_main = it != function_constant_namespace.end() &&
                             it->second->kind == Symbol::Kind::Function;
    // E1：可执行程序必须有一个顶层 fn main。
    if (!has_fn_main) {
        diagnostics_.error(Span{}, "an executable needs a top-level main");
        return;
    }
    const auto *fn = static_cast<const FunctionSymbol *>(it->second);
    // E2：main 不能有值参数。
    if (!fn->node->parameters.empty())
        diagnostics_.error(fn->span, "main cannot have value parameters");
    // E3：main 不能有泛型/生命周期参数。
    if (fn->node->has_generic_parameters)
        diagnostics_.error(fn->span, "main cannot have generic parameters");
}

// 驱动所有常量求值：constants_ 同时包含顶层常量和 impl 关联常量。
// evaluate_constant 内部负责三色环检测、C1 比对与缓存，这里只负责遍历。
// 无引用者、只声明不求值的常量也会在这里被检查。
void SemanticAnalyzer::evaluate_all_constants() {
    for (auto &sym : constants_)
        evaluate_constant(*sym);
}

// 遍历所有结构体和函数，把 AST 类型节点定型为 SemanticType。
// 单个字段/签名解析失败时保留 Error，不中断其余项。
void SemanticAnalyzer::resolve_declared_types() {
    for (auto &sym : structs_) {
        // 处理 struct 定义中的字段
        for (const auto &field : sym->fields) {
            sym->field_types[field.name] =
                field.type ? resolve_type(*field.type, sym.get())
                           : SemanticType::error();
        }
    }
    for (auto &sym : functions_)
        resolve_function_signature(*sym, sym->owner);
}

// 定型一个函数的参数、返回类型和接收者形态。
// `fn f(mut x: i32) -> &mut S` → parameters={i32, is_mut=true}、return_type=&mut S。
void SemanticAnalyzer::resolve_function_signature(FunctionSymbol &fn,
                                                  StructSymbol *owner) {
    fn.parameters.clear();
    fn.self_parameter = ResolvedReceiver{};
    if (!fn.node) {
        fn.return_type = SemanticType::error();
        return;
    }

    // 处理函数形式参数
    fn.parameters.reserve(fn.node->parameters.size());
    for (const auto &param : fn.node->parameters) {
        const SemanticType type =
            param.type ? resolve_type(*param.type, owner)
                       : SemanticType::error();
        fn.parameters.push_back(ResolvedParameter{type, param.is_mut});
    }
    // 处理函数返回类型
    fn.return_type =
        fn.node->return_type ? resolve_type(*fn.node->return_type, owner)
                             : SemanticType::error();

    if (!fn.node->self_parameter)
        return;
    fn.self_parameter.present = true;
    switch (fn.node->self_parameter->kind) {
        case SelfParameter::Kind::Value:
            break;
        case SelfParameter::Kind::MutableValue:
            fn.self_parameter.is_mut = true;
            break;
        case SelfParameter::Kind::Reference:
            fn.self_parameter.by_reference = true;
            break;
        case SelfParameter::Kind::MutableReference:
            fn.self_parameter.by_reference = true;
            fn.self_parameter.is_mut = true;
            break;
    }
}

// 检查 main 的返回类型（E4）。
// 避免把 ConstantSymbol 当成 FunctionSymbol；返回类型解析失败时静默放行。
void SemanticAnalyzer::check_main_return_type() {
    const auto it = function_constant_namespace.find("main");
    // 没有 main（E1 已报）或 main 不是函数时跳过。
    if (it == function_constant_namespace.end() ||
        it->second->kind != Symbol::Kind::Function)
        return;
    const auto *fn = static_cast<const FunctionSymbol *>(it->second);
    if (fn->return_type.is_error() || fn->return_type.is_unit())
        return;

    const Span at = fn->node && fn->node->return_type
                        ? fn->node->return_type->span
                        : fn->span;
    diagnostics_.error(at, "main must return unit");
}

// 从每个结构体出发做三色 DFS，共享同一张颜色表。
void SemanticAnalyzer::check_inline_layout_cycles() {
    LayoutVisitStates states;
    for (const auto &sym : structs_)
        visit_layout(sym.get(), states);
}

// 三色 DFS：Visiting 表示当前递归栈上存在该结构体，再次遇到即为内联环。
void SemanticAnalyzer::visit_layout(const StructSymbol *sym,
                                LayoutVisitStates &states) {
    if (!sym)
        return;

    LayoutVisitState &state = states[sym];
    if (state == LayoutVisitState::Visited)
        return;
    if (state == LayoutVisitState::Visiting) {
        diagnostics_.error(sym->span, "infinite inline layout cycle");
        return;
    }

    state = LayoutVisitState::Visiting;
    // 检查结构体内的所有字段
    for (const auto &field : sym->fields) {
        const auto it = sym->field_types.find(field.name);
        // 字段类型解析失败时已报过错，不在这里制造连锁布局错误。
        if (it == sym->field_types.end() || it->second.is_error())
            continue;
        visit_layout_type(it->second, field.span, states);
    }
    state = LayoutVisitState::Visited;
}

// 数组按值内联元素，继续向下；引用/Box/Vec 是间接层，打断内联。
void SemanticAnalyzer::visit_layout_type(const SemanticType &type, Span at,
                                     LayoutVisitStates &states) {
    switch (type.kind) {
        case SemanticType::Kind::Struct: {
            if (!type.struct_symbol)
                return;
            // 在访问表中找到这个结构体的visit态
            const auto it = states.find(type.struct_symbol);
            if (it != states.end() &&
                it->second == LayoutVisitState::Visiting) {
                diagnostics_.error(at, "infinite inline layout cycle");
                return;
            }
            visit_layout(type.struct_symbol, states);
            return;
        }
        // 若为数组则对其类型递归检查
        case SemanticType::Kind::Array:
            if (type.inner)
                visit_layout_type(*type.inner, at, states);
            return;
        case SemanticType::Kind::Unit:
        case SemanticType::Kind::Never:
        case SemanticType::Kind::Error:
        case SemanticType::Kind::Int:
        case SemanticType::Kind::Bool:
        case SemanticType::Kind::Reference:
        case SemanticType::Kind::Box:
        case SemanticType::Kind::Vec:
            return;
    }
}

// 查询类型是否具备 trait 能力（D6）；结构体递归时用 visiting 假设成立，
// 保证递归类型能够终止（D7）。Error 视为满足，避免连锁诊断。
bool SemanticAnalyzer::has_trait(
    const SemanticType &type, Derive trait,
    std::set<std::pair<const StructSymbol *, Derive>> &visiting) const {
    switch (type.kind) {
        case SemanticType::Kind::Int:
        case SemanticType::Kind::Bool:
            return true;
        case SemanticType::Kind::Error:
            return true;
        case SemanticType::Kind::Unit:
        case SemanticType::Kind::Never:
            return false;
        case SemanticType::Kind::Reference:
            if (!type.inner)
                return false;
            // &T 无条件 Copy/Clone；&mut T 两者都没有。
            if (trait == Derive::Copy || trait == Derive::Clone)
                return !type.is_mut;
            // 别的trait解引用递归查
            return has_trait(*type.inner, trait, visiting);
        case SemanticType::Kind::Array:
            return type.inner && has_trait(*type.inner, trait, visiting);
        case SemanticType::Kind::Box:
        case SemanticType::Kind::Vec:
            if (trait == Derive::Copy)
                return false;
            return type.inner && has_trait(*type.inner, trait, visiting);
        case SemanticType::Kind::Struct: {
            const StructSymbol *sym = type.struct_symbol;
            // 结构体必须显式请求该 trait。
            if (!sym || !sym->derives.count(trait))
                return false;
            // Copy 必须显式请求 Clone；Eq 必须显式请求 PartialEq。
            if (trait == Derive::Copy && !sym->derives.count(Derive::Clone))
                return false;
            if (trait == Derive::Eq && !sym->derives.count(Derive::PartialEq))
                return false;

            const auto key = std::make_pair(sym, trait);
            if (visiting.count(key))
                return true;
            visiting.insert(key);
            bool ok = true;
            // 所有字段都必须具备该能力。
            for (const auto &field : sym->field_types) {
                if (!has_trait(field.second, trait, visiting)) {
                    ok = false;
                    break;
                }
            }
            visiting.erase(key);
            return ok;
        }
    }
    return false;
}

// 逐个结构体、逐个显式 derive 检查配套要求和字段能力。
void SemanticAnalyzer::check_derives() {
    for (auto &sym : structs_) {
        for (Derive trait : sym->derives)
            check_one_derive(*sym, trait);
    }
}

// 检查一个 derive：Copy 必须显式带 Clone，Eq 必须显式带 PartialEq；
// 然后要求每个字段都具备该能力。
void SemanticAnalyzer::check_one_derive(StructSymbol &sym, Derive trait) {
    if (trait == Derive::Copy && !sym.derives.count(Derive::Clone))
        diagnostics_.error(sym.span, "Copy requires an explicit Clone derive");
    if (trait == Derive::Eq && !sym.derives.count(Derive::PartialEq))
        diagnostics_.error(
            sym.span, "Eq requires an explicit PartialEq derive");

    for (const auto &field : sym.fields) {
        const auto it = sym.field_types.find(field.name);
        if (it == sym.field_types.end())
            continue;

        std::set<std::pair<const StructSymbol *, Derive>> visiting;
        if (!has_trait(it->second, trait, visiting)) {
            diagnostics_.error(field.span, "field " + field.name +
                                          " does not support " + name(trait));
        }
    }
}

// 4.1 的公共入口；具体表达式规则从 4.3 起逐步接入。
// 当前未接通函数体检查，失败返回 Error，避免后续检查产生连锁诊断。
ExpressionResult
SemanticAnalyzer::check_expression(const Expression &,
                                   const SemanticType *) {
    return ExpressionResult{SemanticType::error(), PlaceAccessInformation{}};
}

ExpressionResult SemanticAnalyzer::check_block(const BlockExpression &,
                                               const SemanticType *) {
    return ExpressionResult{SemanticType::error(), PlaceAccessInformation{}};
}

// 值表达式需要存储位置时物化为可写临时 place；已有 place 保留原权限，
// 不能把不可变 place 变成可变副本。
PlaceAccessInformation SemanticAnalyzer::materialize_place(
    const PlaceAccessInformation &place) const {
    if (place.is_place)
        return place;
    return PlaceAccessInformation{true, true, false};
}

// 执行一次内置解引用：&T / &mut T / Box<T>。
// 值基表达式先物化为临时 place；其他类型报错并返回 Error。
ExpressionResult
SemanticAnalyzer::dereference_place(const SemanticType &base_type,
                                    const PlaceAccessInformation &base_place,
                                    Span at) {
    if (base_type.is_error())
        return ExpressionResult{SemanticType::error(),
                                PlaceAccessInformation{}};

    const PlaceAccessInformation place = materialize_place(base_place);

    if (base_type.kind == SemanticType::Kind::Reference) {
        if (!base_type.inner)
            return ExpressionResult{SemanticType::error(),
                                    PlaceAccessInformation{}};
        // 共享引用总是只读且跨过一层共享访问，只要曾跨过共享引用，可变引用也不可写
        // x = &&mut i32，**x不可写，x = &mut &mut i32可以
        PlaceAccessInformation dereferenced{true, false, true};
        if (base_type.is_mut) {
            dereferenced.is_mutable = !place.crossed_shared;
            dereferenced.crossed_shared = place.crossed_shared;
        }
        return ExpressionResult{*base_type.inner, dereferenced};
    }
    // box直接开盒
    if (base_type.kind == SemanticType::Kind::Box) {
        if (!base_type.inner)
            return ExpressionResult{SemanticType::error(),
                                    PlaceAccessInformation{}};
        return ExpressionResult{
            *base_type.inner,
            PlaceAccessInformation{true, place.is_mutable,
                                   place.crossed_shared}};
    }

    diagnostics_.error(at, "type " + base_type.toString() +
                               " cannot be dereferenced");
    return ExpressionResult{SemanticType::error(), PlaceAccessInformation{}};
}

// 一对一转换判定（CV5/CV6）。转换只做判定，不改变源 ExpressionResult。
// true：可以转换。同类不需转换或合法隐式转换，false：不能转换为目标类
bool SemanticAnalyzer::can_coerce(const SemanticType &from,
                                  const PlaceAccessInformation &place,
                                  const SemanticType &to) const {
    // 错误恢复与发散：Error 静默放行，Never 可转任意目标。
    if (from.is_error() || to.is_error() || from.is_never())
        return true;

    const bool target_is_mutable_reference =
        to.kind == SemanticType::Kind::Reference && to.is_mut;

    // 可变重借用不能跨过共享访问；必须先于同类型判断。
    if (target_is_mutable_reference && place.crossed_shared)
        return false;

    // 同类型直接保持；&mut T → &T 也在此后按引用解引用规则处理。
    if (from.equals(to))
        return true;

    // 只有引用到引用才可能发生强制转换。
    if (from.kind != SemanticType::Kind::Reference ||
        to.kind != SemanticType::Kind::Reference || !from.inner ||
        !to.inner)
        return false;

    // 可变目标要求源外层为 &mut；共享目标允许 &T 和 &mut T。
    if (target_is_mutable_reference && !from.is_mut)
        return false;

    // 从源引用的内部类型开始逐层内置解引用，直到与目标内部类型完全相等。
    const SemanticType *current = from.inner.get();
    while (current) {
        if (current->equals(*to.inner))
            return true;

        if (current->kind == SemanticType::Kind::Reference) {
            if (!current->inner)
                return false;
            // 可变目标不能经过共享引用；共享目标允许。
            if (target_is_mutable_reference && !current->is_mut)
                return false;
            current = current->inner.get();
            continue;
        }

        if (current->kind == SemanticType::Kind::Box) {
            if (!current->inner)
                return false;
            current = current->inner.get();
            continue;
        }

        // Vec、数组、整数、结构体等都不产生隐式解引用/切片转换。
        return false;
    }
    return false;
}

// 在固定目标位置统一报告一次不兼容错误；转换本身不改写结果类型。
void SemanticAnalyzer::require_coerce(const ExpressionResult &from,
                                      const SemanticType &to, Span at) {
    if (can_coerce(from.type, from.place, to))
        return;
    diagnostics_.error(at, "type mismatch: expected " + to.toString() +
                               ", found " + from.type.toString());
}

// 无期望类型时，为一组表达式结果寻找公共类型
// 调用方必须先用 nullptr 期望独立检查每个结果，不能把 LUB 目标反过来
// 作为后续字面量的期望类型。失败属未定义行为：不追加诊断，返回 Error。
SemanticType SemanticAnalyzer::compute_least_upper_bound(
    const std::vector<ExpressionResult> &results) const {
    // 任一结果已经出错时不再连锁诊断。
    for (const auto &result : results) {
        if (result.type.is_error())
            return SemanticType::error();
    }

    bool has_target = false;
    SemanticType target;

    for (std::size_t index = 0; index < results.size(); ++index) {
        const ExpressionResult &result = results[index];
        if (result.type.is_never())
            continue;
        // 当前无目标类型，设为当前类型
        if (!has_target) {
            target = result.type;
            has_target = true;
            continue;
        }
        // 可以类型转换，公共类型不变
        if (can_coerce(result.type, result.place, target))
            continue;

        // 当前结果不能转成现有目标时，只有此前所有原始结果都能转成
        // 当前结果，才允许把目标更新为当前结果。
        bool all_previous_convert = true;
        for (std::size_t previous = 0; previous < index; ++previous) {
            const ExpressionResult &earlier = results[previous];
            if (earlier.type.is_never())
                continue;
            if (!can_coerce(earlier.type, earlier.place, result.type)) {
                all_previous_convert = false;
                break;
            }
        }
        if (!all_previous_convert)
            return SemanticType::error();
        target = result.type;
    }

    // 全部结果都是 !（含空结果集）时，LUB 为 !。
    if (!has_target)
        return SemanticType::never();

    // 最终目标确定后逐项复核；尤其要检查首个结果本身能否适配目标，
    // 避免漏掉可变重借用等 place 权限限制。
    for (const auto &result : results) {
        if (!can_coerce(result.type, result.place, target))
            return SemanticType::error();
    }
    return target;
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

// 把基本类型、Unit、引用、数组、Box/Vec、用户结构体和 Self 转换成语义类型；
// 数组长度在这里按 usize 常量求值，保证 [i32; N] 与 [i32; 3] 等价。
SemanticType SemanticAnalyzer::resolve_type(const Type &type,
                                    const StructSymbol *current) {
    // `()`：函数/控制流结果，也是省略返回类型时的类型。
    if (dynamic_cast<const UnitType *>(&type))
        return SemanticType::unit();

    if (const auto *pt = dynamic_cast<const PathType *>(&type)) {
        // 类型位置没有 associated type；多段路径（如 S::T）无法解析。
        if (pt->segments.size() != 1) {
            diagnostics_.error(type.span, "unresolved type: " + pt->name());
            return SemanticType::error();
        }

        const PathSegment &seg = pt->segments[0];

        // Self 只表示当前结构体/impl 目标
        if (seg.name == "Self") {
            // self无指代
            if (!current) {
                diagnostics_.error(
                    type.span,
                    "Self is not available outside a struct or impl");
                return SemanticType::error();
            }
            // self不能有实参
            if (!seg.generics.empty()) {
                diagnostics_.error(type.span,
                             "structs have no type parameters: Self");
                return SemanticType::error();
            }
            return SemanticType::struct_(current);
        }

        // 基本类型不接受类型实参。
        const SemanticType basic = basic_type_from_name(seg.name);
        if (!basic.is_error()) {
            if (!seg.generics.empty()) {
                diagnostics_.error(type.span,
                             "primitives take no type parameters: " +
                                 seg.name);
                return SemanticType::error();
            }
            return basic;
        }

        // Box/Vec 恰好一个具体类型实参
        if (seg.name == "Box" || seg.name == "Vec") {
            if (seg.generics.size() != 1) {
                diagnostics_.error(
                    type.span,
                    seg.name + " requires exactly one concrete type argument");
                return SemanticType::error();
            }
            if (!seg.generics[0])
                return SemanticType::error();
            // 从 AST 类型节点得到 SemanticType
            const SemanticType inner = resolve_type(*seg.generics[0], current);
            if (inner.is_error())
                return SemanticType::error();
            return seg.name == "Box" ? SemanticType::boxed(inner)
                                     : SemanticType::vec(inner);
        }

        // 用户结构体：名义类型，不接受类型实参。
        const auto it = type_namespace.find(seg.name);
        if (it == type_namespace.end()) {  // 没找到
            diagnostics_.error(type.span, "unresolved type: " + seg.name);
            return SemanticType::error();
        }
        if (!seg.generics.empty()) { // 不能有实参
            diagnostics_.error(type.span,
                         "structs have no type parameters: " + seg.name);
            return SemanticType::error();
        }
        return SemanticType::struct_(it->second);
    }

    // &T / &mut T
    if (const auto *rt = dynamic_cast<const ReferenceType *>(&type)) {
        if (!rt->inner)
            return SemanticType::error();
        const SemanticType inner = resolve_type(*rt->inner, current);
        if (inner.is_error())
            return SemanticType::error();
        return SemanticType::reference(rt->is_mut, inner);
    }

    // [T; N]：元素递归解析，长度按 usize 常量求值后存成 uint64_t。
    if (const auto *at = dynamic_cast<const ArrayType *>(&type)) {
        if (!at->element)
            return SemanticType::error();
        const SemanticType element = resolve_type(*at->element, current);
        if (element.is_error())
            return SemanticType::error();

        const SemanticType usize_type =
            SemanticType::int_(SemanticType::IntKind::Usize);
        // 得到数组长度
        const EvaluatedConstant length =
            evaluate_constant_value(at->length, &usize_type, current);
        // 求值器已报 C1/C2/C4/C5 等错误时不再重复报 C3。
        if (!length.ok || length.type.is_error())
            return SemanticType::error();
        if (!length.type.equals(usize_type)) {  // 数组长度不是 usize
            diagnostics_.error(at->length.span,
                         "array length must be a usize constant");
            return SemanticType::error();
        }
        // variant 是否是 uint64_t
        const auto *len = std::get_if<uint64_t>(&length.value);
        if (!len) {
            diagnostics_.error(at->length.span,
                         "array length must be a usize constant");
            return SemanticType::error();
        }
        return SemanticType::array(element, *len);
    }

    return SemanticType::error();
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
