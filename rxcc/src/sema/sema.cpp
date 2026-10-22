#include "sema/sema.hpp"

namespace rx {

namespace {

// 语言自带的类型名：结构体不能与它们重名
const std::set<std::string> kBuiltinTypes = {"i32",  "u32", "isize", "usize",
                                             "bool", "Box", "Vec"};

// 内置 I/O 函数名：不能再拿它们当顶层值名（N18）
const std::set<std::string> kBuiltinValueNames = {"get_i32", "print_i32",
                                                  "println_i32"};

// 求值失败哨兵：ty 默认是 Error，value 默认是 monostate，ok = false。
EvaluatedConst failed_evaluated() { return EvaluatedConst{}; }

// 把整数字面量后缀映射成对应的语义整数类型。
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

// 是否是i32/isize
bool is_signed_int(const SemaType &ty) {
    return ty.kind == SemaType::Kind::Int &&
           (ty.int_kind == SemaType::IntKind::I32 ||
            ty.int_kind == SemaType::IntKind::Isize);
}

} // namespace

// 先登记内置 I/O 名，再逐条登记顶层声明，最后检查 main；错误进 diags_。
// `struct S { x: i32 } fn main() {}`
//   → type_namespace = {"S" → S}，func_const_namespace = {kBuiltinValueNames, "main"}
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
}

// 把内置 I/O 名 get_i32/print_i32/println_i32 以 Kind::Builtin 放进 func_const_namespace，
// 对象由 builtins_ 持有（func_const_namespace 里只存指针）。
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
// 成功 → type_namespace["Point"] = 新 StructSymbol；名字非法 → 只追加诊断，不登记。
void SemaAnalyzer::collect_struct(const Struct &s) {
    // N17：结构体名不能占用语言自带的类型名。报错后不登记，避免污染 type_namespace。
    if (kBuiltinTypes.count(s.name)) {
        diags_.error(s.span, "protected type name: " + s.name);
        return;
    }
    // N2：结构体名在类型名空间里必须唯一。保留先登记的那个，后一个不覆盖。
    if (type_namespace.count(s.name)) {
        diags_.error(s.span, "duplicate struct: " + s.name);
        return;
    }
    // N4：同一结构体内字段名必须唯一。StructField 没有span，用字段类型节点的位置。
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
// 效果：成功 → func_const_namespace["MAX"] = 新 ConstSymbol；名字非法 → 只追加诊断，不登记。
// 例：第二个 `const MAX` → 报 N1；`const get_i32` → 报 N18。
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
// 效果：成功 → func_const_namespace["f"] = 新 FnSymbol；名字非法 → 只追加诊断，不登记。
// 例：`fn f(a: i32, a: i32)` → 报 N5；顶层 `fn f(&self)` → 报 N8；
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
// 输出：S 对应的 StructSymbol；不是用户结构体或带类型实参时追加诊断并返回 nullptr。
// 例：`impl S` → S；`impl Box<i32>``impl [i32; 2]``impl &S` → G7；`impl S<i32>` → G1。
StructSymbol *SemaAnalyzer::resolve_impl_target(const Type &t) {
    const auto *pt = dynamic_cast<const PathType *>(&t);
    if (!pt || pt->segments.size() != 1) {  // impl对象不唯一
        diags_.error(t.span, "impl target must be a user-defined struct");
        return nullptr;
    }
    const PathSegment &seg = pt->segments[0];
    const auto it = type_namespace.find(seg.name);
    if (it == type_namespace.end()) {  // impl对象还未定义或者是自留类名
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
//       目标不是用户结构体 → 报 G7，目标带类型实参 → 报 G1，成员重名 → 报 N6，
//       方法参数重名 → 报 N5。
// 例：两个 impl 都定义 get → 报 N6；`impl i32 {}` → 报 G7。
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
                diags_.error(c->span,
                             "duplicate associated item: " + c->name);
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
    const bool has_fn_main =
        it != func_const_namespace.end() && it->second->kind == Symbol::Kind::Fn;
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

// value要求值的ast节点，expected期望的类型，current所在结构体
// 例：`3u32` → {u32, 3, true}；`const N: i32 = -1;` 的 `-1` → {i32, -1, true}；
//     `true` 在期望 i32 的位置 → C1；`-1u32` → C5。Path 分支留给 2.3。
EvaluatedConst SemaAnalyzer::evaluated_const_value(const ConstValue &value,
                                                   const SemaType *expected,
                                                   const StructSymbol *current) {
    // 期望类型已经出错时不再追加诊断
    if (expected && expected->is_error())
        return failed_evaluated();

    switch (value.kind) {
        case ConstValue::Kind::Int: {
            SemaType ty = SemaType::error();
            if (value.suffix != IntSuffix::None) {
                // 有后缀时后缀优先；与声明/上下文的期望类型冲突 → C1。
                ty = type_from_suffix(value.suffix);
                if (expected && !expected->equals(ty)) {
                    diags_.error(value.span,
                                 "integer literal suffix does not match the "
                                 "expected type");
                    return failed_evaluated();
                }
            } else if (expected) {
                // 无后缀，有期望的整数类型；非整数期望（如 bool）→ C1。
                if (!expected->is_int()) {
                    diags_.error(value.span,
                                 "integer constant does not match the expected "
                                 "type");
                    return failed_evaluated();
                }
                ty = *expected;
            } else {
                // 无后缀无期望类型时回退 i32（§10 整数文字规则）。
                ty = SemaType::int_(SemaType::IntKind::I32);
            }

            EvaluatedConst result;
            result.ty = ty;
            if (ty.int_kind == SemaType::IntKind::U32 ||
                ty.int_kind == SemaType::IntKind::Usize)
                result.value = static_cast<uint64_t>(value.int_value);
            else
                result.value = value.int_value;
            result.ok = true;
            return result;
        }

        case ConstValue::Kind::Bool: {
            // 期望不是bool类型
            if (expected && expected->kind != SemaType::Kind::Bool) {
                diags_.error(value.span,
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
            if (!value.operand)  // 操作数为空
                return failed_evaluated();
            // 规范 §12：期望类型不穿过一元负号
            // 递归求-后部分，取无期望类型
            EvaluatedConst operand =
                evaluated_const_value(*value.operand, nullptr, current);
            if (!operand.ok || operand.ty.is_error()) // 子结果失败
                return failed_evaluated();
            if (!is_signed_int(operand.ty)) {
                // 对 u32/usize/bool 等取负 → C5。
                diags_.error(value.span,
                             "cannot negate an unsigned or non-integer constant");
                return failed_evaluated();
            }
            // 取出int64_t值并取负
            const auto *magnitude = std::get_if<int64_t>(&operand.value);
            if (!magnitude)
                return failed_evaluated();
            EvaluatedConst result;
            result.ty = operand.ty;
            result.value = -*magnitude;
            result.ok = true;
            return result;
        }

        case ConstValue::Kind::Path:
            // 常量路径解析（含前向引用与环检测）在 2.3 实现。
            return failed_evaluated();
    }

    return failed_evaluated();
}

} // namespace rx
