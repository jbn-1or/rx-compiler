// 根据 AST 和语义分析结果生成 IR 模块，调用前必须完成整个程序的语义检查。
// 当前只生成空的、无参数、返回 unit 的普通函数，不支持的代码会报告错误。
// 另外生成机器入口 main，调用源程序的 main 后返回整数 0。
// 返回的模块独立保存所需数据，生成后不再依赖 AST 和语义结果。

#pragma once

#include "ir/artifact.hpp"
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace rx {

struct Crate;
struct SemanticResult;

// 为所有源函数（包括方法）生成 __rx_fn_<编号> 形式的名称，按 FunctionId.index 查询。
// 这里只命名，不生成函数体，也不代表已经支持方法；函数记录有误时返回空值。
std::optional<std::vector<std::string>> assign_function_names(
    const SemanticResult &semantics, std::ostream &diagnostics);

// 为已经通过语义检查的 crate 生成模块，semantics 必须是这个 crate 的检查结果。
// 不支持的代码或内部错误会输出原因并返回空值，不返回生成到一半的模块。
std::optional<IRArtifact> generate_ir(const Crate &crate,
                                      const SemanticResult &semantics,
                                      std::ostream &diagnostics);

} // namespace rx
