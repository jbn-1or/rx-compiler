#pragma once
#include "ir/model.hpp"
#include <iosfwd>

namespace rx::ir {
// 只序列化已通过 check_module 的模块，不推导类型或改写指令。
void print_module(const Module &module, std::ostream &output);
}
