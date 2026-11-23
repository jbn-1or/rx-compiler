// 按语义记录生成普通函数和 I/O 调用，并保持实参求值顺序。
#include "ir/lowering.hpp"
#include "ast/ast.hpp"
#include "ir/builder.hpp"
#include <cstddef>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace rx {

// 按语义记录生成用户函数或 I/O 调用，并返回值、unit 或已结束的路径。
std::optional<EmitResult>
GenerationContext::emit_call(const CallExpression &call,
                             LocalPlaces &locals) {
    const auto &callee = static_cast<const PathExpression &>(*call.callee);
    // 只有单段普通调用能使用 ordinary_calls 记录。
    if (callee.segments.size() != 1) {
        diagnostics << "IR 生成尚未支持限定调用\n";
        return std::nullopt;
    }
    const auto &target = semantics.ordinary_calls.at(&call);

    ir::IRFunctionId ir_target;
    if (const auto *function_id = std::get_if<FunctionId>(&target)) {
        ir_target = functions[function_id->index];
    } else {
        // builtin 使用语义阶段保存的签名，再映射到受控运行时符号。
        const auto builtin = std::get<BuiltinIO>(target);
        const char *name = nullptr;
        switch (builtin) {
        case BuiltinIO::GetI32: name = "get_i32"; break;
        case BuiltinIO::PrintI32: name = "print_i32"; break;
        case BuiltinIO::PrintlnI32: name = "println_i32"; break;
        }
        if (const auto *existing = module.find_function(name)) {
            ir_target = existing->id;
        } else {
            const auto &signature = semantics.builtin_signatures.at(builtin);
            ir_target = module.add_function(name,
                {signature.return_type.is_unit() ? ir::Type::Void : ir::Type::I32,
                 std::vector<ir::Type>(signature.parameters.size(), ir::Type::I32)},
                ir::Linkage::External);
        }
    }

    const auto &abi = module.function(ir_target).abi;
    // 聚合返回先准备调用者结果 slot；它只是内部传递地址，不改变按值语义。
    std::optional<ir::Operand> result_slot;
    std::vector<ir::Operand> arguments;
    arguments.reserve(abi.signature.parameters.size());
    if (abi.result_storage) {
        result_slot = allocate_storage_slot(*abi.result_storage);
        arguments.push_back(*result_slot);
    }
    // 每个实参从左到右立即求值并固定快照，聚合实参传独立存储地址。
    for (std::size_t index = 0; index < call.arguments.size(); ++index) {
        auto prepared = emit_prepared_value(*call.arguments[index], locals,
                                            {&call, UseKind::Argument, index});
        if (!prepared || path_ended(*prepared))
            return prepared;
        if (const auto *aggregate = std::get_if<AggregateValue>(&*prepared))
            arguments.push_back(aggregate->address);
        else
            arguments.push_back(std::get<ir::Operand>(*prepared));
    }
    const auto value = builder.call(ir_target, std::move(arguments));
    // 聚合结果已在调用者 slot 中；标量/引用结果使用直接返回值。
    if (result_slot) {
        const auto &result_type = semantics.expressions.at(&call).type;
        return EmitResult{AggregateValue{*result_slot, &result_type}};
    }
    // 标量调用产生 SSA 值；unit 调用只有副作用。
    if (value)
        return EmitResult{*value};
    return EmitResult{UnitResult{}};
}

} // namespace rx
