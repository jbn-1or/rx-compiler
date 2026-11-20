// 检查模块的目标配置、函数和块的 ID、参数和返回类型，以及值是否先定义后使用。
// 除入口分配外，不允许直接使用其他块产生的值；不修改被检查的模块。

#include "ir/check.hpp"
#include <algorithm>
#include <ostream>

namespace rx::ir {
namespace {

// 输出带函数名的诊断信息，并返回 false，供检查函数在失败时直接返回。
bool report_error(std::ostream &diagnostics, const Function &function,
                  const char *message) {
    diagnostics << "IR 内部错误：" << function.name << ": " << message << '\n';
    return false;
}

// 检查值属于当前函数、类型匹配且未定义过，再将它标记为已定义。
bool define_value(const Function &function, std::vector<bool> &defined,
                  ValueId value_id, Type type) {
    // 类型必须已知且不是 void，值必须属于本函数、下标有效、未定义过且类型一致。
    if (type == Type::Void || !is_known_type(type) ||
        value_id.function != function.id || value_id.index >= defined.size() ||
        defined[value_id.index] ||
        function.value_types[value_id.index] != type)
        return false;
    defined[value_id.index] = true;
    return true;
}

// 常量可直接使用；SSA 值必须属于本函数，并且已经在当前块中可用。
bool value_available(const Function &function,
                     const std::vector<bool> &available,
                     const Operand &value) {
    if (!function.contains(value))
        return false;
    const auto *value_id = value.value_id();
    return !value_id || available[value_id->index];
}

// 判断值类型表中的某一项是否仍未标记为已定义。
bool is_undefined(bool defined) { return !defined; }

// 参数按声明顺序从 0 编号，它们在进入任何基本块之前就已经可用。
bool check_parameters(const Function &function, std::vector<bool> &defined,
                      std::ostream &diagnostics) {
    for (std::size_t parameter_index = 0;
         parameter_index < function.parameters.size(); ++parameter_index) {
        const auto &parameter = function.parameters[parameter_index];
        const auto *value_id = parameter.value_id();
        // 参数必须有值 ID，序号、类型和签名一致，并且是第一次定义。
        if (!value_id || value_id->index != parameter_index ||
            parameter.type !=
                function.signature.parameters[parameter_index] ||
            !define_value(function, defined, *value_id, parameter.type))
            return report_error(diagnostics, function,
                                "函数参数身份或类型无效");
    }
    return true;
}

// 没有函数体时，必须是外部函数声明，值类型表中也只能保存参数类型。
bool check_declaration(const Function &function,
                       const std::vector<bool> &defined,
                       std::ostream &diagnostics) {
    if (function.linkage != Linkage::External ||
        defined.size() != function.parameters.size() ||
        !function.entry_allocas.empty())
        return report_error(diagnostics, function,
                            "内部函数未完成定义或声明携带函数体数据");
    return true;
}

// 入口分配必须位于正文之前，存储类型和对齐合法，并且产生 ptr 结果。
bool check_entry_allocas(const Function &function,
                         std::vector<bool> &defined,
                         std::ostream &diagnostics) {
    for (const auto &instruction : function.entry_allocas) {
        const auto *allocation = std::get_if<Alloca>(&instruction.operation);
        if (!allocation ||
            !valid_storage(allocation->storage_type, allocation->alignment) ||
            instruction.type != Type::Ptr || !instruction.result ||
            !define_value(function, defined, *instruction.result, Type::Ptr))
            return report_error(diagnostics, function,
                                "入口 alloca 类型、对齐或结果无效");
    }
    return true;
}

// 检查一条普通指令的操作数、结果类型和结果值的定义状态。
bool check_instruction(const Module &module, const Function &function,
                       const Instruction &instruction,
                       std::vector<bool> &defined,
                       std::vector<bool> &available,
                       std::ostream &diagnostics) {
    if (const auto *call = std::get_if<Call>(&instruction.operation)) {
        if (!module.contains(call->function))
            return report_error(diagnostics, function,
                                "调用目标无效或不属于当前模块");
        const auto &signature = module.function(call->function).signature;
        // 实参数量、结果类型和“是否有结果值”必须与被调用签名一致。
        if (call->arguments.size() != signature.parameters.size() ||
            instruction.type != signature.result ||
            instruction.result.has_value() !=
                (signature.result != Type::Void))
            return report_error(diagnostics, function,
                                "call 的签名或结果不一致");
        for (std::size_t argument_index = 0;
             argument_index < call->arguments.size(); ++argument_index)
            if (!value_available(function, available,
                                 call->arguments[argument_index]) ||
                call->arguments[argument_index].type !=
                    signature.parameters[argument_index])
                return report_error(diagnostics, function,
                                    "call 操作数类型、归属或定义位置无效");
    } else if (const auto *load = std::get_if<Load>(&instruction.operation)) {
        // 读取类型和对齐合法，结果类型匹配，地址必须是可用的 ptr。
        if (!valid_storage(load->storage_type, load->alignment) ||
            instruction.type != load->storage_type ||
            !instruction.result || load->address.type != Type::Ptr ||
            !value_available(function, available, load->address))
            return report_error(diagnostics, function,
                                "load 类型、地址、对齐或结果无效");
    } else if (const auto *store = std::get_if<Store>(&instruction.operation)) {
        // 写入类型和对齐合法，store 不产生结果，值和地址类型必须匹配。
        if (!valid_storage(store->storage_type, store->alignment) ||
            instruction.type != Type::Void || instruction.result ||
            store->value.type != store->storage_type ||
            !value_available(function, available, store->value) ||
            store->address.type != Type::Ptr ||
            !value_available(function, available, store->address))
            return report_error(diagnostics, function,
                                "store 类型、地址、对齐或结果无效");
    } else if (const auto *extension = std::get_if<ZExt>(&instruction.operation)) {
        // zext 只允许较窄整数到较宽整数，输入属于当前块，结果类型等于目标类型。
        if (integer_width(extension->value.type) == 0 ||
            integer_width(extension->target_type) <=
                integer_width(extension->value.type) ||
            instruction.type != extension->target_type || !instruction.result ||
            !value_available(function, available, extension->value))
            return report_error(diagnostics, function,
                                "zext 类型、操作数或结果无效");
    } else if (const auto *truncation = std::get_if<Trunc>(&instruction.operation)) {
        // trunc 只允许较宽整数到较窄整数，输入属于当前块，结果类型等于目标类型。
        if (integer_width(truncation->value.type) == 0 ||
            integer_width(truncation->target_type) >=
                integer_width(truncation->value.type) ||
            instruction.type != truncation->target_type || !instruction.result ||
            !value_available(function, available, truncation->value))
            return report_error(diagnostics, function,
                                "trunc 类型、操作数或结果无效");
    } else {
        return report_error(diagnostics, function,
                            "alloca 只能出现在入口分配区");
    }
    // 先检查操作数，再把结果标为可用，防止指令引用自己的结果。
    if (instruction.result) {
        if (!define_value(function, defined, *instruction.result,
                          instruction.type))
            return report_error(diagnostics, function,
                                "指令结果身份无效或重复定义");
        available[instruction.result->index] = true;
    }
    return true;
}

// 检查一个基本块的 ID、终结指令、指令序列和 ret 操作数。
bool check_block(const Module &module, const Function &function,
                 const BasicBlock &block, std::size_t block_index,
                 const std::vector<bool> &parameters,
                 std::vector<bool> &defined, std::ostream &diagnostics) {
    if (block.id != BlockId{function.id, block_index} || !block.terminator)
        return report_error(diagnostics, function,
                            "基本块身份无效或缺少终结指令");
    // 每次检查新块时，只把参数和入口分配标为可用。
    auto available = parameters;
    for (const auto &instruction : block.instructions)
        if (!check_instruction(module, function, instruction, defined,
                               available, diagnostics))
            return false;
    // ret 的值也要先定义，并与函数返回类型相同；没有值时函数必须返回 void。
    const auto &return_instruction = *block.terminator;
    bool valid_return = false;
    if (return_instruction.value) {
        valid_return =
            value_available(function, available, *return_instruction.value) &&
            return_instruction.value->type == function.signature.result;
    } else {
        valid_return = function.signature.result == Type::Void;
    }
    if (!valid_return)
        return report_error(diagnostics, function,
                            "ret 的操作数、定义位置或返回类型无效");
    return true;
}

// 检查一个函数的声明或定义；发现错误时输出函数名和原因，并返回 false。
bool check_function(const Module &module, const Function &function,
                    std::ostream &diagnostics) {
    // 返回类型必须已知，参数表数量必须和签名一致。
    if (!is_known_type(function.signature.result) ||
        function.parameters.size() != function.signature.parameters.size())
        return report_error(diagnostics, function,
                            "函数签名或参数数量无效");

    // defined 记录整个函数中哪些值已经定义，防止同一个 ID 被定义两次。
    // 后面的 available 只记录当前块中已经可以使用的值。
    std::vector<bool> defined(function.value_types.size(), false);
    if (!check_parameters(function, defined, diagnostics))
        return false;
    if (function.is_declaration())
        return check_declaration(function, defined, diagnostics);
    if (!check_entry_allocas(function, defined, diagnostics))
        return false;

    // 当前没有块间跳转，每个块只使用参数、入口分配和本块前面指令的结果。
    // 以后支持分支和循环时，再检查一个块产生的值能否在其他块中使用。
    const auto parameters = defined;
    for (std::size_t block_index = 0;
         block_index < function.blocks.size(); ++block_index)
        if (!check_block(module, function, function.blocks[block_index],
                         block_index, parameters, defined, diagnostics))
            return false;
    // 每个记录了类型的值都必须实际出现在参数表或某条指令的结果中。
    if (std::any_of(defined.begin(), defined.end(), is_undefined))
        return report_error(diagnostics, function,
                            "值登记表包含未定义的 SSA 值");
    return true;
}

} // namespace

// 先检查目标配置和函数表，再逐个检查函数；发现第一个错误就返回 false。
// 这里只检查自有模型，生成的 LLVM 文本仍需由外部 LLVM 工具验证。
bool check_module(const Module &module, std::ostream &diagnostics) {
    if (module.target_triple != rv32_triple ||
        module.data_layout != rv32_data_layout) {
        diagnostics << "IR 内部错误：目标配置必须为固定 RV32IM/ILP32 布局\n";
        return false;
    }
    // 函数 ID 要匹配所属模块和函数表下标，函数名也不能在模块中重复。
    for (std::size_t function_index = 0;
         function_index < module.functions().size(); ++function_index) {
        const auto &function = *module.functions()[function_index];
        if (function.id != IRFunctionId{module.identity(), function_index} ||
            !valid_symbol_name(function.name) ||
            module.find_function(function.name) != &function ||
            (function.linkage != Linkage::Internal &&
             function.linkage != Linkage::External)) {
            diagnostics << "IR 内部错误：函数身份、符号或链接属性无效/重复\n";
            return false;
        }
        if (!check_function(module, function, diagnostics))
            return false;
    }
    return true;
}

} // namespace rx::ir
