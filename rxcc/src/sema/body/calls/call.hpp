// 调用签名是普通、关联和点方法调用共享的规则来源。
#pragma once

#include "sema/type.hpp"
#include <optional>
#include <string>
#include <vector>

namespace rx {

struct FunctionSymbol;

// 普通参数不包含接收者；显式 S::method 调用把 receiver 放到参数表首位。
struct CallableSignature {
    std::vector<SemanticType> parameters;
    SemanticType return_type;
    std::optional<SemanticType> receiver = std::nullopt;

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
    enum class Borrow { None, Shared, Mutable };
    std::size_t dereferences = 0;
    Borrow borrow = Borrow::None;
};

struct ResolvedMethod {
    CallableSignature signature;
    ReceiverAdjustment receiver;
};

} // namespace rx
