// 将模块中保存的类型、函数、基本块和指令序列化为 LLVM 文本。

#pragma once
#include "ir/model.hpp"
#include <string>

namespace rx::ir {
// 只处理已通过 check_module 的模块，不修改模块或重新推导类型。
std::string module_text(const Module &module);
}
