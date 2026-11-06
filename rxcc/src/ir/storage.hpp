// 每个源函数的固定存储：局部绑定按 LocalId，临时值按 AST 静态位置复用。
#pragma once

#include "ir/value.hpp"
#include "sema/identity.hpp"
#include <map>
#include <string>

namespace llvm {
class Function;
}

namespace rx {
struct Expression;
}

namespace rx::ir {

// 每个正在生成的函数使用一个实例。仅分配位置，不求值或初始化；
// AST 和 LLVM 函数必须比本对象存活更久。
class FunctionStorage {
public:
    explicit FunctionStorage(llvm::Function &function) : function_(function) {}
    FunctionStorage(const FunctionStorage &) = delete;
    FunctionStorage &operator=(const FunctionStorage &) = delete;

    std::optional<Place> create_local(LocalId id, const SemanticType &type,
                                      llvm::raw_ostream &diagnostics);
    const Place *local(LocalId id) const;

    // 仅用于需要物化的值；已有局部变量应通过 local() 取得原位置。
    // 同一位置再次请求须具有相同源类型，不创建新的 alloca。
    std::optional<Place> temporary(const Expression &site, const SemanticType &type,
                                   llvm::raw_ostream &diagnostics);

private:
    std::optional<Place> allocate(const SemanticType &type, const std::string &name,
                                  llvm::raw_ostream &diagnostics);
    llvm::Function &function_;
    std::map<LocalId, Place> locals_;
    std::map<const Expression *, Place> temporaries_;
};

} // namespace rx::ir
