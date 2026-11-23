// 将源类型映射为 IR 存储类型，并按固定 RV32 目标查询存储布局。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rx {
struct SemanticType;

namespace ir {
class Module;
enum class Type;

// 一个类型在目标上的大小、对齐和结构体字段偏移。
// field_offsets 只对结构体（包括 Vec 头）有意义，按字段声明顺序保存字节偏移。
struct Layout {
    uint32_t size = 0; // 目标上的存储大小（字节）。
    uint32_t align = 1; // 目标上的对齐要求（字节），必须是 2 的幂。
    std::vector<uint32_t> field_offsets; // 结构体字段偏移；其他类型为空。

    bool operator==(const Layout &) const = default;
};

// 自有 IR 的存储类型表示。它只描述存储布局需要的结构，
// 不携带源类型或其诊断信息；命名结构体通过模块类型表下标引用。
struct StorageType {
    enum class Kind {
        Unit,  // 不占数据；用于 unit 字段。
        Scalar, // 沿用 IR 标量类型；bool 存储使用 i8。
        Array, // [T; N]，element 和 length 描述元素与长度。
        Struct, // 用户命名结构体，struct_index 指向模块类型表。
        Vec,   // Vec<T> 头，固定打印为 { ptr, i32, i32 }。
    };

    Kind kind = Kind::Unit; // 这个存储类型是哪一类。
    Type scalar_type{}; // 仅 Kind::Scalar 使用。
    std::shared_ptr<const StorageType> element; // 仅 Kind::Array 使用。
    uint64_t length = 0; // 数组长度；仅 Kind::Array 使用。
    std::size_t struct_index = 0; // 模块内类型下标；仅 Kind::Struct 使用。

    StorageType() = default;
    // 创建给定标量种类的存储类型。
    static StorageType scalar(Type type);
    // 从计算值类型构造标量存储类型；调用方需保证它是可存储的标量。
    StorageType(Type type);
    // 创建数组存储类型，元素类型和长度由调用方提供。
    static StorageType array_of(const StorageType &element, uint64_t length);
    // 创建指向模块类型表中某个命名结构体的存储类型。
    static StorageType struct_of(std::size_t struct_index);
    // 创建固定大小的 Vec 头，不携带源元素类型。
    static StorageType vec();

    // 按结构判断两个存储类型是否表示同一种存储布局。
    bool same_type(const StorageType &other) const;
    bool operator==(const StorageType &other) const { return same_type(other); }
    // 标量存储类型与计算值类型比较，保持旧模型的自然调用方式。
    bool operator==(Type scalar) const;
};

// 标量计算值使用的存储类型；i1 在内存中使用 i8。
Type scalar_storage_type(Type value_type);
// 标量存储的目标大小和对齐；不需要访问模块类型表。
Layout scalar_layout(Type storage_type);

// 把语义类型映射成自有存储类型：
// 整数映射 i32，bool 映射 i8 存储，引用/Box 映射 ptr，Vec 映射固定头。
StorageType storage_type_of(const SemanticType &type);

// 把存储类型打印成 LLVM 文本中的类型，例如 i32、[3 x i8] 或 %__rx_struct_0。
std::string storage_type_text(const StorageType &type, const Module &module);

// 在一个模块的类型表定义完成后查询布局，复用命名结构体结果。
class LayoutService {
public:
    explicit LayoutService(const Module &module) : module_(module) {}
    Layout layout(const StorageType &type);

private:
    const Layout &struct_layout(std::size_t index);

    const Module &module_; // 命名类型下标所属的模块。
    std::unordered_map<std::size_t, Layout> structures_; // 已完成的命名结构体布局。
    std::unordered_set<std::size_t> visiting_; // 当前递归路径上的命名类型。
};

} // namespace ir
} // namespace rx
