#pragma once

#include "ast/ast.hpp"
#include "diagnostics.hpp"
#include "sema/type.hpp"

namespace rx {

// SEMANTIC 阶段分析器。
class SemaAnalyzer {
public:
    explicit SemaAnalyzer(Diagnostics &diags) : diags_(diags) {}

    // 对整棵 crate 做语义检查，诊断写入构造时传入的 Diagnostics。
    void run(const Crate &crate);

private:
    Diagnostics &diags_;
};

} // namespace rx
