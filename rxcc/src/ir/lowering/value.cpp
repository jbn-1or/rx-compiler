// 实现 lowering 的类型映射、基础值读取、存储和临时值物化。
#include "ir/lowering/value.hpp"
#include "ast/ast.hpp"
#include "ir/builder.hpp"
#include "ir/layout.hpp"
#include "ir/lowering.hpp"
#include <cstdint>
#include <variant>

namespace rx {
// 局部绑定、临时值和分支结果都使用同一套标量存储规则。
ir::Operand GenerationContext::allocate_slot(ir::Type value_type) {
    const auto storage_type = ir::scalar_storage_type(value_type);
    return builder.entry_alloca(storage_type, ir::scalar_layout(storage_type).align);
}

// 判断源类型是否是当前生成器支持的标量（整数或 bool）。
bool is_scalar(const SemanticType &type) {
    return type.is_int() || type.kind == SemanticType::Kind::Bool;
}

// 计算标量值使用的 IR 类型：bool 为 i1，整数为 i32。
ir::Type lower_scalar_type(const SemanticType &type) {
    return type.kind == SemanticType::Kind::Bool ? ir::Type::I1 : ir::Type::I32;
}

// 判断源类型是否是引用；引用的计算值和存储单元都只保存目标地址。
bool is_reference(const SemanticType &type) {
    return type.kind == SemanticType::Kind::Reference;
}

// 判断源类型是否是当前可保存和读取的普通值。
// 引用可以继续指向引用，但最内层必须是标量；聚合引用留到后续阶段。
bool is_supported_value(const SemanticType &type) {
    if (is_scalar(type))
        return true;
    if (!is_reference(type))
        return false;
    return is_supported_value(*type.inner);
}

// 计算普通值使用的 IR 类型：标量沿用标量映射，引用统一为 ptr。
ir::Type lower_value_type(const SemanticType &type) {
    if (is_reference(type))
        return ir::Type::Ptr;
    return lower_scalar_type(type);
}

// 把字面量、路径或引用本地读取生成为 IR 操作数。
// 引用路径只加载 slot 中保存的指针，不会顺便读取目标内容。
ir::Operand GenerationContext::emit_leaf(const Expression &expression,
                                         const LocalPlaces &locals) {
    // 整数文字直接使用语义阶段保存的类型和位模式。
    if (const auto *literal =
            dynamic_cast<const IntegerLiteral *>(&expression)) {
        const auto &literal_record = semantics.integer_literals.at(literal);
        return ir::Operand::constant(ir::Type::I32, literal_record.bits);
    }
    // true/false 直接生成 i1 常量，false 为 0，true 为 1。
    if (const auto *boolean =
            dynamic_cast<const BooleanLiteral *>(&expression)) {
        return ir::Operand::constant(ir::Type::I1, boolean->value ? 1u : 0u);
    }
    const auto &path = static_cast<const PathExpression &>(expression);
    const auto &target = semantics.paths.at(&path);
    // 局部变量按 LocalId 找存储位置，不能按名称处理遮蔽。
    if (const auto *local_id = std::get_if<LocalId>(&target)) {
        const auto &type = semantics.locals[local_id->index].type;
        // 标量和引用读取统一经过 load_value：bool 补 trunc，引用读出 ptr。
        return load_value(locals.at(*local_id), type);
    }
    // 常量路径从已求值缓存取值，不生成运行时存储。
    const auto constant_id = std::get<ConstantId>(target);
    const auto &value = semantics.constants[constant_id.index]->evaluated;
    uint32_t bits = 0;
    // bool 缓存保存 true/false，先转成 0/1；整数区分有符号与无符号表示。
    if (value.type.kind == SemanticType::Kind::Bool)
        bits = std::get<bool>(value.value) ? 1u : 0u;
    else if (value.type.int_kind == SemanticType::IntKind::I32 ||
             value.type.int_kind == SemanticType::IntKind::Isize)
        bits = static_cast<uint32_t>(std::get<int64_t>(value.value));
    else
        bits = static_cast<uint32_t>(std::get<uint64_t>(value.value));
    return ir::Operand::constant(lower_scalar_type(value.type), bits);
}

// 把普通值写入它的存储单元；bool 先 zext 成 i8，引用直接写 ptr，整数写 i32。
void GenerationContext::store_value(const ir::Operand &value,
                                    const ir::Operand &address) {
    const auto storage_type = ir::scalar_storage_type(value.type);
    const auto stored = storage_type == value.type
                            ? value : builder.zext(storage_type, value);
    builder.store(storage_type, stored, address, ir::scalar_layout(storage_type).align);
}

// 按源类型选择要读回的计算值类型。
ir::Operand GenerationContext::load_value(const ir::Operand &address,
                                           const SemanticType &type) {
    return load_value(address, lower_value_type(type));
}

// 从存储单元读回普通值；bool 先 load i8 再 trunc 成 i1，引用只读出 ptr。
ir::Operand GenerationContext::load_value(const ir::Operand &address,
                                           ir::Type value_type) {
    const auto storage_type = ir::scalar_storage_type(value_type);
    const auto loaded = builder.load(storage_type, address,
                                     ir::scalar_layout(storage_type).align);
    return storage_type == value_type ? loaded : builder.trunc(value_type, loaded);
}

// 每个借用位置只发射一次；循环回边只重复执行正文中的 store。
ir::Operand GenerationContext::materialize_value(ir::Operand value) {
    const auto address = allocate_slot(value.type);
    store_value(value, address);
    return address;
}

} // namespace rx
