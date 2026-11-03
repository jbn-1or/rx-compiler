#include "RxLexer.h"
#include "RxParser.h"
#include "ast_builder.hpp"
#include "sema/body/calls/call.hpp"
#include "sema/body/checker.hpp"
#include "sema/body/context.hpp"
#include "sema/sema.hpp"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {
void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::shared_ptr<rx::Crate> parse_crate(const std::string &source) {
    antlr4::ANTLRInputStream input(source);
    RxLexer lexer(&input);
    antlr4::CommonTokenStream tokens(&lexer);
    RxParser parser(&tokens);
    auto *tree = parser.crate();
    check(lexer.getNumberOfSyntaxErrors() == 0 &&
              parser.getNumberOfSyntaxErrors() == 0,
          "semantic result fixture must parse");
    rx::Diagnostics diagnostics;
    rx::AstBuilder builder(diagnostics);
    auto crate = builder.build(tree);
    check(crate && diagnostics.ok(), "semantic result fixture must build an AST");
    return crate;
}

void check_semantic_result_ownership() {
    static_assert(!std::is_copy_constructible_v<rx::SemanticResult>);
    static_assert(!std::is_copy_assignable_v<rx::SemanticResult>);
    static_assert(std::is_nothrow_move_constructible_v<rx::SemanticResult>);
    static_assert(std::is_nothrow_move_assignable_v<rx::SemanticResult>);

    const auto crate = parse_crate(R"(
        const LEN: usize = 2;
        const LIMIT: i32 = 42;
        struct Node { next: Box<Node>, data: [i32; LEN] }
        impl Node {
            const LIMIT: i32 = 7;
            const COPY: i32 = Self::LIMIT;
            const ENABLED: bool = true;
            fn echo(&self, value: i32) -> &Self { self }
        }
        fn identity(value: Node) -> Node { value }
        fn main() {}
    )");
    rx::Diagnostics diagnostics;
    // Both the analyzer and its name lookup tables die before any result query.
    auto result = rx::SemanticAnalyzer(diagnostics).analyze(*crate);
    check(result && diagnostics.ok(), "valid crate must produce a result");
    check(result->structs.size() == 1 && result->functions.size() == 3 &&
              result->constants.size() == 5 && result->builtins.size() == 3,
          "result must own all global and associated symbols, including builtins");
    const auto *node = result->structs[0].get();
    const auto *echo = static_cast<const rx::FunctionSymbol *>(
        node->associated_items.at("echo"));
    const auto *copy = static_cast<const rx::ConstantSymbol *>(
        node->associated_items.at("COPY"));
    const auto *builtin = result->builtins[2].get();

    rx::SemanticResult moved(std::move(*result));
    result.reset();
    rx::SemanticResult owned;
    owned = std::move(moved);
    check(owned.structs[0].get() == node && owned.builtins[2].get() == builtin,
          "moving a result must preserve symbol addresses");
    check(node->node && node->node->name == "Node" &&
              node->fields[0].type == node->node->fields[0].type.get(),
          "borrowed AST pointers must remain usable while the crate lives");
    const auto &next = node->field_types.at("next");
    check(next.kind == rx::SemanticType::Kind::Box &&
              next.inner->struct_symbol == node && next.toString() == "Box<Node>",
          "recursive field types must refer to the owned struct after transfer");
    const auto &data = node->field_types.at("data");
    const auto i32 = rx::SemanticType::int_(rx::SemanticType::IntKind::I32);
    check(data.kind == rx::SemanticType::Kind::Array && data.length == 2 &&
              data.inner->same_type(i32),
          "resolved field types and constant array lengths must survive");
    check(echo->owner == node && echo->node->name == "echo" &&
              echo->parameters.size() == 1 && echo->parameters[0].type.same_type(i32) &&
              echo->receiver && echo->receiver->type.inner->struct_symbol == node &&
              echo->return_type.inner->struct_symbol == node,
          "method signatures, receiver and owner pointers must survive");
    const auto &identity = *owned.functions[1];
    check(identity.name == "identity" && identity.owner == nullptr &&
              identity.parameters[0].type.struct_symbol == node &&
              identity.return_type.struct_symbol == node,
          "ordinary function signatures must retain nominal type identity");
    check(copy->owner == node && copy->evaluated.ok &&
              copy->state == rx::ConstantEvaluationState::Visited &&
              copy->evaluated.type.same_type(i32) &&
              std::get<int64_t>(copy->evaluated.value) == 7,
          "associated constant caches and owner pointers must survive");
    check(owned.constants[1]->evaluated.ok &&
              std::get<int64_t>(owned.constants[1]->evaluated.value) == 42,
          "top-level constant caches must survive");
    const auto *enabled = static_cast<const rx::ConstantSymbol *>(
        node->associated_items.at("ENABLED"));
    check(enabled->evaluated.ok && std::get<bool>(enabled->evaluated.value),
          "boolean constant caches must survive");
    check(builtin->kind == rx::Symbol::Kind::Builtin &&
              builtin->name == "println_i32",
          "builtin symbols must remain usable after analyzer destruction");
}

void check_failed_semantic_result() {
    for (const char *source : {
             "struct S {}", // Missing main.
             "const BAD: i32 = true; fn main() {}",
             "struct S { value: Missing } fn main() {}",
             "fn main() { let value: bool = 1; }"}) {
        const auto crate = parse_crate(source);
        rx::Diagnostics diagnostics;
        const auto result = rx::SemanticAnalyzer(diagnostics).analyze(*crate);
        check(!diagnostics.ok() && !result,
              "semantic diagnostics must prevent delivery of incomplete results");
    }
}

struct UnknownExpression : rx::Expression {
    void accept(rx::AstVisitor &) override {}
};
struct UnknownStatement : rx::Statement {
    void accept(rx::AstVisitor &) override {}
};
struct UnknownType : rx::Type {
    std::string name() const override { return "unknown"; }
};

void check_internal_error(int kind) {
    const rx::Span at{{1, 0}, {1, 1}};
    auto function = std::make_shared<rx::Function>();
    function->span = at;
    function->name = "main";
    function->return_type = std::make_shared<rx::UnitType>();
    function->body = std::make_shared<rx::BlockExpression>();
    function->body->span = at;
    if (kind == 0) {
        auto expression = std::make_shared<UnknownExpression>();
        expression->span = at;
        function->body->tail = expression;
    } else if (kind == 1) {
        auto statement = std::make_shared<UnknownStatement>();
        statement->span = at;
        function->body->statements.push_back(statement);
    } else {
        auto type = std::make_shared<UnknownType>();
        type->span = at;
        function->return_type = type;
    }
    rx::Crate crate;
    crate.items.push_back(function);
    rx::Diagnostics diagnostics;
    rx::SemanticAnalyzer analyzer(diagnostics);
    const auto result = analyzer.analyze(crate);
    check(!result, "internal AST error must not produce a semantic result");
    check(diagnostics.items.size() == 1, "internal AST error must not cascade");
    check(diagnostics.items[0].message.starts_with("internal error:"),
          "unknown AST node must be diagnosed");
}

void check_body_checker_reuse() {
    const rx::Span at{{1, 0}, {1, 1}};
    const auto i32 = rx::SemanticType::int_(rx::SemanticType::IntKind::I32);
    rx::Diagnostics diagnostics;
    rx::SemanticAnalyzer analyzer(diagnostics);
    rx::BodyChecker checker(analyzer);
    rx::StructSymbol owner;
    owner.name = "S";

    rx::Function first;
    first.span = at;
    first.name = "first";
    first.return_type = std::make_shared<rx::UnitType>();
    first.return_type->span = at;
    first.self_parameter = std::make_shared<rx::SelfParameter>();
    first.self_parameter->kind = rx::SelfParameter::Kind::Reference;
    first.body = std::make_shared<rx::BlockExpression>();
    first.body->span = at;
    auto parameter_type = std::make_shared<rx::PathType>();
    parameter_type->span = at;
    parameter_type->segments.push_back({"i32", {}});
    first.parameters.push_back({"x", false, parameter_type});
    rx::FunctionSymbol first_symbol;
    first_symbol.node = &first;
    first_symbol.owner = &owner;
    first_symbol.parameters.push_back({i32, false});
    first_symbol.return_type = rx::SemanticType::unit();
    first_symbol.receiver = rx::ResolvedReceiver{
        rx::SemanticType::reference(false, rx::SemanticType::struct_(&owner)), false};
    checker.check_function(first_symbol);
    check(diagnostics.ok(), "first body must be valid");

    rx::Function second;
    second.span = at;
    second.name = "second";
    second.return_type = std::make_shared<rx::UnitType>();
    second.return_type->span = at;
    second.body = std::make_shared<rx::BlockExpression>();
    second.body->span = at;
    for (const char *name : {"x", "self", "Self"}) {
        auto path = std::make_shared<rx::PathExpression>();
        path->span = at;
        path->segments.push_back({name, {}});
        auto statement = std::make_shared<rx::ExpressionStatement>();
        statement->span = at;
        statement->expression = path;
        second.body->statements.push_back(statement);
    }
    rx::FunctionSymbol second_symbol;
    second_symbol.node = &second;
    second_symbol.return_type = rx::SemanticType::unit();
    checker.check_function(second_symbol);
    check(diagnostics.items.size() == 3,
          "reusing a checker must clear locals, owner, and receiver context");
    check(diagnostics.items[0].message == "unresolved value name: x" &&
              diagnostics.items[1].message == "self is not available outside a method" &&
              diagnostics.items[2].message == "Self is not available outside a struct or impl",
          "the second body must not inherit the first body's context");
}
} // namespace

int main() {
    using T = rx::SemanticType;
    const auto i32 = T::int_(T::IntKind::I32);
    const auto error = T::error();
    check(error.same_type(error), "Error must equal itself");
    check(!error.same_type(i32) && !i32.same_type(error),
          "strict equality must not perform error recovery");
    check(!T::reference(false, error).same_type(T::reference(false, i32)),
          "nested Error must not match a concrete type");
    check(!T::reference(true, i32).same_type(T::reference(false, i32)),
          "reference mutability is part of type identity");
    rx::StructSymbol a, b;
    a.name = "A";
    b.name = "B";
    check(!T::struct_(&a).same_type(T::struct_(&b)),
          "struct type identity must remain nominal");
    const auto nested = T::vec(T::boxed(T::reference(true, T::struct_(&a))));
    check(nested.toString() == "Vec<Box<&mut A>>", "nested struct names must survive formatting");
    check(T::array(T::struct_(&b), 2).toString() == "[B; 2]", "array names must show their element");
    const auto vector = T::vec(i32);
    const auto push = rx::builtin_method_signature(vector, "push");
    check(push && push->receiver && push->parameters.size() == 1,
          "dot signature must separate receiver from ordinary arguments");
    const auto explicit_parameters = push->explicit_parameters();
    check(explicit_parameters.size() == 2 &&
              explicit_parameters[0].same_type(*push->receiver) &&
              explicit_parameters[1].same_type(push->parameters[0]),
          "explicit and dot calls must share the same signature");
    rx::FunctionSymbol function;
    function.receiver = rx::ResolvedReceiver{T::reference(true, T::struct_(&a)), false};
    function.parameters.push_back({i32, false});
    function.return_type = T::unit();
    const auto signature = rx::function_signature(function);
    check(signature.receiver->same_type(function.receiver->type) &&
              signature.explicit_parameters().size() == 2,
          "user method signatures must use the resolved receiver");
    const auto value = rx::ExpressionResult::value(i32);
    check(!value.place.is_place, "value factory must not grant place permissions");
    const auto place = rx::ExpressionResult::from_place(i32, {true, false, true});
    check(place.place.crossed_shared && !place.place.is_mutable,
          "place factory must preserve shared access restrictions");
    for (int kind = 0; kind != 3; ++kind)
        check_internal_error(kind);
    check_body_checker_reuse();
    check_semantic_result_ownership();
    check_failed_semantic_result();
    std::cout << "semantic invariants: passed\n";
}
