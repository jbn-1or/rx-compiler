// 为每个具体类型生成深克隆：拥有 Box/Vec 的内容得到独立存储，
// 共享引用只复制地址，不克隆引用目标。
#include "ir/lowering.hpp"
#include "ir/builder.hpp"
#include "ir/layout.hpp"
#include "sema/symbol.hpp"
#include <stdexcept>

namespace rx {
namespace {

// 复合类型需要独立辅助函数；标量、bool 和引用直接复制当前值。
bool needs_clone_helper(const SemanticType &type) {
    return is_aggregate_value(type) || type.kind == SemanticType::Kind::Box;
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
            const auto zero = ir::Operand::constant(ir::Type::I32, 0);
            const auto source_data = vec_field_address(source, VecField::Data);
            const auto source_length = vec_field_address(source, VecField::Length);
            const auto destination_data = vec_field_address(destination, VecField::Data);
            const auto destination_length = vec_field_address(destination, VecField::Length);
            const auto destination_capacity = vec_field_address(destination, VecField::Capacity);
            const auto data = builder.load(ir::Type::Ptr, source_data, 4);
            const auto length = builder.load(ir::Type::I32, source_length, 4);
            // 克隆只保留活动元素，新头的长度和容量都等于原长度。
            store_value(length, destination_length);
            store_value(length, destination_capacity);

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
            const auto loop = begin_runtime_loop(length);
            const auto source_address = vec_element_address(data, element_type, loop.index);
            const auto destination_address =
                vec_element_address(new_data, element_type, loop.index);
            emit_clone_member(destination_address, source_address, element_type);
            end_runtime_loop(loop);
            store_value(new_data, destination_data);
            builder.branch(done_block);

            builder.set_insert_point(done_block);
            return;
        }
        case SemanticType::Kind::Struct: {
            const auto &structure = *type.struct_symbol;
            for (std::size_t index = 0; index < structure.fields.size(); ++index) {
                const auto &field = structure.fields[index];
                const auto field_index = ir::Operand::constant(ir::Type::I32, index);
                const auto destination_address = member_address(destination, type, field_index);
                const auto source_address = member_address(source, type, field_index);
                const auto &member_type = structure.field_types.at(field.name);
                emit_clone_member(destination_address, source_address, member_type);
            }
            return;
        }
        case SemanticType::Kind::Array: {
            if (type.length <= array_unroll_limit) {
                for (uint64_t index = 0; index < type.length; ++index) {
                    const auto element_index = ir::Operand::constant(ir::Type::I32, index);
                    const auto destination_address =
                        member_address(destination, type, element_index);
                    const auto source_address = member_address(source, type, element_index);
                    emit_clone_member(destination_address, source_address, *type.inner);
                }
            } else {
                const auto loop = begin_runtime_loop(
                    ir::Operand::constant(ir::Type::I32, type.length));
                const auto destination_address = member_address(destination, type, loop.index);
                const auto source_address = member_address(source, type, loop.index);
                emit_clone_member(destination_address, source_address, *type.inner);
                end_runtime_loop(loop);
            }
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
EmitResult
GenerationContext::emit_clone(const SemanticType &type, ir::Operand source) {
    if (!needs_clone_helper(type))
        return load_value(source, type);
    const auto destination =
        allocate_storage_slot(ir::storage_type_of(type));
    builder.call(request_helper(HelperKind::Clone, type), {destination, source});
    // 聚合结果留在独立存储中；Box 这类指针值从存储读回 ptr。
    if (is_aggregate_value(type))
        return AggregateValue{destination};
    return load_value(destination, type);
}

} // namespace rx
