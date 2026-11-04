// 根据已通过检查的语义结果生成 LLVM 模块。
// 当前只支持空源 main；其他尚未实现的函数会明确返回失败。

#pragma once

#include "ir/artifact.hpp"
#include <optional>

namespace llvm {
class raw_ostream;
}

namespace rx {

struct Crate;
struct SemanticResult;

// 调用方必须先成功完成整个 crate 的语义检查，且结果必须属于该 crate。
// 失败时写出诊断并返回空值，不交付部分模块。
std::optional<IRArtifact> generate_ir(const Crate &crate,
                                      const SemanticResult &semantics,
                                      llvm::raw_ostream &diagnostics);

} // namespace rx
