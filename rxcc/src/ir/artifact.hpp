// 管理 LLVM context 和模块的共同生命周期，并在输出前检查模块。
// 这里只提供 IR 的基础设施，不读取源码，也不实现语义到 IR 的转换。

#pragma once

#include <memory>
#include <string>

namespace llvm {
class LLVMContext;
class Module;
class raw_ostream;
} // namespace llvm

namespace rx {

class IRArtifact {
public:
    explicit IRArtifact(const std::string &name);
    ~IRArtifact();
    IRArtifact(const IRArtifact &) = delete;
    IRArtifact &operator=(const IRArtifact &) = delete;
    IRArtifact(IRArtifact &&) noexcept;
    // 默认移动赋值会先释放旧 context，再释放仍引用它的旧 module。
    IRArtifact &operator=(IRArtifact &&) = delete;

    llvm::LLVMContext &context() const;
    llvm::Module &module() const;

    // 检查失败时把原因写入 diagnostics，不向 output 写入模块。
    // 返回 true 表示校验成功并已交给输出流；文件错误由调用方检查。
    bool verify_and_print(llvm::raw_ostream &output,
                          llvm::raw_ostream &diagnostics) const;

private:
    // 析构顺序与声明顺序相反：先释放 module，再释放 context。
    std::unique_ptr<llvm::LLVMContext> context_;
    std::unique_ptr<llvm::Module> module_;
};

} // namespace rx
