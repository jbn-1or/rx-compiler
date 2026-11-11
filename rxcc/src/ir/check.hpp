// 检查模块中的函数、基本块和指令是否合法，不修改模块，也不调用 LLVM 工具。
// 发现错误时输出原因，调用方不应继续输出或使用这个模块。

#pragma once
#include "ir/model.hpp"
#include <iosfwd>

namespace rx::ir {
// 检查目标配置、名称、ID、类型及块末尾的结束指令，并确保值在使用前已定义。
// 全部通过返回 true，发现错误时输出错误信息并返回 false。
bool check_module(const Module &module, std::ostream &diagnostics);
}
