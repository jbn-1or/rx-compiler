#pragma once

#include "ir/value.hpp"

namespace rx {
enum class BinaryOperator;
enum class UnaryOperator;
}

namespace rx::ir {

// 操作数须已按源码顺序求值；这里不读取 AST、求值操作数或处理控制流。
// 返回值保留源类型，比较结果为 bool；无效输入诊断失败且不发射指令。
std::optional<IRValue> emit_scalar_binary(llvm::IRBuilderBase &builder,
                                          BinaryOperator operation,
                                          const IRValue &left, const IRValue &right,
                                          llvm::raw_ostream &diagnostics);
std::optional<IRValue> emit_scalar_unary(llvm::IRBuilderBase &builder,
                                         UnaryOperator operation, const IRValue &operand,
                                         llvm::raw_ostream &diagnostics);
std::optional<IRValue> emit_scalar_cast(llvm::IRBuilderBase &builder,
                                        const IRValue &operand,
                                        const SemanticType &target_type,
                                        llvm::raw_ostream &diagnostics);

} // namespace rx::ir
