#include "sema/sema.hpp"

namespace rx {

namespace {

// 语言自带的类型名：结构体不能与它们重名
const std::set<std::string> kBuiltinTypes = {
    "i32", "u32", "isize", "usize", "bool", "Box", "Vec"};

} // namespace

// 输入：crate 先登记内置 I/O 名，再逐条登记顶层声明，最后检查 main；错误进 diags_。
// `struct S { x: i32 } fn main() {}`
//   → type_ns_ = {"S" → S}，value_ns_ = {"get_i32", "print_i32", "println_i32", "main"}
void SemaAnalyzer::run(const Crate &crate) {
    init_builtins();
    for (const auto &item : crate.items) {    // 第一遍：登记名字
        if (auto *s = dynamic_cast<Struct *>(item.get()))          collect_struct(*s);
        else if (auto *c = dynamic_cast<Const *>(item.get()))      collect_const(*c);
        else if (auto *f = dynamic_cast<Function *>(item.get()))   collect_function(*f);
        else if (auto *i = dynamic_cast<InherentImpl *>(item.get())) collect_impl(*i);
    }
    check_entry();
}

// 把内置 I/O 名 get_i32/print_i32/println_i32 以 Kind::Builtin 放进 value_ns_，
// 对象由 builtins_ 持有（value_ns_ 里只存指针）。
void SemaAnalyzer::init_builtins() {
    for (const char *n : {"get_i32", "print_i32", "println_i32"}) {
        auto sym = std::make_unique<Symbol>();
        sym->kind = Symbol::Kind::Builtin;
        sym->name = n;
        value_ns_[n] = sym.get();
        builtins_.push_back(std::move(sym));
    }
}

// 输入：`struct Point { x: i32 }` 的 AST 节点
// 效果：成功 → type_ns_["Point"] = 新 StructSymbol（fields 先存语法类型指针）；
//       名字非法 → 只追加诊断，不登记。
// 例：正常 Point → 登记；`struct i32 {}` → 报 N17；第二个 `struct Point {}` → 报 N2；
//     `struct P { x: i32, x: i32 }` → 报 N4；`#[derive(Clone, Clone)]` → 报 D1。
void SemaAnalyzer::collect_struct(const Struct &s) {
    // N17：结构体名不能占用语言自带的类型名。报错后不登记，避免污染 type_ns_。
    if (kBuiltinTypes.count(s.name)) {
        diags_.error(s.span, "protected type name: " + s.name);
        return;
    }
    // N2：结构体名在类型名空间里必须唯一。保留先登记的那个，后一个不覆盖。
    if (type_ns_.count(s.name)) {
        diags_.error(s.span, "duplicate struct: " + s.name);
        return;
    }

    // N4：同一结构体内字段名必须唯一。StructField 没有 span，用字段类型节点的位置。
    std::set<std::string> seen_fields;
    for (const auto &f : s.fields) {
        if (!seen_fields.insert(f.name).second) {
            const Span at = f.type ? f.type->span : s.span;
            diags_.error(at, "duplicate field: " + f.name);
        }
    }

    // D1：derive 列表去重后若长度变小，说明同一 trait 被请求了多次。
    // AstBuilder 已把多个 #[derive(..)] 合并进 s.derives，因此本检查同时覆盖「同一属性内重复」与「跨属性重复」。
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
    type_ns_[sym->name] = sym.get();
    structs_.push_back(std::move(sym));
}

// 输入：`const MAX: i32 = 10;` 的 AST 节点
// 效果：成功 → value_ns_["MAX"] = 新 ConstSymbol；与已有值名冲突 → 报 N1/N18。
// 第二个 `const MAX` → 报 N1；`const get_i32` → 报 N18。
// TODO(1.3)：实现 N18/N1 与 ConstSymbol 登记。
void SemaAnalyzer::collect_const(const Const &c) { (void)c; }

// 输入：`fn f(a: i32) {}` 的 AST 节点
// 效果：成功 → value_ns_["f"] = 新 FnSymbol；与已有值名冲突 → 报 N1/N18。
// `fn f(a: i32, a: i32)` → 报 N5；顶层 `fn f(&self)` → 报 N8。
// TODO(1.3)：实现 N18/N1/N5/N8 与 FnSymbol 登记。
void SemaAnalyzer::collect_function(const Function &f) { (void)f; }

// 输入：`impl S { fn get(&self) {} }` 的 AST 节点
// 效果：成功 → S 的 assoc_items["get"] = FnSymbol；
//       目标不是用户结构体 → 报 G7，目标带类型实参 → 报 G1，成员重名 → 报 N6。
// 两个 impl 都定义 get → 报 N6；`impl i32 {}` → 报 G7。
// TODO(1.5)：实现 G7/G1/G8/N6/N5 与关联项登记。
void SemaAnalyzer::collect_impl(const InherentImpl &impl) { (void)impl; }

// 输入：已填好的 value_ns_；输出：无返回值；不合法时追加 E1–E3 诊断。
// 没有 main → E1；`fn main(x: i32)` → E2；`fn main<T>()` → E3；正常 main → 无诊断。
// TODO(1.4)：实现 E1/E2/E3。
void SemaAnalyzer::check_entry() {}

} // namespace rx
