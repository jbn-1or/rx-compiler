#pragma once
#include <iosfwd>

#include "ast/ast.hpp"

namespace rx {

// 将表达式及其后代打印到 os，每层缩进两个空格。
// 输出是便于调试的 AST 摘要，不包含 span、预留语义槽位或所有语法细节。
void dump(Expression &expression, std::ostream &os);
// 先打印 Crate，再按声明顺序打印各 item 及其后代。
void dump(Crate &crate, std::ostream &os);

} // namespace rx
