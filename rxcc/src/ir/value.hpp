// 定义 IR 中的值和存储位置，并集中处理整数、bool 的读写表示。
// LLVM 指针只属于 IR 层，不进入语义分析结果。

#pragma once

#include "sema/type.hpp"
#include <llvm/Support/Alignment.h>
#include <optional>
#include <variant>

namespace llvm {
class IRBuilderBase;
class LLVMContext;
class Type;
class Value;
class raw_ostream;
} // namespace llvm

namespace rx::ir {

// IRValue 借用 LLVM 值，同时保留其对应的源语言类型。
struct IRValue {
    llvm::Value *value;
    SemanticType source_type;
};

struct Unit {};
struct NoContinuation {};

using EmitResult = std::variant<IRValue, Unit, NoContinuation>;

struct Place {
    llvm::Value *address;
    SemanticType source_type;
    llvm::Type *storage_type;
    llvm::Align alignment;
};

using PlaceResult = std::variant<Place, NoContinuation>;

// 当前支持四种 i32 语义整数及以 i1 计算、i8 存储的 bool。
// 其他源类型返回空值并写明原因，不猜测 opaque pointer 的指向类型。
llvm::Type *value_type(llvm::LLVMContext &context, const SemanticType &type,
                       llvm::raw_ostream &diagnostics);

std::optional<Place> make_place(llvm::Value *address,
                                const SemanticType &source_type,
                                llvm::raw_ostream &diagnostics);

// 存取要求 builder 指向未终结的块；否则报告内部错误且不发射指令。
// 失败不改变插入点，也不代替上层传播 NoContinuation。
std::optional<IRValue> load_value(llvm::IRBuilderBase &builder,
                                  const Place &place,
                                  llvm::raw_ostream &diagnostics);

bool store_value(llvm::IRBuilderBase &builder, const Place &place,
                 const IRValue &value, llvm::raw_ostream &diagnostics);

} // namespace rx::ir
