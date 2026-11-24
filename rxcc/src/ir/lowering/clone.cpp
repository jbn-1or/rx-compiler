// 为每个具体类型生成深克隆：拥有 Box/Vec 的内容得到独立存储，
// 共享引用只复制地址，不克隆引用目标。
#include "ir/lowering.hpp"
#include "ir/builder.hpp"
#include "ir/layout.hpp"
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace rx {
namespace {

// 复合类型需要独立辅助函数；标量、bool 和引用直接复制当前值。
bool needs_clone_helper(const SemanticType &type) {
    switch (type.kind) {
        case SemanticType::Kind::Struct:
        case SemanticType::Kind::Array:
        case SemanticType::Kind::Box:
        case SemanticType::Kind::Vec:
            return true;
        case SemanticType::Kind::Unit:
        case SemanticType::Kind::Never:
        case SemanticType::Kind::Error:
        case SemanticType::Kind::Int:
        case SemanticType::Kind::Bool:
        case SemanticType::Kind::Reference:
            return false;
    }
    return false;
}

// 字段/元素下标统一用 i32 常量，和现有聚合投影保持一致。
ir::Operand index_constant(std::size_t index) {
    return ir::Operand::constant(ir::Type::I32, static_cast<uint32_t>(index));
}

} // namespace

// 克隆值本身：destination 和 source 都是按 T 布局的存储地址。
// 数组和结构体逐项克隆；Box 分配新目标后克隆内容；Vec 只复制活动元素。
void GenerationContext::emit_clone_into(const ir::Operand &destination,
                                        const ir::Operand &source,
                                        const SemanticType &type) {
    switch (type.kind) {
        case SemanticType::Kind::Unit:
            return;
        case SemanticType::Kind::Int:
        case SemanticType::Kind::Bool:
        case SemanticType::Kind::Reference:
            // 标量和 bool 复制值；引用只复制地址，目标继续共享。
            copy_value(destination, source, type);
            return;
        case SemanticType::Kind::Box: {
            const auto &inner_type = *type.inner;
            const auto inner_layout =
                layouts_.layout(ir::storage_type_of(inner_type));
            // 先读取原 Box 保存的指针，再申请一份新的内容存储。
            const auto box_pointer = load_value(source, ir::Type::Ptr);
            const auto allocated = emit_alloc(
                ir::Operand::constant(ir::Type::I32, inner_layout.size),
                ir::Operand::constant(ir::Type::I32, inner_layout.align));
            emit_clone_member(allocated, box_pointer, inner_type);
            // 新 Box 保存独立分配地址，原 Box 的拥有关系不受影响。
            store_value(allocated, destination);
            return;
        }
        case SemanticType::Kind::Vec: {
            const auto &element_type = *type.inner;
            const auto header = ir::StorageType::vec();
            const auto zero = ir::Operand::constant(ir::Type::I32, 0);
            const auto field_address = [&](const ir::Operand &base,
                                           uint32_t field) {
                return builder.get_element_ptr(
                    header, base, {zero, ir::Operand::constant(ir::Type::I32,
                                                               field)});
            };
            const auto source_data = field_address(source, 0);
            const auto source_length = field_address(source, 1);
            const auto destination_data = field_address(destination, 0);
            const auto destination_length = field_address(destination, 1);
            const auto destination_capacity = field_address(destination, 2);
            const auto data = builder.load(ir::Type::Ptr, source_data, 4);
            const auto length = builder.load(ir::Type::I32, source_length, 4);
            // 新活动长度与原 Vec 相同；容量不复制，也不读取备用容量。
            store_value(length, destination_length);

            const auto insertion = builder.insert_point();
            auto &function = module.function(insertion->function);
            const auto empty_block = function.add_block();
            const auto copy_block = function.add_block();
            const auto done_block = function.add_block();
            const auto is_empty =
                builder.compare(ir::Comparison::Equal, length, zero);
            builder.cond_branch(is_empty, empty_block, copy_block);

            // 空 Vec 不申请零字节缓冲区，数据指针保持 null、容量为 0。
            builder.set_insert_point(empty_block);
            store_value(ir::Operand::constant(ir::Type::Ptr, 0),
                        destination_data);
            store_value(zero, destination_capacity);
            builder.branch(done_block);

            // 非空时申请恰好能容纳活动元素的新缓冲区，再逐元素克隆。
            builder.set_insert_point(copy_block);
            const auto element_layout =
                layouts_.layout(ir::storage_type_of(element_type));
            const auto bytes = builder.binary(
                ir::BinaryOp::Multiply, length,
                ir::Operand::constant(ir::Type::I32, element_layout.size));
            const auto new_data = emit_alloc(
                bytes,
                ir::Operand::constant(ir::Type::I32, element_layout.align));
            const auto buffer = ir::StorageType::array_of(
                ir::storage_type_of(element_type), 0);
            for_each_runtime_index(length, [&](ir::Operand index) {
                const auto source_element = builder.get_element_ptr(
                    buffer, data, {zero, index});
                const auto destination_element = builder.get_element_ptr(
                    buffer, new_data, {zero, index});
                emit_clone_member(destination_element, source_element,
                                  element_type);
            });
            store_value(new_data, destination_data);
            store_value(length, destination_capacity);
            builder.branch(done_block);

            builder.set_insert_point(done_block);
            return;
        }
        case SemanticType::Kind::Struct: {
            const auto storage = ir::storage_type_of(type);
            const auto &structure = *type.struct_symbol;
            for (std::size_t index = 0; index < structure.fields.size();
                 ++index) {
                const auto &field = structure.fields[index];
                const auto &field_type =
                    structure.field_types.at(field.name);
                const auto field_index = index_constant(index);
                const auto source_field = builder.get_element_ptr(
                    storage, source, {index_constant(0), field_index});
                const auto destination_field = builder.get_element_ptr(
                    storage, destination, {index_constant(0), field_index});
                emit_clone_member(destination_field, source_field, field_type);
            }
            return;
        }
        case SemanticType::Kind::Array: {
            const auto storage = ir::storage_type_of(type);
            for_each_array_index(type.length, [&](ir::Operand index) {
                const auto source_element = builder.get_element_ptr(
                    storage, source, {index_constant(0), index});
                const auto destination_element = builder.get_element_ptr(
                    storage, destination, {index_constant(0), index});
                emit_clone_member(destination_element, source_element,
                                  *type.inner);
            });
            return;
        }
        case SemanticType::Kind::Never:
        case SemanticType::Kind::Error:
            throw std::logic_error("发散或错误类型没有可克隆的值");
    }
    throw std::logic_error("未知源类型不能深克隆");
}

// 复合成员走自己的辅助函数，避免把整个类型树内联到每个调用点；
// 标量、bool 和引用直接复制，引用不会顺着指针克隆目标。
void GenerationContext::emit_clone_member(const ir::Operand &destination,
                                          const ir::Operand &source,
                                          const SemanticType &type) {
    if (needs_clone_helper(type)) {
        builder.call(request_helper(HelperKind::Clone, type), {destination, source});
        return;
    }
    emit_clone_into(destination, source, type);
}

// 从保存 T 的 source 地址产生克隆结果：标量/引用直接读取，
// 拥有容器先调用辅助函数写入独立存储，再按 T 的值形式读回。
std::optional<EmitResult>
GenerationContext::emit_clone(const SemanticType &type, ir::Operand source) {
    if (!needs_clone_helper(type))
        return EmitResult{load_value(source, type)};
    const auto destination =
        allocate_storage_slot(ir::storage_type_of(type));
    builder.call(request_helper(HelperKind::Clone, type), {destination, source});
    // 聚合结果留在独立存储中；Box 这类指针值从存储读回 ptr。
    if (is_aggregate_value(type))
        return EmitResult{AggregateValue{destination, &type}};
    return EmitResult{load_value(destination, type)};
}

} // namespace rx