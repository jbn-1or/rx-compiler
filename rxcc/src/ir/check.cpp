#include "ir/check.hpp"
#include <algorithm>
#include <ostream>
#include <set>

namespace rx::ir {
namespace {

bool check_function(const Module &module, const Function &function, std::ostream &diagnostics) {
    const auto fail = [&](const char *message) {
        diagnostics << "IR 内部错误：" << function.name << ": " << message << '\n';
        return false;
    };
    if (!*type_name(function.signature.result) ||
        function.parameters.size() != function.signature.parameters.size())
        return fail("函数签名或参数数量无效");

    std::vector<bool> defined(function.value_types.size(), false);
    const auto define = [&](ValueId id, Type type) {
        if (type == Type::Void || !*type_name(type) || id.function != function.id ||
            id.index >= defined.size() || defined[id.index] || function.value_types[id.index] != type)
            return false;
        defined[id.index] = true;
        return true;
    };
    for (std::size_t i = 0; i < function.parameters.size(); ++i) {
        const auto &parameter = function.parameters[i];
        const auto *id = parameter.value_id();
        if (!id || id->index != i || parameter.type != function.signature.parameters[i] ||
            !define(*id, parameter.type))
            return fail("函数参数身份或类型无效");
    }
    if (function.is_declaration()) {
        if (function.linkage != Linkage::External || defined.size() != function.parameters.size())
            return fail("内部函数未完成定义或声明携带函数体数据");
        return true;
    }

    // P2 没有跳转指令，各块只允许引用参数及本块此前已定义的 call 结果。
    // 跨块 SSA 与支配关系随 P6 控制流一起引入。
    const auto parameters = defined;
    for (std::size_t b = 0; b < function.blocks.size(); ++b) {
        const auto &block = function.blocks[b];
        if (block.id != BlockId{function.id, b} || !block.terminator)
            return fail("基本块身份无效或缺少终结指令");
        auto available = parameters;
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
            if (instruction.result) {
                if (!define(*instruction.result, instruction.type))
                    return fail("call 结果身份无效或重复定义");
                available[instruction.result->index] = true;
            }
        }
        const auto &ret = std::get<Return>(*block.terminator);
        if (ret.value ? !operand(*ret.value) || ret.value->type != function.signature.result
                      : function.signature.result != Type::Void)
            return fail("ret 的操作数、定义位置或返回类型无效");
    }
    if (std::any_of(defined.begin(), defined.end(), [](bool value) { return !value; }))
        return fail("值登记表包含未定义的 SSA 值");
    return true;
}

} // namespace

bool check_module(const Module &module, std::ostream &diagnostics) {
    if (module.target_triple != rv32_triple || module.data_layout != rv32_data_layout) {
        diagnostics << "IR 内部错误：目标配置必须为固定 RV32IM/ILP32 布局\n";
        return false;
    }
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
