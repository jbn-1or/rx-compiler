// 按语义记录生成普通函数和 I/O 调用，并保持实参求值顺序。
#include "ir/lowering.hpp"
#include "ast/ast.hpp"
#include "ir/builder.hpp"
#include <cstddef>
#include <limits>
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

// 求值 Box::<T>::new 的实参，再申请 T 的存储并把值复制/移动到新分配。
// 分配失败不单独分支；有效程序满足规范的非零大小成功保证。
std::optional<EmitResult>
GenerationContext::emit_box_new(const SemanticType &box_type,
                                std::vector<EmitResult> prepared) {
    if (box_type.kind != SemanticType::Kind::Box || !box_type.inner ||
        prepared.size() != 1)
        throw std::logic_error("Box::new 的目标或实参数与签名不一致");
    const auto &value_type = *box_type.inner;
    const auto layout = layouts_.layout(ir::storage_type_of(value_type));
    const auto allocated = emit_alloc(
        ir::Operand::constant(ir::Type::I32, layout.size),
        ir::Operand::constant(ir::Type::I32, layout.align));
    // 聚合值按完整布局搬运；标量、引用和 Box 写入一个存储单元。
    if (const auto *aggregate = std::get_if<AggregateValue>(&prepared.front()))
        copy_value(allocated, aggregate->address, value_type);
    else if (const auto *operand = std::get_if<ir::Operand>(&prepared.front()))
        store_value(*operand, allocated);
    else
        throw std::logic_error("Box::new 的参数没有可存储的值");
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
    const auto result = builder.call(alloc, {std::move(size), std::move(align)});
    if (!result)
        throw std::logic_error("分配器声明必须返回指针");
    return *result;
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

// Vec::push 按当前头决定是否增长；增长只搬运 len 个活动元素，不做深克隆。
std::optional<EmitResult>
GenerationContext::emit_vec_push(const SemanticType &vec_type,
                                 ir::Operand header, EmitResult element) {
    if (vec_type.kind != SemanticType::Kind::Vec || !vec_type.inner)
        throw std::logic_error("Vec::push 的目标不是 Vec");
    const auto &element_type = *vec_type.inner;
    const auto header_storage = ir::StorageType::vec();
    const auto zero = ir::Operand::constant(ir::Type::I32, 0);
    const auto one = ir::Operand::constant(ir::Type::I32, 1);
    const auto two = ir::Operand::constant(ir::Type::I32, 2);
    const auto field_address = [&](uint32_t field) {
        return builder.get_element_ptr(
            header_storage, header,
            {zero, ir::Operand::constant(ir::Type::I32, field)});
    };
    const auto data_field = field_address(0);
    const auto length_field = field_address(1);
    const auto capacity_field = field_address(2);
    // 进入 push 后先读取此刻的头；实参在此之前已经求值完成。
    const auto length = builder.load(ir::Type::I32, length_field, 4);
    const auto capacity = builder.load(ir::Type::I32, capacity_field, 4);
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
    const auto required = builder.binary(ir::BinaryOp::Add, length, one);
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
    builder.store(ir::Type::I32, required, capacity_slot, 4);
    builder.branch(capacity_done);
    builder.set_insert_point(capacity_done);
    const auto new_capacity = load_value(capacity_slot, ir::Type::I32);

    const auto bytes = builder.binary(
        ir::BinaryOp::Multiply, new_capacity,
        ir::Operand::constant(ir::Type::I32, element_layout.size));
    const auto new_data = emit_alloc(
        bytes, ir::Operand::constant(ir::Type::I32, element_layout.align));
    const auto old_data = builder.load(ir::Type::Ptr, data_field, 4);
    // 缓冲区按活动元素类型布局；零长度只用于计算元素步长。
    const auto buffer_storage = ir::StorageType::array_of(
        ir::storage_type_of(element_type), 0);
    for_each_runtime_index(length, [&](ir::Operand index) {
        const auto source = builder.get_element_ptr(
            buffer_storage, old_data, {zero, index});
        const auto destination = builder.get_element_ptr(
            buffer_storage, new_data, {zero, index});
        copy_value(destination, source, element_type);
    });
    builder.store(ir::Type::Ptr, new_data, data_field, 4);
    builder.store(ir::Type::I32, new_capacity, capacity_field, 4);
    builder.branch(after_block);

    // 正常路径：重新读取头中的 data（增长时已更新），写 data[len]，最后 len+1。
    builder.set_insert_point(after_block);
    const auto data = builder.load(ir::Type::Ptr, data_field, 4);
    const auto element_address = builder.get_element_ptr(
        buffer_storage, data, {zero, length});
    store_prepared_value(element_address, element, element_type);
    const auto next_length = builder.binary(ir::BinaryOp::Add, length, one);
    builder.store(ir::Type::I32, next_length, length_field, 4);
    return EmitResult{UnitResult{}};
}

// Vec::remove 先把目标元素保存到独立结果，再把后面的活动元素左移一位。
// 后缀搬运用正向循环：目标总比源低一个元素，先读后写不会覆盖尚未搬运的值。
std::optional<EmitResult>
GenerationContext::emit_vec_remove(const SemanticType &vec_type,
                                   ir::Operand header, ir::Operand index) {
    if (vec_type.kind != SemanticType::Kind::Vec || !vec_type.inner)
        throw std::logic_error("Vec::remove 的目标不是 Vec");
    const auto &element_type = *vec_type.inner;
    const auto header_storage = ir::StorageType::vec();
    const auto zero = ir::Operand::constant(ir::Type::I32, 0);
    const auto one = ir::Operand::constant(ir::Type::I32, 1);
    const auto field_address = [&](uint32_t field) {
        return builder.get_element_ptr(
            header_storage, header,
            {zero, ir::Operand::constant(ir::Type::I32, field)});
    };
    const auto data_field = field_address(0);
    const auto length_field = field_address(1);
    const auto data = builder.load(ir::Type::Ptr, data_field, 4);
    const auto length = builder.load(ir::Type::I32, length_field, 4);
    const auto buffer_storage = ir::StorageType::array_of(
        ir::storage_type_of(element_type), 0);
    const auto removed_address = builder.get_element_ptr(
        buffer_storage, data, {zero, index});

    // 先固定被移除的值；聚合元素复制到独立 slot，标量/引用/Box 直接读成操作数。
    std::optional<EmitResult> removed;
    if (is_aggregate_value(element_type)) {
        const auto slot = allocate_storage_slot(
            ir::storage_type_of(element_type));
        copy_value(slot, removed_address, element_type);
        removed = EmitResult{AggregateValue{slot, &element_type}};
    } else {
        removed = EmitResult{load_value(removed_address, element_type)};
    }

    // 后续元素整体左移一位；数量为 len - index - 1，删除末尾时为零。
    const auto suffix_start = builder.binary(ir::BinaryOp::Add, index, one);
    const auto suffix_length = builder.binary(ir::BinaryOp::Subtract, length, index);
    const auto suffix_count = builder.binary(
        ir::BinaryOp::Subtract, suffix_length, one);
    for_each_runtime_index(suffix_count, [&](ir::Operand offset) {
        const auto source_index = builder.binary(
            ir::BinaryOp::Add, suffix_start, offset);
        const auto destination_index = builder.binary(
            ir::BinaryOp::Add, index, offset);
        const auto source = builder.get_element_ptr(
            buffer_storage, data, {zero, source_index});
        const auto destination = builder.get_element_ptr(
            buffer_storage, data, {zero, destination_index});
        copy_value(destination, source, element_type);
    });

    // 最后缩短活动长度；原末尾位置的字节不再属于活动元素。
    const auto next_length = builder.binary(ir::BinaryOp::Subtract, length, one);
    builder.store(ir::Type::I32, next_length, length_field, 4);
    return std::move(*removed);
}

// Vec::<T>::new 只建立空头：数据指针为 null，长度和容量为 0。
// 不调用 __rx_alloc(0, ...)，因此后续读取长度也不解引用空缓冲区。
std::optional<EmitResult>
GenerationContext::emit_vec_new(const SemanticType &vec_type) {
    const auto storage = ir::StorageType::vec();
    const auto address = allocate_storage_slot(storage);
    const auto zero_index = ir::Operand::constant(ir::Type::I32, 0);
    const auto field_address = [&](uint32_t field) {
        return builder.get_element_ptr(
            storage, address,
            {zero_index, ir::Operand::constant(ir::Type::I32, field)});
    };
    // 三个字段按 {ptr data, i32 len, i32 cap} 顺序写入。
    const auto data_address = field_address(0);
    builder.store(ir::Type::Ptr, ir::Operand::constant(ir::Type::Ptr, 0),
                  data_address, 4);
    const auto length_address = field_address(1);
    builder.store(ir::Type::I32, zero_index, length_address, 4);
    const auto capacity_address = field_address(2);
    builder.store(ir::Type::I32, zero_index, capacity_address, 4);
    return EmitResult{AggregateValue{address, &vec_type}};
}

// 从 Vec 头的 len 字段读取当前长度；is_empty 只比较该长度是否为零。
std::optional<EmitResult>
GenerationContext::emit_vec_query(BuiltinCallKind kind, ir::Operand header) {
    const auto storage = ir::StorageType::vec();
    const auto zero_index = ir::Operand::constant(ir::Type::I32, 0);
    const auto length_address = builder.get_element_ptr(
        storage, header,
        {zero_index, ir::Operand::constant(ir::Type::I32, 1)});
    const auto length = builder.load(ir::Type::I32, length_address, 4);
    if (kind == BuiltinCallKind::VecLen)
        return EmitResult{length};
    if (kind == BuiltinCallKind::VecIsEmpty)
        return EmitResult{builder.compare(ir::Comparison::Equal, length, zero_index)};
    throw std::logic_error("Vec 查询目标必须是 len 或 is_empty");
}

// 按语义记录生成用户函数或 I/O 调用，并返回值、unit 或已结束的路径。
std::optional<EmitResult>
GenerationContext::emit_call(const CallExpression &call,
                             LocalPlaces &locals) {
    // 普通调用和限定调用都只读取语义阶段保存的目标，不按源码拼写重新解析。
    const auto &target = semantics.ordinary_calls.at(&call);

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

    if (const auto *function_id = std::get_if<FunctionId>(&target))
        return emit_target_call(functions[function_id->index],
                                std::move(prepared),
                                semantics.expressions.at(&call).type);
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
        return emit_target_call(ir_target, std::move(prepared),
                                semantics.expressions.at(&call).type);
    }
    const auto *builtin = std::get_if<BuiltinCall>(&target);
    if (builtin) {
        if (builtin->kind == BuiltinCallKind::BoxNew)
            return emit_box_new(builtin->owner, std::move(prepared));
        if (builtin->kind == BuiltinCallKind::VecNew)
            return emit_vec_new(builtin->owner);
        if (builtin->kind == BuiltinCallKind::VecLen ||
            builtin->kind == BuiltinCallKind::VecIsEmpty) {
            // 显式 Vec::len(&v) 的接收者就是指向真实头的引用。
            if (prepared.size() != 1)
                throw std::logic_error("Vec 查询的显式参数数量不匹配");
            const auto *header = std::get_if<ir::Operand>(&prepared.front());
            if (!header)
                throw std::logic_error("Vec 查询的接收者必须是指针");
            return emit_vec_query(builtin->kind, *header);
        }
        if (builtin->kind == BuiltinCallKind::VecPush) {
            // 显式 Vec::push(&mut v, value)：接收者先于元素求值。
            if (prepared.size() != 2)
                throw std::logic_error("Vec::push 的显式参数数量不匹配");
            const auto *header = std::get_if<ir::Operand>(&prepared.front());
            if (!header)
                throw std::logic_error("Vec::push 的接收者必须是指针");
            return emit_vec_push(builtin->owner, *header,
                                 std::move(prepared.back()));
        }
        if (builtin->kind == BuiltinCallKind::VecRemove) {
            // 显式 Vec::remove(&mut v, index)：接收者和下标按调用顺序求值。
            if (prepared.size() != 2)
                throw std::logic_error("Vec::remove 的显式参数数量不匹配");
            const auto *header = std::get_if<ir::Operand>(&prepared[0]);
            const auto *index = std::get_if<ir::Operand>(&prepared[1]);
            if (!header || !index)
                throw std::logic_error("Vec::remove 的接收者和下标必须是指针/整数");
            return emit_vec_remove(builtin->owner, *header, *index);
        }
    }
    // clone 等剩余容器操作留到 P10。
    diagnostics << "IR 生成尚未支持内置方法或容器构造器\n";
    return std::nullopt;
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
    // 每层只读取一次该层保存的指针；引用和 Box 保存的都是目标地址。
    for (; remaining > 0; --remaining) {
        const bool indirect =
            current.type->kind == SemanticType::Kind::Reference ||
            current.type->kind == SemanticType::Kind::Box;
        if (!indirect || !current.type->inner) {
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
    // 内置查询先按已选路径求值接收者一次；固定数组的 len 结果来自类型，
    // Vec 的 len/is_empty 读取真实头，两者都不能略过接收者求值。
    if (const auto *builtin = std::get_if<BuiltinCall>(&resolved.target)) {
        const auto kind = builtin->kind;
        // 查询类内置方法只读取接收者求值后的头；固定数组的 len 直接取类型长度。
        if (kind == BuiltinCallKind::ArrayLen ||
            kind == BuiltinCallKind::VecLen ||
            kind == BuiltinCallKind::VecIsEmpty) {
            auto receiver = emit_receiver(*call.receiver, resolved, locals);
            if (!receiver || path_ended(*receiver))
                return receiver;
            if (kind == BuiltinCallKind::ArrayLen) {
                // 只返回已求值长度，不读取数组内容也不复制整个数组。
                return EmitResult{ir::Operand::constant(
                    ir::Type::I32, static_cast<uint32_t>(builtin->owner.length))};
            }
            // Vec 查询的接收者按 &self 调整后是指向真实头的指针。
            const auto *header = std::get_if<ir::Operand>(&*receiver);
            if (!header)
                throw std::logic_error("Vec 查询的接收者必须是指针");
            return emit_vec_query(kind, *header);
        }
        // push 的接收者先固定为真实头的地址，普通实参全部求值后才修改头。
        if (kind == BuiltinCallKind::VecPush) {
            auto receiver = emit_receiver(*call.receiver, resolved, locals);
            if (!receiver || path_ended(*receiver))
                return receiver;
            const auto *header = std::get_if<ir::Operand>(&*receiver);
            if (!header)
                throw std::logic_error("Vec::push 的接收者必须是指针");
            if (call.arguments.size() != 1)
                throw std::logic_error("Vec::push 的参数数量与签名不一致");
            auto element = emit_prepared_value(
                *call.arguments[0], locals, {&call, UseKind::Argument, 0});
            if (!element || path_ended(*element))
                return element;
            return emit_vec_push(builtin->owner, *header, std::move(*element));
        }
        // remove 的接收者先固定为真实头地址，再求一次下标后保存/左移。
        if (kind == BuiltinCallKind::VecRemove) {
            if (call.arguments.size() != 1)
                throw std::logic_error("Vec::remove 的参数数量与签名不一致");
            auto receiver = emit_receiver(*call.receiver, resolved, locals);
            if (!receiver || path_ended(*receiver))
                return receiver;
            const auto *header = std::get_if<ir::Operand>(&*receiver);
            if (!header)
                throw std::logic_error("Vec::remove 的接收者必须是指针");
            auto index = emit_prepared_value(
                *call.arguments[0], locals, {&call, UseKind::Argument, 0});
            if (!index || path_ended(*index))
                return index;
            const auto *index_operand = std::get_if<ir::Operand>(&*index);
            if (!index_operand)
                throw std::logic_error("Vec::remove 的下标必须是整数");
            return emit_vec_remove(builtin->owner, *header, *index_operand);
        }
        diagnostics << "IR 生成尚未支持此内置方法调用\n";
        return std::nullopt;
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
