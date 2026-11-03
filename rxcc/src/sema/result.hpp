#pragma once

#include "sema/symbol.hpp"
#include <memory>
#include <vector>

namespace rx {

// Owns semantic symbols independently of the analyzer. Moving the result keeps
// symbol addresses stable, including pointers inside types and associated items.
// AST pointers are borrowed: the caller must keep the Crate alive while using
// this result. LLVM objects and generation-time mappings do not belong here.
struct SemanticResult {
    SemanticResult() = default;
    SemanticResult(const SemanticResult &) = delete;
    SemanticResult &operator=(const SemanticResult &) = delete;
    SemanticResult(SemanticResult &&) noexcept = default;
    SemanticResult &operator=(SemanticResult &&) noexcept = default;

    std::vector<std::unique_ptr<StructSymbol>> structs;
    std::vector<std::unique_ptr<FunctionSymbol>> functions;
    std::vector<std::unique_ptr<ConstantSymbol>> constants;
    std::vector<std::unique_ptr<Symbol>> builtins;
};

} // namespace rx
