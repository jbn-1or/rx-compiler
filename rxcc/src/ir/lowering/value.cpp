// 实现 lowering 的类型映射、基础值读取、存储和临时值物化。
#include "ir/lowering/value.hpp"
#include "ast/ast.hpp"
#include "ir/builder.hpp"
#include "ir/lowering.hpp"
#include <cstdint>
#include <variant>

namespace rx {
namespace {

// 按已求出的 IR 值类型选择临时 slot 的存储类型：i1 存成 i8，其余保持自身。
ir::Type temporary_storage_type(ir::Type value_type) {
    return value_type == ir::Type::I1 ? ir::Type::I8 : value_type;
}

// bool 的临时 slot 按 1 字节对齐；整数和指针按 4 字节对齐。
uint32_t temporary_alignment(ir::Type value_type) {
    return value_type == ir::Type::I1 ? 1u : 4u;
}

} // namespace

// 局部存储使用的 IR 类型：bool 用 i8，整数用 i32，引用保存目标地址用 ptr。
ir::Type GenerationContext::slot_type(const SemanticType &type) {
    if (type.kind == SemanticType::Kind::Reference)
        return ir::Type::Ptr;
    return type.kind == SemanticType::Kind::Bool ? ir::Type::I8 : ir::Type::I32;
}

// bool 的 i8 存储单元按 1 字节对齐；整数和引用地址按 4 字节对齐。
uint32_t GenerationContext::slot_alignment(const SemanticType &type) {
    return type.kind == SemanticType::Kind::Bool ? 1u : 4u;
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
    // bool 的存储宽度是 i8，写入前先把 i1 零扩展。
    if (value.type == ir::Type::I1) {
        builder.store(ir::Type::I8, builder.zext(ir::Type::I8, value),
                      address, 1);
        return;
    }
    // 引用值本身就是目标地址，slot 保存 ptr；读取它不会读取目标内容。
    if (value.type == ir::Type::Ptr) {
        builder.store(ir::Type::Ptr, value, address, 4);
        return;
    }
    builder.store(ir::Type::I32, value, address, 4);
}

// 从存储单元读回普通值；bool 先 load i8 再 trunc 成 i1，引用读出 ptr。
ir::Operand GenerationContext::load_value(const ir::Operand &address,
                                          const SemanticType &type) {
    // bool 的读取必须经过 i8，不能直接把 i8 当 i1 使用。
    if (type.kind == SemanticType::Kind::Bool)
        return builder.trunc(ir::Type::I1,
                             builder.load(ir::Type::I8, address, 1));
    // 读取引用变量只得到它保存的目标地址；目标内容需要显式解引用。
    if (is_reference(type))
        return builder.load(ir::Type::Ptr, address, 4);
    return builder.load(ir::Type::I32, address, 4);
}

// 把已求得的值写入固定临时 slot，并返回可借用的地址。
// 缓存按当前函数的 AST 静态位置区分；同一位置重复执行时复用入口分配，
// 只在当前求值位置重新 store，不重复求值也不重复分配。
ir::Operand GenerationContext::materialize_once(const Expression &position,
                                                ir::Operand value) {
    const auto found = temporary_slots.find(&position);
    if (found != temporary_slots.end()) {
        store_value(value, found->second);
        return found->second;
    }
    const auto address = builder.entry_alloca(
        temporary_storage_type(value.type), temporary_alignment(value.type));
    store_value(value, address);
    temporary_slots.emplace(&position, address);
    return address;
}

} // namespace rx
