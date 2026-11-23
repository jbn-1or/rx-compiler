// 实现结构体和数组构造、值快照以及按目标布局搬运聚合值。
// 构造按源码顺序求值；大数组的填充和复制生成计数循环，不按长度展开。
#include "ir/lowering.hpp"
#include "ast/ast.hpp"
#include "ir/builder.hpp"
#include "sema/symbol.hpp"
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace rx {
namespace {

// 数组元素下标和字段序号在 LLVM 中统一使用 i32 常量。
ir::Operand index_constant(std::size_t index) {
    if (index > std::numeric_limits<uint32_t>::max())
        throw std::logic_error("聚合下标超出 i32 常量范围");
    return ir::Operand::constant(ir::Type::I32,
                                 static_cast<uint32_t>(index));
}

} // namespace

// 查找字段在声明列表中的序号；sema 已保证字段存在。
std::size_t
GenerationContext::field_declaration_index(const StructSymbol &structure,
                                           const std::string &name) const {
    for (std::size_t index = 0; index < structure.fields.size(); ++index)
        if (structure.fields[index].name == name)
            return index;
    throw std::logic_error("结构体字段缺少声明顺序");
}

// 按源码顺序生成结构体字段初始化，并返回独立临时存储中的聚合值。
std::optional<EmitResult>
GenerationContext::emit_struct(const StructExpression &expression,
                               LocalPlaces &locals) {
    const auto &type = semantics.expressions.at(&expression).type;
    const auto storage = ir::storage_type_of(type);
    const auto address = allocate_storage_slot(storage);
    const StructSymbol &structure = *type.struct_symbol;
    for (std::size_t source_index = 0;
         source_index < expression.fields.size(); ++source_index) {
        const auto &field = expression.fields[source_index];
        const auto &field_type = structure.field_types.at(field.name);
        auto prepared = emit_prepared_value(
            *field.value, locals,
            UseSite{&expression, UseKind::Field, source_index});
        if (!prepared)
            return std::nullopt;
        if (path_ended(*prepared))
            return EmitResult{NoContinuation{}};
        if (std::holds_alternative<UnitResult>(*prepared))
            continue;
        const auto field_place = builder.get_element_ptr(
            storage, address,
            {index_constant(0),
             index_constant(field_declaration_index(structure, field.name))});
        store_prepared_value(field_place, *prepared, field_type);
    }
    return EmitResult{AggregateValue{address, &type}};
}

// 按源码顺序生成数组元素；重复形式只求值一次元素，再复制到各位置。
std::optional<EmitResult>
GenerationContext::emit_array(const ArrayExpression &expression,
                              LocalPlaces &locals) {
    // 每个源码元素先固定为独立的值。[return] 的元素类型可能是 !，没有布局。
    const auto count = expression.repeat_value ? 1 : expression.elements.size();
    std::vector<EmitResult> values;
    values.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto &element = expression.repeat_value
            ? expression.repeat_value : expression.elements[index];
        auto prepared = emit_prepared_value(
            *element, locals, UseSite{&expression, UseKind::ArrayElement, index});
        if (!prepared || path_ended(*prepared))
            return prepared;
        values.push_back(std::move(*prepared));
    }

    const auto &type = semantics.expressions.at(&expression).type;
    const auto storage = ir::storage_type_of(type);
    const auto address = allocate_storage_slot(storage);
    const auto store_element = [&](ir::Operand index, const EmitResult &value) {
        const auto target = builder.get_element_ptr(storage, address,
                                                    {index_constant(0), index});
        store_prepared_value(target, value, *type.inner);
    };
    if (expression.repeat_value) {
        for_each_array_index(type.length, [&](ir::Operand index) {
            store_element(index, values.front());
        });
    } else {
        for (std::size_t index = 0; index < values.size(); ++index)
            store_element(index_constant(index), values[index]);
    }
    return EmitResult{AggregateValue{address, &type}};
}

// 求值一个固定使用位置，并按 sema 已选定的路径调整到目标类型。
std::optional<EmitResult>
GenerationContext::emit_prepared_value(const Expression &expression,
                                       LocalPlaces &locals,
                                       const UseSite &site) {
    auto value = emit_value(expression, locals);
    if (!value || path_ended(*value))
        return value;
    return adjust_value(*value, site);
}

// 对已经求好的结果执行引用调整；聚合值没有逐层指针可读，保持原存储。
std::optional<EmitResult>
GenerationContext::adjust_value(const EmitResult &value,
                                const UseSite &site) {
    if (const auto *operand = std::get_if<ir::Operand>(&value)) {
        auto adjusted = emit_coercion_at(site, *operand);
        if (!adjusted)
            return std::nullopt;
        return EmitResult{*adjusted};
    }
    return value;
}

// 读取源 place 时固定聚合的当前内容；if/loop 等私有结果槽本身就是快照。
std::optional<EmitResult> GenerationContext::read_stored_value(
    const ir::Operand &address, const SemanticType &type, StorageOrigin origin) {
    if (type.is_unit())
        return UnitResult{};
    if (!is_storable_value(type)) {
        diagnostics << "IR 生成尚未支持读取此类型的值（P9）\n";
        return std::nullopt;
    }
    if (is_aggregate_value(type)) {
        if (origin == StorageOrigin::SourcePlace) {
            const auto snapshot =
                allocate_storage_slot(ir::storage_type_of(type));
            copy_value(snapshot, address, type);
            return AggregateValue{snapshot, &type};
        }
        return AggregateValue{address, &type};
    }
    return load_value(address, type);
}

// 把已准备好的值写入目标存储；聚合值按完整结构搬运。
void GenerationContext::store_prepared_value(const ir::Operand &destination,
                                             const EmitResult &prepared,
                                             const SemanticType &type) {
    if (const auto *value = std::get_if<ir::Operand>(&prepared)) {
        store_value(*value, destination);
        return;
    }
    if (const auto *aggregate = std::get_if<AggregateValue>(&prepared)) {
        copy_value(destination, aggregate->address, type);
        return;
    }
    if (std::holds_alternative<UnitResult>(prepared))
        return;
    throw std::logic_error("已结束的路径不能写入聚合目标");
}

// 按聚合结构搬运值：标量直接 load/store，结构体逐字段，数组使用有界循环。
void GenerationContext::copy_value(const ir::Operand &destination,
                                   const ir::Operand &source,
                                   const SemanticType &type) {
    switch (type.kind) {
        case SemanticType::Kind::Unit:
            return;
        case SemanticType::Kind::Int:
        case SemanticType::Kind::Bool:
        case SemanticType::Kind::Reference: {
            store_value(load_value(source, type), destination);
            return;
        }
        case SemanticType::Kind::Box:
            // 聚合内按布局搬运 Box 指针，不读取堆中目标。
            store_value(load_value(source, ir::Type::Ptr), destination);
            return;
        case SemanticType::Kind::Struct: {
            const auto storage = ir::storage_type_of(type);
            const StructSymbol &structure = *type.struct_symbol;
            for (std::size_t index = 0; index < structure.fields.size();
                 ++index) {
                const auto &field = structure.fields[index];
                const auto &field_type = structure.field_types.at(field.name);
                const auto field_index = index_constant(index);
                const auto source_field = builder.get_element_ptr(
                    storage, source, {index_constant(0), field_index});
                const auto destination_field = builder.get_element_ptr(
                    storage, destination, {index_constant(0), field_index});
                copy_value(destination_field, source_field, field_type);
            }
            return;
        }
        case SemanticType::Kind::Array: {
            const auto &element_type = *type.inner;
            const auto storage = ir::storage_type_of(type);
            for_each_array_index(type.length, [&](ir::Operand index) {
                const auto source_element = builder.get_element_ptr(
                    storage, source, {index_constant(0), index});
                const auto destination_element = builder.get_element_ptr(
                    storage, destination, {index_constant(0), index});
                copy_value(destination_element, source_element, element_type);
            });
            return;
        }
        case SemanticType::Kind::Vec: {
            // Vec 头固定为 { ptr, i32, i32 }；P9 再解释其堆内容。
            const auto storage = ir::StorageType::vec();
            for (std::size_t index = 0; index < 3; ++index) {
                const auto field_index = index_constant(index);
                const auto source_field = builder.get_element_ptr(
                    storage, source, {index_constant(0), field_index});
                const auto destination_field = builder.get_element_ptr(
                    storage, destination, {index_constant(0), field_index});
                const auto field_type =
                    index == 0 ? ir::Type::Ptr : ir::Type::I32;
                const auto value = builder.load(
                    field_type, source_field, ir::scalar_layout(field_type).align);
                builder.store(field_type, value, destination_field,
                              ir::scalar_layout(field_type).align);
            }
            return;
        }
        case SemanticType::Kind::Never:
        case SemanticType::Kind::Error:
            throw std::logic_error("发散或错误类型没有聚合值");
    }
}

// 小数组直接使用常量下标；大数组在循环体中使用运行时的 i32 下标。
void GenerationContext::for_each_array_index(
    uint64_t count, const std::function<void(ir::Operand)> &body) {
    constexpr uint64_t unroll_limit = 8;
    if (count <= unroll_limit) {
        for (uint64_t index = 0; index < count; ++index)
            body(index_constant(index));
        return;
    }
    if (count > std::numeric_limits<uint32_t>::max())
        throw std::logic_error("聚合循环长度超出 i32 范围");
    const auto insertion = builder.insert_point();
    auto &function = module.function(insertion->function);
    const auto index_slot = allocate_slot(ir::Type::I32);
    builder.store(ir::Type::I32, index_constant(0), index_slot, 4);

    const auto condition_block = function.add_block();
    const auto body_block = function.add_block();
    const auto exit_block = function.add_block();
    builder.branch(condition_block);

    builder.set_insert_point(condition_block);
    const auto index = load_value(index_slot, ir::Type::I32);
    const auto less = builder.compare(
        ir::Comparison::UnsignedLessThan, index, index_constant(count));
    builder.cond_branch(less, body_block, exit_block);

    builder.set_insert_point(body_block);
    body(index);
    const auto next = builder.binary(ir::BinaryOp::Add, index,
                                     index_constant(1));
    builder.store(ir::Type::I32, next, index_slot, 4);
    builder.branch(condition_block);
    builder.set_insert_point(exit_block);
}

} // namespace rx
