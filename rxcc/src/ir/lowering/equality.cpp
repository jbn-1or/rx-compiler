// 为每个具体类型生成按内容比较的相等辅助函数。
// 比较借用两侧地址，Box 比较内容、Vec 先比长度再比活动元素，引用比较目标。
#include "ir/lowering.hpp"
#include "ast/ast.hpp"
#include "ir/builder.hpp"
#include "ir/layout.hpp"
#include "ir/lowering/value.hpp"
#include "sema/symbol.hpp"
#include <stdexcept>
#include <variant>

namespace rx {
namespace {

// 复合类型和引用需要独立辅助函数；整数和 bool 直接比较计算值。
bool needs_equality_helper(const SemanticType &type) {
    return is_aggregate_value(type) || type.kind == SemanticType::Kind::Box ||
           is_reference(type);
}

// 生成 i1 常量，供相等结果累加的初值和取反使用。
ir::Operand boolean_constant(bool value) {
    return ir::Operand::constant(ir::Type::I1, value ? 1u : 0u);
}

} // namespace

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
            // 空结构体没有字段，语义上彼此相等。
            auto result = boolean_constant(true);
            const auto &structure = *type.struct_symbol;
            for (std::size_t index = 0; index < structure.fields.size(); ++index) {
                const auto &field = structure.fields[index];
                const auto field_index = ir::Operand::constant(ir::Type::I32, index);
                const auto left_address = member_address(left, type, field_index);
                const auto right_address = member_address(right, type, field_index);
                const auto &member_type = structure.field_types.at(field.name);
                result = builder.binary(ir::BinaryOp::And, result,
                    emit_equality_member(left_address, right_address, member_type));
            }
            return result;
        }
        case SemanticType::Kind::Array: {
            // 用私有 i1 槽累计所有元素结果；大数组循环也不会按长度展开。
            const auto result_slot = allocate_slot(ir::Type::I1);
            store_value(boolean_constant(true), result_slot);
            if (type.length <= array_unroll_limit) {
                for (uint64_t index = 0; index < type.length; ++index) {
                    const auto element_index = ir::Operand::constant(ir::Type::I32, index);
                    const auto left_address = member_address(left, type, element_index);
                    const auto right_address = member_address(right, type, element_index);
                    accumulate_equality(result_slot, left_address, right_address, *type.inner);
                }
            } else {
                const auto loop = begin_runtime_loop(
                    ir::Operand::constant(ir::Type::I32, type.length));
                const auto left_address = member_address(left, type, loop.index);
                const auto right_address = member_address(right, type, loop.index);
                accumulate_equality(result_slot, left_address, right_address, *type.inner);
                end_runtime_loop(loop);
            }
            return load_value(result_slot, ir::Type::I1);
        }
        case SemanticType::Kind::Vec: {
            const auto &element_type = *type.inner;
            const auto left_length_field = vec_field_address(left, VecField::Length);
            const auto right_length_field = vec_field_address(right, VecField::Length);
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
            const auto left_data =
                load_value(vec_field_address(left, VecField::Data), ir::Type::Ptr);
            const auto right_data =
                load_value(vec_field_address(right, VecField::Data), ir::Type::Ptr);
            const auto loop = begin_runtime_loop(left_length);
            const auto left_address = vec_element_address(left_data, element_type, loop.index);
            const auto right_address = vec_element_address(right_data, element_type, loop.index);
            accumulate_equality(result_slot, left_address, right_address, element_type);
            end_runtime_loop(loop);
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

// 所有成员都相等时结果才为 true；槽用于跨运行时循环保存累计结果。
void GenerationContext::accumulate_equality(
    const ir::Operand &result_slot, const ir::Operand &left,
    const ir::Operand &right, const SemanticType &type) {
    const auto equal = emit_equality_member(left, right, type);
    const auto combined = builder.binary(
        ir::BinaryOp::And, load_value(result_slot, ir::Type::I1), equal);
    store_value(combined, result_slot);
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
        if (!left || path_ended(*left))
            return left;
        auto right = emit_value(*expression.right, locals);
        if (!right || path_ended(*right))
            return right;
        auto equal = builder.compare(
            ir::Comparison::Equal, std::get<ir::Operand>(*left),
            std::get<ir::Operand>(*right));
        if (expression.operation == BinaryOperator::NotEqual)
            equal = builder.binary(ir::BinaryOp::Xor, equal,
                                   boolean_constant(true));
        return EmitResult{equal};
    }

    auto left = emit_borrowed_place(*expression.left, locals);
    if (!left)
        return std::nullopt;
    if (path_ended(*left))
        return EmitResult{NoContinuation{}};
    auto right = emit_borrowed_place(*expression.right, locals);
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
