// 根据语义分析结果生成 IR 模块，调用前必须完成整个程序的语义检查。
// 普通函数支持标量、引用和按值聚合；尚未实现的源操作在对应的 lowering 入口报告。
// 另外生成机器入口 main，调用源程序的 main 后返回整数 0。
// 返回的模块独立保存所需数据，生成后不再依赖 AST 和语义结果。

#pragma once

#include "ir/artifact.hpp"
#include "sema/identity.hpp"
#include <iosfwd>
#include <optional>
#include <string>

namespace rx {

struct SemanticResult;

// 普通函数和方法都使用稳定的 FunctionId 命名，不依赖用户拼写。
std::string source_function_name(FunctionId id);

// 使用成功的语义检查结果生成模块，调用期间须保持原 Crate 存活，不重复检查语义事实。
// 不支持的代码或构建错误会输出原因并返回空值，不返回生成到一半的模块。
// 返回的模块仍可修改，使用前通过 verify_and_print 或 check_module 校验。
std::optional<IRArtifact> generate_ir(const SemanticResult &semantics,
                                      std::ostream &diagnostics);

} // namespace rx
