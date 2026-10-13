#pragma once

#include "ast/ast.hpp"

#include <string>
#include <utility>
#include <vector>

namespace rx {

// 单条诊断：定位 + 消息。span 是出错点的最小区间（end 不含）。
struct Diagnostic {
    Span span;
    std::string message;
};

struct Diagnostics {
    std::vector<Diagnostic> items;

    // 记录一条错误。span 为空（Span{}）表示无源码定位的诊断。
    void error(Span span, std::string message) {
        items.push_back(Diagnostic{std::move(span), std::move(message)});
    }

    // true = 没有任何错误，可以进入下一阶段（构建 AST）。
    bool ok() const { return items.empty(); }
};

} // namespace rx
