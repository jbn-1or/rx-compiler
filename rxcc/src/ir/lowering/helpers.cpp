// 按“操作种类 + 完整具体类型”登记并生成 clone/相等辅助函数。
// 先登记函数身份和签名，再统一补全函数体；递归请求直接复用已有身份。
#include "ir/lowering.hpp"
#include "ir/builder.hpp"
#include "ir/lowering/value.hpp"
#include <string>

namespace rx {

// 查找已有辅助函数；没有时先分配身份、签名并登记为 Pending。
// 登记先于函数体生成，因此递归类型在生成时能直接引用自身或尚未展开的对方。
ir::IRFunctionId
GenerationContext::request_helper(HelperKind kind, const SemanticType &type) {
    // 操作种类同时进入缓存键、符号前缀和签名，clone_T 与 eq_T 不会串用。
    const bool is_clone = kind == HelperKind::Clone;
    const auto key =
        std::string(is_clone ? "clone:" : "eq:") + type_identity_key(type);
    if (const auto found = helper_indices_.find(key); found != helper_indices_.end())
        return helpers_[found->second].function;
    const auto helper_index = helpers_.size();
    const auto name = std::string(is_clone ? "__rx_clone_helper_"
                                           : "__rx_eq_helper_") +
                      std::to_string(helper_index);
    const ir::Signature signature =
        is_clone
            ? ir::Signature{ir::Type::Void,
                            {ir::Type::Ptr, ir::Type::Ptr}}
            : ir::Signature{ir::Type::I1,
                            {ir::Type::Ptr, ir::Type::Ptr}};
    const auto function =
        module.add_function(name, signature, ir::Linkage::Internal);
    helper_indices_.emplace(key, helper_index);
    helpers_.push_back(
        Helper{kind, function, type, Helper::State::Pending});
    return function;
}

// 按登记顺序生成所有尚未完成的辅助函数体；生成过程中新登记的会追加到末尾。
// 主函数体和机器入口已经结束，因此这里复用主 builder 不会覆盖任何源函数状态。
bool GenerationContext::emit_pending_helpers() {
    for (std::size_t index = 0; index < helpers_.size(); ++index) {
        if (helpers_[index].state == Helper::State::Complete)
            continue;
        // 复制身份、种类和类型，避免生成期间 helpers_ 扩容使引用失效。
        helpers_[index].state = Helper::State::Generating;
        const auto kind = helpers_[index].kind;
        const auto function = helpers_[index].function;
        const auto type = helpers_[index].type;
        auto &ir_function = module.function(function);
        builder.set_insert_point(ir_function.add_block());
        if (kind == HelperKind::Clone) {
            emit_clone_into(ir_function.argument(0), ir_function.argument(1),
                            type);
            builder.ret();
        } else {
            const auto equal = emit_equality_into(
                ir_function.argument(0), ir_function.argument(1), type);
            builder.ret(equal);
        }
        // 生成期间若递归请求了自己，这里仍通过原下标把同一项标为完成。
        helpers_[index].state = Helper::State::Complete;
    }
    return true;
}

} // namespace rx