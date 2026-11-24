// 实现 lowering 的类型映射、基础值读取、存储和临时值物化。
#include "ir/lowering/value.hpp"
#include "ast/ast.hpp"
#include "ir/builder.hpp"
#include "ir/layout.hpp"
#include "ir/lowering.hpp"
#include <cstdint>
#include <stdexcept>
#include <variant>

namespace rx {
// 判断源类型是否是当前生成器支持的标量（整数或 bool）。
bool is_scalar(const SemanticType &type) {
    return type.is_int() || type.kind == SemanticType::Kind::Bool;
}

// 判断源类型是否是引用；引用的计算值和存储单元都只保存目标地址。
bool is_reference(const SemanticType &type) {
    return type.kind == SemanticType::Kind::Reference;
}

// 结构体、固定数组和 Vec 头都按自身布局保存为独立值。
// Vec 头固定为 {ptr,i32,i32}，元素缓冲区仍在堆上，不随头一起展开。
bool is_aggregate_value(const SemanticType &type) {
    return type.kind == SemanticType::Kind::Struct ||
           type.kind == SemanticType::Kind::Array ||
           type.kind == SemanticType::Kind::Vec;
}

// 当前 lowering 可作为局部值保存的类型：标量、引用/Box（都保存指针）或聚合。
bool is_storable_value(const SemanticType &type) {
    if (is_reference(type))
        return is_storable_value(*type.inner);
    // Box 的存储就是拥有者指针，不展开被拥有的目标布局。
    if (type.kind == SemanticType::Kind::Box)
        return true;
    return is_scalar(type) || is_aggregate_value(type);
}

// 生成完整具体类型的稳定键，区分整数种类、结构体身份、子类型和数组长度。
// 结构体只记录 StructId，不展开字段，因此递归类型也能有限生成键。
std::string type_identity_key(const SemanticType &type) {
    switch (type.kind) {
        case SemanticType::Kind::Unit:
            return "unit";
        case SemanticType::Kind::Never:
            return "never";
        case SemanticType::Kind::Error:
            return "error";
        case SemanticType::Kind::Int:
            return "int" + std::to_string(static_cast<int>(type.int_kind));
        case SemanticType::Kind::Bool:
            return "bool";
        case SemanticType::Kind::Reference:
            return (type.is_mut ? "refmut" : "ref") +
                   type_identity_key(*type.inner);
        case SemanticType::Kind::Array:
            return "array" + std::to_string(type.length) + ":" +
                   type_identity_key(*type.inner);
        case SemanticType::Kind::Box:
            return "box" + type_identity_key(*type.inner);
        case SemanticType::Kind::Vec:
            return "vec" + type_identity_key(*type.inner);
        case SemanticType::Kind::Struct:
            return "struct" + std::to_string(type.struct_symbol->id.index);
    }
    throw std::logic_error("未知源类型不能生成稳定类型键");
}

// 计算普通值使用的 IR 类型：标量沿用标量映射，引用和 Box 统一为 ptr。
ir::Type lower_value_type(const SemanticType &type) {
    if (is_reference(type) || type.kind == SemanticType::Kind::Box)
        return ir::Type::Ptr;
    if (type.is_int())
        return ir::Type::I32;
    if (type.kind == SemanticType::Kind::Bool)
        return ir::Type::I1;
    throw std::logic_error("该源类型不能映射为单个 IR 操作数");
}

// 按存储类型和对齐分配入口 slot；标量和聚合使用同一套布局服务。
ir::Operand
GenerationContext::allocate_storage_slot(const ir::StorageType &storage_type) {
    return builder.entry_alloca(storage_type,
                                layouts_.layout(storage_type).align);
}

// 计算值 slot 先映射成存储类型：bool 使用 i8，整数 i32，引用 ptr。
ir::Operand GenerationContext::allocate_slot(ir::Type value_type) {
    return allocate_storage_slot(
        ir::StorageType::scalar(ir::scalar_storage_type(value_type)));
}

// 把字面量或常量路径生成为 IR 操作数；局部路径走统一的 place 读取。
ir::Operand GenerationContext::emit_constant(const Expression &expression) {
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
    return ir::Operand::constant(lower_value_type(value.type), bits);
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
