// 根据已通过检查的语义结果生成自有 LLVM IR 模块。
// P2 仅支持空的无参 unit 普通函数；机器 main 调用内部源 main 并返回 i32 0。
// 其他尚未实现的表达式或语句会明确返回失败。

#pragma once

#include "ir/artifact.hpp"
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace rx {

struct Crate;
struct SemanticResult;

// 为所有源函数（含方法）建立 FunctionId.index 到内部名的映射。
// 命名不依赖函数签名或函数体 lowering；错误的身份记录不交付命名表。
std::optional<std::vector<std::string>> assign_function_names(
    const SemanticResult &semantics, std::ostream &diagnostics);

// 调用方必须先成功完成整个 crate 的语义检查，且结果必须属于该 crate。
// 失败时写出诊断并返回空值，不交付部分模块。
std::optional<IRArtifact> generate_ir(const Crate &crate,
                                      const SemanticResult &semantics,
                                      std::ostream &diagnostics);

} // namespace rx
