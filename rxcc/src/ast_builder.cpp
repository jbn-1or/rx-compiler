#include "ast_builder.hpp"

#include <cassert>
#include <cctype>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace rx {

namespace {

// 求 span：把一个 token 变成行列号范围（终点列不含）。
// 例：token "abc" 起于第 1 行第 5 列 → Span{{1,5},{1,8}}。
Span span_of(antlr4::Token *token) {
    if (!token)
        return Span{};
    const Position start{static_cast<int>(token->getLine()),
                         static_cast<int>(token->getCharPositionInLine())};
    return Span{start, Position{start.line,
                                start.column +
                                    static_cast<int>(token->getText().size())}};
}

// 求 span：终结符叶子节点（比如 `true`）转发它 token 的 span；空节点给空 span。
// 例：`true` 叶子 → span_of(它的 token)。
Span span_of(antlr4::tree::TerminalNode *node) {
    return node ? span_of(node->getSymbol()) : Span{};
}

// 求 span：取子树第一个 token 的起点到最后一个 token 的终点，覆盖整句。
// 例：`let a: i32 = 1;` 子树 → Span{{1,0},{1,17}}。
Span span_of(antlr4::ParserRuleContext *ctx) {
    if (!ctx || !ctx->getStart() || !ctx->getStop())
        return Span{};
    const Position start{
        static_cast<int>(ctx->getStart()->getLine()),
        static_cast<int>(ctx->getStart()->getCharPositionInLine())};
    const auto *stop = ctx->getStop();
    const Position end{
        static_cast<int>(stop->getLine()),
        static_cast<int>(stop->getCharPositionInLine() +
                         static_cast<int>(stop->getText().size()))};
    return Span{start, end};
}

// parse_int_literal 的结果：解析出的数值 + 类型后缀。
struct ParsedInt {
    int64_t value;
    IntSuffix suffix;
};

// 解析整数字面量文本：剥掉类型后缀（i32/u32/…），去掉 `_`，按 0x/0o/0b 前缀定进制。
// 例："0xFFu32" → {值 255, 后缀 U32}；"1_000" → {值 1000, 无后缀}。
ParsedInt parse_int_literal(const std::string &raw) {
    std::string body = raw;
    IntSuffix suffix = IntSuffix::None;
    static constexpr std::pair<const char *, IntSuffix> kSuffixes[] = {
        {"i32", IntSuffix::I32},
        {"u32", IntSuffix::U32},
        {"isize", IntSuffix::Isize},
        {"usize", IntSuffix::Usize},
    };
    for (const auto &[text, kind] : kSuffixes) {
        const std::size_t n = std::char_traits<char>::length(text);
        if (body.size() > n && body.compare(body.size() - n, n, text) == 0) {
            suffix = kind;
            body.resize(body.size() - n);
            break;
        }
    }

    std::string digits;
    digits.reserve(body.size());
    for (const char c : body) {
        if (c != '_')
            digits.push_back(c);
    }

    int base = 10;
    if (digits.size() >= 2 && digits[0] == '0') {
        const char p = static_cast<char>(
            std::tolower(static_cast<unsigned char>(digits[1])));
        if (p == 'x') {
            base = 16;
        } else if (p == 'o') {
            base = 8;
        } else if (p == 'b') {
            base = 2;
        }
        if (base != 10)
            digits.erase(0, 2); // 前缀不参与数值
    }

    int64_t value = 0;
    try {
        value = std::stoll(digits, nullptr, base);
    } catch (const std::out_of_range &) {
        value = 0; // 溢出以 raw 为准，语义期复查
    } catch (const std::invalid_argument &) {
        value = 0; // 防御
    }
    return {value, suffix};
}

// 判断一个语法树节点是不是二元运算符（折叠二元链时用来区分运算符和操作数）。
// 例：`+`、`&&`、`<<` → true；`1`、`a::b` 这类操作数 → false。
bool is_op_piece(antlr4::tree::ParseTree *node) {
    if (dynamic_cast<RxParser::AdditiveOperatorContext *>(node) ||      // + -
        dynamic_cast<RxParser::MultiplicativeOperatorContext *>(node) || // * / %
        dynamic_cast<RxParser::ComparisonExceptLtContext *>(node) ||     // == != <= > >=
        dynamic_cast<RxParser::ShiftRightContext *>(node)) {             // >>
        return true;
    }
    auto *term = dynamic_cast<antlr4::tree::TerminalNode *>(node);
    if (!term)
        return false;
    switch (term->getSymbol()->getType()) {
        case RxParser::ANDAND:
        case RxParser::OROR:
        case RxParser::AMP:
        case RxParser::CARET:
        case RxParser::PIPE:
        case RxParser::SHL:
        case RxParser::LT:
            return true;
        default:
            return false;
    }
}

// 把二元运算符翻译成 BinOp 枚举；不是运算符就返回空。
// 例：`*` → Mul、`&&` → LogAnd、`>=` → Ge。
std::optional<BinOp> map_binop(antlr4::tree::ParseTree *node) {
    if (auto *op = dynamic_cast<RxParser::AdditiveOperatorContext *>(node)) {
        (void)op;
        return node->getText() == "+" ? std::optional(BinOp::Add)
                                      : std::optional(BinOp::Sub);
    }
    if (auto *op =
            dynamic_cast<RxParser::MultiplicativeOperatorContext *>(node)) {
        if (op->STAR())
            return BinOp::Mul;
        if (op->SLASH())
            return BinOp::Div;
        return BinOp::Rem;
    }
    if (auto *op = dynamic_cast<RxParser::ComparisonExceptLtContext *>(node)) {
        if (op->EQEQ())
            return BinOp::Eq;
        if (op->NE())
            return BinOp::Ne;
        if (op->LE())
            return BinOp::Le;
        if (op->genericClose()) // genericClose 拼出来的源码都是 `>`
            return BinOp::Gt;
        return BinOp::Ge; // 剩下的两种 token 拼装，源码都是 `>=`
    }
    if (dynamic_cast<RxParser::ShiftRightContext *>(node)) {
        return BinOp::Shr; // `>>` 由 GT GT_SECOND 拼装
    }
    auto *term = dynamic_cast<antlr4::tree::TerminalNode *>(node);
    if (!term)
        return std::nullopt;
    switch (term->getSymbol()->getType()) {
        case RxParser::ANDAND:
            return BinOp::LogAnd;
        case RxParser::OROR:
            return BinOp::LogOr;
        case RxParser::AMP:
            return BinOp::BitAnd;
        case RxParser::CARET:
            return BinOp::BitXor;
        case RxParser::PIPE:
            return BinOp::BitOr;
        case RxParser::SHL:
            return BinOp::Shl;
        case RxParser::LT:
            return BinOp::Lt;
        default:
            return std::nullopt;
    }
}

// 把赋值号翻译成 AssignOp 枚举。
// 例：`=` → Assign、`+=` → AddAssign、`>>=` → ShrAssign。
std::optional<AssignOp> map_assign_op(RxParser::AssignmentOperatorContext *op) {
    if (op->equalsSign())
        return AssignOp::Assign; // 几种 token 拼装，源码都是 `=`
    if (op->GT())
        return AssignOp::ShrAssign; // `>>=` 由三个 token 拼装
    if (op->PLUS_ASSIGN())
        return AssignOp::AddAssign;
    if (op->MINUS_ASSIGN())
        return AssignOp::SubAssign;
    if (op->STAR_ASSIGN())
        return AssignOp::MulAssign;
    if (op->SLASH_ASSIGN())
        return AssignOp::DivAssign;
    if (op->PERCENT_ASSIGN())
        return AssignOp::RemAssign;
    if (op->AMP_ASSIGN())
        return AssignOp::BitAndAssign;
    if (op->PIPE_ASSIGN())
        return AssignOp::BitOrAssign;
    if (op->CARET_ASSIGN())
        return AssignOp::BitXorAssign;
    if (op->SHL_ASSIGN())
        return AssignOp::ShlAssign;
    return std::nullopt;
}

// 造一个 as 强制转换节点：操作数加目标类型，span 从操作数开头到类型结尾。
// 例：`x as i32` → CastExpression{operand=x, target=i32}。
std::shared_ptr<CastExpression> wrap_cast(std::shared_ptr<Expression> operand,
                                          std::shared_ptr<Type> target) {
    auto node = std::make_shared<CastExpression>();
    node->span = Span{operand->span.start, target->span.end};
    node->operand = std::move(operand);
    node->target = std::move(target);
    return node;
}

} // namespace

// ══════ 0. 分派机制：表达式族的统一入口 ══════
// dispatch 把表达式族规则路由到对应构建函数；空指针表示跳过 / 已报错。
std::shared_ptr<Expression>
AstBuilder::dispatch(antlr4::tree::ParseTree *node) {
    auto *ctx = dynamic_cast<antlr4::ParserRuleContext *>(node);
    if (!ctx)
        return nullptr; // 终结符没有规则可分派
    switch (ctx->getRuleIndex()) {
        // 单孩子直通
        case RxParser::RuleExpression:
        case RxParser::RuleStatementExpression:
        case RxParser::RuleConditionExpression:
        case RxParser::RuleConditionBreakExpression:
        case RxParser::RulePrimaryExpression:
            // 若没有孩子则返回空指针
            if (ctx->children.empty())
                return nullptr;
            return dispatch(ctx->children[0]);
        // 赋值层：四族镜像
        case RxParser::RuleAssignmentExpression:
        case RxParser::RuleStatementAssignmentExpression:
        case RxParser::RuleConditionAssignmentExpression:
        case RxParser::RuleConditionBreakAssignmentExpression:
            return build_assignment_layer(ctx);
        // 二元运算各层：同一 handler 左折叠
        case RxParser::RuleLogicalOrExpression:
        case RxParser::RuleLogicalAndExpression:
        case RxParser::RuleComparisonExpression:
        case RxParser::RuleBitOrExpression:
        case RxParser::RuleClosedBitOrExpression:
        case RxParser::RuleBitXorExpression:
        case RxParser::RuleClosedBitXorExpression:
        case RxParser::RuleBitAndExpression:
        case RxParser::RuleClosedBitAndExpression:
        case RxParser::RuleShiftExpression:
        case RxParser::RuleClosedShiftExpression:
        case RxParser::RuleAdditiveExpression:
        case RxParser::RuleClosedAdditiveExpression:
        case RxParser::RuleMultiplicativeExpression:
        case RxParser::RuleClosedMultiplicativeExpression:
        // —— condition 族
        case RxParser::RuleConditionLogicalOrExpression:
        case RxParser::RuleConditionLogicalAndExpression:
        case RxParser::RuleConditionComparisonExpression:
        case RxParser::RuleConditionBitOrExpression:
        case RxParser::RuleConditionClosedBitOrExpression:
        case RxParser::RuleConditionBitXorExpression:
        case RxParser::RuleConditionClosedBitXorExpression:
        case RxParser::RuleConditionBitAndExpression:
        case RxParser::RuleConditionClosedBitAndExpression:
        case RxParser::RuleConditionShiftExpression:
        case RxParser::RuleConditionClosedShiftExpression:
        case RxParser::RuleConditionAdditiveExpression:
        case RxParser::RuleConditionClosedAdditiveExpression:
        case RxParser::RuleConditionMultiplicativeExpression:
        case RxParser::RuleConditionClosedMultiplicativeExpression:
        // —— conditionBreak 族
        case RxParser::RuleConditionBreakLogicalOrExpression:
        case RxParser::RuleConditionBreakLogicalAndExpression:
        case RxParser::RuleConditionBreakComparisonExpression:
        case RxParser::RuleConditionBreakBitOrExpression:
        case RxParser::RuleConditionBreakClosedBitOrExpression:
        case RxParser::RuleConditionBreakBitXorExpression:
        case RxParser::RuleConditionBreakClosedBitXorExpression:
        case RxParser::RuleConditionBreakBitAndExpression:
        case RxParser::RuleConditionBreakClosedBitAndExpression:
        case RxParser::RuleConditionBreakShiftExpression:
        case RxParser::RuleConditionBreakClosedShiftExpression:
        case RxParser::RuleConditionBreakAdditiveExpression:
        case RxParser::RuleConditionBreakClosedAdditiveExpression:
        case RxParser::RuleConditionBreakMultiplicativeExpression:
        case RxParser::RuleConditionBreakClosedMultiplicativeExpression:
        // —— statement 族
        case RxParser::RuleStatementLogicalOrExpression:
        case RxParser::RuleStatementLogicalAndExpression:
        case RxParser::RuleStatementComparisonExpression:
        case RxParser::RuleStatementBitOrExpression:
        case RxParser::RuleStatementClosedBitOrExpression:
        case RxParser::RuleStatementBitXorExpression:
        case RxParser::RuleStatementClosedBitXorExpression:
        case RxParser::RuleStatementBitAndExpression:
        case RxParser::RuleStatementClosedBitAndExpression:
        case RxParser::RuleStatementShiftExpression:
        case RxParser::RuleStatementClosedShiftExpression:
        case RxParser::RuleStatementAdditiveExpression:
        case RxParser::RuleStatementClosedAdditiveExpression:
        case RxParser::RuleStatementMultiplicativeExpression:
        case RxParser::RuleStatementClosedMultiplicativeExpression:
            return fold_chain(ctx);
        // —— cast / 一元 / 后缀：四族 × 开放/closed ——
        case RxParser::RuleCastExpression:
        case RxParser::RuleClosedCastExpression:
        case RxParser::RuleConditionCastExpression:
        case RxParser::RuleConditionClosedCastExpression:
        case RxParser::RuleConditionBreakCastExpression:
        case RxParser::RuleConditionBreakClosedCastExpression:
        case RxParser::RuleStatementCastExpression:
        case RxParser::RuleStatementClosedCastExpression:
            return build_cast_layer(ctx);
        case RxParser::RuleUnaryExpression:
        case RxParser::RuleConditionUnaryExpression:
        case RxParser::RuleConditionBreakUnaryExpression:
        case RxParser::RuleStatementUnaryExpression:
            return build_unary_layer(ctx);
        case RxParser::RulePostfixExpression:
        case RxParser::RuleConditionPostfixExpression:
        case RxParser::RuleConditionBreakPostfixExpression:
        case RxParser::RuleStatementPostfixExpression:
            return build_postfix_layer(ctx);
        // —— primary 与字面量——
        case RxParser::RuleNonBlockPrimary:
            return build_non_block_primary(
                static_cast<RxParser::NonBlockPrimaryContext *>(ctx));
        case RxParser::RuleLiteralExpression:
            return build_literal(
                static_cast<RxParser::LiteralExpressionContext *>(ctx));
        case RxParser::RuleArrayExpression:
            return build_array_expr(
                static_cast<RxParser::ArrayExpressionContext *>(ctx));
        case RxParser::RulePathInExpression:
            return build_path(
                static_cast<RxParser::PathInExpressionContext *>(ctx));
        // —— 块类与控制流
        case RxParser::RuleExpressionWithBlock:
            return build_expression_with_block(
                static_cast<RxParser::ExpressionWithBlockContext *>(ctx));
        case RxParser::RuleIfExpression:
            return build_if(
                static_cast<RxParser::IfExpressionContext *>(ctx));
        case RxParser::RuleConditionPrimary:
            return build_condition_primary(
                static_cast<RxParser::ConditionPrimaryContext *>(ctx));
        case RxParser::RuleConditionPrimaryWithoutBareBlock:
            return build_condition_primary_nb(
                static_cast<
                    RxParser::ConditionPrimaryWithoutBareBlockContext *>(ctx));
        default:
            assert(false &&
                   "dispatch：未登记规则（走类型化 helper，不应到达）");
            diags_.error(span_of(ctx), "dispatch：未登记规则 index=" +
                                           std::to_string(ctx->getRuleIndex()));
            return nullptr;
    }
}

// ══════ 1. 文件与 item 层（17 条：crate、item、四类定义；use 跳过） ══════
// 自顶向下：整个文件 → 顶层项 → 各类定义；参数/字段/属性片段由这些函数内联处理。

// 入口：把整个文件的解析树变成 AST。
// 例：源码 `fn main() {}` → Crate{items=[main]}（转发 build_crate）。
std::shared_ptr<Crate> AstBuilder::build(RxParser::CrateContext *ctx) {
    return build_crate(ctx);
}

// 收集文件里所有顶层项（函数、struct、常量、impl），装进 Crate。
// 例：`fn main() {} fn f() {}` → Crate{items=[Function main, Function f]}。
std::shared_ptr<Crate> AstBuilder::build_crate(RxParser::CrateContext *ctx) {
    auto crate = std::make_shared<Crate>();
    // 遍历所有顶层项，构建成功的才收进 crate
    for (auto *item : ctx->item()) {
        auto node = build_item(item);
        if (node)
            crate->items.push_back(std::move(node));
    }
    return crate;
}

// 看顶层项是哪种，转给对应函数；use 声明合法但暂时忽略（返回空，不报错）。
// 例：`fn f() {}` → build_function_definition；`struct S { … }` → build_struct_definition。
std::shared_ptr<Item> AstBuilder::build_item(RxParser::ItemContext *ctx) {
    // 若是 use 声明则合法跳过，不报错
    if (ctx->useDeclaration())
        return nullptr;
    if (ctx->functionDefinition())
        return build_function_definition(ctx->functionDefinition());
    if (ctx->structDefinition())
        return build_struct_definition(ctx->structDefinition());
    if (ctx->constantItem())
        return build_constant_item(ctx->constantItem());
    return build_inherent_impl(ctx->inherentImpl());
}

// 处理函数定义：名字、self 参数、普通参数、返回类型、函数体；没写 `-> T` 时返回类型按空元组 () 算。
// 例：`fn main() { 1; }` → Function{name="main", ret=(), body=Block{…}}。
std::shared_ptr<Item> AstBuilder::build_function_definition(
    RxParser::FunctionDefinitionContext *ctx) {
    auto fn = std::make_shared<Function>();
    fn->span = span_of(ctx);
    fn->name = ctx->identifier()->getText();
    fn->has_generic_params = ctx->genericParams() != nullptr;
    if (auto *params = ctx->functionParameters()) {
        if (params->selfParam()) {
            fn->self_param = build_self_param(params->selfParam());
        }
        // 遍历普通参数：取 mut 标记、名字和类型
        for (auto *p : params->functionParam()) {
            Param param;
            auto *binding = p->identifierBinding();
            param.is_mut = binding->MUT() != nullptr;
            param.name = binding->identifier()->getText();
            auto ty = build_type_ref(p->typeRef());
            if (!ty)
                return nullptr;
            param.type = std::move(ty);
            fn->params.push_back(std::move(param));
        }
    }
    // 若写了 -> T 则构建返回类型，否则默认空元组 ()
    if (ctx->ARROW()) {
        auto ret = build_type_ref(ctx->typeRef());
        if (!ret)
            return nullptr;
        fn->ret = std::move(ret);
    } else {
        auto unit = std::make_shared<UnitType>();
        unit->span = Span{}; // 省略的返回类型没有源码文本，span 留空
        fn->ret = std::move(unit);
    }
    auto body = build_block(ctx->blockExpression());
    if (!body)
        return nullptr;
    fn->body = std::move(body);
    return fn;
}

// 处理 struct 定义：名字、#[derive(..)] 属性、字段列表；泛型参数和 where 子句忽略。
// 例：`#[derive(Clone, Copy)] struct S { a: i32 }` → Struct{name="S", derives=[Clone, Copy], fields=[{a, i32}]}。
std::shared_ptr<Item>
AstBuilder::build_struct_definition(RxParser::StructDefinitionContext *ctx) {
    auto node = std::make_shared<Struct>();
    node->span = span_of(ctx);
    node->name = ctx->identifier()->getText();
    for (auto *attr : ctx->outerAttribute()) {
        // 属性只有 #[derive(..)] 一种：遍历名字映射成 Derive
        for (auto *d : attr->deriveName()) {
            if (d->COPY()) {
                node->derives.push_back(Derive::Copy);
            } else if (d->CLONE()) {
                node->derives.push_back(Derive::Clone);
            } else if (d->PARTIAL_EQ()) {
                node->derives.push_back(Derive::PartialEq);
            } else {
                node->derives.push_back(Derive::Eq);
            }
        }
    }
    // 遍历字段：取名字和类型
    for (auto *f : ctx->structField()) {
        StructField field;
        field.name = f->identifier()->getText();
        auto ty = build_type_ref(f->typeRef());
        if (!ty)
            return nullptr;
        field.type = std::move(ty);
        node->fields.push_back(std::move(field));
    }
    // genericParams/whereClause 无视。
    return node;
}

// 处理常量定义。
// 例：`const MAX: i32 = 100;` → Const{name="MAX", type=i32, value=100}。
std::shared_ptr<Item>
AstBuilder::build_constant_item(RxParser::ConstantItemContext *ctx) {
    auto node = std::make_shared<Const>();
    node->span = span_of(ctx);
    node->name = ctx->identifier()->getText();
    auto ty = build_type_ref(ctx->typeRef());
    if (!ty)
        return nullptr;
    node->type = std::move(ty);
    node->value = build_const(ctx->constValue()); // equalsSign 全是 `=`，无视
    return node;
}

// 处理 impl 块：self 类型加上里面的常量和函数。
// 例：`impl S { const C: i32 = 1; fn f(&self) {} }` → InherentImpl{self_type=S, items=[Const C, Function f]}。
std::shared_ptr<Item>
AstBuilder::build_inherent_impl(RxParser::InherentImplContext *ctx) {
    auto node = std::make_shared<InherentImpl>();
    node->span = span_of(ctx);
    auto self_ty = build_type_ref(ctx->typeRef());
    if (!self_ty)
        return nullptr;
    node->self_type = std::move(self_ty);
    // 遍历 impl 成员：成员只有常量和函数两种，按实际类型直调，成功的收进来
    for (auto *assoc : ctx->associatedItem()) {
        std::shared_ptr<Item> item =
            assoc->constantItem()
                ? build_constant_item(assoc->constantItem())
                : build_function_definition(assoc->functionDefinition());
        if (item)
            node->items.push_back(std::move(item));
    }
    // genericParams/whereClause 无视。
    return node;
}

// ══════ 2. 语句与块（statement、letStatement、blockExpression） ══════
// 块的尾值与语句里的表达式仍经 dispatch 回到表达式族。

// 处理块 `{…}`：开头一串语句，末尾可以有一个表达式当整个块的值。
// 例：`{ 1; }` → 语句 [1;]，无尾值；`{ 1 }` → 无语句，尾值 1。
std::shared_ptr<BlockExpression>
AstBuilder::build_block(RxParser::BlockExpressionContext *ctx) {
    auto block = std::make_shared<BlockExpression>();
    block->span = span_of(ctx);
    // 遍历块内语句，构建成功的收进 statements
    for (auto *st : ctx->statement()) {
        auto node = build_statement(st);
        if (node)
            block->statements.push_back(std::move(node));
    }
    if (ctx->statementExpression()) {
        block->tail = dispatch(ctx->statementExpression());
    }
    return block;
}

// 处理语句的三种形态：let 语句、表达式语句、光分号。
// 例：`1;` → ExpressionStatement(1)；`;` → 忽略；`let a = 1;` → build_let。
std::shared_ptr<Statement>
AstBuilder::build_statement(RxParser::StatementContext *ctx) {
    // 裸分号没有专属访问器，排除完其他分支后剩下的才是它
    std::shared_ptr<Expression> expr;
    if (ctx->letStatement()) {
        return build_let(ctx->letStatement());
    }
    if (ctx->expressionWithBlock()) {
        // 块类表达式语句，分号可省
        expr = build_expression_with_block(ctx->expressionWithBlock());
    } else if (ctx->statementExpression()) {
        expr = dispatch(ctx->statementExpression());
    } else {
        return nullptr; // 只剩裸分号，合法跳过
    }
    if (!expr)
        return nullptr;
    auto st = std::make_shared<ExpressionStatement>();
    st->span = span_of(ctx);
    st->has_semi = (ctx->SEMI() != nullptr);
    st->expr = std::move(expr);
    return st;
}

// 处理 let 语句：变量名、可不可变、可选的类型标注、初始化表达式。
// 例：`let mut a: i32 = 1;` → Let{可变, 名 a, 类型 i32, 初值 1}。
std::shared_ptr<Statement>
AstBuilder::build_let(RxParser::LetStatementContext *ctx) {
    auto st = std::make_shared<LetStatement>();
    st->span = span_of(ctx);
    auto *binding = ctx->identifierBinding();
    st->is_mut = binding->MUT() != nullptr;
    st->name = binding->identifier()->getText();
    if (ctx->typeRef()) {
        auto ty = build_type_ref(ctx->typeRef());
        if (!ty)
            return nullptr;
        st->type_annotation = std::move(ty);
    }
    // equalsSign 几种 token 拼装全是 `=`，无视
    auto init = dispatch(ctx->expression());
    if (!init)
        return nullptr;
    st->init = std::move(init);
    return st;
}

// ══════ 3. 表达式族（全部经 dispatch） ══════
// 形状层按优先级从低到高（= 树从顶到底）→ primary → 块与控制流。

// 把 `1 + 2 * 3` 这样的一条运算链折叠成一棵树：每碰到一个运算符，就把左边已有的树和右边操作数合成一个节点，靠左结合。
// 例：`1 + 2 + 3` → Binary{Add, Binary{Add, 1, 2}, 3}；只有一个操作数时原样返回。
std::shared_ptr<Expression>
AstBuilder::fold_chain(antlr4::ParserRuleContext *ctx) {
    std::shared_ptr<Expression> acc;
    std::optional<BinOp> pending_op;
    // 遍历运算链上的每个孩子：是运算符就先记下，是操作数就往左边的树上合并
    for (auto *child : ctx->children) {
        if (is_op_piece(child)) {
            pending_op = map_binop(child);
            continue;
        }
        auto operand = dispatch(child);
        if (!operand)
            return nullptr;
        if (!acc) {
            acc = std::move(operand); // 第一个操作数：直接当左值
        } else if (pending_op) {
            // 左值 + 记下的运算符 + 右操作数：合并成新的左值（左结合）
            auto bin = std::make_shared<BinaryExpression>();
            bin->op = *pending_op;
            bin->span =
                Span{acc->span.start, operand->span.end};
            bin->lhs = std::move(acc);
            bin->rhs = std::move(operand);
            acc = std::move(bin);
            pending_op.reset();
        } else {
            diags_.error(span_of(ctx),
                         "二元链：连续操作数缺少运算符（防御分支）");
            return nullptr;
        }
    }
    return acc;
}

// 处理赋值表达式（a = b、a += b 这类）；孩子里没有赋值号时说明只是普通表达式，原样转发。
// 例：`a += 1` → Assignment{op=AddAssign, lhs=a, rhs=1}；`a + 1` → 转发。
std::shared_ptr<Expression>
AstBuilder::build_assignment_layer(antlr4::ParserRuleContext *ctx) {
    const auto &children = ctx->children;
    // 若没有赋值号，说明只是普通表达式，转发
    if (children.size() == 1)
        return dispatch(children[0]);
    if (children.size() != 3) {
        diags_.error(span_of(ctx), "赋值层：孩子形状异常（防御分支）");
        return nullptr;
    }
    auto lhs = dispatch(children[0]);
    if (!lhs)
        return nullptr;
    auto *op_ctx =
        dynamic_cast<RxParser::AssignmentOperatorContext *>(children[1]);
    if (!op_ctx) {
        diags_.error(span_of(ctx), "赋值层：第二孩子不是赋值号（防御分支）");
        return nullptr;
    }
    auto op = map_assign_op(op_ctx);
    if (!op) {
        diags_.error(span_of(op_ctx), "未知赋值运算符（防御分支）");
        return nullptr;
    }
    // 右结合：rhs 里再遇到赋值号会递归回到这里
    auto rhs = dispatch(children[2]);
    if (!rhs)
        return nullptr;
    auto node = std::make_shared<AssignmentExpression>();
    node->span = span_of(ctx);
    node->op = *op;
    node->lhs = std::move(lhs);
    node->rhs = std::move(rhs);
    return node;
}

// 处理 as 强制转换，连续转换靠左结合；没有 as 时原样转发。
// 例：`x as i32 as u32` → Cast{Cast{x, i32}, u32}；`x as Vec<i32>` → Cast{x, Vec<i32>}。
std::shared_ptr<Expression>
AstBuilder::build_cast_layer(antlr4::ParserRuleContext *ctx) {
    const auto &children = ctx->children;
    if (children.empty()) {
        diags_.error(span_of(ctx), "cast 层：空孩子（防御分支）");
        return nullptr;
    }
    // 若孩子里有 closedCastType，说明是泛型收尾的收口形（文法消歧用）
    RxParser::ClosedCastTypeContext *closed = nullptr;
    for (auto *child : children) {
        closed = dynamic_cast<RxParser::ClosedCastTypeContext *>(child);
        if (closed)
            break;
    }
    auto e = dispatch(children[0]);
    if (!e)
        return nullptr;
    if (closed) {
        auto ty = build_closed_cast_type(closed);
        if (!ty)
            return nullptr;
        return wrap_cast(std::move(e), std::move(ty));
    }
    // 开放形：遍历剩余孩子，每遇到一个 as T 就在外面包一层 cast
    for (auto *child : children) {
        auto *t = dynamic_cast<RxParser::TypeRefContext *>(child);
        if (!t)
            continue;
        auto ty = build_type_ref(t);
        if (!ty)
            return nullptr;
        e = wrap_cast(std::move(e), std::move(ty));
    }
    return e;
}

// 处理一元运算（- ! * &）；第一个孩子不是运算符时原样转发。
// 例：`-a` → Unary{Neg, a}；`f(1)`（无运算符）→ 转发后缀子树。
std::shared_ptr<Expression>
AstBuilder::build_unary_layer(antlr4::ParserRuleContext *ctx) {
    const auto &children = ctx->children;
    // 若首孩子是一元运算符，则连同操作数构建
    if (children.size() >= 2) {
        if (auto *op_ctx =
                dynamic_cast<RxParser::UnaryOperatorContext *>(children[0])) {
            return make_unary(op_ctx, dispatch(children[1]));
        }
    }
    // 若没有一元运算符，说明还没到一元层，转发
    if (children.size() == 1)
        return dispatch(children[0]);
    diags_.error(span_of(ctx), "一元层：孩子形状异常（防御分支）");
    return nullptr;
}

// 处理后缀链：主表达式后面接 0 个或多个后缀（调用、下标、点后缀），从左到右逐个套上。
// 例：`f(1)[0].x` → Field{Index{Call{f, [1]}, 0}, x}。
std::shared_ptr<Expression>
AstBuilder::build_postfix_layer(antlr4::ParserRuleContext *ctx) {
    const auto &children = ctx->children;
    if (children.empty()) {
        diags_.error(span_of(ctx), "后缀层：空孩子（防御分支）");
        return nullptr;
    }
    auto e = dispatch(children[0]);
    std::size_t i = 1;
    // 若以块开头，后面只允许点后缀
    if (dynamic_cast<RxParser::ExpressionWithBlockContext *>(children[0])) {
        auto *dot =
            i < children.size()
                ? dynamic_cast<RxParser::DotSuffixContext *>(children[i])
                : nullptr;
        if (!dot) {
            diags_.error(span_of(ctx),
                         "后缀层：块首缺少 dotSuffix（防御分支）");
            return nullptr;
        }
        e = apply_dot_suffix(std::move(e), dot);
        ++i;
    }
    // 剩余的孩子逐个当后缀套上
    for (; i < children.size(); ++i) {
        auto *suffix =
            dynamic_cast<RxParser::PostfixSuffixContext *>(children[i]);
        if (!suffix) {
            diags_.error(span_of(ctx), "后缀层：未知后缀片段（防御分支）");
            return nullptr;
        }
        e = apply_suffix(std::move(e), suffix);
        if (!e)
            return nullptr;
    }
    return e;
}

// 处理最基础的表达式：字面量、路径、struct 构造、括号、空元组、数组、break/return/continue。
// 例：`Point { x: 1 }` → StructExpression；`(e)` → 解包出 e；`()` → 空元组。
std::shared_ptr<Expression>
AstBuilder::build_non_block_primary(RxParser::NonBlockPrimaryContext *ctx) {
    if (ctx->literalExpression()) {
        return build_literal(ctx->literalExpression());
    }
    if (ctx->pathInExpression()) {
        // 若路径后跟 `{`，是 struct 构造
        if (ctx->LBRACE()) {
            auto node = std::make_shared<StructExpression>();
            node->span = span_of(ctx);
            node->path = build_path(ctx->pathInExpression());
            // 遍历 struct 字段：取名字和值
            if (auto *fields = ctx->structExprFields()) {
                for (auto *f : fields->structExprField()) {
                    StructExprField field;
                    field.name = f->identifier()->getText();
                    field.value = dispatch(f->expression());
                    if (!field.value)
                        return nullptr;
                    node->fields.push_back(std::move(field));
                }
            }
            return node;
        }
        return build_path(ctx->pathInExpression());
    }
    if (ctx->LPAREN()) {
        // 若括号里有表达式则解包，否则是空元组 ()
        if (ctx->expression()) {
            return dispatch(ctx->expression());
        }
        auto unit = std::make_shared<UnitExpression>();
        unit->span = span_of(ctx);
        return unit;
    }
    if (ctx->BREAK()) {
        auto node = std::make_shared<BreakExpression>();
        node->span = span_of(ctx);
        if (ctx->expression()) {
            node->value = dispatch(ctx->expression());
            if (!node->value)
                return nullptr;
        }
        return node;
    }
    if (ctx->RETURN()) {
        auto node = std::make_shared<ReturnExpression>();
        node->span = span_of(ctx);
        if (ctx->expression()) {
            node->value = dispatch(ctx->expression());
            if (!node->value)
                return nullptr;
        }
        return node;
    }
    if (ctx->CONTINUE()) {
        auto node = std::make_shared<ContinueExpression>();
        node->span = span_of(ctx);
        return node;
    }
    return build_array_expr(ctx->arrayExpression()); // 数组（7.7）
}

// 处理字面量：整数和布尔。
// 例：`1` → IntLit{值 1}；`0x1F` → IntLit{值 31, 原文 "0x1F"}；`true` → BoolLit{true}。
std::shared_ptr<Expression>
AstBuilder::build_literal(RxParser::LiteralExpressionContext *ctx) {
    if (ctx->INTEGER_LITERAL()) {
        auto node = std::make_shared<IntLit>();
        node->span = span_of(ctx->INTEGER_LITERAL());
        node->raw =
            ctx->INTEGER_LITERAL()->getText(); // 原始全文（含后缀与下划线）
        const auto parsed = parse_int_literal(node->raw);
        node->value = parsed.value;
        node->suffix = parsed.suffix;
        return node;
    }
    if (ctx->TRUE() || ctx->FALSE()) {
        auto node = std::make_shared<BoolLit>();
        node->span = span_of(ctx);
        node->value = ctx->TRUE() != nullptr;
        return node;
    }
    assert(false && "literalExpression：文法只有三种形态，不可达");
    return nullptr;
}

// 处理数组表达式：列表形和重复形。
// 例：`[a, b]` → elements=[a, b]；`[0; 3]` → 重复：值 0 × 3 个。
std::shared_ptr<Expression>
AstBuilder::build_array_expr(RxParser::ArrayExpressionContext *ctx) {
    auto node = std::make_shared<ArrayExpression>();
    node->span = span_of(ctx);
    if (ctx->expression().empty()) {
        return node; // 空数组 []
    }
    // 若有 `;` 则是重复形 `[值; 个数]`
    if (ctx->SEMI()) {
        auto value = dispatch(ctx->expression(0));
        if (!value)
            return nullptr;
        node->repeat_value = std::move(value);
        node->repeat_count = build_const(ctx->constValue());
        return node;
    }
    for (auto *e : ctx->expression()) {
        auto el = dispatch(e);
        if (!el)
            return nullptr;
        node->elements.push_back(std::move(el));
    }
    return node;
}

// 处理路径表达式：变量名、带命名空间的名字、带泛型实参的名字。
// 例：`a::b::c` → PathExpression{段 [a, b, c]}；`Vec::<i32>` → 段 [Vec]，泛型 [i32]。
std::shared_ptr<PathExpression>
AstBuilder::build_path(RxParser::PathInExpressionContext *ctx) {
    auto node = std::make_shared<PathExpression>();
    node->span = span_of(ctx);
    for (auto *seg : ctx->pathExprSegment()) {
        node->segments.push_back(build_segment(seg));
    }
    return node;
}

// 处理带块的表达式，四种形态分别转给对应函数。
// 例：`loop {…}` → build_loop；`while c {…}` → build_while；`{…}` → build_block；`if …` → build_if。
std::shared_ptr<Expression> AstBuilder::build_expression_with_block(
    RxParser::ExpressionWithBlockContext *ctx) {
    // 注意：LOOP/WHILE 分支里 blockExpression() 也非空，必须先判关键字
    if (ctx->LOOP()) {
        return build_loop(ctx, ctx->blockExpression());
    }
    if (ctx->WHILE()) {
        return build_while(ctx, ctx->conditionExpression(),
                           ctx->blockExpression());
    }
    if (ctx->blockExpression()) {
        return build_block(ctx->blockExpression());
    }
    return build_if(ctx->ifExpression());
}

// 处理 if 表达式；`else if` 链通过递归嵌套在 else 上。
// 例：`if c { a } else { b }` → IfExpression{condition=c, then=a, else_=b}。
std::shared_ptr<Expression>
AstBuilder::build_if(RxParser::IfExpressionContext *ctx) {
    auto cond = dispatch(ctx->conditionExpression());
    if (!cond)
        return nullptr;
    auto then = build_block(ctx->blockExpression(0));
    if (!then)
        return nullptr;
    auto node = std::make_shared<IfExpression>();
    node->span = span_of(ctx);
    node->condition = std::move(cond);
    node->then = std::move(then);
    // 若 else 后面还是 if 则递归嵌套，否则是块
    if (ctx->ELSE()) {
        if (ctx->ifExpression()) {
            node->else_ = build_if(ctx->ifExpression());
        } else {
            node->else_ = build_block(ctx->blockExpression(1));
        }
        if (!node->else_)
            return nullptr;
    }
    return node;
}

// 处理条件位的最基础表达式：比普通表达式多允许裸块（`if { … }` 这种写法），其余转发。
// 例：条件是裸块 `{…}` → 转成块表达式；其他（如 `1`、`a`）→ build_condition_primary_nb。
std::shared_ptr<Expression>
AstBuilder::build_condition_primary(RxParser::ConditionPrimaryContext *ctx) {
    // 若是裸块则直接当块表达式
    if (ctx->blockExpression()) {
        return build_block(ctx->blockExpression());
    }
    return build_condition_primary_nb(ctx->conditionPrimaryWithoutBareBlock());
}

// 同上但不允许裸块：字面量、路径、括号、空元组、数组、if/loop/while、break/return/continue（struct 构造在条件位被文法禁止）。
// 例：`while n > 0 {…}` 里的 `n`、`if flag {…}` 里的 `flag` 都在这里变成路径表达式。
std::shared_ptr<Expression> AstBuilder::build_condition_primary_nb(
    RxParser::ConditionPrimaryWithoutBareBlockContext *ctx) {
    if (ctx->literalExpression()) {
        return build_literal(ctx->literalExpression());
    }
    // 条件位禁 struct 构造（文法保证），路径直接上转
    if (ctx->pathInExpression()) {
        return build_path(ctx->pathInExpression());
    }
    if (ctx->LPAREN()) {
        if (ctx->expression()) {
            return dispatch(ctx->expression()); // (e) 解包
        }
        auto unit = std::make_shared<UnitExpression>();
        unit->span = span_of(ctx);
        return unit;
    }
    if (ctx->arrayExpression()) {
        return build_array_expr(ctx->arrayExpression());
    }
    if (ctx->ifExpression()) {
        return build_if(ctx->ifExpression());
    }
    if (ctx->LOOP()) {
        return build_loop(ctx, ctx->blockExpression());
    }
    if (ctx->WHILE()) {
        return build_while(ctx, ctx->conditionExpression(),
                           ctx->blockExpression());
    }
    if (ctx->BREAK()) {
        auto node = std::make_shared<BreakExpression>();
        node->span = span_of(ctx);
        // break 的值走 conditionBreak 族
        if (ctx->conditionBreakExpression()) {
            node->value = dispatch(ctx->conditionBreakExpression());
            if (!node->value)
                return nullptr;
        }
        return node;
    }
    if (ctx->RETURN()) {
        auto node = std::make_shared<ReturnExpression>();
        node->span = span_of(ctx);
        if (ctx->conditionExpression()) {
            node->value = dispatch(ctx->conditionExpression());
            if (!node->value)
                return nullptr;
        }
        return node;
    }
    auto node = std::make_shared<ContinueExpression>();
    node->span = span_of(ctx);
    return node;
}

// ══════ 4. 类型层（typeRef、referenceType、arrayType、typePath、typePathSegment、closedCastType） ══════
// 被 item 层（函数签名/字段）与表达式族（cast 目标）调用；(T) 解包、() 空元组。

// 处理类型，各形态分给对应函数；`(T)` 直接解包，不要括号节点。
// 例：`i32` → 路径类型；`&mut T` → build_reference_type；`[T; N]` → build_array；`()` → 空元组类型。
std::shared_ptr<Type>
AstBuilder::build_type_ref(RxParser::TypeRefContext *ctx) {
    if (ctx->LPAREN()) {
        // 若括号里有类型则解包，否则是空元组类型 ()
        if (ctx->typeRef()) {
            return build_type_ref(ctx->typeRef());
        }
        auto unit = std::make_shared<UnitType>();
        unit->span = span_of(ctx); // 手写的 () 有源码文本
        return unit;
    }
    if (ctx->typePath())
        return build_type_path(ctx->typePath());
    if (ctx->referenceType())
        return build_reference_type(ctx->referenceType());
    if (ctx->arrayType())
        return build_array(ctx->arrayType());
    assert(false && "typeRef：五备选未穷尽");
    return nullptr;
}

// 处理类型路径：带 :: 的类型名，每段可以带泛型实参。
// 例：`Vec<Box<i32>>` → PathType{段 [Vec, Box]}，Box 带实参 i32。
std::shared_ptr<Type>
AstBuilder::build_type_path(RxParser::TypePathContext *ctx) {
    auto node = std::make_shared<PathType>();
    node->span = span_of(ctx);
    for (auto *seg : ctx->typePathSegment()) {
        node->segments.push_back(build_type_segment(seg));
    }
    return node;
}

// 处理引用类型（lifetime 忽略）。
// 例：`&i32` → 一层引用；`&&mut i32` → 两层引用，mut 归内层。
std::shared_ptr<Type>
AstBuilder::build_reference_type(RxParser::ReferenceTypeContext *ctx) {
    auto inner = build_type_ref(ctx->typeRef());
    if (!inner)
        return nullptr;
    return build_ref_type(ctx, ctx->ANDAND(), ctx->MUT() != nullptr,
                          std::move(inner));
}

// 处理数组类型。
// 例：`[i32; 3]` → ArrayType{元素 i32, 长度 3}。
std::shared_ptr<Type> AstBuilder::build_array(RxParser::ArrayTypeContext *ctx) {
    auto elem = build_type_ref(ctx->typeRef());
    if (!elem)
        return nullptr;
    auto node = std::make_shared<ArrayType>();
    node->span = span_of(ctx);
    node->element = std::move(elem);
    node->length = build_const(ctx->constValue());
    return node;
}

// 处理 as 转换的目标类型：和普通类型基本一样，只是泛型结尾的路径在这里专门收口（解决和 `<`/`>>` 的歧义）。
// 例：`x as Vec<i32>` 的 `Vec<i32>`、`x as (i32)` 的 `(i32)` → 与 build_type_ref 同形的结果。
std::shared_ptr<Type>
AstBuilder::build_closed_cast_type(RxParser::ClosedCastTypeContext *ctx) {
    if (ctx->LPAREN()) {
        if (ctx->typeRef()) {
            return build_type_ref(ctx->typeRef()); // (T) 解包
        }
        auto unit = std::make_shared<UnitType>();
        unit->span = span_of(ctx);
        return unit;
    }
    if (ctx->arrayType()) {
        return build_array(ctx->arrayType());
    }
    if (ctx->closedCastType()) {
        // 引用形：递归构建内层
        auto inner = build_closed_cast_type(ctx->closedCastType());
        if (!inner)
            return nullptr;
        return build_ref_type(ctx, ctx->ANDAND(), ctx->MUT() != nullptr,
                              std::move(inner));
    }
    // 剩下的形态：多段路径，最后一段可带泛型实参
    auto node = std::make_shared<PathType>();
    node->span = span_of(ctx);
    for (auto *seg : ctx->typePathSegment()) {
        node->segments.push_back(build_type_segment(seg));
    }
    node->segments.push_back(
        build_segment_core(ctx->pathIdentSegment(), ctx->genericArgs()));
    return node;
}

// ══════ 5. 路径与纯值：表达式/类型两层的共用材料 ══════
// 路径段/泛型实参、常量值、引用构造、self 参数、一元与后缀构造。

// 处理路径的一段：名字加可选泛型实参。
// 例：`a::b::c` 的段 `b` → PathSegment{name="b"}；`s::<i32>` → {name="s", generics=[i32]}。
PathSegment AstBuilder::build_segment(RxParser::PathExprSegmentContext *ctx) {
    return build_segment_core(ctx->pathIdentSegment(), ctx->genericArgs());
}

// 造路径段（表达式段和类型段共用的核心）：取名字，泛型实参有就收。
// 例：ident="Vec"、generics=[i32] → PathSegment{name="Vec", generics=[i32]}。
PathSegment
AstBuilder::build_segment_core(RxParser::PathIdentSegmentContext *ident,
                               RxParser::GenericArgsContext *generics) {
    PathSegment seg;
    seg.name = ident->getText();
    if (generics) {
        seg.generics = build_generic_types(generics);
    }
    return seg;
}

// 处理类型路径的一段（跟表达式段结构相同，只是文法上 `::` 可省）。
// 例：`Vec<Box<i32>>` 的段 `Vec` → PathSegment{name="Vec", generics=[Box<i32>]}。
PathSegment
AstBuilder::build_type_segment(RxParser::TypePathSegmentContext *ctx) {
    // 与表达式段同构，只是 `::` 可省
    return build_segment_core(ctx->pathIdentSegment(), ctx->genericArgs());
}

// 收集泛型实参里的类型；lifetime 直接丢弃，某个实参失败就跳过、继续收剩下的。
// 例：`<i32, 'a, bool>` → [i32, bool]（'a 丢弃）。
std::vector<std::shared_ptr<Type>>
AstBuilder::build_generic_types(RxParser::GenericArgsContext *ctx) {
    std::vector<std::shared_ptr<Type>> out;
    out.reserve(ctx->genericArg().size());
    // 遍历泛型实参：只收类型，lifetime 丢弃
    for (auto *arg : ctx->genericArg()) {
        if (!arg->typeRef())
            continue;
        auto t = build_type_ref(arg->typeRef());
        if (!t)
            continue; // 失败的实参跳过，继续收其余
        out.push_back(std::move(t));
    }
    return out;
}

// 造引用类型节点（引用类型和 as 收口类型共用）：& 和 &mut 一层；&& 拆两层，mut 归内层，内层 span 从 && 的第二个字符起。
// 例：`&T` → 一层 ReferenceType；`&&T` → 两层，外层非 mut。
std::shared_ptr<Type>
AstBuilder::build_ref_type(antlr4::ParserRuleContext *whole,
                           antlr4::tree::TerminalNode *andand, bool is_mut,
                           std::shared_ptr<Type> inner) {
    auto node = std::make_shared<ReferenceType>();
    node->is_mut = is_mut;
    node->inner = std::move(inner);
    // 若是 `&&` 则拆两层：mut 归内层，内层 span 从第二个 & 起才盖得住
    // `&mut`，外层盖整体且不可变
    if (andand) {
        const Span whole_span = span_of(whole);
        const Position start{
            static_cast<int>(andand->getSymbol()->getLine()),
            static_cast<int>(andand->getSymbol()->getCharPositionInLine()) + 1};
        node->span = Span{start, whole_span.end};
        auto outer = std::make_shared<ReferenceType>();
        outer->span = whole_span;
        outer->is_mut = false;
        outer->inner = std::move(node);
        return outer;
    }
    node->span = span_of(whole);
    return node;
}

// 解析常量值（数组长度、const 项的值）：整数、布尔、路径、负数、括号。
// 例：`0x10` → 整数 16；`N` → 路径 N；`-3` → 负数(3)；`(2)` → 解包出 2。
ConstValue AstBuilder::build_const(RxParser::ConstValueContext *ctx) {
    if (ctx->INTEGER_LITERAL()) {
        ConstValue v;
        v.kind = ConstValue::Kind::Int;
        const auto parsed =
            parse_int_literal(ctx->INTEGER_LITERAL()->getText());
        v.int_value = parsed.value;
        v.suffix = parsed.suffix;
        v.span = span_of(ctx);
        return v;
    }
    if (ctx->TRUE() || ctx->FALSE()) {
        ConstValue v;
        v.kind = ConstValue::Kind::Bool;
        v.bool_value = ctx->TRUE() != nullptr;
        v.span = span_of(ctx);
        return v;
    }
    if (ctx->pathInExpression()) {
        ConstValue v;
        v.kind = ConstValue::Kind::Path;
        for (auto *seg : ctx->pathInExpression()->pathExprSegment()) {
            v.path.push_back(build_segment(seg));
        }
        v.span = span_of(ctx);
        return v;
    }
    if (ctx->MINUS()) {
        ConstValue v;
        v.kind = ConstValue::Kind::Neg;
        v.operand =
            std::make_shared<ConstValue>(build_magnitude(ctx->magnitude()));
        v.span = span_of(ctx);
        return v;
    }
    return build_const(ctx->constValue()); // 括号解包
}

// 解析负数常量的绝对值部分（`-3` 里的 `3`）。
// 例：`-3` → 整数 3；`(4)` → 解包出 4；`-N` → 路径 N。
ConstValue AstBuilder::build_magnitude(RxParser::MagnitudeContext *ctx) {
    if (ctx->INTEGER_LITERAL()) {
        ConstValue v;
        v.kind = ConstValue::Kind::Int;
        const auto parsed =
            parse_int_literal(ctx->INTEGER_LITERAL()->getText());
        v.int_value = parsed.value;
        v.suffix = parsed.suffix;
        v.span = span_of(ctx);
        return v;
    }
    if (ctx->pathInExpression()) {
        ConstValue v;
        v.kind = ConstValue::Kind::Path;
        for (auto *seg : ctx->pathInExpression()->pathExprSegment()) {
            v.path.push_back(build_segment(seg));
        }
        v.span = span_of(ctx);
        return v;
    }
    return build_magnitude(ctx->magnitude()); // 括号解包
}

// 处理方法的 self 参数，四种写法（lifetime 忽略）。
// 例：`self` → 值；`mut self` → 可变值；`&self` → 共享引用；`&mut self` → 可变引用。
std::shared_ptr<SelfParam>
AstBuilder::build_self_param(RxParser::SelfParamContext *ctx) {
    auto node = std::make_shared<SelfParam>();
    if (ctx->AMP()) {
        node->kind =
            ctx->MUT() ? SelfParam::Kind::RefMut : SelfParam::Kind::Ref;
    } else {
        node->kind =
            ctx->MUT() ? SelfParam::Kind::MutValue : SelfParam::Kind::Value;
    }
    return node;
}

// 造一元运算节点：- ! * 各一层；& 和 &mut 一层；&& 拆成两层借用，mut 归内层。
// 例：`-x` → Unary{Neg, x}；`&&mut x` → Borrow{BorrowMut{x}}。
std::shared_ptr<Expression>
AstBuilder::make_unary(RxParser::UnaryOperatorContext *op_ctx,
                       std::shared_ptr<Expression> operand) {
    if (!operand)
        return nullptr;
    auto make = [&](UnOp op, Span span, std::shared_ptr<Expression> inner) {
        auto node = std::make_shared<UnaryExpression>();
        node->op = op;
        node->span = std::move(span);
        node->operand = std::move(inner);
        return node;
    };
    const Span whole{span_of(op_ctx).start, operand->span.end};
    if (op_ctx->MINUS())
        return make(UnOp::Neg, whole, std::move(operand));
    if (op_ctx->NOT())
        return make(UnOp::Not, whole, std::move(operand));
    if (op_ctx->STAR())
        return make(UnOp::Deref, whole, std::move(operand));
    // 若是 `&&` 则拆两层借用：mut 归内层，内层 span 从第二个 & 起
    if (op_ctx->ANDAND()) {
        auto *tok = op_ctx->ANDAND()->getSymbol();
        const Position inner_start{
            static_cast<int>(tok->getLine()),
            static_cast<int>(tok->getCharPositionInLine()) + 1};
        auto inner = make(op_ctx->MUT() ? UnOp::BorrowMut : UnOp::Borrow,
                          Span{inner_start, whole.end}, std::move(operand));
        return make(UnOp::Borrow, whole, std::move(inner));
    }
    return make(op_ctx->MUT() ? UnOp::BorrowMut : UnOp::Borrow, whole,
                std::move(operand));
}

// 给表达式套一个后缀：调用、下标、或点后缀。
// 例：`f` + `(a)` → Call{callee=f, args=[a]}；`a` + `[0]` → Index{base=a, index=0}。
std::shared_ptr<Expression>
AstBuilder::apply_suffix(std::shared_ptr<Expression> e,
                         RxParser::PostfixSuffixContext *ctx) {
    if (!e)
        return nullptr;
    const Span whole{e->span.start, span_of(ctx).end};
    if (auto *call = ctx->callArguments()) {
        auto node = std::make_shared<CallExpression>();
        node->span = whole;
        node->callee = std::move(e);
        // 遍历实参逐个构建收进 args
        for (auto *arg : call->expression()) {
            auto a = dispatch(arg);
            if (!a)
                return nullptr;
            node->args.push_back(std::move(a));
        }
        return node;
    }
    if (ctx->LBRACKET()) {
        auto idx = dispatch(ctx->expression());
        if (!idx)
            return nullptr;
        auto node = std::make_shared<IndexExpression>();
        node->span = whole;
        node->base = std::move(e);
        node->index = std::move(idx);
        return node;
    }
    return apply_dot_suffix(std::move(e), ctx->dotSuffix());
}

// 给表达式套一个点后缀：取字段或方法调用（方法名上可带 turbofish 泛型）。
// 例：`s` + `.f` → Field{base=s, field=f}；`v` + `.push::<i32>(1)` → MethodCall{receiver=v, method=push, args=[1]}。
std::shared_ptr<Expression>
AstBuilder::apply_dot_suffix(std::shared_ptr<Expression> e,
                             RxParser::DotSuffixContext *dot) {
    if (!e)
        return nullptr;
    const Span whole{e->span.start, span_of(dot).end};
    // 若带路径段则是方法调用（turbofish 泛型记在段上）
    if (dot->pathExprSegment()) {
        auto node = std::make_shared<MethodCallExpression>();
        node->span = whole;
        node->receiver = std::move(e);
        node->method = build_segment(dot->pathExprSegment());
        for (auto *arg : dot->callArguments()->expression()) {
            auto a = dispatch(arg);
            if (!a)
                return nullptr;
            node->args.push_back(std::move(a));
        }
        return node;
    }
    auto node = std::make_shared<FieldExpression>();
    node->span = whole;
    node->base = std::move(e);
    node->field = dot->identifier()->getText();
    return node;
}

// 造 loop 循环节点：只有循环体。
// 例：`loop {…}` → LoopExpression{body}。
std::shared_ptr<Expression>
AstBuilder::build_loop(antlr4::ParserRuleContext *whole,
                       RxParser::BlockExpressionContext *body_ctx) {
    auto body = build_block(body_ctx);
    if (!body)
        return nullptr;
    auto node = std::make_shared<LoopExpression>();
    node->span = span_of(whole);
    node->body = std::move(body);
    return node;
}

// 造 while 循环节点：条件加循环体。
// 例：`while c {…}` → WhileExpression{condition=c, body}。
std::shared_ptr<Expression>
AstBuilder::build_while(antlr4::ParserRuleContext *whole,
                        RxParser::ConditionExpressionContext *cond_ctx,
                        RxParser::BlockExpressionContext *body_ctx) {
    auto cond = dispatch(cond_ctx);
    if (!cond)
        return nullptr;
    auto body = build_block(body_ctx);
    if (!body)
        return nullptr;
    auto node = std::make_shared<WhileExpression>();
    node->span = span_of(whole);
    node->condition = std::move(cond);
    node->body = std::move(body);
    return node;
}

} // namespace rx
