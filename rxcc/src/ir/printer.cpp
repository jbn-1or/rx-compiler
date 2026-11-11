// 按函数和基本块的添加顺序输出 LLVM 文本，不读取 AST，也不修改指令。
// 调用前必须检查模块；值用 %v_<编号> 命名，块用 bb_<编号> 命名，避免重名。

#include "ir/printer.hpp"
#include <ostream>

namespace rx::ir {
namespace {

// 输出操作数的类型和值，例如 i32 42 或 i32 %v_0。
void operand(std::ostream &out, const Operand &value) {
    out << type_name(value.type) << ' ';
    if (const auto *id = value.value_id()) {
        out << "%v_" << id->index;
    } else {
        // 最高位为 1 时按负数输出，先转成 64 位再减 2^32，避免 32 位计算溢出。
        const auto bits = *value.constant_bits();
        out << (bits >= 0x80000000u ? static_cast<int64_t>(bits) - 0x100000000LL
                                  : static_cast<int64_t>(bits));
    }
}

// 输出 call 指令，从模块中查询被调用函数的名称和返回类型。
// 有返回数据时先输出 %v_<编号> =，返回 void 时只输出调用本身。
void instruction(std::ostream &out, const Module &module, const Instruction &instruction) {
    out << "  ";
    if (instruction.result)
        out << "%v_" << instruction.result->index << " = ";
    const auto &call = std::get<Call>(instruction.operation);
    const auto &function = module.function(call.function);
    out << "call " << type_name(function.signature.result) << " @" << function.name << '(';
    for (std::size_t i = 0; i < call.arguments.size(); ++i) {
        if (i)
            out << ", ";
        operand(out, call.arguments[i]);
    }
    out << ")\n";
}

} // namespace

// 依次输出目标配置、函数和基本块，顺序与添加时一致，不使用内存地址命名。
void print_module(const Module &module, std::ostream &out) {
    out << "target datalayout = \"" << module.data_layout << "\"\n"
        << "target triple = \"" << module.target_triple << "\"\n\n";
    for (const auto &owned : module.functions()) {
        const auto &function = *owned;
        out << (function.is_declaration() ? "declare " : "define ");
        if (function.linkage == Linkage::Internal)
            out << "internal ";
        out << type_name(function.signature.result) << " @" << function.name << '(';
        for (std::size_t i = 0; i < function.parameters.size(); ++i) {
            if (i)
                out << ", ";
            // 函数声明只打印参数类型，函数定义还要打印参数名，供函数体引用。
            if (function.is_declaration())
                out << type_name(function.signature.parameters[i]);
            else
                operand(out, function.parameters[i]);
        }
        out << ')';
        if (function.is_declaration()) {
            out << "\n\n";
            continue;
        }
        out << " {\n";
        // 每个块都打印标签，包括唯一的入口块；先打印普通指令，最后打印 ret。
        for (const auto &block : function.blocks) {
            out << "bb_" << block.id.index << ":\n";
            for (const auto &operation : block.instructions)
                instruction(out, module, operation);
            const auto &ret = std::get<Return>(*block.terminator);
            out << "  ret ";
            if (ret.value)
                operand(out, *ret.value);
            else
                out << "void";
            out << '\n';
        }
        out << "}\n\n";
    }
}

} // namespace rx::ir
