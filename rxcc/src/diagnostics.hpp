#pragma once

#include "ast/ast.hpp"

#include <string>
#include <utility>
#include <vector>

namespace rx {

// 单条诊断：源码区间 [start, end) 与错误消息；区间大小由报错处决定。
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

    // true 表示尚未记录错误；调用方据此决定是否继续下一阶段或接受程序。
    bool ok() const { return items.empty(); }
};

} // namespace rx
