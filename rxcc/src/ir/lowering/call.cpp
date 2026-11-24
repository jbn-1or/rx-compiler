// 按语义记录生成普通、点方法和内置调用，保持接收者与实参的求值顺序。
// 同时实现 Box/Vec 操作及其共用的分配、寻址和运行时计数循环。
#include "ir/lowering.hpp"
#include "ast/ast.hpp"
#include "ir/builder.hpp"
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace rx {

// 把已按调用顺序求好的实参传给目标函数。
// 聚合返回先分配调用者 slot 并作为隐藏首参数；标量/引用结果直接使用返回值。
EmitResult
GenerationContext::emit_target_call(ir::IRFunctionId target,
                                    const std::vector<EmitResult> &prepared) {
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
        else
            arguments.push_back(std::get<ir::Operand>(argument));
    }
    const auto value = builder.call(target, std::move(arguments));
    // 聚合结果已在调用者 slot 中；标量/引用结果使用直接返回值。
    if (result_slot)
        return AggregateValue{*result_slot};
    // 标量调用产生 SSA 值；unit 调用只有副作用。
    if (value)
        return EmitResult{*value};
    return EmitResult{UnitResult{}};
}

// 求值 Box::<T>::new 的实参，再申请 T 的存储并把值复制/移动到新分配。
// 分配失败不单独分支；有效程序满足规范的非零大小成功保证。
EmitResult
GenerationContext::emit_box_new(const SemanticType &box_type,
                                const EmitResult &value) {
    const auto &value_type = *box_type.inner;
    const auto layout = layouts_.layout(ir::storage_type_of(value_type));
    const auto allocated = emit_alloc(
        ir::Operand::constant(ir::Type::I32, layout.size),
        ir::Operand::constant(ir::Type::I32, layout.align));
    // 聚合值按完整布局搬运；标量、引用和 Box 写入一个存储单元。
    store_prepared_value(allocated, value, value_type);
    // 返回拥有者指针；Box 的移动只是转移这个指针，不复制目标。
    return EmitResult{allocated};
}

// 首次使用时声明与 C 运行时对接的分配器，后续调用复用同一声明。
ir::Operand GenerationContext::emit_alloc(ir::Operand size, ir::Operand align) {
    ir::IRFunctionId alloc;
    if (const auto *existing = module.find_function("__rx_alloc")) {
        alloc = existing->id;
    } else {
        alloc = module.add_function(
            "__rx_alloc",
            {ir::Type::Ptr, {ir::Type::I32, ir::Type::I32}},
            ir::Linkage::External);
    }
    return builder.call(alloc, {std::move(size), std::move(align)}).value();
}

// 生成运行时计数循环；下标从 0 递增到 count（不含），每轮调用 body。
void GenerationContext::for_each_runtime_index(
    ir::Operand count, const std::function<void(ir::Operand)> &body) {
    const auto insertion = builder.insert_point();
    auto &function = module.function(insertion->function);
    const auto zero = ir::Operand::constant(ir::Type::I32, 0);
    const auto one = ir::Operand::constant(ir::Type::I32, 1);
    const auto index_slot = allocate_slot(ir::Type::I32);
    builder.store(ir::Type::I32, zero, index_slot, 4);

    const auto condition_block = function.add_block();
    const auto body_block = function.add_block();
    const auto exit_block = function.add_block();
    builder.branch(condition_block);

    builder.set_insert_point(condition_block);
    const auto index = load_value(index_slot, ir::Type::I32);
    const auto less = builder.compare(ir::Comparison::UnsignedLessThan,
                                      index, count);
    builder.cond_branch(less, body_block, exit_block);

    builder.set_insert_point(body_block);
    body(index);
    const auto next = builder.binary(ir::BinaryOp::Add, index, one);
    builder.store(ir::Type::I32, next, index_slot, 4);
    builder.branch(condition_block);
    builder.set_insert_point(exit_block);
}

// 投影真实 Vec 头的指定字段，只取地址，不提前读取内容。
ir::Operand GenerationContext::vec_field_address(ir::Operand header, VecField field) {
    return builder.get_element_ptr(ir::StorageType::vec(), header,
        {ir::Operand::constant(ir::Type::I32, 0),
         ir::Operand::constant(ir::Type::I32, static_cast<uint32_t>(field))});
}

// 零长度数组只描述缓冲区的元素步长，实际活动长度来自 Vec 头。
ir::Operand GenerationContext::vec_element_address(ir::Operand data,
    const SemanticType &element, ir::Operand index) {
    return builder.get_element_ptr(ir::StorageType::array_of(ir::storage_type_of(element), 0),
        data, {ir::Operand::constant(ir::Type::I32, 0), index});
}

// Vec::push 按当前头决定是否增长；增长只搬运 len 个活动元素，不做深克隆。
EmitResult
GenerationContext::emit_vec_push(const SemanticType &vec_type,
                                 ir::Operand header, const EmitResult &element) {
    const auto &element_type = *vec_type.inner;
    const auto zero = ir::Operand::constant(ir::Type::I32, 0);
    const auto one = ir::Operand::constant(ir::Type::I32, 1);
    const auto two = ir::Operand::constant(ir::Type::I32, 2);
    const auto data_field = vec_field_address(header, VecField::Data);
    const auto length_field = vec_field_address(header, VecField::Length);
    const auto capacity_field = vec_field_address(header, VecField::Capacity);
    // 进入 push 后先读取此刻的头；实参在此之前已经求值完成。
    const auto length = builder.load(ir::Type::I32, length_field, 4);
    const auto capacity = builder.load(ir::Type::I32, capacity_field, 4);
    const auto next_length = builder.binary(ir::BinaryOp::Add, length, one);
    const auto is_full = builder.compare(ir::Comparison::Equal, length, capacity);

    const auto insertion = builder.insert_point();
    auto &function = module.function(insertion->function);
    const auto grow_block = function.add_block();
    const auto after_block = function.add_block();
    builder.cond_branch(is_full, grow_block, after_block);

    // 增长路径：选择新容量，申请存储，搬运已有活动元素，再更新头。
    builder.set_insert_point(grow_block);
    const auto element_layout = layouts_.layout(ir::storage_type_of(element_type));
    const auto max_bytes = std::numeric_limits<uint32_t>::max();
    // 零大小的堆分配不属于有效执行，但生成 IR 时仍不能除以零。
    const auto max_capacity = element_layout.size == 0
        ? max_bytes : max_bytes / element_layout.size;
    // 翻倍前检查字节数能否表示；否则只申请 len + 1 个元素。
    const auto nonzero = builder.compare(ir::Comparison::NotEqual, capacity, zero);
    const auto fits = builder.compare(
        ir::Comparison::UnsignedLessThanOrEqual, capacity,
        ir::Operand::constant(ir::Type::I32, max_capacity / 2));
    const auto can_double = builder.binary(ir::BinaryOp::And, nonzero, fits);
    // 容量选择通过私有 slot 汇合，避免依赖不存在的 select 指令。
    const auto capacity_slot = allocate_slot(ir::Type::I32);
    const auto double_block = function.add_block();
    const auto required_block = function.add_block();
    const auto capacity_done = function.add_block();
    builder.cond_branch(can_double, double_block, required_block);
    builder.set_insert_point(double_block);
    const auto doubled = builder.binary(ir::BinaryOp::Multiply, capacity, two);
    builder.store(ir::Type::I32, doubled, capacity_slot, 4);
    builder.branch(capacity_done);
    builder.set_insert_point(required_block);
    builder.store(ir::Type::I32, next_length, capacity_slot, 4);
    builder.branch(capacity_done);
    builder.set_insert_point(capacity_done);
    const auto new_capacity = load_value(capacity_slot, ir::Type::I32);

    const auto bytes = builder.binary(
        ir::BinaryOp::Multiply, new_capacity,
        ir::Operand::constant(ir::Type::I32, element_layout.size));
    const auto new_data = emit_alloc(
        bytes, ir::Operand::constant(ir::Type::I32, element_layout.align));
    const auto old_data = builder.load(ir::Type::Ptr, data_field, 4);
    for_each_runtime_index(length, [&](ir::Operand index) {
        const auto source = vec_element_address(old_data, element_type, index);
        const auto destination = vec_element_address(new_data, element_type, index);
        copy_value(destination, source, element_type);
    });
    builder.store(ir::Type::Ptr, new_data, data_field, 4);
    builder.store(ir::Type::I32, new_capacity, capacity_field, 4);
    builder.branch(after_block);

    // 正常路径：重新读取头中的 data（增长时已更新），写 data[len]，最后 len+1。
    builder.set_insert_point(after_block);
    const auto data = builder.load(ir::Type::Ptr, data_field, 4);
    const auto element_address = vec_element_address(data, element_type, length);
    store_prepared_value(element_address, element, element_type);
    builder.store(ir::Type::I32, next_length, length_field, 4);
    return EmitResult{UnitResult{}};
}

// Vec::remove 先把目标元素保存到独立结果，再把后面的活动元素左移一位。
// 后缀搬运用正向循环：目标总比源低一个元素，先读后写不会覆盖尚未搬运的值。
std::optional<EmitResult>
GenerationContext::emit_vec_remove(const SemanticType &vec_type,
                                   ir::Operand header, ir::Operand index) {
    const auto &element_type = *vec_type.inner;
    const auto one = ir::Operand::constant(ir::Type::I32, 1);
    const auto data_field = vec_field_address(header, VecField::Data);
    const auto length_field = vec_field_address(header, VecField::Length);
    const auto data = builder.load(ir::Type::Ptr, data_field, 4);
    const auto length = builder.load(ir::Type::I32, length_field, 4);
    const auto removed_address = vec_element_address(data, element_type, index);

    // 先固定被移除的值；聚合元素复制到独立 slot，标量/引用/Box 直接读成操作数。
    auto removed = read_stored_value(removed_address, element_type,
                                     StorageOrigin::SourcePlace);
    if (!removed)
        return std::nullopt;

    // 先算删除后的长度，复用于后缀数量；删除末尾时不需要搬运。
    const auto next_length = builder.binary(ir::BinaryOp::Subtract, length, one);
    const auto suffix_start = builder.binary(ir::BinaryOp::Add, index, one);
    const auto suffix_count = builder.binary(
        ir::BinaryOp::Subtract, next_length, index);
    for_each_runtime_index(suffix_count, [&](ir::Operand offset) {
        const auto source_index = builder.binary(
            ir::BinaryOp::Add, suffix_start, offset);
        const auto destination_index = builder.binary(
            ir::BinaryOp::Add, index, offset);
        const auto source = vec_element_address(data, element_type, source_index);
        const auto destination = vec_element_address(data, element_type, destination_index);
        copy_value(destination, source, element_type);
    });

    // 最后缩短活动长度；原末尾位置的字节不再属于活动元素。
    builder.store(ir::Type::I32, next_length, length_field, 4);
    return removed;
}

// Vec::<T>::new 只建立空头：数据指针为 null，长度和容量为 0。
// 不调用 __rx_alloc(0, ...)，因此后续读取长度也不解引用空缓冲区。
EmitResult
GenerationContext::emit_vec_new() {
    const auto storage = ir::StorageType::vec();
    const auto address = allocate_storage_slot(storage);
    const auto zero_index = ir::Operand::constant(ir::Type::I32, 0);
    // 三个字段按 {ptr data, i32 len, i32 cap} 顺序写入。
    const auto data_address = vec_field_address(address, VecField::Data);
    builder.store(ir::Type::Ptr, ir::Operand::constant(ir::Type::Ptr, 0),
                  data_address, 4);
    const auto length_address = vec_field_address(address, VecField::Length);
    builder.store(ir::Type::I32, zero_index, length_address, 4);
    const auto capacity_address = vec_field_address(address, VecField::Capacity);
    builder.store(ir::Type::I32, zero_index, capacity_address, 4);
    return AggregateValue{address};
}

// 从 Vec 头的 len 字段读取当前长度；is_empty 只比较该长度是否为零。
EmitResult
GenerationContext::emit_vec_query(BuiltinCallKind kind, ir::Operand header) {
    const auto zero_index = ir::Operand::constant(ir::Type::I32, 0);
    const auto length_address = vec_field_address(header, VecField::Length);
    const auto length = builder.load(ir::Type::I32, length_address, 4);
    if (kind == BuiltinCallKind::VecLen)
        return EmitResult{length};
    return EmitResult{builder.compare(ir::Comparison::Equal, length, zero_index)};
}

// 按语义记录生成用户函数或 I/O 调用，并返回值、unit 或已结束的路径。
std::optional<EmitResult>
GenerationContext::emit_call(const CallExpression &call,
                             LocalPlaces &locals) {
    // 普通调用和限定调用都只读取语义阶段保存的目标，不按源码拼写重新解析。
    return finish_call(call, semantics.ordinary_calls.at(&call), call.arguments,
                       locals, {});
}

// 点调用在 prepared 中带入接收者，但转换记录仍以普通实参的源码下标为准。
std::optional<EmitResult> GenerationContext::finish_call(
    const Expression &call, const CallTarget &target,
    const std::vector<std::shared_ptr<Expression>> &arguments,
    LocalPlaces &locals, std::vector<EmitResult> prepared) {
    // 每个实参从左到右立即求值并固定快照；求值顺序与普通调用一致。
    prepared.reserve(prepared.size() + arguments.size());
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        auto argument = emit_prepared_value(*arguments[index], locals,
                                            {&call, UseKind::Argument, index});
        if (!argument || path_ended(*argument))
            return argument;
        prepared.push_back(std::move(*argument));
    }

    if (const auto *function_id = std::get_if<FunctionId>(&target))
        return emit_target_call(functions[function_id->index], prepared);
    if (const auto *builtin = std::get_if<BuiltinIO>(&target)) {
        // builtin 使用语义阶段保存的签名，再映射到受控运行时符号。
        const char *name = nullptr;
        switch (*builtin) {
        case BuiltinIO::GetI32: name = "get_i32"; break;
        case BuiltinIO::PrintI32: name = "print_i32"; break;
        case BuiltinIO::PrintlnI32: name = "println_i32"; break;
        }
        ir::IRFunctionId ir_target;
        if (const auto *existing = module.find_function(name)) {
            ir_target = existing->id;
        } else {
            const auto &signature = semantics.builtin_signatures.at(*builtin);
            ir_target = module.add_function(name,
                {signature.return_type.is_unit() ? ir::Type::Void : ir::Type::I32,
                 std::vector<ir::Type>(signature.parameters.size(), ir::Type::I32)},
                ir::Linkage::External);
        }
        return emit_target_call(ir_target, prepared);
    }
    const auto &builtin = std::get<BuiltinCall>(target);
    switch (builtin.kind) {
    case BuiltinCallKind::Clone:
        return emit_clone(builtin.owner, std::get<ir::Operand>(prepared[0]));
    case BuiltinCallKind::BoxNew:
        return emit_box_new(builtin.owner, prepared[0]);
    case BuiltinCallKind::VecNew:
        return emit_vec_new();
    case BuiltinCallKind::ArrayLen:
        return ir::Operand::constant(ir::Type::I32, builtin.owner.length);
    case BuiltinCallKind::VecLen:
    case BuiltinCallKind::VecIsEmpty:
        return emit_vec_query(builtin.kind, std::get<ir::Operand>(prepared[0]));
    case BuiltinCallKind::VecPush:
        return emit_vec_push(builtin.owner, std::get<ir::Operand>(prepared[0]),
                             prepared[1]);
    case BuiltinCallKind::VecRemove:
        return emit_vec_remove(builtin.owner, std::get<ir::Operand>(prepared[0]),
                               std::get<ir::Operand>(prepared[1]));
    }
    diagnostics << "IR 生成尚未支持此内置方法或容器构造器\n";
    return std::nullopt;
}

// 按已选 ReceiverAdjustment 准备接收者：
// 不需要借用/解引用时直接取值；需要地址时保持已有 place，值只物化一次；
// 需要解引用时先求出第一层目标指针，再逐层读取剩余引用或 Box 保存的指针。
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
        auto place = emit_borrowed_place(receiver, locals);
        if (!place)
            return std::nullopt;
        if (path_ended(*place))
            return NoContinuation{};
        return std::get<Place>(*place).address;
    }

    // 引用/Box 的值就是第一层目标指针，位置和值表达式都只求值一次。
    auto value = emit_value(receiver, locals);
    if (!value || path_ended(*value))
        return value;
    Place current{std::get<ir::Operand>(*value), record.type.inner.get()};
    // 第一层已由求值得到；剩余每层只读取一次该层保存的指针。
    for (std::size_t step = 1; step < method.receiver.dereferences; ++step) {
        current = Place{load_value(current.address, ir::Type::Ptr),
                        current.type->inner.get()};
    }
    // 候选是引用时传该层保存的指针；候选是按值类型时固定一份快照。
    if (borrow == Borrow::None)
        return read_stored_value(current.address, *current.type,
                                 StorageOrigin::SourcePlace);
    // 共享/可变借用传目标位置的地址，不在实参求值前读取方法体需要的内容。
    return EmitResult{current.address};
}

// 点方法调用：目标已由 sema 选定，生成阶段不再筛选候选。
std::optional<EmitResult>
GenerationContext::emit_method_call(const MethodCallExpression &call,
                                    LocalPlaces &locals) {
    const auto &resolved = semantics.method_calls.at(&call);
    // 接收者先求值一次并固定，再从左到右求普通实参，最后发出调用。
    auto receiver = emit_receiver(*call.receiver, resolved, locals);
    if (!receiver || path_ended(*receiver))
        return receiver;
    return finish_call(call, resolved.target, call.arguments, locals, {*receiver});
}

} // namespace rx
