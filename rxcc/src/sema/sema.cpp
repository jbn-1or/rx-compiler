#include "sema/sema.hpp"

namespace rx {

namespace {

// 语言自带的类型名：结构体不能与它们重名
const std::set<std::string> kBuiltinTypes = {"i32",  "u32", "isize", "usize",
                                             "bool", "Box", "Vec"};

// 内置 I/O 函数名：不能再拿它们当顶层值名（N18）
const std::set<std::string> kBuiltinValueNames = {"get_i32", "print_i32",
                                                  "println_i32"};

} // namespace

// 先登记内置 I/O 名，再逐条登记顶层声明，最后检查 main；错误进 diags_。
// `struct S { x: i32 } fn main() {}`
//   → type_namespace = {"S" → S}，func_const_namespace = {kBuiltinValueNames, "main"}
void SemaAnalyzer::run(const Crate &crate) {
    init_builtins();
    for (const auto &item : crate.items) { // 第一遍：登记名字
        if (auto *s = dynamic_cast<Struct *>(item.get()))
            collect_struct(*s);
        else if (auto *c = dynamic_cast<Const *>(item.get()))
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

// 输入：`impl S { fn get(&self) {} }` 的 AST 节点
// 效果：成功 → S 的 assoc_items["get"] = FnSymbol；
//       目标不是用户结构体 → 报 G7，目标带类型实参 → 报 G1，成员重名 → 报 N6。
// 两个 impl 都定义 get → 报 N6；`impl i32 {}` → 报 G7。
// TODO(1.5)：实现 G7/G1/G8/N6/N5 与关联项登记。
void SemaAnalyzer::collect_impl(const InherentImpl &impl) {
    (void)impl;
}

// 输入：已填好的 func_const_namespace；输出：无返回值；不合法时追加 E1–E3 诊断。
// 没有 main → E1；`fn main(x: i32)` → E2；`fn main<T>()` → E3；正常 main →
// 无诊断。
// TODO(1.4)：实现 E1/E2/E3。
void SemaAnalyzer::check_entry() {}

} // namespace rx
