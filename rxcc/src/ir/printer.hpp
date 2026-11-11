// 将模块中保存的类型、函数、基本块和指令写成 LLVM 文本。
// 调用前必须检查模块，调用后由调用方检查输出流及文件关闭是否成功。

#pragma once
#include "ir/model.hpp"
#include <iosfwd>

namespace rx::ir {
// 将已通过 check_module 的模块写入输出流，不修改模块或重新推导类型。
void print_module(const Module &module, std::ostream &output);
}
