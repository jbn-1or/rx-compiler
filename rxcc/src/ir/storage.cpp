#include "ir/storage.hpp"
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

namespace rx::ir {

std::optional<Place> FunctionStorage::allocate(const SemanticType &type,
                                               const std::string &name,
                                               llvm::raw_ostream &diagnostics) {
    if (function_.empty()) {
        diagnostics << "IR 内部错误：固定存储需要函数入口块\n";
        return std::nullopt;
    }
    auto &context = function_.getContext();
    auto *computed = value_type(context, type, diagnostics);
    if (!computed)
        return std::nullopt;
    auto *storage_type = type.kind == SemanticType::Kind::Bool
                             ? llvm::Type::getInt8Ty(context) : computed;
    const auto alignment = type.kind == SemanticType::Kind::Bool
                               ? llvm::Align(1) : llvm::Align(4);
    auto &entry = function_.getEntryBlock();
    auto insertion = entry.begin();
    while (insertion != entry.end() && llvm::isa<llvm::AllocaInst>(&*insertion))
        ++insertion;
    // 入口可已有 terminator；只在 alloca 前缀插入，且不触碰正文 builder。
    llvm::IRBuilder<> builder(context);
    builder.SetInsertPoint(&entry, insertion);
    auto *allocation = builder.CreateAlloca(storage_type, nullptr, name);
    allocation->setAlignment(alignment);
    return make_place(allocation, type, diagnostics);
}

std::optional<Place> FunctionStorage::create_local(LocalId id,
                                                   const SemanticType &type,
                                                   llvm::raw_ostream &diagnostics) {
    if (id == LocalId{} || locals_.contains(id)) {
        diagnostics << "IR 内部错误：LocalId 无效或已分配存储\n";
        return std::nullopt;
    }
    auto place = allocate(type, "local." + std::to_string(id.index) + ".slot", diagnostics);
    if (place)
        locals_.emplace(id, *place);
    return place;
}

const Place *FunctionStorage::local(LocalId id) const {
    const auto found = locals_.find(id);
    return found == locals_.end() ? nullptr : &found->second;
}

std::optional<Place> FunctionStorage::temporary(const Expression &site,
                                                const SemanticType &type,
                                                llvm::raw_ostream &diagnostics) {
    const auto found = temporaries_.find(&site);
    if (found != temporaries_.end()) {
        if (!found->second.source_type.same_type(type)) {
            diagnostics << "IR 内部错误：同一临时位置的源类型不一致\n";
            return std::nullopt;
        }
        return found->second;
    }
    auto place = allocate(type, "temporary." + std::to_string(temporaries_.size()) + ".slot",
                          diagnostics);
    if (place)
        temporaries_.emplace(&site, *place);
    return place;
}

} // namespace rx::ir
