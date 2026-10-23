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
// 输出：失败的 EvaluatedConst（ty=Error、value=monostate、ok=false）。
// 作用：统一构造求值失败哨兵，避免每个失败分支重复写空结果。
// 例：常量路径解析失败时 → return failed_evaluated();
EvaluatedConst failed_evaluated() {
    return EvaluatedConst{};
}

// 输入：整数字面量的后缀 IntSuffix。
// 输出：对应的 SemaType 整数类型；没有后缀时返回 Error。
// 作用：把语法后缀（如 `1u32` 的 u32）转成语义类型。
// 例：IntSuffix::U32 → u32；IntSuffix::None → Error。
SemaType type_from_suffix(IntSuffix suffix) {
    switch (suffix) {
        case IntSuffix::I32:
            return SemaType::int_(SemaType::IntKind::I32);
        case IntSuffix::U32:
            return SemaType::int_(SemaType::IntKind::U32);
        case IntSuffix::Isize:
            return SemaType::int_(SemaType::IntKind::Isize);
        case IntSuffix::Usize:
            return SemaType::int_(SemaType::IntKind::Usize);
        case IntSuffix::None:
            break;
    }
    return SemaType::error();
}

// 输入：一个语义类型。
// 输出：true = 该类型是 i32 或 isize。
// 作用：判断负数常量 `-magnitude` 的操作数是否为有符号整数（C5）。
// 例：i32 → true；isize → true；u32/usize/bool → false。
bool is_signed_int(const SemaType &ty) {
    return ty.kind == SemaType::Kind::Int &&
           (ty.int_kind == SemaType::IntKind::I32 ||
            ty.int_kind == SemaType::IntKind::Isize);
}

// 在类型解析和常量路径中识别 i32/u32/isize/usize/bool。输出对应的 SemaType
// 例："usize" → SemaType::int_(Usize)；"S" 或 "Box" → Error。
SemaType basic_type_from_name(const std::string &name) {
    if (name == "i32")
        return SemaType::int_(SemaType::IntKind::I32);
    if (name == "u32")
        return SemaType::int_(SemaType::IntKind::U32);
    if (name == "isize")
        return SemaType::int_(SemaType::IntKind::Isize);
    if (name == "usize")
        return SemaType::int_(SemaType::IntKind::Usize);
    if (name == "bool")
        return SemaType::bool_();
    return SemaType::error();
}

} // namespace

// 先登记内置 I/O 名，再逐条登记顶层声明，最后检查 main；错误进 diags_。
// `struct S { x: i32 } fn main() {}`
//   → type_namespace = {"S" → S}，func_const_namespace = {kBuiltinValueNames,
//   "main"}
void SemaAnalyzer::run(const Crate &crate) {
    init_builtins();
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
    check_entry();
    evaluated_all_constants();
}

// 把内置 I/O 名 get_i32/print_i32/println_i32 以 Kind::Builtin
// 放进func_const_namespace 对象由 builtins_
// 持有（func_const_namespace里只存指针）。
void SemaAnalyzer::init_builtins() {
    for (const char *n : {"get_i32", "print_i32", "println_i32"}) {
        auto sym = std::make_unique<Symbol>();
        sym->kind = Symbol::Kind::Builtin;
        sym->name = n;
        func_const_namespace[n] = sym.get();
        builtins_.push_back(std::move(sym));
    }
}

// 输入：`struct Point { x: i32 }` 的 AST 节点
// 成功 → type_namespace["Point"] = 新 StructSymbol；名字非法 →
// 追加诊断，不登记。
void SemaAnalyzer::collect_struct(const Struct &s) {
    // N17：结构体名不能占用语言自带的类型名。报错后不登记，避免污染
    // type_namespace。
    if (kBuiltinTypes.count(s.name)) {
        diags_.error(s.span, "protected type name: " + s.name);
        return;
    }
    // N2：结构体名在类型名空间里必须唯一。保留先登记的那个，后一个不覆盖。
    if (type_namespace.count(s.name)) {
        diags_.error(s.span, "duplicate struct: " + s.name);
        return;
    }
    // N4：同一结构体内字段名必须唯一。StructField
    // 没有span，用字段类型节点的位置。
    std::set<std::string> seen_fields;
    for (const auto &f : s.fields) {
        if (!seen_fields.insert(f.name).second) {
            const Span at = f.type ? f.type->span : s.span;
            diags_.error(at, "duplicate field: " + f.name);
        }
    }
    // D1：derive 列表去重后若长度变小，说明同一 trait 被请求了多次。
    // AstBuilder 已把多个 #[derive(..)] 合并进s.derives
    std::set<Derive> derives;
    for (Derive d : s.derives) {
        if (!derives.insert(d).second)
            diags_.error(s.span, std::string("repeated derive: ") + name(d));
    }
    // 建档案并登记。字段/derive 有错时仍登记，让后续 Phase 能继续解析。
    auto sym = std::make_unique<StructSymbol>();
    sym->name = s.name;
    sym->span = s.span;
    sym->node = &s;
    for (const auto &f : s.fields) {
        sym->fields.push_back(
            FieldDecl{f.name, f.type.get(), f.type ? f.type->span : s.span});
    }
    sym->derives = std::move(derives);
    type_namespace[sym->name] = sym.get();
    structs_.push_back(std::move(sym));
}

// 输入：`const MAX: i32 = 10;` 的 AST 节点
// 效果：成功 → func_const_namespace["MAX"] = 新 ConstSymbol；名字非法 →
// 只追加诊断，不登记。 例：第二个 `const MAX` → 报 N1；`const get_i32` → 报
// N18。
void SemaAnalyzer::collect_const(const Const &c) {
    // N18：内置 I/O 名是受保护值名，用户常量不能占用。
    // 必须先于 N1 检查：这三个名字已经预登记在 func_const_namespace 中。
    if (kBuiltinValueNames.count(c.name)) {
        diags_.error(c.span, "protected value name: " + c.name);
        return;
    }
    // N1：函数与常量共用值名空间，名字必须唯一；保留先登记的那个。
    if (func_const_namespace.count(c.name)) {
        diags_.error(c.span, "duplicate value name: " + c.name);
        return;
    }
    // 登记档案：owner = nullptr 表示顶层常量。
    auto sym = std::make_unique<ConstSymbol>();
    sym->kind = Symbol::Kind::Const;
    sym->name = c.name;
    sym->span = c.span;
    sym->node = &c;
    sym->owner = nullptr;
    func_const_namespace[sym->name] = sym.get();
    consts_.push_back(std::move(sym));
}

// 输入：`fn f(a: i32) {}` 的 AST 节点
// 效果：成功 → func_const_namespace["f"] = 新 FnSymbol；名字非法 →
// 只追加诊断，不登记。 例：`fn f(a: i32, a: i32)` → 报 N5；顶层 `fn f(&self)` →
// 报 N8；
void SemaAnalyzer::collect_function(const Function &f) {
    // N18：内置 I/O 名是受保护值名，用户函数不能占用。
    if (kBuiltinValueNames.count(f.name)) {
        diags_.error(f.span, "protected value name: " + f.name);
        return;
    }
    // N1：函数与常量共用值名空间，名字必须唯一。
    if (func_const_namespace.count(f.name)) {
        diags_.error(f.span, "duplicate value name: " + f.name);
        return;
    }
    // N5：参数名必须唯一。
    check_param_names(f.params, f.span);
    // N8：顶层函数不能带接收者（self 只允许出现在 impl 方法里）。
    if (f.self_param)
        diags_.error(f.span,
                     "top-level function cannot have a receiver: " + f.name);

    // 参数/接收者有错也登记，避免后续按名字找不到函数而连锁报错。
    auto sym = std::make_unique<FnSymbol>();
    sym->kind = Symbol::Kind::Fn;
    sym->name = f.name;
    sym->span = f.span;
    sym->node = &f;
    sym->owner = nullptr;
    func_const_namespace[sym->name] = sym.get();
    fns_.push_back(std::move(sym));
}

// 输入：函数参数列表 + 出错时回退的位置
// 参数名重复时追加 N5 诊断。Param 没有 span，用参数类型节点的位置。
void SemaAnalyzer::check_param_names(const std::vector<Param> &params,
                                     Span sp) {
    std::set<std::string> seen;
    for (const auto &p : params) {
        if (!seen.insert(p.name).second) {
            const Span at = p.type ? p.type->span : sp;
            diags_.error(at, "duplicate parameter: " + p.name);
        }
    }
}

// 输入：`impl S` 的 self_type 节点
// 输出：S 对应的 StructSymbol；不是用户结构体或带类型实参时追加诊断并返回
// nullptr。 例：`impl S` → S；`impl Box<i32>``impl [i32; 2]``impl &S` →
// G7；`impl S<i32>` → G1。
StructSymbol *SemaAnalyzer::resolve_impl_target(const Type &t) {
    const auto *pt = dynamic_cast<const PathType *>(&t);
    if (!pt || pt->segments.size() != 1) { // impl对象不唯一
        diags_.error(t.span, "impl target must be a user-defined struct");
        return nullptr;
    }
    const PathSegment &seg = pt->segments[0];
    const auto it = type_namespace.find(seg.name);
    if (it == type_namespace.end()) { // impl对象还未定义或者是自留类名
        diags_.error(t.span,
                     "impl target must be a user-defined struct: " + seg.name);
        return nullptr;
    }
    if (!seg.generics.empty()) { // impl对象带类型实参
        diags_.error(t.span, "structs have no type parameters: " + seg.name);
        return nullptr;
    }
    return it->second;
}

// 把 impl 的目标类型解析成已登记的结构体。
// 输入：`impl S { fn get(&self) {} }` 的 AST 节点
// 效果：成功 → S 的 assoc_items["get"] = FnSymbol；
// 目标不是用户结构体 → 报 G7，目标带类型实参 → G1，成员重名 → N6，方法参数重名
// → N5。 例：两个 impl 都定义 get → 报 N6；`impl i32 {}` → 报 G7。
void SemaAnalyzer::collect_impl(const InherentImpl &impl) {
    StructSymbol *target = resolve_impl_target(*impl.self_type);
    if (!target)
        return;

    for (const auto &item : impl.items) {
        // impl成员是函数还是常量
        if (const auto *fn = dynamic_cast<const Function *>(item.get())) {
            // N6：跨 impl 方法/关联函数/关联常量也不能重名。
            if (target->assoc_items.count(fn->name)) {
                diags_.error(fn->span,
                             "duplicate associated item: " + fn->name);
                continue; // 保留先登记的那个
            }
            // N5：方法参数名必须唯一。
            check_param_names(fn->params, fn->span);
            // 创建函数symbol
            auto sym = std::make_unique<FnSymbol>();
            sym->kind = Symbol::Kind::Fn;
            sym->name = fn->name;
            sym->span = fn->span;
            sym->node = fn;
            sym->owner = target;
            target->assoc_items[fn->name] = sym.get();
            fns_.push_back(std::move(sym));
        } else if (const auto *c = dynamic_cast<const Const *>(item.get())) {
            // N6：关联常量与同名方法/关联函数冲突。
            if (target->assoc_items.count(c->name)) {
                diags_.error(c->span, "duplicate associated item: " + c->name);
                continue;
            }
            // 创建常量symbol
            auto sym = std::make_unique<ConstSymbol>();
            sym->kind = Symbol::Kind::Const;
            sym->name = c->name;
            sym->span = c->span;
            sym->node = c;
            sym->owner = target;
            target->assoc_items[c->name] = sym.get();
            consts_.push_back(std::move(sym));
        }
    }
}

// 检查填好的 func_const_namespace；
// 例：没有 main → E1；`const main: i32 = 1;` → E1；`fn main(x: i32)` → E2；
//     `fn main<'a>()` → E3；`fn main() {}` → 无诊断。
void SemaAnalyzer::check_entry() {
    const auto it = func_const_namespace.find("main");
    const bool has_fn_main = it != func_const_namespace.end() &&
                             it->second->kind == Symbol::Kind::Fn;
    // E1：可执行程序必须有一个顶层 fn main。
    if (!has_fn_main) {
        diags_.error(Span{}, "an executable needs a top-level main");
        return;
    }
    const auto *fn = static_cast<const FnSymbol *>(it->second);
    // E2：main 不能有值参数。
    if (!fn->node->params.empty())
        diags_.error(fn->span, "main cannot have value parameters");
    // E3：main 不能有泛型/生命周期参数。
    if (fn->node->has_generic_params)
        diags_.error(fn->span, "main cannot have generic parameters");
}

// 驱动所有常量求值：consts_ 同时包含顶层常量和 impl 关联常量。
// evaluated_const 内部负责三色环检测、C1 比对与缓存，这里只负责遍历。
// 无引用者、只声明不求值的常量也会在这里被检查。
void SemaAnalyzer::evaluated_all_constants() {
    for (auto &sym : consts_)
        evaluated_const(*sym);
}

// 输入：path（常量路径段列表）、span（报错位置）、current（当前 impl 的
//       结构体；顶层常量为 nullptr）。解析到 ConstSymbol*
// 作用：把 `N`、`Type::N`、`Self::N` 解析成常量符号，只查名字不求值；
//       关联常量只查 assoc_items，不进入顶层值名空间（C7）。
// 例：`N` → 顶层 const N；`Config::N` → Config 的关联常量；
//     `Self::B` 在 impl S 内 → S::B；`fn_name` 或 `S::method` → C2。
ConstSymbol *
SemaAnalyzer::resolve_const_path(const std::vector<PathSegment> &path,
                                 Span span, const StructSymbol *current) {
    // 常量路径只允许 1 段或 2 段；0/>2 段都不可能是常量项。
    if (path.empty() || path.size() > 2) {
        diags_.error(span, "constant path must resolve to a constant");
        return nullptr;
    }

    const PathSegment &first = path[0];
    if (path.size() == 1) {
        // 顶层常量不能带类型实参；带实参说明不是常量项。
        if (!first.generics.empty()) {
            diags_.error(span, "constant path must resolve to a constant");
            return nullptr;
        }
        const auto it = func_const_namespace.find(first.name);
        // 名字不存在，或名字存在但是函数/内置 I/O
        if (it == func_const_namespace.end() ||
            it->second->kind != Symbol::Kind::Const) {
            // C2：未找到，或找到函数/内置 I/O（都不是常量）。
            diags_.error(span, "constant path must resolve to a constant");
            return nullptr;
        }
        return static_cast<ConstSymbol *>(it->second);
    }

    // 2 段：先解析前缀结构体，再查 associated const。
    const StructSymbol *owner = nullptr;
    if (first.name == "Self") {
        // N10：Self 只在结构体/impl 的上下文中有目标。
        if (!current) {
            diags_.error(span,
                         "Self is not available outside a struct or impl");
            return nullptr;
        }
        if (!first.generics.empty()) {
            // G1：Self不接受类型实参。
            diags_.error(span, "structs have no type parameters: Self");
            return nullptr;
        }
        owner = current;
    } else {  // 前缀不是 `Self`
        // 已登记的用户结构体才能有 associated const。
        const auto it = type_namespace.find(first.name);
        if (it != type_namespace.end()) {  // 在type名字空间中找到
            if (!first.generics.empty()) {
                // G1：用户结构体不接受类型实参。
                diags_.error(span,
                             "structs have no type parameters: " + first.name);
                return nullptr;
            }
            owner = it->second;
        } else {
            // 带类型实参的基本类型（如 i32<bool>::X）是 G3；
            if (!first.generics.empty() &&
                !basic_type_from_name(first.name).is_error()) {
                diags_.error(span, "primitives take no type parameters: " +
                                       first.name);
            } else { //// Box/Vec 或其他非结构体前缀没有 associated const， C2。
                diags_.error(span, "constant path must resolve to a constant");
            }
            return nullptr;
        }
    }

    // associated constant 本身不接受类型实参。
    const PathSegment &second = path[1];
    if (!second.generics.empty()) {
        diags_.error(span, "constant path must resolve to a constant");
        return nullptr;
    }
    const auto it = owner->assoc_items.find(second.name);
    // 联合条件：成员不存在，或成员存在但它是方法/关联函数而不是常量。
    if (it == owner->assoc_items.end() ||
        it->second->kind != Symbol::Kind::Const) {
        // C2：不存在，或成员是方法/关联函数。
        diags_.error(span, "constant path must resolve to a constant");
        return nullptr;
    }
    return static_cast<ConstSymbol *>(it->second);
}

// 输入：type（语法类型节点）、current（Self 所属的结构体，可为 nullptr）。
// 输出：归一化后的 SemaType；解析失败统一返回 SemaType::error()。
// 作用：把基本类型、Unit、引用、数组、Box/Vec、用户结构体和 Self 转换成语义
//       类型；数组长度在这里按 usize 常量求值，保证 [i32; N] 与 [i32; 3] 等价。
// 例：`usize` → usize；`&mut S` → &mut S；`[i32; 3]` → [i32; 3]；
//     `Box<Vec<i32>>` → Box<Vec<i32>>；无 current 的 `Self` → N10。
SemaType SemaAnalyzer::resolve_type(const Type &type,
                                    const StructSymbol *current) {
    // `()`：函数/控制流结果，也是省略返回类型时的类型。
    if (dynamic_cast<const UnitType *>(&type))
        return SemaType::unit();

    if (const auto *pt = dynamic_cast<const PathType *>(&type)) {
        // 类型位置没有 associated type；多段路径（如 S::T）无法解析。
        if (pt->segments.size() != 1) {
            diags_.error(type.span, "unresolved type: " + pt->name());
            return SemaType::error();
        }

        const PathSegment &seg = pt->segments[0];

        // Self 只表示当前结构体/impl 目标
        if (seg.name == "Self") {
            // self无指代
            if (!current) {
                diags_.error(
                    type.span,
                    "Self is not available outside a struct or impl");
                return SemaType::error();
            }
            // self不能有实参
            if (!seg.generics.empty()) {
                diags_.error(type.span,
                             "structs have no type parameters: Self");
                return SemaType::error();
            }
            return SemaType::struct_(current);
        }

        // 基本类型不接受类型实参。
        const SemaType basic = basic_type_from_name(seg.name);
        if (!basic.is_error()) {
            if (!seg.generics.empty()) {
                diags_.error(type.span,
                             "primitives take no type parameters: " +
                                 seg.name);
                return SemaType::error();
            }
            return basic;
        }

        // Box/Vec 恰好一个具体类型实参
        if (seg.name == "Box" || seg.name == "Vec") {
            if (seg.generics.size() != 1) {
                diags_.error(
                    type.span,
                    seg.name + " requires exactly one concrete type argument");
                return SemaType::error();
            }
            if (!seg.generics[0])
                return SemaType::error();
            // 从typeAST节点得到sematype
            const SemaType inner = resolve_type(*seg.generics[0], current);
            if (inner.is_error())
                return SemaType::error();
            return seg.name == "Box" ? SemaType::boxed(inner)
                                     : SemaType::vec(inner);
        }

        // 用户结构体：名义类型，不接受类型实参。
        const auto it = type_namespace.find(seg.name);
        if (it == type_namespace.end()) {  // 没找到
            diags_.error(type.span, "unresolved type: " + seg.name);
            return SemaType::error();
        }
        if (!seg.generics.empty()) { // 不能有实参
            diags_.error(type.span,
                         "structs have no type parameters: " + seg.name);
            return SemaType::error();
        }
        return SemaType::struct_(it->second);
    }

    // &T / &mut T
    if (const auto *rt = dynamic_cast<const ReferenceType *>(&type)) {
        if (!rt->inner)
            return SemaType::error();
        const SemaType inner = resolve_type(*rt->inner, current);
        if (inner.is_error())
            return SemaType::error();
        return SemaType::ref(rt->is_mut, inner);
    }

    // [T; N]：元素递归解析，长度按 usize 常量求值后存成 uint64_t。
    if (const auto *at = dynamic_cast<const ArrayType *>(&type)) {
        if (!at->element)
            return SemaType::error();
        const SemaType element = resolve_type(*at->element, current);
        if (element.is_error())
            return SemaType::error();

        const SemaType usize_ty = SemaType::int_(SemaType::IntKind::Usize);
        // 得到数组长度
        const EvaluatedConst length =
            evaluated_const_value(at->length, &usize_ty, current);
        // 求值器已报 C1/C2/C4/C5 等错误时不再重复报 C3。
        if (!length.ok || length.ty.is_error())
            return SemaType::error();
        if (!length.ty.equals(usize_ty)) {  //数组长度不是usize
            diags_.error(at->length.span,
                         "array length must be a usize constant");
            return SemaType::error();
        }
        // variant是否是uint_64
        const auto *len = std::get_if<uint64_t>(&length.value);
        if (!len) {
            diags_.error(at->length.span,
                         "array length must be a usize constant");
            return SemaType::error();
        }
        return SemaType::array(element, *len);
    }

    return SemaType::error();
}

// 根据ConstSymbol输出EvaluatedConst
// 成功时 ty/value 为初始化器结果，失败时ty=Error、value=monostate、ok=false。
// 作用：DFS求值常量并写缓存；求值时使用目标自己的声明类型，引用方的 expected 不传入。
// 例：`const A: i32 = B; const B: i32 = 3;` 求 A 时递归求 B；
//     `const A: i32 = A;` → C4 并把 A 缓存为 Error。
EvaluatedConst SemaAnalyzer::evaluated_const(ConstSymbol &sym) {
    // 已求过直接取缓存
    if (sym.state == ConstState::Visited)
        return sym.evaluated;
    // 还在当前递归栈上又被访问 → 形成常量依赖环（C4）。
    if (sym.state == ConstState::Visiting) {
        diags_.error(sym.span, "constant dependency cycle");
        return failed_evaluated();
    }

    // 先置 Visiting，再解析声明类型；这样数组长度里的自引用
    // （如 `const A: [i32; A]`）也会被 C4 捕获。
    sym.state = ConstState::Visiting;
    // AST 正常情况下由 builder 保证 node/type 非空；这里防御性缓存失败。
    if (!sym.node || !sym.node->type) {
        sym.state = ConstState::Visited;
        sym.evaluated = failed_evaluated();
        return sym.evaluated;
    }

    const SemaType declared = resolve_type(*sym.node->type, sym.owner);
    // 声明类型解析失败时已经报过 N10/N15/G1/G3/G4/G5 等诊断；
    // 统一缓存 Error，后续引用直接静默放行。
    if (declared.is_error()) {
        sym.state = ConstState::Visited;
        sym.evaluated = failed_evaluated();
        return sym.evaluated;
    }

    EvaluatedConst result =
        evaluated_const_value(sym.node->value, &declared, sym.owner);
    // 求值器没报错但结果类型与声明不一致（例如去掉路径期望传播后的
    // `const N: isize = -1;`）时，在单个常量内部补 C1，避免把错误结果
    // 发布给后续引用者。
    if (result.ok && !result.ty.is_error() && !result.ty.equals(declared)) {
        diags_.error(sym.node->value.span,
                     "constant initializer does not match declared type");
        result = failed_evaluated();
    }
    // 只有“求值成功且类型不是 Error”才保留结果；C1/C5 等失败统一缓存 Error。
    sym.state = ConstState::Visited;
    sym.evaluated =
        (result.ok && !result.ty.is_error()) ? result : failed_evaluated();
    return sym.evaluated;
}

// 输入：value（常量值 AST）、expected（期望类型，可为 nullptr）、
//       current（Self 所属结构体，供路径求值使用）。
// 输出：EvaluatedConst；成功 = 类型 + 值 + ok，失败 = Error/monostate/false。
// 求不依赖名字的常量值（整数、布尔、负数）；
// 常量路径委托给resolve_const_path/evaluated_const 递归处理。
// 例：`3u32` → {u32, 3, true}；`const N: i32 = -1;` 的 `-1` →
//     {i32, -1, true}；`true` 在 expected=i32 下 → C1；`-1u32` → C5；
//     `N` → 走 Path 分支。
EvaluatedConst
SemaAnalyzer::evaluated_const_value(const ConstValue &value,
                                    const SemaType *expected,
                                    const StructSymbol *current) {
    // 期望类型本身已经出错：不再追加新诊断，直接把失败向上传递。
    if (expected && expected->is_error())
        return failed_evaluated();

    switch (value.kind) {
        case ConstValue::Kind::Int: {
            SemaType ty = SemaType::error();
            if (value.suffix != IntSuffix::None) {
                // 有后缀时后缀优先；若与声明/上下文期望的类型不同 → C1。
                ty = type_from_suffix(value.suffix);
                if (expected && !expected->equals(ty)) {
                    diags_.error(value.span,
                                 "integer literal suffix does not match the "
                                 "expected type");
                    return failed_evaluated();
                }
            } else if (expected) {
                // 无后缀但有期望类型：只有期望是整数时才能直接采用；
                // 期望 bool/结构体等非整数 → C1。
                if (!expected->is_int()) {
                    diags_.error(value.span,
                                 "integer constant does not match the expected "
                                 "type");
                    return failed_evaluated();
                }
                ty = *expected;
            } else {
                // 无后缀且无期望类型：按 §10 规则回退 i32。
                ty = SemaType::int_(SemaType::IntKind::I32);
            }

            EvaluatedConst result;
            result.ty = ty;
            // 无符号整数存 uint64_t，有符号整数存 int64_t；这样负数也能
            // 保留符号，后续 C5/数组长度检查不会混淆符号性。
            if (ty.int_kind == SemaType::IntKind::U32 ||
                ty.int_kind == SemaType::IntKind::Usize)
                result.value = static_cast<uint64_t>(value.int_value);
            else
                result.value = value.int_value;
            result.ok = true;
            return result;
        }

        case ConstValue::Kind::Bool: {
            // 有期望类型时它必须是 bool；期望是整数/结构体等 → C1。
            if (expected && expected->kind != SemaType::Kind::Bool) {
                diags_.error(
                    value.span,
                    "boolean constant does not match the expected type");
                return failed_evaluated();
            }
            EvaluatedConst result;
            result.ty = SemaType::bool_();
            result.value = value.bool_value;
            result.ok = true;
            return result;
        }

        case ConstValue::Kind::Neg: {
            // 负数 AST 必须带 magnitude 子节点；缺失说明 AST 不完整。
            if (!value.operand)
                return failed_evaluated();
            // 规范 §12：期望类型不穿过一元负号，因此子节点传 nullptr。
            // 子节点会按自己的后缀/常量声明类型或 i32 回退求值。
            EvaluatedConst operand =
                evaluated_const_value(*value.operand, nullptr, current);
            // 子节点已经失败或类型是 Error：直接向上传递，避免重复诊断。
            if (!operand.ok || operand.ty.is_error())
                return failed_evaluated();
            // 只有 i32/isize 能取负；u32/usize/bool 等 → C5。
            if (!is_signed_int(operand.ty)) {
                diags_.error(
                    value.span,
                    "cannot negate an unsigned or non-integer constant");
                return failed_evaluated();
            }
            // 有符号整数应存 int64_t；variant 不是 int64_t 说明内部状态不一致。
            const auto *magnitude = std::get_if<int64_t>(&operand.value);
            if (!magnitude)
                return failed_evaluated();
            EvaluatedConst result;
            result.ty = operand.ty;
            result.value = -*magnitude;
            result.ok = true;
            return result;
        }

        case ConstValue::Kind::Path: {
            // 先做名字解析；失败时 resolve_const_path 已报 C2/N10/G1/G3。
            ConstSymbol *target =
                resolve_const_path(value.path, value.span, current);
            if (!target)
                return failed_evaluated();
            // 递归求目标常量
            const EvaluatedConst target_eval = evaluated_const(*target);
            // 目标求值失败（C1/C4/C5/C2 等）时，路径也返回失败，
            if (!target_eval.ok || target_eval.ty.is_error())
                return failed_evaluated();
            // 目标常量已在 evaluated_const 内用声明类型求值并通过 C1 比对，
            // 缓存里的 ty 就是目标声明类型；直接复用，不能再用调用方的
            // expected 重新定型目标常量。
            return target_eval;
        }
    }

    return failed_evaluated();
}

} // namespace rx
