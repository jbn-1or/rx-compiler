// 语义检查总入口。先读 run 的阶段顺序，再进入 declarations/ 和 body/。

#include "sema/body/checker.hpp"
#include "sema/sema.hpp"

namespace rx {

void SemanticAnalyzer::run(const Crate &crate) {
    // 预登记内置 I/O；先收集全部结构体，再收集常量、函数与 impl；
    // 建立全局名字表和关联项索引
    collect_declarations(crate);
    // 检查顶层 main 是否存在、是否为函数、有无值参数和泛型参数
    check_main_declaration();
    // 递归求值全部顶层/关联常量，把结果和求值状态缓存到 ConstantSymbol
    evaluate_all_constants();
    // 将字段、参数、返回类型从 AST 类型解析成 SemanticType，写入 StructSymbol / FunctionSymbol
    resolve_declared_types();
    // 利用定型后的签名检查 main 返回类型为 ()
    check_main_return_type();
    // 沿结构体和数组的按值字段检测无限布局环；引用、Box、Vec 打断内联路径
    check_inline_layout_cycles();
    // 检查 derive 的配套要求以及字段是否具有所需能力
    check_derives();
    // 遍历全部已登记函数，为每个函数创建独立的 BodyChecker
    check_function_bodies();
}

void SemanticAnalyzer::check_function_bodies() {
    // 每个函数使用独立的 BodyChecker：局部作用域和循环状态不跨函数共享。
    // 未调用、方法和关联函数也在 functions_ 中，一并检查。
    for (const auto &fn : functions_) {
        BodyChecker checker(*this);
        checker.check_function(*fn);
    }
}

} // namespace rx
