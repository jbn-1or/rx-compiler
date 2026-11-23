// 根据语义分析结果生成 IR 模块，调用前必须完成整个程序的语义检查。
// 普通函数支持标量、引用和按值聚合；尚未实现的源操作在对应的 lowering 入口报告。
// 另外生成机器入口 main，调用源程序的 main 后返回整数 0。
// 返回的模块独立保存所需数据，生成后不再依赖 AST 和语义结果。

#pragma once

#include "ir/artifact.hpp"
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace rx {

struct SemanticResult;

// 为所有源函数（方法）生成 __rx_fn_<编号> 名称，按 FunctionId.index查询。
// 这里只命名，不生成函数体，也不代表已经支持方法；语义结果必须完整有效。
std::vector<std::string> assign_function_names(const SemanticResult &semantics);

// 使用成功的语义检查结果生成模块，调用期间须保持原 Crate 存活，不重复检查语义事实。
// 不支持的代码或构建错误会输出原因并返回空值，不返回生成到一半的模块。
// 返回的模块仍可修改，使用前通过 verify_and_print 或 check_module 校验。
std::optional<IRArtifact> generate_ir(const SemanticResult &semantics,
                                      std::ostream &diagnostics);

} // namespace rx
