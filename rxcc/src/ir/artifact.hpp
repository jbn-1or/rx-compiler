// 保存生成的 IR 模块，并在检查通过后将它输出为 LLVM 文本。
// 模块中的数据独立保存，生成完成后不需要继续保留 AST 和语义结果。
#pragma once

#include "ir/model.hpp"
#include <iosfwd>
#include <memory>

namespace rx {

// 保存一次生成得到的模块，负责释放模块，并提供检查和输出接口。
class IRArtifact {
public:
    // 创建模块，由 Module 构造函数分配模块编号。
    IRArtifact();
    ~IRArtifact();
    IRArtifact(const IRArtifact &) = delete;
    IRArtifact &operator=(const IRArtifact &) = delete;
    IRArtifact(IRArtifact &&) noexcept;
    IRArtifact &operator=(IRArtifact &&) = delete;

    // 返回保存的模块；生成器通过非 const 版本添加函数和指令。
    ir::Module &module() { return *module_; }
    const ir::Module &module() const { return *module_; }
    // 检查模块并输出 LLVM 文本；检查或写入失败时返回 false 并输出错误信息。
    // 检查失败时不会写入 output；写入中途失败时可能已写入部分文本。
    bool verify_and_print(std::ostream &output,
                          std::ostream &diagnostics) const;

private:
    std::unique_ptr<ir::Module> module_; // 保存并拥有模块；移动 IRArtifact 不会改变模块地址或 ID。
};

} // namespace rx
