// 调用签名是普通、关联和点方法调用共享的规则来源。
#pragma once

#include "sema/type.hpp"
#include <optional>
#include <string>
#include <vector>

namespace rx {

struct FunctionSymbol;

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
std::optional<CallableSignature>
container_constructor_signature(const SemanticType &container);
// Vec、数组和 Clone 的声明签名；Clone 能力由查找方查询。
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

// 方法查找结果：签名以及接收者检查需要复用的调整路径。
struct ResolvedMethod {
    // 选中方法的完整签名，包含参数、返回值和声明的接收者类型。
    CallableSignature signature;
    // 从接收者表达式到声明接收者的解引用/借用调整。
    ReceiverAdjustment receiver;
};

} // namespace rx
