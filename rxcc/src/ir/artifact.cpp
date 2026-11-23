// 创建和释放 IR 模块，并实现先检查模块、再输出 LLVM 文本的过程。
// 这里只调用自己的检查器和输出器，不启动外部 LLVM 工具。

#include "ir/artifact.hpp"
#include "ir/check.hpp"
#include "ir/printer.hpp"
#include <ostream>

namespace rx {

// 创建模块，供生成器通过 module() 添加函数和指令。
IRArtifact::IRArtifact() : module_(std::make_unique<ir::Module>()) {}
// 释放保存的模块，以及模块中的函数、基本块和指令。
IRArtifact::~IRArtifact() = default;
// 将模块指针移交给新对象，不改变模块地址，也不重新分配任何 ID。
IRArtifact::IRArtifact(IRArtifact &&) noexcept = default;

// 校验失败不交付文本；正常情况下只生成一份完整字符串。
std::optional<std::string> IRArtifact::verified_text(std::ostream &diagnostics) const {
    if (!ir::check_module(*module_, diagnostics))
        return std::nullopt;
    try {
        return ir::module_text(*module_);
    } catch (const std::ios_base::failure &error) {
        diagnostics << "IR 文本生成失败：" << error.what() << '\n';
        return std::nullopt;
    }
}

// 使用非格式化写入，不受调用方设置的字段宽度或数字格式影响。
bool IRArtifact::verify_and_print(std::ostream &output,
                                  std::ostream &diagnostics) const {
    const auto text = verified_text(diagnostics);
    if (!text)
        return false;
    output.write(text->data(), static_cast<std::streamsize>(text->size()));
    // 输出流失败时可能已经写入部分文本；文件的刷新和关闭由调用方检查。
    if (!output) {
        diagnostics << "IR 输出流写入失败\n";
        return false;
    }
    return true;
}

} // namespace rx
