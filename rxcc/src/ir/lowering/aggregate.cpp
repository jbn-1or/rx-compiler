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
ir::Operand index_constant(uint64_t index) {
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
        if (!prepared || path_ended(*prepared))
            return prepared;
        if (std::holds_alternative<UnitResult>(*prepared))
            continue;
        const auto field_place = builder.get_element_ptr(
            storage, address,
            {index_constant(0),
             index_constant(field_declaration_index(structure, field.name))});
        store_prepared_value(field_place, *prepared, field_type);
    }
    return AggregateValue{address};
}

// 按源码顺序生成数组元素；重复形式只求值一次元素，再复制到各位置。
std::optional<EmitResult>
GenerationContext::emit_array(const ArrayExpression &expression,
                              LocalPlaces &locals) {
    const auto &type = semantics.expressions.at(&expression).type;
    // 先求值，再分配存储：元素可能提前 return，此时整个数组不需要布局。
    if (expression.repeat_value) {
        // [value; N] 只求值一次 value，后面再将这个值写入各位置。
        auto prepared = emit_prepared_value(
            *expression.repeat_value, locals, UseSite{&expression, UseKind::ArrayElement, 0});
        if (!prepared || path_ended(*prepared))
            return prepared;
        const auto address = allocate_storage_slot(ir::storage_type_of(type));
        fill_array(address, type, *prepared);
        return AggregateValue{address};
    }

    // 普通数组按源码顺序固定各元素的值，再写入数组存储。
    std::vector<EmitResult> values;
    values.reserve(expression.elements.size());
    for (std::size_t index = 0; index < expression.elements.size(); ++index) {
        auto prepared = emit_prepared_value(
            *expression.elements[index], locals,
            UseSite{&expression, UseKind::ArrayElement, index});
        if (!prepared || path_ended(*prepared))
            return prepared;
        values.push_back(std::move(*prepared));
    }

    const auto address = allocate_storage_slot(ir::storage_type_of(type));
    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto element_address = member_address(address, type, index_constant(index));
        store_prepared_value(element_address, values[index], *type.inner);
    }
    return AggregateValue{address};
}

// 求值一个固定使用位置，并按 sema 已选定的路径调整到目标类型。
std::optional<EmitResult>
GenerationContext::emit_prepared_value(const Expression &expression,
                                       LocalPlaces &locals,
                                       const UseSite &site) {
    auto value = emit_value(expression, locals);
    if (!value)
        return value;
    // 只有标量需要引用调整；聚合值、unit 和已结束的路径原样传递。
    if (const auto *operand = std::get_if<ir::Operand>(&*value))
        return emit_coercion_at(site, *operand);
    return value;
}

// 读取源 place 时固定聚合的当前内容；if/loop 等私有结果槽本身就是快照。
std::optional<EmitResult> GenerationContext::read_stored_value(
    const ir::Operand &address, const SemanticType &type, StorageOrigin origin) {
    if (type.is_unit())
        return UnitResult{};
    if (!is_storable_value(type)) {
        diagnostics << "IR 生成尚未支持从存储中读取类型 " << type.toString() << '\n';
        return std::nullopt;
    }
    if (is_aggregate_value(type)) {
        if (origin == StorageOrigin::SourcePlace) {
            const auto snapshot =
                allocate_storage_slot(ir::storage_type_of(type));
            copy_value(snapshot, address, type);
            return AggregateValue{snapshot};
        }
        return AggregateValue{address};
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
        case SemanticType::Kind::Reference:
        case SemanticType::Kind::Box: {
            store_value(load_value(source, type), destination);
            return;
        }
        case SemanticType::Kind::Struct: {
            const auto &structure = *type.struct_symbol;
            for (std::size_t index = 0; index < structure.fields.size(); ++index) {
                const auto &field = structure.fields[index];
                const auto destination_address =
                    member_address(destination, type, index_constant(index));
                const auto source_address = member_address(source, type, index_constant(index));
                const auto &member_type = structure.field_types.at(field.name);
                copy_value(destination_address, source_address, member_type);
            }
            return;
        }
        case SemanticType::Kind::Array: {
            if (type.length <= array_unroll_limit) {
                for (uint64_t index = 0; index < type.length; ++index) {
                    const auto destination_address =
                        member_address(destination, type, index_constant(index));
                    const auto source_address =
                        member_address(source, type, index_constant(index));
                    copy_value(destination_address, source_address, *type.inner);
                }
            } else {
                const auto loop = begin_runtime_loop(index_constant(type.length));
                const auto destination_address = member_address(destination, type, loop.index);
                const auto source_address = member_address(source, type, loop.index);
                copy_value(destination_address, source_address, *type.inner);
                end_runtime_loop(loop);
            }
            return;
        }
        case SemanticType::Kind::Vec: {
            // 移动 Vec 只搬运头，不复制缓冲区。
            for (auto field : {VecField::Data, VecField::Length, VecField::Capacity}) {
                const auto source_address = vec_field_address(source, field);
                const auto destination_address = vec_field_address(destination, field);
                const auto field_type =
                    field == VecField::Data ? ir::Type::Ptr : ir::Type::I32;
                store_value(load_value(source_address, field_type), destination_address);
            }
            return;
        }
        case SemanticType::Kind::Never:
        case SemanticType::Kind::Error:
            throw std::logic_error("发散或错误类型没有聚合值");
    }
}

// 用同一种 GEP 写法取得数组元素或结构体字段的地址。
ir::Operand GenerationContext::member_address(
    const ir::Operand &address, const SemanticType &type, ir::Operand index) {
    return builder.get_element_ptr(ir::storage_type_of(type), address,
                                   {index_constant(0), index});
}

// 重复数组的元素只求值一次；这里只生成向各个位置写入该值的操作。
void GenerationContext::fill_array(const ir::Operand &address,
                                    const SemanticType &type,
                                    const EmitResult &value) {
    if (type.length <= array_unroll_limit) {
        for (uint64_t index = 0; index < type.length; ++index) {
            const auto element_address = member_address(address, type, index_constant(index));
            store_prepared_value(element_address, value, *type.inner);
        }
    } else {
        const auto loop = begin_runtime_loop(index_constant(type.length));
        const auto element_address = member_address(address, type, loop.index);
        store_prepared_value(element_address, value, *type.inner);
        end_runtime_loop(loop);
    }
}

} // namespace rx
