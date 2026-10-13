#pragma once
#include <iosfwd>

#include "ast/ast.hpp"

namespace rx {

// 调试打印：一行一节点、子节点缩进一级
void dump(Expression &expr, std::ostream &os);
void dump(Crate &crate, std::ostream &os); // 根容器：手动迭代 items

} // namespace rx
