#pragma once

#include "sema/identity.hpp"
#include "sema/type.hpp"
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace rx {

struct FunctionSymbol;

// 内置方法或容器构造器的具体种类。
// 方法名本身不足以还原选择：同名 clone 可能是用户方法，也可能是派生/容器实现。
enum class BuiltinCallKind {
    Clone,      // 按 Clone 能力提供的派生或容器 clone
    ArrayLen,   // 固定数组长度
    VecLen,     // Vec 当前长度
    VecIsEmpty, // Vec 是否为空
    VecPush,    // Vec 尾部追加
    VecRemove,  // Vec 按下标删除
    BoxNew,     // Box::new 构造器
    VecNew,     // Vec::new 构造器
};

// 一个已确定的内置调用目标：具体种类 + 所属的具体类型。
// 方法目标的 owner 是接收者剥掉引用后的类型；构造器目标的 owner 是结果容器类型。
struct BuiltinCall {
    BuiltinCallKind kind = BuiltinCallKind::Clone; // 具体内置操作
    SemanticType owner; // 操作所属的具体类型，例如 &T 候选剥掉引用后的 T
};

// 所有调用共用的具体目标身份：用户函数、内置 I/O 或具体内置操作。
// 生成器只按这份记录执行，不再根据实参或结果重新挑选目标。
using CallTarget = std::variant<FunctionId, BuiltinIO, BuiltinCall>;

// 一次调用的签名：普通参数、返回类型和可选接收者。
// 普通参数不包含接收者；显式 S::method 调用把 receiver 放到参数表首位。
struct CallableSignature {
    std::vector<SemanticType> parameters; // 普通参数类型，按声明顺序排列，不包含接收者。
    SemanticType return_type; // 调用结果类型
    std::optional<SemanticType> receiver = std::nullopt; // 方法的接收者类型（self / &self / &mut self）；
    // 生成显式 S::method(...) 的参数表：receiver 在前，普通参数在后。
    std::vector<SemanticType> explicit_parameters() const;
};

CallableSignature function_signature(const FunctionSymbol &function);
std::optional<CallableSignature> builtin_signature(const std::string &name);

// 容器构造器命中：具体种类与签名一起返回，查找方不必再按类型猜种类。
struct BuiltinConstructor {
    BuiltinCallKind kind;       // BoxNew 或 VecNew
    CallableSignature signature; // 构造器的完整签名
};
std::optional<BuiltinConstructor>
builtin_container_constructor(const SemanticType &container);
std::optional<CallableSignature>
container_constructor_signature(const SemanticType &container);

// 内置方法命中：具体种类与签名一起返回，避免同名 clone 的选择在后续丢失。
struct BuiltinMethod {
    BuiltinCallKind kind;       // Clone、ArrayLen、VecLen、VecIsEmpty、VecPush 或 VecRemove
    CallableSignature signature; // 该方法的完整签名
};
std::optional<BuiltinMethod>
builtin_method(const SemanticType &owner, const std::string &name);
// 仅需要签名时的兼容查询；与 builtin_method 共用同一套匹配条件。
std::optional<CallableSignature>
builtin_method_signature(const SemanticType &owner, const std::string &name);

// 保存查找命中的调整路径；权限检查按同一路径执行，不重新选择候选。
struct ReceiverAdjustment {
    // 在匹配层选中的候选形式。
    enum class Borrow {
        None,    // 直接使用当前层类型
        Shared,  // 对当前层加共享借用，即候选 &T
        Mutable, // 对当前层加可变借用，即候选 &mut T
    };
    // 接收者表达式需要先自动解引用的层数（每层引用或 Box）。
    std::size_t dereferences = 0;
    // 命中候选时采用的借用形式，决定后续接收者权限检查的方式。
    Borrow borrow = Borrow::None;
};

// 一次调用解析出的具体目标和签名；限定调用与点方法查找共用。
struct ResolvedCall {
    CallTarget target;           // 选中的具体目标：用户 FunctionId 或内置操作
    CallableSignature signature; // 该目标的完整签名
};

// 点方法查找结果：具体目标、签名以及接收者检查需要复用的调整路径。
struct ResolvedMethod {
    CallTarget target;           // 选中的具体目标；同名方法也不会混成另一个身份
    // 选中方法的完整签名，包含参数、返回值和声明的接收者类型。
    CallableSignature signature;
    // 从接收者表达式到声明接收者的解引用/借用调整。
    ReceiverAdjustment receiver;
};

} // namespace rx
