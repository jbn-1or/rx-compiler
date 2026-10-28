#pragma once

#include <memory>

#include "RxParser.h"

#include "ast/ast.hpp"
#include "diagnostics.hpp"

namespace rx {

struct AstBuilder {
public:
    explicit AstBuilder(Diagnostics &diagnostics) : diagnostics_(diagnostics) {}

    // 总入口：从 ANTLR 解析树的根节点开始，递归构建整个 AST。
    std::shared_ptr<Crate> build(RxParser::CrateContext *ctx);

private:
    // 分派：把表达式族规则映射到对应的构建函数。每条规则都必须在这里登记，
    // 没登记的规则会报错（debug 下断言，release 下直接诊断）。
    // 返回空指针表示跳过或已报错。
    std::shared_ptr<Expression> dispatch(antlr4::tree::ParseTree *node);

    // —— 表达式构建工具：各层按文法递归处理对应优先级 ——
    //
    // 二元运算链：把一串平铺的孩子折叠成一棵左结合的运算树，
    // 运算符按白名单逐个识别。
    std::shared_ptr<Expression> fold_chain(antlr4::ParserRuleContext *ctx);

    // 赋值：只有左边时原样转发；有赋值号时递归构建右边（右结合）。
    std::shared_ptr<Expression>
    build_assignment_layer(antlr4::ParserRuleContext *ctx);

    // 类型转换：处理 `as`，从左往右折叠；没有 as 时原样转发。
    std::shared_ptr<Expression>
    build_cast_layer(antlr4::ParserRuleContext *ctx);

    // 一元运算：负号、取反、解引用、借用；不是一元运算时转发给后缀层。
    std::shared_ptr<Expression>
    build_unary_layer(antlr4::ParserRuleContext *ctx);

    // 后缀运算：先构建基础表达式，再把方法调用、字段访问、索引
    // 一个个挂上去。
    std::shared_ptr<Expression>
    build_postfix_layer(antlr4::ParserRuleContext *ctx);

    // 最内层表达式：字面量、数组、if、块等，各自按语法形状构建。
    std::shared_ptr<Expression>
    build_non_block_primary(RxParser::NonBlockPrimaryContext *ctx);
    std::shared_ptr<Expression>
    build_literal(RxParser::LiteralExpressionContext *ctx);
    std::shared_ptr<Expression>
    build_array_expression(RxParser::ArrayExpressionContext *ctx);
    std::shared_ptr<Expression>
    build_expression_with_block(RxParser::ExpressionWithBlockContext *ctx);
    std::shared_ptr<Expression> build_if(RxParser::IfExpressionContext *ctx);
    std::shared_ptr<Expression>
    build_condition_primary(RxParser::ConditionPrimaryContext *ctx);
    std::shared_ptr<Expression> build_condition_primary_without_bare_block(
        RxParser::ConditionPrimaryWithoutBareBlockContext *ctx);

    // —— 语句层：块、普通语句、let 语句 ——
    std::shared_ptr<BlockExpression>
    build_block(RxParser::BlockExpressionContext *ctx);
    std::shared_ptr<Statement> build_statement(RxParser::StatementContext *ctx);
    std::shared_ptr<Statement> build_let(RxParser::LetStatementContext *ctx);

    // —— 类型层：路径类型、引用类型、数组类型等 ——
    std::shared_ptr<Type> build_type_reference(RxParser::TypeRefContext *ctx);
    std::shared_ptr<Type> build_type_path(RxParser::TypePathContext *ctx);
    std::shared_ptr<Type>
    build_reference_type(RxParser::ReferenceTypeContext *ctx);
    std::shared_ptr<Type> build_array(RxParser::ArrayTypeContext *ctx);
    std::shared_ptr<Type>
    build_closed_cast_type(RxParser::ClosedCastTypeContext *ctx);

    // —— 顶层定义层：crate、函数、结构体、常量、impl ——
    std::shared_ptr<Crate> build_crate(RxParser::CrateContext *ctx);
    std::shared_ptr<Item> build_item(RxParser::ItemContext *ctx);
    std::shared_ptr<Item>
    build_function_definition(RxParser::FunctionDefinitionContext *ctx);
    std::shared_ptr<Item>
    build_struct_definition(RxParser::StructDefinitionContext *ctx);
    std::shared_ptr<Item>
    build_constant_item(RxParser::ConstantItemContext *ctx);
    std::shared_ptr<Item>
    build_inherent_impl(RxParser::InherentImplContext *ctx);

    // 构建一个路径段：名字 + 泛型实参，不经过 dispatch。
    PathSegment build_segment(RxParser::PathExprSegmentContext *ctx);

    // 构建路径表达式，返回具体的 PathExpression，方便别处直接用。
    std::shared_ptr<PathExpression>
    build_path(RxParser::PathInExpressionContext *ctx);

    // 路径段的共用核心：表达式段和类型段长得一样，都是名字 + 泛型；
    // 泛型里只收类型，lifetime 一律丢弃。
    PathSegment build_segment_core(RxParser::PathIdentSegmentContext *ident,
                                   RxParser::GenericArgsContext *generics);
    PathSegment build_type_segment(RxParser::TypePathSegmentContext *ctx);

    // 收集泛型实参，只收类型，构建失败的跳过（错误已就地报出）。
    std::vector<std::shared_ptr<Type>>
    build_generic_types(RxParser::GenericArgsContext *ctx);

    // 构建引用类型：`&&` 算两层引用，`&` 算一层。
    std::shared_ptr<Type>
    make_reference_type(antlr4::ParserRuleContext *whole,
                        antlr4::tree::TerminalNode *andand, bool is_mut,
                        std::shared_ptr<Type> inner);

    // 解析常量值（数组长度 `[x; N]` 和 const 项的初始化器共用）。
    ConstantValue build_constant_value(RxParser::ConstValueContext *ctx);
    ConstantValue build_magnitude(RxParser::MagnitudeContext *ctx);

    // 构建 self 参数的四种形态：self / mut self / &self / &mut self。
    std::shared_ptr<SelfParameter>
    build_self_parameter(RxParser::SelfParamContext *ctx);

    // —— 一元与后缀的共用构件 ——
    //
    // 造一层一元运算：MINUS/NOT/STAR/AMP 各一层；`&&` 算两层借用。
    std::shared_ptr<Expression>
    make_unary(RxParser::UnaryOperatorContext *op_ctx,
               std::shared_ptr<Expression> operand);

    // 给表达式挂一个后缀：函数调用、索引、方法调用或字段访问。
    std::shared_ptr<Expression>
    apply_suffix(std::shared_ptr<Expression> e,
                 RxParser::PostfixSuffixContext *ctx);

    // 点后缀（方法调用 / 字段访问）的共用核心。
    std::shared_ptr<Expression>
    apply_dot_suffix(std::shared_ptr<Expression> e,
                     RxParser::DotSuffixContext *ctx);

    // 分别构建 loop 与 while；循环体复用 build_block，while 条件走 dispatch。
    std::shared_ptr<Expression>
    build_loop(antlr4::ParserRuleContext *whole,
               RxParser::BlockExpressionContext *body_ctx);
    std::shared_ptr<Expression>
    build_while(antlr4::ParserRuleContext *whole,
                RxParser::ConditionExpressionContext *cond_ctx,
                RxParser::BlockExpressionContext *body_ctx);

    Diagnostics &diagnostics_;
};

} // namespace rx
