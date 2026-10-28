// 函数体检查主线：函数入口 → 块 → 语句/尾表达式 → 表达式分派。
// 各类表达式处理器通过 check_expression 递归检查子表达式。
// 函数参数和局部作用域的维护放在文件末尾。

#include "sema/body/checker.hpp"
#include "sema/sema.hpp"
#include <cassert>

namespace rx {

BodyChecker::BodyChecker(SemanticAnalyzer &analyzer)
    : analyzer_(analyzer), diagnostics_(analyzer.diagnostics_) {}

void BodyChecker::check_function(const FunctionSymbol &fn) {
    enter_function(fn);
    assert(fn.node && fn.node->body);
    const auto body = check_block(*fn.node->body, &fn.return_type);
    require_coerce(body, fn.return_type, fn.node->body->span);
}

ExpressionResult BodyChecker::check_block(const BlockExpression &block,
                                          const SemanticType *expected) {
    push_scope();
    bool diverges = false;
    bool has_error = false;
    // 不可达语句仍检查；无尾块用语句摘要区分 !、Error 和 ()。
    for (const auto &statement : block.statements) {
        const auto result = check_statement(*statement);
        diverges = diverges || result.diverges;
        has_error = has_error || result.has_error;
    }
    ExpressionResult result;
    if (block.tail) {
        const auto tail = check_expression(*block.tail, expected);
        // 块产生值，不继承尾表达式的 place 权限。
        result = ExpressionResult::value(tail.type);
    } else if (diverges) {
        result = ExpressionResult::value(SemanticType::never());
    } else if (has_error) {
        result = ExpressionResult::error();
    } else {
        result = ExpressionResult::value(SemanticType::unit());
    }
    pop_scope();
    return result;
}

BodyChecker::StatementResult BodyChecker::check_statement(const Statement &statement) {
    if (const auto *let = dynamic_cast<const LetStatement *>(&statement)) {
        assert(let->initializer);
        std::optional<SemanticType> annotation;
        if (let->type_annotation)
            annotation = analyzer_.resolve_type(*let->type_annotation, current_owner_);
        const auto initializer = check_expression(*let->initializer,
                                                  annotation ? &*annotation : nullptr);
        const bool ok = annotation
                            ? require_coerce(initializer, *annotation,
                                             let->initializer->span)
                            : !initializer.type.is_error();
        // 初始化器先查旧绑定，再登记新绑定，允许 let x = x 式遮蔽。
        declare_local(let->name, Binding{annotation ? *annotation : initializer.type,
                                         let->is_mut});
        return {initializer.type.is_never(), !ok};
    }
    if (const auto *expression =
            dynamic_cast<const ExpressionStatement *>(&statement)) {
        assert(expression->expression);
        const auto value = check_expression(*expression->expression, nullptr);
        // 非尾的无分号块类表达式只能产生 () 或 !。
        const bool ok = expression->has_semicolon
                            ? !value.type.is_error()
                            : require_coerce(value, SemanticType::unit(),
                                             expression->expression->span);
        return {value.type.is_never(), !ok};
    }
    diagnostics_.error(statement.span, "internal error: unrecognized statement node");
    return {false, true};
}

// 先判断表达式是哪一种，再分别算出它的类型和“能不能当存储位置”。
// expected 是外面希望这里得到的类型，可以为空（表示外面没有要求）。
ExpressionResult
BodyChecker::check_expression(const Expression &expression,
                              const SemanticType *expected) {
    // 数字：有后缀就按后缀；没后缀但外面要整数就跟着外面；否则按 i32。
    if (const auto *literal = dynamic_cast<const IntegerLiteral *>(&expression))
        return check_integer_literal(*literal, expected);

    // true / false：结果是 bool，本身不是一个可存的变量。
    if (dynamic_cast<const BooleanLiteral *>(&expression))
        return ExpressionResult::value(SemanticType::bool_());

    // ()：结果是 ()，本身也不是一个可存的变量。
    if (dynamic_cast<const UnitExpression *>(&expression))
        return ExpressionResult::value(SemanticType::unit());

    // 名字路径：可能是局部变量、常量或 self，交给专门的值路径检查。
    if (const auto *path = dynamic_cast<const PathExpression *>(&expression))
        return check_value_path(*path);

    // 普通函数调用、内置 I/O 和显式关联调用。
    if (const auto *call = dynamic_cast<const CallExpression *>(&expression))
        return check_call(*call);

    // 点方法调用：先按接收者候选顺序选定目标。
    if (const auto *method_call =
            dynamic_cast<const MethodCallExpression *>(&expression))
        return check_method_call(*method_call);

    // 运算、赋值、借用、解引用和 cast 不吸收外层期望类型。
    if (const auto *binary = dynamic_cast<const BinaryExpression *>(&expression))
        return check_binary(*binary);
    if (const auto *unary = dynamic_cast<const UnaryExpression *>(&expression))
        return check_unary(*unary);
    if (const auto *assignment =
            dynamic_cast<const AssignmentExpression *>(&expression))
        return check_assignment(*assignment);
    if (const auto *cast = dynamic_cast<const CastExpression *>(&expression))
        return check_cast(*cast);

    // 聚合：结构体构造与数组表达式是值；字段和索引返回 place。
    if (const auto *structure =
            dynamic_cast<const StructExpression *>(&expression))
        return check_struct_expression(*structure);
    if (const auto *field = dynamic_cast<const FieldExpression *>(&expression))
        return check_field(*field);
    if (const auto *array = dynamic_cast<const ArrayExpression *>(&expression))
        return check_array(*array, expected);
    if (const auto *index = dynamic_cast<const IndexExpression *>(&expression))
        return check_index(*index);

    // 控制流：if/loop/while 是值；break/continue 是发散表达式。
    if (const auto *if_expression =
            dynamic_cast<const IfExpression *>(&expression))
        return check_if(*if_expression, expected);
    if (const auto *loop = dynamic_cast<const LoopExpression *>(&expression))
        return check_loop(*loop, expected);
    if (const auto *while_expression =
            dynamic_cast<const WhileExpression *>(&expression))
        return check_while(*while_expression);
    if (const auto *break_expression =
            dynamic_cast<const BreakExpression *>(&expression))
        return check_break(*break_expression);
    if (const auto *continue_expression =
            dynamic_cast<const ContinueExpression *>(&expression))
        return check_continue(*continue_expression);

    // 块：检查里面的语句和最后的值；外面希望的类型继续往里传。
    if (const auto *block = dynamic_cast<const BlockExpression *>(&expression))
        return check_block(*block, expected);

    // return：它不产生值，执行后后面的代码也不会再跑，所以类型是 !。
    // 有返回值时按函数声明的返回类型检查；没有返回值时要求返回类型是 ()。
    if (const auto *return_expression =
            dynamic_cast<const ReturnExpression *>(&expression)) {
        if (current_function_) {
            if (return_expression->value) {
                const ExpressionResult value = check_expression(
                    *return_expression->value,
                    &current_function_->return_type);
                require_coerce(value, current_function_->return_type,
                               return_expression->value->span);
            } else {
                require_coerce(
                    ExpressionResult::value(SemanticType::unit()),
                    current_function_->return_type, return_expression->span);
            }
        }
        return ExpressionResult::value(SemanticType::never());
    }

    // 分派遗漏也必须产生诊断，不能只返回 Error 而让程序静默通过。
    diagnostics_.error(expression.span,
                       "internal error: unrecognized expression node");
    return ExpressionResult::error();
}

// 建立函数级作用域，登记接收者和普通参数。
// 顶层函数没有 owner；impl 内无接收者的关联函数仍有 owner，供 Self 使用。
void BodyChecker::enter_function(const FunctionSymbol &fn) {
    scopes_.clear();
    loop_frames_.clear();
    current_function_ = &fn;
    current_owner_ = fn.owner;
    current_has_self_ = false;

    push_scope();
    assert(fn.node && fn.parameters.size() == fn.node->parameters.size());
    if (fn.receiver) {
        current_has_self_ = true;
        declare_local("self", Binding{fn.receiver->type,
                                      fn.receiver->binding_is_mut});
    }
    for (std::size_t index = 0; index < fn.parameters.size(); ++index) {
        const auto &parameter = fn.parameters[index];
        declare_local(fn.node->parameters[index].name,
                      Binding{parameter.type, parameter.is_mut});
    }
}

void BodyChecker::push_scope() { scopes_.emplace_back(); }

void BodyChecker::pop_scope() {
    assert(!scopes_.empty());
    scopes_.pop_back();
}

void BodyChecker::declare_local(const std::string &name,
                                const Binding &binding) {
    assert(!scopes_.empty());
    // 同一块内新 let 覆盖旧绑定；不报重复。
    scopes_.back()[name] = binding; // 新建或替换
}

// 从最内层作用域向外查找绑定，实现局部名字遮蔽。
std::optional<Binding>
BodyChecker::lookup_local(const std::string &name) const {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        const auto it = scope->find(name);
        if (it != scope->end())
            return it->second;
    }
    return std::nullopt;
}

} // namespace rx
