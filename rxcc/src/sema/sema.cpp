#include "sema/sema.hpp"

namespace rx {

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
// 正常 Point → 登记；`struct i32 {}` → 报 N17；第二个 `struct Point {}` → 报 N2。
// TODO(1.2)：实现 N17/N2/N4/D1 与 StructSymbol 登记。
void SemaAnalyzer::collect_struct(const Struct &s) { (void)s; }

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
