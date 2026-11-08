// 拥有完整的自有 IR 模块；检查成功后才向输出流写入 LLVM 文本。
#pragma once

#include "ir/model.hpp"
#include <iosfwd>
#include <memory>
#include <string>

namespace rx {

class IRArtifact {
public:
    explicit IRArtifact(const std::string &name);
    ~IRArtifact();
    IRArtifact(const IRArtifact &) = delete;
    IRArtifact &operator=(const IRArtifact &) = delete;
    IRArtifact(IRArtifact &&) noexcept;
    IRArtifact &operator=(IRArtifact &&) = delete;

    ir::Module &module() { return *module_; }
    const ir::Module &module() const { return *module_; }
    // true 表示检查和写入成功；非法模块不会向 output 写入任何文本。
    bool verify_and_print(std::ostream &output, std::ostream &diagnostics) const;

private:
    std::unique_ptr<ir::Module> module_;
};

} // namespace rx
