// 检查模块的目标配置、函数和块的 ID、参数和返回类型，以及值是否先定义后使用。
// 当前只支持 call/ret，不允许直接使用其他块产生的值，也不修改被检查的模块。

#include "ir/check.hpp"
#include <algorithm>
#include <ostream>
#include <set>

namespace rx::ir {
namespace {

// 检查一个函数的声明或定义；发现错误时输出函数名和原因，并返回 false。
bool check_function(const Module &module, const Function &function, std::ostream &diagnostics) {
    const auto fail = [&](const char *message) {
        diagnostics << "IR 内部错误：" << function.name << ": " << message << '\n';
        return false;
    };
    if (!*type_name(function.signature.result) ||
        function.parameters.size() != function.signature.parameters.size())
        return fail("函数签名或参数数量无效");

    // defined 记录整个函数中哪些值已经定义，防止同一个 ID 被定义两次。
    // 后面的 available 只记录当前块中已经可以使用的值。
    std::vector<bool> defined(function.value_types.size(), false);
    // 检查值属于当前函数、类型匹配且未定义过，再将它标记为已定义。
    const auto define = [&](ValueId id, Type type) {
        if (type == Type::Void || !*type_name(type) || id.function != function.id ||
            id.index >= defined.size() || defined[id.index] || function.value_types[id.index] != type)
            return false;
        defined[id.index] = true;
        return true;
    };
    // 参数按声明顺序从 0 编号，它们在进入任何基本块之前就已经可用。
    for (std::size_t i = 0; i < function.parameters.size(); ++i) {
        const auto &parameter = function.parameters[i];
        const auto *id = parameter.value_id();
        if (!id || id->index != i || parameter.type != function.signature.parameters[i] ||
            !define(*id, parameter.type))
            return fail("函数参数身份或类型无效");
    }
    // 没有函数体时，必须是外部函数声明，值类型表中也只能保存参数类型。
    if (function.is_declaration()) {
        if (function.linkage != Linkage::External || defined.size() != function.parameters.size())
            return fail("内部函数未完成定义或声明携带函数体数据");
        return true;
    }

    // 当前没有块间跳转，因此每个块只使用参数和本块前面 call 指令的结果。
    // 以后支持分支和循环时，再检查一个块产生的值能否在其他块中使用。
    const auto parameters = defined;
    for (std::size_t b = 0; b < function.blocks.size(); ++b) {
        const auto &block = function.blocks[b];
        if (block.id != BlockId{function.id, b} || !block.terminator)
            return fail("基本块身份无效或缺少终结指令");
        // 每次检查新块时，只把参数标为可用，不继承其他块的指令结果。
        auto available = parameters;
        // 常量可直接使用；SSA 值必须属于本函数，并且已经在当前块中可用。
        const auto operand = [&](const Operand &value) {
            if (!function.contains(value))
                return false;
            const auto *id = value.value_id();
            return !id || available[id->index];
        };
        for (const auto &instruction : block.instructions) {
            const auto &call = std::get<Call>(instruction.operation);
            if (!module.contains(call.function))
                return fail("调用目标无效或不属于当前模块");
            const auto &signature = module.function(call.function).signature;
            if (call.arguments.size() != signature.parameters.size() ||
                instruction.type != signature.result ||
                instruction.result.has_value() != (signature.result != Type::Void))
                return fail("call 的签名或结果不一致");
            for (std::size_t i = 0; i < call.arguments.size(); ++i)
                if (!operand(call.arguments[i]) || call.arguments[i].type != signature.parameters[i])
                    return fail("call 操作数类型、归属或定义位置无效");
            // 先检查实参，再把调用结果标为可用，防止 call 引用自己尚未产生的结果。
            if (instruction.result) {
                if (!define(*instruction.result, instruction.type))
                    return fail("call 结果身份无效或重复定义");
                available[instruction.result->index] = true;
            }
        }
        // ret 的值也要先定义，并与函数返回类型相同；没有值时函数必须返回 void。
        const auto &ret = std::get<Return>(*block.terminator);
        if (ret.value ? !operand(*ret.value) || ret.value->type != function.signature.result
                      : function.signature.result != Type::Void)
            return fail("ret 的操作数、定义位置或返回类型无效");
    }
    // 每个记录了类型的值都必须实际出现在参数表或某条指令的结果中。
    if (std::any_of(defined.begin(), defined.end(), [](bool value) { return !value; }))
        return fail("值登记表包含未定义的 SSA 值");
    return true;
}

} // namespace

// 先检查目标配置和函数表，再逐个检查函数；发现第一个错误就返回 false。
// 这里只检查自有模型，生成的 LLVM 文本仍需由外部 LLVM 工具验证。
bool check_module(const Module &module, std::ostream &diagnostics) {
    if (module.target_triple != rv32_triple || module.data_layout != rv32_data_layout) {
        diagnostics << "IR 内部错误：目标配置必须为固定 RV32IM/ILP32 布局\n";
        return false;
    }
    // 函数 ID 要匹配所属模块和函数表下标，函数名也不能在模块中重复。
    std::set<std::string> names;
    for (std::size_t i = 0; i < module.functions().size(); ++i) {
        const auto &function = *module.functions()[i];
        if (function.id != IRFunctionId{module.identity(), i} ||
            !valid_symbol_name(function.name) || !names.insert(function.name).second ||
            (function.linkage != Linkage::Internal && function.linkage != Linkage::External)) {
            diagnostics << "IR 内部错误：函数身份、符号或链接属性无效/重复\n";
            return false;
        }
        if (!check_function(module, function, diagnostics))
            return false;
    }
    return true;
}

} // namespace rx::ir
