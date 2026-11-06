// 实现源值与 LLVM 存储表示之间的映射，以及统一的 load/store 操作。

#include "ir/value.hpp"
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/raw_ostream.h>

namespace rx::ir {
namespace {

bool check_insertion_point(const llvm::IRBuilderBase &builder,
                           llvm::raw_ostream &diagnostics) {
    const auto *block = builder.GetInsertBlock();
    if (!block || block->getTerminator()) {
        diagnostics << "IR 内部错误：存取指令需要尚未终结的插入块\n";
        return false;
    }
    return true;
}

const char *source_kind_name(SemanticType::Kind kind) {
    switch (kind) {
        case SemanticType::Kind::Unit: return "()";
        case SemanticType::Kind::Never: return "!";
        case SemanticType::Kind::Error: return "错误类型";
        case SemanticType::Kind::Int: return "整数";
        case SemanticType::Kind::Bool: return "bool";
        case SemanticType::Kind::Struct: return "结构体";
        case SemanticType::Kind::Reference: return "引用";
        case SemanticType::Kind::Array: return "数组";
        case SemanticType::Kind::Box: return "Box";
        case SemanticType::Kind::Vec: return "Vec";
    }
    return "未知类型";
}

} // namespace

llvm::Type *value_type(llvm::LLVMContext &context, const SemanticType &type,
                       llvm::raw_ostream &diagnostics) {
    if (type.kind == SemanticType::Kind::Int)
        return llvm::Type::getInt32Ty(context);
    if (type.kind == SemanticType::Kind::Bool)
        return llvm::Type::getInt1Ty(context);
    diagnostics << "当前 IR 存取尚不支持源类型 " << source_kind_name(type.kind) << '\n';
    return nullptr;
}

std::optional<Place> make_place(llvm::Value *address,
                                const SemanticType &source_type,
                                llvm::raw_ostream &diagnostics) {
    if (!address || !address->getType()->isPointerTy()) {
        diagnostics << "IR 内部错误：存储位置必须使用 LLVM 指针\n";
        return std::nullopt;
    }
    if (source_type.kind == SemanticType::Kind::Int)
        return Place{address, source_type, llvm::Type::getInt32Ty(address->getContext()),
                     llvm::Align(4)};
    if (source_type.kind == SemanticType::Kind::Bool)
        return Place{address, source_type, llvm::Type::getInt8Ty(address->getContext()),
                     llvm::Align(1)};
    diagnostics << "当前 IR 不支持为源类型 " << source_kind_name(source_type.kind)
                << " 建立存储位置\n";
    return std::nullopt;
}

std::optional<IRValue> load_value(llvm::IRBuilderBase &builder,
                                  const Place &place,
                                  llvm::raw_ostream &diagnostics) {
    if (!check_insertion_point(builder, diagnostics))
        return std::nullopt;
    if (!place.address || !place.address->getType()->isPointerTy() ||
        !place.storage_type) {
        diagnostics << "IR 内部错误：无效的存储位置\n";
        return std::nullopt;
    }
    const auto *expected_storage = place.source_type.kind == SemanticType::Kind::Int
                                       ? llvm::Type::getInt32Ty(place.address->getContext())
                                   : place.source_type.kind == SemanticType::Kind::Bool
                                       ? llvm::Type::getInt8Ty(place.address->getContext())
                                       : nullptr;
    if (!expected_storage || place.storage_type != expected_storage ||
        (place.source_type.kind == SemanticType::Kind::Int && place.alignment != llvm::Align(4)) ||
        (place.source_type.kind == SemanticType::Kind::Bool && place.alignment != llvm::Align(1))) {
        diagnostics << "IR 内部错误：存储位置的源类型、存储类型或对齐不一致\n";
        return std::nullopt;
    }
    auto *load = builder.CreateLoad(place.storage_type, place.address);
    load->setAlignment(place.alignment);
    llvm::Value *loaded = load;
    if (place.source_type.kind == SemanticType::Kind::Bool)
        loaded = builder.CreateTrunc(loaded, llvm::Type::getInt1Ty(place.address->getContext()));
    return IRValue{loaded, place.source_type};
}

bool store_value(llvm::IRBuilderBase &builder, const Place &place,
                 const IRValue &value, llvm::raw_ostream &diagnostics) {
    if (!check_insertion_point(builder, diagnostics))
        return false;
    if (!place.address || !place.address->getType()->isPointerTy() ||
        !place.storage_type || !value.value ||
        !place.source_type.same_type(value.source_type)) {
        diagnostics << "IR 内部错误：存入位置的值类型不一致\n";
        return false;
    }
    const auto *expected_value = place.source_type.kind == SemanticType::Kind::Int
                                     ? llvm::Type::getInt32Ty(place.address->getContext())
                                 : place.source_type.kind == SemanticType::Kind::Bool
                                     ? llvm::Type::getInt1Ty(place.address->getContext())
                                     : nullptr;
    const auto *expected_storage = place.source_type.kind == SemanticType::Kind::Int
                                       ? llvm::Type::getInt32Ty(place.address->getContext())
                                   : place.source_type.kind == SemanticType::Kind::Bool
                                       ? llvm::Type::getInt8Ty(place.address->getContext())
                                       : nullptr;
    const auto expected_alignment = place.source_type.kind == SemanticType::Kind::Int
                                        ? llvm::Align(4)
                                    : place.source_type.kind == SemanticType::Kind::Bool
                                        ? llvm::Align(1)
                                        : llvm::Align(1);
    if (!expected_value || place.storage_type != expected_storage ||
        place.alignment != expected_alignment || value.value->getType() != expected_value) {
        diagnostics << "IR 内部错误：存入位置的 LLVM 类型或对齐不一致\n";
        return false;
    }
    auto *stored = value.value;
    if (place.source_type.kind == SemanticType::Kind::Bool)
        stored = builder.CreateZExt(stored, place.storage_type);
    auto *instruction = builder.CreateStore(stored, place.address);
    instruction->setAlignment(place.alignment);
    return true;
}

} // namespace rx::ir
