// 创建面向 RV32IM/ILP32 的 LLVM 模块，检查通过后输出文本 IR。

#include "ir/artifact.hpp"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Triple.h>

namespace rx {

IRArtifact::IRArtifact(const std::string &name)
    : context_(std::make_unique<llvm::LLVMContext>()),
      module_(std::make_unique<llvm::Module>(name, *context_)) {
    module_->setTargetTriple(llvm::Triple("riscv32-unknown-unknown-elf"));
    module_->setDataLayout("e-m:e-p:32:32-i64:64-n32-S128");
}

IRArtifact::~IRArtifact() = default;
IRArtifact::IRArtifact(IRArtifact &&) noexcept = default;

llvm::LLVMContext &IRArtifact::context() const { return *context_; }
llvm::Module &IRArtifact::module() const { return *module_; }

bool IRArtifact::verify_and_print(llvm::raw_ostream &output,
                                  llvm::raw_ostream &diagnostics) const {
    if (llvm::verifyModule(*module_, &diagnostics))
        return false;
    module_->print(output, nullptr);
    return true;
}

} // namespace rx
