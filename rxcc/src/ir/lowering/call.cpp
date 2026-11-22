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
        const auto &signature = semantics.builtin_signatures.at(builtin);
        const char *name = nullptr;
        switch (builtin) {
        case BuiltinIO::GetI32: name = "get_i32"; break;
        case BuiltinIO::PrintI32: name = "print_i32"; break;
        case BuiltinIO::PrintlnI32: name = "println_i32"; break;
        }
        // 三个 I/O builtin 的参数固定为 i32，签名已由语义阶段登记。
        std::vector<ir::Type> lowered_parameters(signature.parameters.size(),
                                                 ir::Type::I32);
        const ir::Signature lowered_signature{
            signature.return_type.is_unit() ? ir::Type::Void : ir::Type::I32,
            std::move(lowered_parameters)};
        if (const auto *existing = module.find_function(name)) {
            ir_target = existing->id;
        } else {
            ir_target = module.add_function(name, lowered_signature,
                                            ir::Linkage::External);
        }
    }

    std::vector<ir::Operand> arguments;
    arguments.reserve(call.arguments.size());
    // 每个参数先独立完成求值，再继续下一个参数。
    for (std::size_t index = 0; index < call.arguments.size(); ++index) {
        const auto &source_argument = *call.arguments[index];
        auto argument = emit_value(source_argument, locals);
        if (!argument)
            return std::nullopt;
        // 前面的实参提前 return 后，后续实参和最终 call 都不能生成。
        if (path_ended(*argument))
            return EmitResult{NoContinuation{}};
        // 实参是固定使用位置，按 sema 已记录的路径执行引用调整。
        auto coerced = emit_coercion_at(
            UseSite{&call, UseKind::Argument, index},
            std::get<ir::Operand>(*argument));
        if (!coerced)
            return std::nullopt;
        arguments.push_back(*coerced);
    }
    const auto value = builder.call(ir_target, std::move(arguments));
    // 标量调用产生 SSA 值；unit 调用只有副作用。
    if (value)
        return EmitResult{*value};
    return EmitResult{UnitResult{}};
}

} // namespace rx
