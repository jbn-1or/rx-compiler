// 实现固定 RV32 目标的自有布局查询和源类型到存储类型的映射。
// 所有大小/偏移都用宿主 64 位无符号中间值计算，再检查目标 32 位范围；
// 不使用宿主 sizeof/alignof，也不调用 LLVM DataLayout API。

#include "ir/layout.hpp"
#include "ir/model.hpp"
#include "sema/symbol.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace rx::ir {
namespace {

// 布局大小和偏移必须能用目标的 32 位地址表示。
uint32_t checked_size(uint64_t size) {
    if (size > std::numeric_limits<uint32_t>::max())
        throw std::logic_error("布局大小超出目标 32 位范围");
    return static_cast<uint32_t>(size);
}

// 上一个字段的偏移和大小均已核对为 32 位；以 64 位计算对齐不会溢出。
uint32_t align_up(uint64_t offset, uint32_t alignment) {
    return checked_size((offset + alignment - 1) / alignment * alignment);
}

} // namespace

// 创建给定标量种类的存储类型。
StorageType StorageType::scalar(Type type) {
    StorageType result;
    result.kind = Kind::Scalar;
    result.scalar_type = type;
    return result;
}

// 创建数组存储类型，元素按值保存在共享指针中。
StorageType StorageType::array_of(const StorageType &element,
                                  uint64_t length) {
    StorageType result;
    result.kind = Kind::Array;
    result.element = std::make_shared<const StorageType>(element);
    result.length = length;
    return result;
}

// 创建命名结构体存储类型，引用模块类型表中的下标。
StorageType StorageType::struct_of(std::size_t struct_index) {
    StorageType result;
    result.kind = Kind::Struct;
    result.struct_index = struct_index;
    return result;
}

// 创建 Vec 头存储类型，不展开元素。
StorageType StorageType::vec() {
    StorageType result;
    result.kind = Kind::Vec;
    return result;
}

// 普通 bool 值在内存中使用一个字节；其他标量维持原类型。
Type scalar_storage_type(Type value_type) {
    return value_type == Type::I1 ? Type::I8 : value_type;
}

// RV32 标量布局供聚合字段和标量存取共同使用。
Layout scalar_layout(Type storage_type) {
    switch (storage_type) {
        case Type::I1:
        case Type::I8:
            return {1, 1, {}};
        case Type::I32:
        case Type::Ptr:
            return {4, 4, {}};
        case Type::Void:
            break;
    }
    throw std::logic_error("此 IR 类型没有标量存储布局");
}

// 计算存储布局；数组元素的大小已包含尾填充，等于元素步长。
Layout LayoutService::layout(const StorageType &type) {
    switch (type.kind) {
        case StorageType::Kind::Unit:
            return {};
        case StorageType::Kind::Scalar:
            return scalar_layout(type.scalar_type);
        case StorageType::Kind::Array: {
            if (!type.element)
                throw std::logic_error("数组缺少元素存储类型");
            const auto element = layout(*type.element);
            // 乘法前检查目标上限，也避免宿主乘法先溢出。
            if (element.size != 0 &&
                type.length > std::numeric_limits<uint32_t>::max() / element.size)
                throw std::logic_error("数组大小超出目标 32 位范围");
            return {static_cast<uint32_t>(element.size * type.length),
                    element.align, {}};
        }
        case StorageType::Kind::Vec:
            return {12, 4, {0, 4, 8}}; // 固定的 { ptr, i32, i32 } 头。
        case StorageType::Kind::Struct:
            return struct_layout(type.struct_index);
    }
    throw std::logic_error("未知存储类型没有布局");
}

// 命名结构体按字段顺序布局，补齐尾填充后缓存，供外层结构体复用。
const Layout &LayoutService::struct_layout(std::size_t index) {
    if (const auto found = structures_.find(index); found != structures_.end())
        return found->second;
    const auto &named = module_.named_type(index);
    if (!named.defined)
        throw std::logic_error("命名结构体尚未完成字段定义");
    if (!visiting_.insert(index).second)
        throw std::logic_error("命名结构体布局存在按值递归环");
    struct VisitGuard {
        std::unordered_set<std::size_t> &visiting; // 当前递归路径。
        std::size_t index; // 此次查询加入路径的类型。
        ~VisitGuard() { visiting.erase(index); }
    } guard{visiting_, index};

    Layout result;
    result.field_offsets.reserve(named.fields.size());
    uint64_t end = 0;
    for (const auto &field : named.fields) {
        const auto field_layout = layout(field);
        const uint32_t offset = align_up(end, field_layout.align);
        result.field_offsets.push_back(offset);
        end = static_cast<uint64_t>(offset) + field_layout.size;
        result.align = std::max(result.align, field_layout.align);
    }
    result.size = align_up(end, result.align);
    return structures_.emplace(index, std::move(result)).first->second;
}

// 把源类型映射为存储类型；引用、Box 和 Vec 都不展开目标/元素布局。
StorageType storage_type_of(const SemanticType &type) {
    switch (type.kind) {
        case SemanticType::Kind::Unit:
            return {};
        case SemanticType::Kind::Int:
            return StorageType::scalar(Type::I32);
        case SemanticType::Kind::Bool:
            return StorageType::scalar(Type::I8);
        case SemanticType::Kind::Reference:
        case SemanticType::Kind::Box:
            return StorageType::scalar(Type::Ptr);
        case SemanticType::Kind::Array:
            return StorageType::array_of(storage_type_of(*type.inner),
                                         type.length);
        case SemanticType::Kind::Struct:
            return StorageType::struct_of(type.struct_symbol->id.index);
        case SemanticType::Kind::Vec:
            return StorageType::vec();
        case SemanticType::Kind::Never:
        case SemanticType::Kind::Error:
            throw std::logic_error("发散或错误类型没有存储类型");
    }
    throw std::logic_error("未知源类型没有存储类型");
}

// 递归输出存储类型；用户结构体只打印模块登记的名称，不展开字段。
std::string storage_type_text(const StorageType &type, const Module &module) {
    switch (type.kind) {
        case StorageType::Kind::Unit:
            return "{}";
        case StorageType::Kind::Scalar:
            return type_name(type.scalar_type);
        case StorageType::Kind::Array:
            return "[" + std::to_string(type.length) + " x " +
                   storage_type_text(*type.element, module) + "]";
        case StorageType::Kind::Struct:
            return "%" + module.named_type(type.struct_index).name;
        case StorageType::Kind::Vec:
            return "{ ptr, i32, i32 }";
    }
    throw std::logic_error("未知存储类型没有文本名称");
}

} // namespace rx::ir
