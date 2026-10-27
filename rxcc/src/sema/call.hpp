// 定义调用检查共享的数据：已解析目标的签名、多段被调用者的解析结果，
// 以及内置 I/O 的签名查询。
// 参数类型按声明顺序保存；返回类型来自函数/内置声明，不因调用位置改变。
// 具体调用解析和实参检查在 call.cpp，内置签名表在 builtins.cpp。

#pragma once

#include "sema/type.hpp"
#include <optional>
#include <string>
#include <vector>

namespace rx {

struct FunctionSymbol;
struct StructSymbol;

// 一次调用可用的签名。
// 例：`fn f(a: i32) -> bool` → {parameters={i32}, return_type=bool}；
//     `println_i32` → {parameters={i32}, return_type=()}。
struct CallableSignature {
    std::vector<SemanticType> parameters;
    SemanticType return_type;
};

// 5.1：`Type::member` 形式的两段被调用者解析出的目标。
// 只描述"目标是谁"，实参检查由 check_arguments 在 5.2/5.3 按目标种类
// 建立签名后统一进行；不保存接收者调整信息（点方法查找在 5.4/5.5）。
// 例：`S::new` → 用户关联函数；`Box::<i32>::new` → 容器构造器；
//     `Vec::<i32>::len` → Vec 显式方法；`Outer::clone`/`Vec::<T>::clone` →
//     内置或派生 Clone。
struct ResolvedCallee {
    enum class Kind {
        AssociatedFunction,   // 关联函数或方法（含接收者）；签名按 5.3 归一
        ContainerConstructor, // Box::<T>::new / Vec::<T>::new；签名按 5.2 建立
        ContainerMethod,      // Vec 的 len/is_empty/push/remove；签名按 5.3
        BuiltinClone,         // 容器或基本类型的 clone；签名按 5.3 建立
    };

    Kind kind = Kind::AssociatedFunction;
    const FunctionSymbol *function = nullptr; // AssociatedFunction 的目标函数
    SemanticType container;                   // Container*：归一化的 Box<T>/Vec<T>
    SemanticType clone_type;                  // BuiltinClone：被克隆的具体类型 U
    std::string member;                       // 容器/Clone 目标的成员名，如 "new"、"len"
};

// 查询内置 I/O 的固定签名；不是内置 I/O 时返回 nullopt。
// 例："get_i32" → {parameters={}, return_type=i32}。
std::optional<CallableSignature>
builtin_signature(const std::string &name);

// 5.2：Box::<T>::new / Vec::<T>::new 的构造器签名。
// 例：Box<i32> → {parameters={i32}, return_type=Box<i32>}；
//     Vec<i32> → {parameters={}, return_type=Vec<i32>}。
// 目标不是容器构造器或容器类型不完整时返回 nullopt。
std::optional<CallableSignature>
container_constructor_signature(const ResolvedCallee &target);

// 5.3：Vec::<T>::len/is_empty/push/remove 的显式调用签名。
// len(&Vec<T>) -> usize；is_empty(&Vec<T>) -> bool；
// push(&mut Vec<T>, T) -> ()；remove(&mut Vec<T>, usize) -> T。
std::optional<CallableSignature>
container_method_signature(const ResolvedCallee &target);

// 5.3：内置/派生 Clone 的显式调用签名 clone(&U) -> U。
// 调用方先确认 U 具备 Clone；目标不是 BuiltinClone 时返回 nullopt。
std::optional<CallableSignature>
builtin_clone_signature(const ResolvedCallee &target);

// 5.4：点调用选中的目标及接收者调整路径。
// 解引用次数表示从原始接收者类型走到 reached_type 经过的内置解引用步数；
// borrow 表示是否在 reached_type 上额外插入自动借用，mutable_borrow 区分
// &T 与 &mut T。5.5 按同一条路径检查 place 权限，用类型猜解引用层数不够。
struct ReceiverAdjustment {
    std::size_t dereferences = 0;
    bool borrow = false;
    bool mutable_borrow = false;
    SemanticType reached_type;
};

// 5.4：一次点调用查找到的方法。
// parameters 不包含接收者；Vec/数组/Clone 的签名由内置表提供，用户方法取
// 声明的普通参数。declared_receiver 必须与候选类型完全相等。
struct ResolvedMethod {
    enum class Kind { User, VecMethod, ArrayMethod, BuiltinClone };

    Kind kind = Kind::User;
    const FunctionSymbol *function = nullptr; // User：目标方法
    SemanticType declared_receiver;           // 命中的候选接收者类型
    ReceiverAdjustment receiver;              // 如何从原始接收者到达该候选
    std::vector<SemanticType> parameters;     // 不含接收者的普通参数
    SemanticType return_type;
    SemanticType container;   // VecMethod：Vec<T>
    SemanticType clone_type;  // BuiltinClone：U
};

} // namespace rx
