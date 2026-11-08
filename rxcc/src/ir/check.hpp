#pragma once
#include "ir/model.hpp"
#include <iosfwd>

namespace rx::ir {
// true 表示通过；P2 检查结构、签名、身份、块终结及 call 结果定义先于使用。
bool check_module(const Module &module, std::ostream &diagnostics);
}
