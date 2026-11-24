// 为每个具体类型生成按内容比较的相等辅助函数。
// 比较借用两侧地址，Box 比较内容、Vec 先比长度再比活动元素，引用比较目标。
#include "ir/lowering.hpp"
#include "ast/ast.hpp"
#include "ir/builder.hpp"
#include "ir/layout.hpp"
#include "ir/lowering/value.hpp"
#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>

namespace rx {
namespace {

// 复合类型和引用需要独立辅助函数；整数和 bool 直接比较计算值。
bool needs_equality_helper(const SemanticType &type) {
    switch (type.kind) {
        case SemanticType::Kind::Struct:
        case SemanticType::Kind::Array:
        case SemanticType::Kind::Box:
        case SemanticType::Kind::Vec:
        case SemanticType::Kind::Reference:
            return true;
        case SemanticType::Kind::Unit:
        case SemanticType::Kind::Never:
        case SemanticType::Kind::Error:
        case SemanticType::Kind::Int:
        case SemanticType::Kind::Bool:
            return false;
    }
    return false;
}

// 字段/元素下标统一使用 i32 常量，和现有聚合投影保持一致。
ir::Operand index_constant(std::size_t index) {
    return ir::Operand::constant(ir::Type::I32, static_cast<uint32_t>(index));
}

// 生成 i1 常量，供相等结果累加的初值和取反使用。
ir::Operand boolean_constant(bool value) {
    return ir::Operand::constant(ir::Type::I1, value ? 1u : 0u);
}

} // namespace

// 取得一侧操作数的借用地址：已有 place 保留原位置，
// 值表达式只物化一次并保留到比较结束，不移动或深克隆源值。
std::optional<PlaceResult>
GenerationContext::emit_equality_operand(const Expression &expression,
                                         LocalPlaces &locals) {
    const auto &record = semantics.expressions.at(&expression);
    if (record.place.is_place)
        return emit_place(expression, locals);
    auto value = emit_value(expression, locals);
    if (!value)
        return std::nullopt;
    if (path_ended(*value))
        return PlaceResult{NoContinuation{}};
    // 聚合值已经在独立临时存储中，借用直接使用该地址。
    if (const auto *aggregate = std::get_if<AggregateValue>(&*value))
        return PlaceResult{Place{aggregate->address, &record.type}};
    return PlaceResult{
        Place{materialize_value(std::get<ir::Operand>(*value)), &record.type}};
}

// 比较一个嵌套字段或元素：复合类型调用自己的辅助函数，
// 整数和 bool 直接读取两侧计算值比较。
ir::Operand GenerationContext::emit_equality_member(const ir::Operand &left,
                                                    const ir::Operand &right,
                                                    const SemanticType &type) {
    if (needs_equality_helper(type)) {
        const auto result =
            builder.call(request_helper(HelperKind::Equality, type), {left, right});
        if (!result)
            throw std::logic_error("相等辅助函数必须返回 i1");
        return *result;
    }
    return builder.compare(ir::Comparison::Equal, load_value(left, type),
                           load_value(right, type));
}

// 比较 left/right 地址中的 T，返回一次相等结果。
// 结构体逐字段累加，数组按元素累加，Vec 先检查长度再比较活动元素。
ir::Operand GenerationContext::emit_equality_into(const ir::Operand &left,
                                                  const ir::Operand &right,
                                                  const SemanticType &type) {
    switch (type.kind) {
        case SemanticType::Kind::Int:
        case SemanticType::Kind::Bool:
            return emit_equality_member(left, right, type);
        case SemanticType::Kind::Reference:
        case SemanticType::Kind::Box: {
            // 引用和 Box 的存储都保存目标指针；比较目标内容而不是指针地址。
            const auto left_pointer = load_value(left, ir::Type::Ptr);
            const auto right_pointer = load_value(right, ir::Type::Ptr);
            return emit_equality_member(left_pointer, right_pointer,
                                        *type.inner);
        }
        case SemanticType::Kind::Struct: {
            const auto storage = ir::storage_type_of(type);
            const auto &structure = *type.struct_symbol;
            // 空结构体没有字段，语义上彼此相等。
            auto result = boolean_constant(true);
            for (std::size_t index = 0; index < structure.fields.size();
                 ++index) {
                const auto &field = structure.fields[index];
                const auto &field_type =
                    structure.field_types.at(field.name);
                const auto field_index = index_constant(index);
                const auto left_field = builder.get_element_ptr(
                    storage, left, {index_constant(0), field_index});
                const auto right_field = builder.get_element_ptr(
                    storage, right, {index_constant(0), field_index});
                result = builder.binary(
                    ir::BinaryOp::And, result,
                    emit_equality_member(left_field, right_field, field_type));
            }
            return result;
        }
        case SemanticType::Kind::Array: {
            const auto storage = ir::storage_type_of(type);
            // 用私有 i1 槽累计所有元素结果；大数组循环也不会按长度展开。
            const auto result_slot = allocate_slot(ir::Type::I1);
            store_value(boolean_constant(true), result_slot);
            for_each_array_index(type.length, [&](ir::Operand index) {
                const auto left_element = builder.get_element_ptr(
                    storage, left, {index_constant(0), index});
                const auto right_element = builder.get_element_ptr(
                    storage, right, {index_constant(0), index});
                const auto element_equal = emit_equality_member(
                    left_element, right_element, *type.inner);
                const auto combined = builder.binary(
                    ir::BinaryOp::And, load_value(result_slot, ir::Type::I1),
                    element_equal);
                store_value(combined, result_slot);
            });
            return load_value(result_slot, ir::Type::I1);
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
            const auto left_data_field = field_address(left, 0);
            const auto left_length_field = field_address(left, 1);
            const auto right_data_field = field_address(right, 0);
            const auto right_length_field = field_address(right, 1);
            const auto left_length =
                load_value(left_length_field, ir::Type::I32);
            const auto right_length =
                load_value(right_length_field, ir::Type::I32);
            // 长度不同立即判不等；只有长度相同才访问活动元素。
            const auto lengths_equal = builder.compare(
                ir::Comparison::Equal, left_length, right_length);
            const auto result_slot = allocate_slot(ir::Type::I1);
            store_value(lengths_equal, result_slot);

            const auto insertion = builder.insert_point();
            auto &function = module.function(insertion->function);
            const auto elements_block = function.add_block();
            const auto done_block = function.add_block();
            builder.cond_branch(lengths_equal, elements_block, done_block);

            builder.set_insert_point(elements_block);
            store_value(boolean_constant(true), result_slot);
            const auto left_data =
                builder.load(ir::Type::Ptr, left_data_field, 4);
            const auto right_data =
                builder.load(ir::Type::Ptr, right_data_field, 4);
            const auto buffer = ir::StorageType::array_of(
                ir::storage_type_of(element_type), 0);
            for_each_runtime_index(left_length, [&](ir::Operand index) {
                const auto left_element = builder.get_element_ptr(
                    buffer, left_data, {zero, index});
                const auto right_element = builder.get_element_ptr(
                    buffer, right_data, {zero, index});
                const auto element_equal = emit_equality_member(
                    left_element, right_element, element_type);
                const auto combined = builder.binary(
                    ir::BinaryOp::And, load_value(result_slot, ir::Type::I1),
                    element_equal);
                store_value(combined, result_slot);
            });
            builder.branch(done_block);

            builder.set_insert_point(done_block);
            return load_value(result_slot, ir::Type::I1);
        }
        case SemanticType::Kind::Unit:
        case SemanticType::Kind::Never:
        case SemanticType::Kind::Error:
            throw std::logic_error("该源类型没有可比较的相等内容");
    }
    throw std::logic_error("未知源类型不能比较相等");
}

// 按普通左右顺序借用两个操作数；两侧都准备好后才生成一次比较。
std::optional<EmitResult>
GenerationContext::emit_equality(const BinaryExpression &expression,
                                 LocalPlaces &locals) {
    const auto &type = semantics.expressions.at(expression.left.get()).type;
    // 直接标量按值比较，不需要为借用在临时存储中再复制一份。
    if (type.kind == SemanticType::Kind::Int ||
        type.kind == SemanticType::Kind::Bool) {
        auto left = emit_value(*expression.left, locals);
        if (!left)
            return std::nullopt;
        if (path_ended(*left))
            return EmitResult{NoContinuation{}};
        auto right = emit_value(*expression.right, locals);
        if (!right)
            return std::nullopt;
        if (path_ended(*right))
            return EmitResult{NoContinuation{}};
        auto equal = builder.compare(
            ir::Comparison::Equal, std::get<ir::Operand>(*left),
            std::get<ir::Operand>(*right));
        if (expression.operation == BinaryOperator::NotEqual)
            equal = builder.binary(ir::BinaryOp::Xor, equal,
                                   boolean_constant(true));
        return EmitResult{equal};
    }

    auto left = emit_equality_operand(*expression.left, locals);
    if (!left)
        return std::nullopt;
    if (path_ended(*left))
        return EmitResult{NoContinuation{}};
    auto right = emit_equality_operand(*expression.right, locals);
    if (!right)
        return std::nullopt;
    if (path_ended(*right))
        return EmitResult{NoContinuation{}};

    auto equal = emit_equality_member(std::get<Place>(*left).address,
                                      std::get<Place>(*right).address, type);
    // != 只对已经得到的相等结果取反一次，不重新求值两个操作数。
    if (expression.operation == BinaryOperator::NotEqual)
        equal = builder.binary(ir::BinaryOp::Xor, equal,
                               boolean_constant(true));
    return EmitResult{equal};
}

} // namespace rx