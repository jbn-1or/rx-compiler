#include "ir/printer.hpp"
#include <ostream>

namespace rx::ir {
namespace {

void operand(std::ostream &out, const Operand &value) {
    out << type_name(value.type) << ' ';
    if (const auto *id = value.value_id()) {
        out << "%v_" << id->index;
    } else {
        const auto bits = *value.constant_bits();
        out << (bits >= 0x80000000u ? static_cast<int64_t>(bits) - 0x100000000LL
                                  : static_cast<int64_t>(bits));
    }
}

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
