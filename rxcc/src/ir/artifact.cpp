#include "ir/artifact.hpp"
#include "ir/check.hpp"
#include "ir/printer.hpp"
#include <ostream>

namespace rx {

IRArtifact::IRArtifact(const std::string &name) : module_(std::make_unique<ir::Module>(name)) {}
IRArtifact::~IRArtifact() = default;
IRArtifact::IRArtifact(IRArtifact &&) noexcept = default;

bool IRArtifact::verify_and_print(std::ostream &output, std::ostream &diagnostics) const {
    if (!ir::check_module(*module_, diagnostics))
        return false;
    ir::print_module(*module_, output);
    if (!output) {
        diagnostics << "IR 输出流写入失败\n";
        return false;
    }
    return true;
}

} // namespace rx
