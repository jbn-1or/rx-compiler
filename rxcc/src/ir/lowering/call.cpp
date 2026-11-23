// 按语义记录生成普通函数和 I/O 调用，并保持实参求值顺序。
#include "ir/lowering.hpp"
#include "ast/ast.hpp"
#include "ir/builder.hpp"
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace rx {

// 把已按调用顺序求好的实参传给目标函数。
// 聚合返回先分配调用者 slot 并作为隐藏首参数；标量/引用结果直接使用返回值。
std::optional<EmitResult>
GenerationContext::emit_target_call(ir::IRFunctionId target,
                                    std::vector<EmitResult> prepared,
                                    const SemanticType &result_type) {
    const auto &abi = module.function(target).abi;
    // 结果 slot 只是内部传递地址，不改变按值语义。
    std::optional<ir::Operand> result_slot;
    std::vector<ir::Operand> arguments;
    arguments.reserve(abi.signature.parameters.size());
    if (abi.result_storage) {
        result_slot = allocate_storage_slot(*abi.result_storage);
        arguments.push_back(*result_slot);
    }
    // 聚合实参传独立存储地址；标量和引用直接传计算值。
    for (auto &argument : prepared) {
        if (const auto *aggregate = std::get_if<AggregateValue>(&argument))
            arguments.push_back(aggregate->address);
        else if (const auto *operand = std::get_if<ir::Operand>(&argument))
            arguments.push_back(*operand);
        else
            throw std::logic_error("已结束或空的表达式不能作为调用实参");
    }
    const auto value = builder.call(target, std::move(arguments));
    // 聚合结果已在调用者 slot 中；标量/引用结果使用直接返回值。
    if (result_slot)
        return EmitResult{AggregateValue{*result_slot, &result_type}};
    // 标量调用产生 SSA 值；unit 调用只有副作用。
    if (value)
        return EmitResult{*value};
    return EmitResult{UnitResult{}};
}

// 按语义记录生成用户函数或 I/O 调用，并返回值、unit 或已结束的路径。
std::optional<EmitResult>
GenerationContext::emit_call(const CallExpression &call,
                             LocalPlaces &locals) {
    // 普通调用和限定调用都只读取语义阶段保存的目标，不按源码拼写重新解析。
    const auto &target = semantics.ordinary_calls.at(&call);

    ir::IRFunctionId ir_target;
    if (const auto *function_id = std::get_if<FunctionId>(&target)) {
        ir_target = functions[function_id->index];
    } else if (const auto *builtin = std::get_if<BuiltinIO>(&target)) {
        // builtin 使用语义阶段保存的签名，再映射到受控运行时符号。
        const char *name = nullptr;
        switch (*builtin) {
        case BuiltinIO::GetI32: name = "get_i32"; break;
        case BuiltinIO::PrintI32: name = "print_i32"; break;
        case BuiltinIO::PrintlnI32: name = "println_i32"; break;
        }
        if (const auto *existing = module.find_function(name)) {
            ir_target = existing->id;
        } else {
            const auto &signature = semantics.builtin_signatures.at(*builtin);
            ir_target = module.add_function(name,
                {signature.return_type.is_unit() ? ir::Type::Void : ir::Type::I32,
                 std::vector<ir::Type>(signature.parameters.size(), ir::Type::I32)},
                ir::Linkage::External);
        }
    } else {
        // 容器构造器和 Vec/clone 等内置方法留到 P9/P10，不能伪造调用。
        diagnostics << "IR 生成尚未支持内置方法或容器构造器\n";
        return std::nullopt;
    }

    // 每个实参从左到右立即求值并固定快照；求值顺序与普通调用一致。
    std::vector<EmitResult> prepared;
    prepared.reserve(call.arguments.size());
    for (std::size_t index = 0; index < call.arguments.size(); ++index) {
        auto argument = emit_prepared_value(*call.arguments[index], locals,
                                            {&call, UseKind::Argument, index});
        if (!argument || path_ended(*argument))
            return argument;
        prepared.push_back(std::move(*argument));
    }
    return emit_target_call(ir_target, std::move(prepared),
                            semantics.expressions.at(&call).type);
}

// 按已选 ReceiverAdjustment 准备接收者：
// 不需要借用/解引用时直接取值；需要地址时保持已有 place，值只物化一次；
// 需要解引用时先得到对应层的 place，再逐层读取引用保存的指针。
std::optional<EmitResult>
GenerationContext::emit_receiver(const Expression &receiver,
                                 const ResolvedMethod &method,
                                 LocalPlaces &locals) {
    using Borrow = ReceiverAdjustment::Borrow;
    const auto &record = semantics.expressions.at(&receiver);
    const auto borrow = method.receiver.borrow;

    // 零次解引用且不自动借用：接收者本身就是要传的值，
    // emit_value 会按 place 快照或引用读取的规则固定它。
    if (method.receiver.dereferences == 0 && borrow == Borrow::None)
        return emit_value(receiver, locals);

    // 自动借用需要地址；已有 place 保持原地址，值表达式只物化一次。
    if (method.receiver.dereferences == 0) {
        if (record.place.is_place) {
            auto place = emit_place(receiver, locals);
            if (!place)
                return std::nullopt;
            if (path_ended(*place))
                return EmitResult{NoContinuation{}};
            return EmitResult{std::get<Place>(*place).address};
        }
        auto value = emit_value(receiver, locals);
        if (!value)
            return std::nullopt;
        if (path_ended(*value))
            return value;
        // 聚合值已在独立临时存储中，借用直接使用该地址。
        if (const auto *aggregate = std::get_if<AggregateValue>(&*value))
            return EmitResult{aggregate->address};
        return EmitResult{materialize_value(std::get<ir::Operand>(*value))};
    }

    // 先取得第一层位置：已有 place 保留其地址，引用值本身则已是指针。
    Place current;
    std::size_t remaining = method.receiver.dereferences;
    if (record.place.is_place) {
        auto place = emit_place(receiver, locals);
        if (!place)
            return std::nullopt;
        if (path_ended(*place))
            return EmitResult{NoContinuation{}};
        current = std::get<Place>(*place);
    } else {
        auto value = emit_value(receiver, locals);
        if (!value)
            return std::nullopt;
        if (path_ended(*value))
            return value;
        if (std::holds_alternative<AggregateValue>(*value)) {
            diagnostics << "IR 生成尚未支持聚合接收者解引用\n";
            return std::nullopt;
        }
        // 引用/Box 值本身就是目标指针，直接得到一层解引用后的位置。
        current = Place{std::get<ir::Operand>(*value), record.type.inner.get()};
        --remaining;
    }
    // 每层只读取一次该层保存的指针；Box 内容随 P9 接入。
    for (; remaining > 0; --remaining) {
        if (current.type->kind == SemanticType::Kind::Box) {
            diagnostics << "IR 生成尚未支持 Box 接收者自动解引用（P9）\n";
            return std::nullopt;
        }
        if (current.type->kind != SemanticType::Kind::Reference ||
            !current.type->inner) {
            diagnostics << "IR 生成遇到不成立的接收者调整路径\n";
            return std::nullopt;
        }
        current = Place{load_value(current.address, ir::Type::Ptr),
                        current.type->inner.get()};
    }
    // 候选是引用时传该层保存的指针；候选是按值类型时固定一份快照。
    if (borrow == Borrow::None) {
        if (is_reference(*current.type))
            return EmitResult{load_value(current.address, ir::Type::Ptr)};
        return read_stored_value(current.address, *current.type,
                                 StorageOrigin::SourcePlace);
    }
    // 共享/可变借用传目标位置的地址，不在实参求值前读取方法体需要的内容。
    return EmitResult{current.address};
}

// 点方法调用：目标已由 sema 选定，生成阶段不再筛选候选。
std::optional<EmitResult>
GenerationContext::emit_method_call(const MethodCallExpression &call,
                                    LocalPlaces &locals) {
    const auto &resolved = semantics.method_calls.at(&call);
    // 固定数组的 len 结果来自类型，但接收者仍必须先求值：
    // 它可能包含调用副作用，也可能在正常完成前提前 return。
    if (const auto *builtin = std::get_if<BuiltinCall>(&resolved.target)) {
        if (builtin->kind != BuiltinCallKind::ArrayLen) {
            diagnostics << "IR 生成尚未支持此内置方法调用\n";
            return std::nullopt;
        }
        auto receiver = emit_receiver(*call.receiver, resolved, locals);
        if (!receiver || path_ended(*receiver))
            return receiver;
        // 接收者正常完成后只返回已求值长度，不读取数组内容也不复制整个数组。
        return EmitResult{ir::Operand::constant(
            ir::Type::I32, static_cast<uint32_t>(builtin->owner.length))};
    }
    const auto *function_id = std::get_if<FunctionId>(&resolved.target);
    // 内置 I/O 不是点方法目标；其余用户方法统一走普通函数调用 ABI。
    if (!function_id) {
        diagnostics << "IR 生成遇到不成立的调用目标\n";
        return std::nullopt;
    }
    // 接收者先求值一次并固定，再从左到右求普通实参，最后发出调用。
    auto receiver = emit_receiver(*call.receiver, resolved, locals);
    if (!receiver || path_ended(*receiver))
        return receiver;
    std::vector<EmitResult> arguments;
    arguments.reserve(call.arguments.size() + 1);
    arguments.push_back(std::move(*receiver));
    for (std::size_t index = 0; index < call.arguments.size(); ++index) {
        auto argument = emit_prepared_value(*call.arguments[index], locals,
                                            {&call, UseKind::Argument, index});
        if (!argument || path_ended(*argument))
            return argument;
        arguments.push_back(std::move(*argument));
    }
    return emit_target_call(functions[function_id->index], std::move(arguments),
                            semantics.expressions.at(&call).type);
}

} // namespace rx
