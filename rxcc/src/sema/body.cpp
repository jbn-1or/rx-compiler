// 实现 BodyChecker 的函数入口、局部作用域、语句和块遍历。
// 表达式分类规则在 expression.cpp / value_path.cpp，place 与转换工具在
// place.cpp / coercion.cpp；本文件只处理状态和块结构。

#include "sema/body.hpp"
#include "sema/sema.hpp"

namespace rx {

BodyChecker::BodyChecker(SemanticAnalyzer &analyzer)
    : analyzer_(analyzer), diagnostics_(analyzer.diagnostics_) {}

// 检查一个函数的函数体。每个 BodyChecker 只检查一个函数，作用域生命周期
// 随对象结束；这里仍显式清空，避免调用方复用时残留状态。
void BodyChecker::check_function(const FunctionSymbol &fn) {
    enter_function(fn);
    if (fn.node && fn.node->body) {
        // 返回类型作为期望类型传入块，让无后缀字面量和块尾正确吸收目标类型。
        const ExpressionResult body =
            check_block(*fn.node->body, &fn.return_type);
        require_coerce(body, fn.return_type, fn.node->body->span);
    }

    scopes_.clear();
    current_function_ = nullptr;
    current_owner_ = nullptr;
    current_has_self_ = false;
}

// 建立函数级作用域，登记接收者和普通参数，存进scope
// 顶层函数没有 owner；impl 内无接收者的关联函数仍有 owner，供 Self 使用。
void BodyChecker::enter_function(const FunctionSymbol &fn) {
    scopes_.clear();
    current_function_ = &fn;
    current_owner_ = fn.owner;
    current_has_self_ = false;

    push_scope();
    if (!fn.node)
        return;
    // 有self参数先处理
    if (fn.node->self_parameter) {
        current_has_self_ = true;
        const SelfParameter::Kind kind = fn.node->self_parameter->kind;
        const bool by_reference = kind == SelfParameter::Kind::Reference ||
                                  kind == SelfParameter::Kind::MutableReference;
        const bool is_mut = kind == SelfParameter::Kind::MutableValue ||
                            kind == SelfParameter::Kind::MutableReference;

        const SemanticType owner_type =
            current_owner_ ? SemanticType::struct_(current_owner_)
                           : SemanticType::error();
        Binding self_binding;
        // self有&
        if (by_reference) {
            // &self / &mut self 的绑定本身都不能重新赋值；可变性由引用类型携带。
            self_binding.type =
                owner_type.is_error()
                    ? owner_type
                    : SemanticType::reference(is_mut, owner_type);
        } else {
            self_binding.type = owner_type;
            self_binding.is_mut = is_mut;
        }
        declare_local("self", self_binding);
    }
    // 处理其他参数（self与其他参数在ast是分开的）
    for (std::size_t index = 0; index < fn.node->parameters.size(); ++index) {
        const Parameter &parameter = fn.node->parameters[index];
        Binding binding;
        if (index < fn.parameters.size()) {
            binding.type = fn.parameters[index].type;
            binding.is_mut = fn.parameters[index].is_mut;
        } else {
            // 签名定型应先完成；这里仅防御不完整 AST，保持检查可继续。
            binding.type =
                parameter.type
                    ? analyzer_.resolve_type(*parameter.type, current_owner_)
                    : SemanticType::error();
            binding.is_mut = parameter.is_mut;
        }
        declare_local(parameter.name, binding);
    }
}

void BodyChecker::push_scope() { scopes_.emplace_back(); }

void BodyChecker::pop_scope() {
    if (!scopes_.empty())
        scopes_.pop_back();
}

void BodyChecker::declare_local(const std::string &name,
                                const Binding &binding) {
    if (scopes_.empty())
        push_scope();
    // 同一块内新 let 覆盖旧绑定；不报重复。
    scopes_.back()[name] = binding; // 新建或替换
}

// 在scope中寻找name的binding
std::optional<Binding>
BodyChecker::lookup_local(const std::string &name) const {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        const auto it = scope->find(name);
        if (it != scope->end())
            return it->second;
    }
    return std::nullopt;
}

// 检查一条非尾语句。返回两个信息：
// 这条语句会不会让后面的代码不再执行；以及检查过程中有没有已经出错。
BodyChecker::StatementResult
BodyChecker::check_statement(const Statement &statement) {
    StatementResult result;

    // 情况一：let 语句。
    if (const auto *let = dynamic_cast<const LetStatement *>(&statement)) {
        if (!let->initializer)
            return result;

        // 如果写了类型，先算出这个类型，后面用它来要求等号右边的值。
        SemanticType annotated;
        const SemanticType *expected = nullptr;
        if (let->type_annotation) {
            annotated =
                analyzer_.resolve_type(*let->type_annotation, current_owner_);
            expected = &annotated;
        }

        // 检查等号右边的值；有类型时要求右边的值能转成这个类型。
        const ExpressionResult initializer =
            check_expression(*let->initializer, expected);
        if (let->type_annotation) {
            require_coerce(initializer, annotated, let->initializer->span);
        }

        // 先检查右边，再把名字放进当前块。
        // 所以 `let x = x;` 右边的 x 看到的是外面原来的 x，不是新声明的 x。
        Binding binding;
        binding.type = let->type_annotation ? annotated : initializer.type;
        binding.is_mut = let->is_mut;
        declare_local(let->name, binding);

        // 右边如果永远不结束（比如 return），整条语句后面的代码也不会执行。
        result.diverges = initializer.type.is_never();
        // 右边已经出错时记下来，避免外层再报一个不相干的返回类型错误。
        result.has_error =
            initializer.type.is_error() ||
            (let->type_annotation && annotated.is_error());
        return result;
    }

    // 情况二：普通表达式语句，比如 `f();` 或 `1 + 2;`。
    if (const auto *expression_statement =
            dynamic_cast<const ExpressionStatement *>(&statement)) {
        if (!expression_statement->expression)
            return result;

        // 算出这个表达式的类型；语句的值一般会被丢掉。
        const ExpressionResult expression_result =
            check_expression(*expression_statement->expression, nullptr);

        // 如果它不在块末尾，又没写分号，那它只能是 () 或永远不结束；
        // 否则这个值会留下来，不合法。带分号的语句可以丢弃任意结果。
        if (!expression_statement->has_semicolon) {
            require_coerce(expression_result, SemanticType::unit(),
                           expression_statement->expression->span);
        }

        // 记录它会不会让后面的代码不执行，以及是否已经出错。
        result.diverges = expression_result.type.is_never();
        result.has_error = expression_result.type.is_error();
        return result;
    }

    return result;
}

// 检查一个块
ExpressionResult BodyChecker::check_block(const BlockExpression &block,
                                          const SemanticType *expected) {
    // 进入新块，先给它准备一层自己的局部名字；离开时再丢掉。
    push_scope();

    // 先找块最后那个表达式，它的值就是整个块的值。
    const Expression *tail = block.tail.get();
    std::size_t statement_count = block.statements.size();
    // 有 AST 把“最后一条没写分号的表达式语句”留在 statements 里；
    // 这里把它也当作块的值，并且只检查一次。
    if (!tail && statement_count != 0) {
        const auto *last = dynamic_cast<const ExpressionStatement *>(
            block.statements.back().get());
        if (last && !last->has_semicolon && last->expression) {
            tail = last->expression.get();
            --statement_count;
        }
    }

    // 先逐条检查前面的普通语句，记录：
    // 有没有语句让后面的代码不再执行；有没有语句已经出错。
    bool diverges = false;
    bool has_error = false;
    for (std::size_t index = 0; index < statement_count; ++index) {
        const StatementResult statement_result =
            check_statement(*block.statements[index]);
        diverges = diverges || statement_result.diverges;
        has_error = has_error || statement_result.has_error;
    }

    ExpressionResult result;
    if (tail) {
        // 有最后表达式：块的值就是它的值；外面期望的类型也传给它。
        result = check_expression(*tail, expected);
    } else if (diverges) {
        // 没有最后表达式，但前面的语句不会往下走，所以整个块也不会返回。
        result = ExpressionResult{SemanticType::never(),
                                  PlaceAccessInformation{}};
    } else if (has_error) {
        // 前面已经有错，块就用 Error 表示已经检查失败，避免外层再拿 ()
        result = ExpressionResult{SemanticType::error(),
                                  PlaceAccessInformation{}};
    } else {
        // 普通的空块或只有普通语句的块，值是 ()。
        result = ExpressionResult{SemanticType::unit(),
                                  PlaceAccessInformation{}};
    }

    pop_scope();
    return result;
}

} // namespace rx
