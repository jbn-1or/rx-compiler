// 创建和释放 IR 模块，并实现先检查模块、再输出 LLVM 文本的过程。
// 这里只调用自己的检查器和输出器，不启动外部 LLVM 工具。

#include "ir/artifact.hpp"
#include "ir/check.hpp"
#include "ir/printer.hpp"
#include <ostream>

namespace rx {

// 创建指定名称的模块，供生成器通过 module() 添加函数和指令。
IRArtifact::IRArtifact(const std::string &name) : module_(std::make_unique<ir::Module>(name)) {}
// 释放保存的模块，以及模块中的函数、基本块和指令。
IRArtifact::~IRArtifact() = default;
// 将模块指针移交给新对象，不改变模块地址，也不重新分配任何 ID。
IRArtifact::IRArtifact(IRArtifact &&) noexcept = default;

// 检查模块并输出 LLVM 文本；检查或写入失败时输出错误信息并返回 false。
bool IRArtifact::verify_and_print(std::ostream &output, std::ostream &diagnostics) const {
    // 检查失败时立即返回，避免把不合法的模块写入输出流。
    if (!ir::check_module(*module_, diagnostics))
        return false;
    ir::print_module(*module_, output);
    // 输出流失败时可能已经写入部分文本；文件的刷新和关闭由调用方检查。
    if (!output) {
        diagnostics << "IR 输出流写入失败\n";
        return false;
    }
    return true;
}

} // namespace rx
