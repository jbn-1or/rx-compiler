// 按函数和基本块的添加顺序输出 LLVM 文本，不读取 AST，也不修改指令。
// 调用前必须检查模块；值用 %v_<编号> 命名，块用 bb_<编号> 命名，避免重名。

#include "ir/printer.hpp"
#include <locale>
#include <ostream>
#include <sstream>
#include <utility>

namespace rx::ir {
namespace {

// 只输出操作数的值，用于同一条指令只打印一次类型的位置。
void value(std::ostream &output, const Operand &operand) {
    // 有值 ID 时输出 SSA 名称，否则输出整数常量。
    if (const auto *value_id = operand.value_id()) {
        output << "%v_" << value_id->index;
    } else {
        const auto bits = *operand.constant_bits();
        // i1 常量按 LLVM 的 true/false 输出。
        if (operand.type == Type::I1) {
            output << (bits ? "true" : "false");
        // i8 常量按无符号字节输出。
        } else if (operand.type == Type::I8) {
            output << bits;
        // ptr 只有空指针常量，按 LLVM 的 null 输出。
        } else if (operand.type == Type::Ptr) {
            output << "null";
        } else {
            // i32 最高位为 1 时按负数输出，先转成 64 位再减 2^32，避免溢出。
            const int64_t number =
                bits >= 0x80000000u
                    ? static_cast<int64_t>(bits) - 0x100000000LL
                    : static_cast<int64_t>(bits);
            output << number;
        }
    }
}

// 输出操作数的类型和值，例如 i32 42 或 i32 %v_0。
void operand(std::ostream &output, const Operand &operand) {
    output << type_name(operand.type) << ' ';
    value(output, operand);
}

// 输出指令；有结果时先输出 %v_<编号> =。
void instruction(std::ostream &output, const Module &module,
                 const Instruction &instruction) {
    output << "  ";
    // 有结果的指令先打印 SSA 名字，再打印指令本身。
    if (instruction.result)
        output << "%v_" << instruction.result->index << " = ";
    // alloca：打印存储类型和对齐。
    if (const auto *allocation = std::get_if<Alloca>(&instruction.operation)) {
        output << "alloca "
               << storage_type_text(allocation->storage_type, module)
               << ", align " << allocation->alignment << '\n';
    // getelementptr：打印显式源存储类型、基址和全部 i32 下标。
    } else if (const auto *gep =
                   std::get_if<GetElementPtr>(&instruction.operation)) {
        output << "getelementptr "
               << storage_type_text(gep->source_type, module) << ", ";
        operand(output, gep->base);
        for (const auto &index : gep->indices) {
            output << ", ";
            operand(output, index);
        }
        output << '\n';
    // load：打印读取类型、地址和对齐。
    } else if (const auto *load = std::get_if<Load>(&instruction.operation)) {
        output << "load " << type_name(load->storage_type) << ", ";
        operand(output, load->address);
        output << ", align " << load->alignment << '\n';
    // store：打印写入值、目标地址和对齐。
    } else if (const auto *store = std::get_if<Store>(&instruction.operation)) {
        output << "store ";
        operand(output, store->value);
        output << ", ";
        operand(output, store->address);
        output << ", align " << store->alignment << '\n';
    // 二元运算：只打印普通指令，不附加 nsw/nuw/exact 等语义承诺。
    // 类型只打印一次，两个操作数用逗号分隔。
    } else if (const auto *binary =
                   std::get_if<BinaryOperation>(&instruction.operation)) {
        output << binary_op_name(binary->operation) << ' '
               << type_name(binary->left.type) << ' ';
        value(output, binary->left);
        output << ", ";
        value(output, binary->right);
        output << '\n';
    // 比较：先打印 icmp 谓词和操作数类型，再打印两个操作数。
    } else if (const auto *comparison =
                   std::get_if<Compare>(&instruction.operation)) {
        output << "icmp " << comparison_name(comparison->predicate) << ' '
               << type_name(comparison->left.type) << ' ';
        value(output, comparison->left);
        output << ", ";
        value(output, comparison->right);
        output << '\n';
    // zext：把较窄整数扩展成较宽整数。
    } else if (const auto *extension = std::get_if<ZExt>(&instruction.operation)) {
        output << "zext ";
        operand(output, extension->value);
        output << " to " << type_name(extension->target_type) << '\n';
    // trunc：把较宽整数截断成较窄整数。
    } else if (const auto *truncation = std::get_if<Trunc>(&instruction.operation)) {
        output << "trunc ";
        operand(output, truncation->value);
        output << " to " << type_name(truncation->target_type) << '\n';
    // call：打印返回类型、被调函数名和实参列表。
    } else {
        const auto &call = std::get<Call>(instruction.operation);
        const auto &function = module.function(call.function);
        output << "call " << type_name(function.abi.result) << " @"
               << function.name << '(';
        for (std::size_t argument_index = 0;
             argument_index < call.arguments.size(); ++argument_index) {
            // 只有第一个实参前不加分隔符。
            if (argument_index)
                output << ", ";
            operand(output, call.arguments[argument_index]);
        }
        output << ")\n";
    }
}

} // namespace

// 依次输出目标配置、函数和基本块，顺序与添加时一致，不使用内存地址命名。
std::string module_text(const Module &module) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << "target datalayout = \"" << module.data_layout << "\"\n"
        << "target triple = \"" << module.target_triple << "\"\n\n";
    // 先输出模块登记的命名结构体类型，再输出函数；字段顺序与声明顺序一致。
    for (const auto &type : module.named_types()) {
        output << '%' << type.name << " = type ";
        if (type.fields.empty()) {
            output << "{}\n\n";
            continue;
        }
        output << "{ ";
        for (std::size_t index = 0; index < type.fields.size(); ++index) {
            if (index)
                output << ", ";
            output << storage_type_text(type.fields[index], module);
        }
        output << " }\n\n";
    }
    for (const auto &owned : module.functions()) {
        const auto &function = *owned;
        const auto &abi = function.abi;
        output << (function.is_declaration() ? "declare " : "define ");
        // 内部源函数加 internal，外部声明和机器入口不加。
        if (function.linkage == Linkage::Internal)
            output << "internal ";
        output << type_name(abi.result) << " @" << function.name
            << '(';
        for (std::size_t parameter_index = 0;
             parameter_index < abi.parameters.size(); ++parameter_index) {
            // 只有第一个参数前不加分隔符。
            if (parameter_index)
                output << ", ";
            // 函数声明只打印参数类型，函数定义还要打印参数名，供函数体引用。
            if (function.is_declaration())
                output << type_name(abi.parameters[parameter_index].type);
            else
                operand(output, function.argument(parameter_index));
        }
        output << ')';
        // 只有声明时到此结束，不打印块和结尾花括号。
        if (function.is_declaration()) {
            output << "\n\n";
            continue;
        }
        output << " {\n";
        // 每个块都打印标签，包括唯一的入口块；先打印普通指令，最后打印终结指令。
        for (const auto &block : function.blocks) {
            output << "bb_" << block.id.index << ":\n";
            // 入口分配只在第一个块开头输出，不属于任何普通指令位置。
            if (block.id.index == 0)
                for (const auto &allocation : function.entry_allocas)
                    instruction(output, module, allocation);
            for (const auto &operation : block.instructions)
                instruction(output, module, operation);
            const auto &terminator = *block.terminator;
            // ret 打印返回值；跳转打印目标标签，condbr 先打印 i1 条件。
            if (const auto *return_instruction =
                    std::get_if<Return>(&terminator)) {
                output << "  ret ";
                // 有返回值时打印操作数，裸返回打印 void。
                if (return_instruction->value)
                    operand(output, *return_instruction->value);
                else
                    output << "void";
            } else if (const auto *branch =
                           std::get_if<Branch>(&terminator)) {
                output << "  br label %bb_" << branch->target.index;
            } else {
                const auto &conditional = std::get<CondBranch>(terminator);
                output << "  br ";
                operand(output, conditional.condition);
                output << ", label %bb_" << conditional.when_true.index
                       << ", label %bb_" << conditional.when_false.index;
            }
            output << '\n';
        }
        output << "}\n\n";
    }
    return std::move(output).str();
}

} // namespace rx::ir
