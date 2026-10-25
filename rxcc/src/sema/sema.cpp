// 编排语义检查流程：声明收集、入口检查、常量求值、签名定型、布局与 derive 检查，
// 最后检查所有函数体。
// 先收集所有结构体，再收集值声明和 impl，保证全局名称查找不受声明顺序影响。
// 各阶段的具体规则由对应模块实现。

#include "sema/body.hpp"
#include "sema/sema.hpp"

namespace rx {

// 编排 crate 级检查：声明收集 → 入口形态 → 常量 → 签名 → 布局与 derive →
// 函数体。
// 各 Pass 的实现按职责放在独立 .cpp。
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

    // 每个函数使用独立的 BodyChecker：局部作用域和循环状态不跨函数共享。
    // 未调用、方法和关联函数也在 functions_ 中，一并检查。
    for (const auto &fn : functions_) {
        BodyChecker checker(*this);
        checker.check_function(*fn);
    }
}

} // namespace rx
