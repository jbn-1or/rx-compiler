#include "RxLexer.h"
#include "RxParser.h"
#include "ast_builder.hpp"
#include "sema/body/calls/call.hpp"
#include "sema/body/checker.hpp"
#include "sema/body/context.hpp"
#include "sema/sema.hpp"
#include <array>
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

template <class T, class U> const T &checked_ast(const U *node) {
    const auto *typed = dynamic_cast<const T *>(node);
    check(typed != nullptr, "fixture AST node must have the expected kind");
    return *typed;
}

template <class T, class U>
const T &checked_ast(const std::shared_ptr<U> &node) {
    return checked_ast<T>(node.get());
}

rx::SemanticResult analyze_fixture(const rx::Crate &crate) {
    rx::Diagnostics diagnostics;
    auto result = rx::SemanticAnalyzer(diagnostics).analyze(crate);
    for (const auto &diagnostic : diagnostics.items)
        std::cerr << diagnostic.message << '\n';
    check(result && diagnostics.ok(), "query fixture must be semantically valid");
    return std::move(*result);
}

const rx::Expression &statement_expression(const rx::BlockExpression &block,
                                          std::size_t index) {
    return *checked_ast<rx::ExpressionStatement>(block.statements.at(index)).expression;
}

void check_binding_and_call_queries() {
    check(rx::StructId{} != rx::StructId{0} &&
              rx::FunctionId{} != rx::FunctionId{0} &&
              rx::ConstantId{} != rx::ConstantId{0} &&
              rx::LocalId{} != rx::LocalId{0},
          "default semantic IDs must not alias slot zero");
    const auto crate = parse_crate(R"(
        fn main() {
            let x = 1;
            let x = 2;
            println_i32(x);
            let x = x;
            { let x = x; println_i32(x); };
            println_i32(x);
            forward(x);
            print_i32(get_i32());
        }
        fn forward(mut x: i32) -> i32 { x = 3; let x = x; x }
        fn call_main() { main(); }
    )");
    auto original = analyze_fixture(*crate);
    rx::SemanticResult moved(std::move(original));
    rx::SemanticResult result;
    result = std::move(moved);
    const auto &main = checked_ast<rx::Function>(crate->items.at(0));
    const auto &forward = checked_ast<rx::Function>(crate->items.at(1));
    const auto &call_main = checked_ast<rx::Function>(crate->items.at(2));
    const auto main_id = result.function_declarations.at(&main);
    const auto forward_id = result.function_declarations.at(&forward);
    check(result.main_function == main_id && main_id != forward_id,
          "entry identity and forward declaration must survive result moves");
    const auto &body = *main.body;
    const auto &first = checked_ast<rx::LetStatement>(body.statements.at(0));
    const auto &second = checked_ast<rx::LetStatement>(body.statements.at(1));
    const auto &third = checked_ast<rx::LetStatement>(body.statements.at(3));
    const auto &nested = checked_ast<rx::BlockExpression>(
        &statement_expression(body, 4));
    const auto &inner = checked_ast<rx::LetStatement>(nested.statements.at(0));
    const auto first_id = result.let_bindings.at(&first);
    const auto second_id = result.let_bindings.at(&second);
    const auto third_id = result.let_bindings.at(&third);
    const auto inner_id = result.let_bindings.at(&inner);
    const std::array ids{first_id, second_id, third_id, inner_id};
    for (std::size_t i = 0; i < ids.size(); ++i)
        for (std::size_t j = i + 1; j < ids.size(); ++j)
            check(ids.at(i) != ids.at(j), "every shadowing let needs its own ID");
    const auto local_target = [&](const std::shared_ptr<rx::Expression> &expression) {
        return std::get<rx::LocalId>(
            result.paths.at(&checked_ast<rx::PathExpression>(expression)));
    };
    check(local_target(third.initializer) == second_id &&
              local_target(inner.initializer) == third_id,
          "shadowing initializers must read the previously visible binding");
    const auto &initial_print = checked_ast<rx::CallExpression>(
        &statement_expression(body, 2));
    const auto &inner_print = checked_ast<rx::CallExpression>(
        &statement_expression(nested, 1));
    const auto &restored_print = checked_ast<rx::CallExpression>(
        &statement_expression(body, 5));
    check(local_target(initial_print.arguments.at(0)) == second_id &&
              local_target(inner_print.arguments.at(0)) == inner_id &&
              local_target(restored_print.arguments.at(0)) == third_id,
          "paths must select the newest binding and restore it after a block");
    const auto &parameters = result.function_locals.at(forward_id);
    check(!parameters.self && parameters.parameters.size() == 1 &&
              result.function_locals.at(main_id).parameters.empty() &&
              !result.function_locals.at(main_id).self,
          "function local tables must distinguish ordinary parameters from self");
    const auto parameter_id = parameters.parameters.at(0);
    const auto &shadow = checked_ast<rx::LetStatement>(forward.body->statements.at(1));
    const auto shadow_id = result.let_bindings.at(&shadow);
    const auto &assignment = checked_ast<rx::AssignmentExpression>(
        &statement_expression(*forward.body, 0));
    const auto i32 = rx::SemanticType::int_(rx::SemanticType::IntKind::I32);
    check(local_target(assignment.left) == parameter_id &&
              local_target(shadow.initializer) == parameter_id &&
              local_target(forward.body->tail) == shadow_id && parameter_id != shadow_id,
          "let must shadow a parameter only after checking its initializer");
    check(result.locals.at(parameter_id.index).is_mut &&
              result.locals.at(parameter_id.index).type.same_type(i32) &&
              !result.locals.at(shadow_id.index).is_mut &&
              result.locals.at(shadow_id.index).type.same_type(i32),
          "parameter mutability and fixed type must not change when shadowed");
    for (const auto id : ids)
        check(id != parameter_id && id != shadow_id,
              "LocalIds must not restart in another function");
    check(result.locals.size() == 6 && result.let_bindings.size() == 5 &&
              result.function_locals.size() == 3,
          "recording facts must not check bodies or allocate locals twice");
    for (std::size_t i = 0; i < result.locals.size(); ++i)
        check(result.locals.at(i).id == rx::LocalId{i},
              "local IDs must index their owning binding table");
    check(result.integer_literals.at(&checked_ast<rx::IntegerLiteral>(first.initializer)).bits == 1 &&
              result.integer_literals.at(&checked_ast<rx::IntegerLiteral>(second.initializer)).bits == 2 &&
              result.expressions.at(main.body.get()).type.same_type(rx::SemanticType::unit()),
          "literal and function body facts must remain queryable after both kinds of move");
    const auto &forward_call = checked_ast<rx::CallExpression>(
        &statement_expression(body, 6));
    const auto &main_call = checked_ast<rx::CallExpression>(
        &statement_expression(*call_main.body, 0));
    for (const auto &[call, target] :
         std::array{std::pair{&forward_call, forward_id}, std::pair{&main_call, main_id}}) {
        const auto &callee = checked_ast<rx::PathExpression>(call->callee);
        check(std::get<rx::FunctionId>(result.ordinary_calls.at(call)) == target &&
                  std::get<rx::FunctionId>(result.paths.at(&callee)) == target &&
                  !result.expressions.contains(&callee),
              "ordinary callees need FunctionIds, not value expression facts");
    }
    const auto &print = checked_ast<rx::CallExpression>(
        &statement_expression(body, 7));
    const auto &get = checked_ast<rx::CallExpression>(print.arguments.at(0));
    for (const auto &[call, target] : std::array{
             std::pair{&get, rx::BuiltinIO::GetI32},
             std::pair{&print, rx::BuiltinIO::PrintI32},
             std::pair{&initial_print, rx::BuiltinIO::PrintlnI32}}) {
        const auto &callee = checked_ast<rx::PathExpression>(call->callee);
        check(std::get<rx::BuiltinIO>(result.ordinary_calls.at(call)) == target &&
                  !result.paths.contains(&callee) && !result.expressions.contains(&callee),
              "each builtin call needs its distinct I/O target, not a value callee");
    }
    check(result.builtin_signatures.size() == 3, "all I/O signatures must be retained");
    for (const auto &builtin : result.builtins) {
        check(builtin->builtin_io.has_value(), "builtin symbols need explicit I/O identity");
        const auto target = *builtin->builtin_io;
        const auto &signature = result.builtin_signatures.at(target);
        const bool input = target == rx::BuiltinIO::GetI32;
        check(!signature.receiver && signature.parameters.size() == (input ? 0u : 1u) &&
                  signature.return_type.same_type(input ? i32 : rx::SemanticType::unit()),
              "I/O signatures must preserve arity, return type and lack of receiver");
        if (!input)
            check(signature.parameters.at(0).same_type(i32), "I/O output takes i32");
        check(builtin->name == (input ? "get_i32" : target == rx::BuiltinIO::PrintI32
                                                      ? "print_i32" : "println_i32"),
              "builtin names must agree with their retained identities");
    }
}

void check_symbol_and_constant_queries() {
    const auto crate = parse_crate(R"(
        const LEN: usize = 2;
        const MIN: i32 = -2147483648;
        const MAX: u32 = 4294967295u32;
        struct Twin { data: [i32; LEN] }
        fn Twin(value: Twin) -> Twin { value }
        impl Twin {
            const K: i32 = 7;
            const COPY: i32 = Self::K;
            fn echo(&self, value: Self) -> &Self { let k = Self::K; self }
            fn make() -> Self { Self { data: [0; LEN] } }
        }
        fn main() {
            let value = Twin { data: [0; LEN] };
            let value = Twin(value);
            let k = Twin::COPY;
            let minimum = MIN;
        }
    )");
    auto result = analyze_fixture(*crate);
    for (std::size_t i = 0; i < result.structs.size(); ++i) {
        const auto &symbol = *result.structs.at(i);
        check(symbol.id == rx::StructId{i} &&
                  result.struct_declarations.at(symbol.node) == symbol.id,
              "struct declarations must map to their owning symbol IDs");
    }
    for (std::size_t i = 0; i < result.functions.size(); ++i) {
        const auto &symbol = *result.functions.at(i);
        check(symbol.id == rx::FunctionId{i} &&
                  result.function_declarations.at(symbol.node) == symbol.id,
              "function declarations, including methods, must map to owning IDs");
    }
    for (std::size_t i = 0; i < result.constants.size(); ++i) {
        const auto &symbol = *result.constants.at(i);
        const auto &cached = result.constant_values.at(&symbol.node->value);
        check(symbol.id == rx::ConstantId{i} &&
                  result.constant_declarations.at(symbol.node) == symbol.id &&
                  cached.ok && cached.type.same_type(symbol.evaluated.type) &&
                  cached.value == symbol.evaluated.value,
              "every global/associated constant initializer needs its evaluated cache");
    }
    const auto &twin = *result.structs.at(0);
    const auto &function = *result.functions.at(0);
    check(twin.name == function.name && function.parameters.at(0).type.struct_symbol == &twin &&
              function.return_type.struct_symbol == &twin,
          "same-named struct and function must retain distinct namespaces");
    const auto &impl = checked_ast<rx::InherentImpl>(crate->items.at(5));
    const auto &k = checked_ast<rx::Const>(impl.items.at(0));
    const auto &copy = checked_ast<rx::Const>(impl.items.at(1));
    const auto &echo = checked_ast<rx::Function>(impl.items.at(2));
    const auto &make = checked_ast<rx::Function>(impl.items.at(3));
    const auto echo_id = result.function_declarations.at(&echo);
    const auto &echo_symbol = *result.functions.at(echo_id.index);
    const auto &echo_locals = result.function_locals.at(echo_id);
    check(echo_locals.self && echo_locals.parameters.size() == 1 &&
              echo_symbol.owner == &twin && echo_symbol.receiver &&
              echo_symbol.receiver->type.inner->struct_symbol == &twin &&
              echo_symbol.return_type.inner->struct_symbol == &twin &&
              result.locals.at(echo_locals.parameters.at(0).index).type.struct_symbol == &twin,
          "Self in signatures and parameter bindings must retain nominal struct identity");
    const auto &self = checked_ast<rx::PathExpression>(echo.body->tail);
    check(std::get<rx::LocalId>(result.paths.at(&self)) == *echo_locals.self &&
              result.expressions.at(&self).type.same_type(echo_symbol.receiver->type) &&
              !result.locals.at(echo_locals.self->index).is_mut,
          "self paths must resolve to the method's dedicated receiver binding");
    const auto &local_k = checked_ast<rx::LetStatement>(echo.body->statements.at(0));
    check(std::get<rx::ConstantId>(result.paths.at(
              &checked_ast<rx::PathExpression>(local_k.initializer))) ==
                  result.constant_declarations.at(&k) &&
              result.constant_paths.at(&copy.value) == result.constant_declarations.at(&k),
          "Self associated paths must retain the same constant identity in both AST forms");
    const auto &length = checked_ast<rx::ArrayType>(twin.node->fields.at(0).type).length;
    const auto len_id = result.constant_declarations.at(
        &checked_ast<rx::Const>(crate->items.at(0)));
    check(result.constant_paths.at(&length) == len_id &&
              std::get<uint64_t>(result.constant_values.at(&length).value) == 2,
          "array type lengths must retain resolved paths and evaluated values");
    const auto &main = checked_ast<rx::Function>(crate->items.at(6));
    const auto &first = checked_ast<rx::LetStatement>(main.body->statements.at(0));
    const auto &constructed = checked_ast<rx::StructExpression>(first.initializer);
    const auto &self_constructed = checked_ast<rx::StructExpression>(make.body->tail);
    for (const auto *expression : {&constructed, &self_constructed}) {
        const auto &repeat = checked_ast<rx::ArrayExpression>(expression->fields.at(0).value);
        const auto &count = result.constant_values.at(&repeat.repeat_count);
        check(result.expressions.at(expression).type.struct_symbol == &twin &&
                  !result.expressions.contains(expression->path.get()) &&
                  count.ok && count.type.same_type(
                      rx::SemanticType::int_(rx::SemanticType::IntKind::Usize)) &&
                  std::get<uint64_t>(count.value) == 2 &&
                  result.constant_paths.at(&repeat.repeat_count) == len_id,
              "Self construction and repeat counts need facts, but type paths are not values");
    }
    const auto &call_let = checked_ast<rx::LetStatement>(main.body->statements.at(1));
    const auto &call = checked_ast<rx::CallExpression>(call_let.initializer);
    check(std::get<rx::FunctionId>(result.ordinary_calls.at(&call)) == function.id,
          "a same-named function call must not resolve to its struct");
    const auto &copy_let = checked_ast<rx::LetStatement>(main.body->statements.at(2));
    const auto &copy_path = checked_ast<rx::PathExpression>(copy_let.initializer);
    check(std::get<rx::ConstantId>(result.paths.at(&copy_path)) ==
              result.constant_declarations.at(&copy) &&
              !result.expressions.at(&copy_path).place.is_place,
          "qualified associated constants must resolve as values, not places");
    const auto &minimum = checked_ast<rx::Const>(crate->items.at(1));
    const auto &maximum = checked_ast<rx::Const>(crate->items.at(2));
    const auto &negative = result.constant_values.at(&minimum.value);
    const auto &magnitude = result.constant_values.at(minimum.value.operand.get());
    const auto &unsigned_value = result.constant_values.at(&maximum.value);
    check(negative.ok && std::get<int64_t>(negative.value) == -2147483648LL &&
              magnitude.ok && magnitude.type.same_type(negative.type) &&
              std::get<int64_t>(magnitude.value) == 2147483648LL &&
              unsigned_value.ok && unsigned_value.type.same_type(
                  rx::SemanticType::int_(rx::SemanticType::IntKind::U32)) &&
              std::get<uint64_t>(unsigned_value.value) == 4294967295ULL,
          "negative constant operands and unsigned maxima must have independent typed caches");
    const auto &min_let = checked_ast<rx::LetStatement>(main.body->statements.at(3));
    check(std::get<rx::ConstantId>(result.paths.at(
              &checked_ast<rx::PathExpression>(min_let.initializer))) ==
                  result.constant_declarations.at(&minimum),
          "ordinary constant value paths must retain ConstantIds");
    check(result.locals.size() == 8 && result.let_bindings.size() == 5,
          "symbol and aggregate fact recording must not duplicate local allocation");
}

void check_integer_queries() {
    const auto crate = parse_crate(R"(
        fn main() {
            let default = 1;
            let expected: usize = 2;
            let signed = 3i32;
            let unsigned = 4u32;
            let pointer_signed = 5isize;
            let pointer_unsigned = 6usize;
            let maximum = 4294967295u32;
            let minimum = -2147483648;
            let yes = true;
            let no = false;
        }
    )");
    const auto result = analyze_fixture(*crate);
    const auto &main = checked_ast<rx::Function>(crate->items.at(0));
    using I = rx::SemanticType::IntKind;
    const std::array kinds{I::I32, I::Usize, I::I32, I::U32,
                           I::Isize, I::Usize, I::U32, I::I32};
    const std::array<uint32_t, 8> bits{1, 2, 3, 4, 5, 6, 0xffffffffu, 0x80000000u};
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        const auto &let = checked_ast<rx::LetStatement>(main.body->statements.at(i));
        const auto &literal = i == 7
            ? checked_ast<rx::IntegerLiteral>(
                  checked_ast<rx::UnaryExpression>(let.initializer).operand)
            : checked_ast<rx::IntegerLiteral>(let.initializer);
        const auto &fact = result.integer_literals.at(&literal);
        const auto type = rx::SemanticType::int_(kinds.at(i));
        check(fact.type.same_type(type) && fact.bits == bits.at(i) &&
                  result.expressions.at(&literal).type.same_type(type) &&
                  !result.expressions.at(&literal).place.is_place &&
                  !result.expressions.at(&literal).diverges &&
                  result.locals.at(result.let_bindings.at(&let).index).type.same_type(type),
              "integer facts must preserve default/expected/suffix types and 32-bit bits");
        check(result.expressions.at(let.initializer.get()).type.same_type(type),
              "the negative expression must keep the magnitude's signed integer type");
    }
    for (std::size_t i = 8; i < 10; ++i) {
        const auto &let = checked_ast<rx::LetStatement>(main.body->statements.at(i));
        const auto &literal = checked_ast<rx::BooleanLiteral>(let.initializer);
        const auto &fact = result.expressions.at(&literal);
        check(literal.value == (i == 8) && fact.type.same_type(rx::SemanticType::bool_()) &&
                  !fact.place.is_place && !fact.diverges,
              "boolean facts retain bool type while values remain readable from the AST");
    }
    check(result.integer_literals.size() == 8 && result.locals.size() == 10 &&
              result.let_bindings.size() == 10,
          "literal fact recording must not repeat semantic checks");
}

void check_expression_queries() {
    const auto crate = parse_crate(R"(
        fn facts(mut x: i32, y: i32) -> i32 {
            let shared: &i32 = &mut x;
            let read = *shared;
            let writable = &mut x;
            *writable = y;
            let selected = if true { let inner = 1; inner } else { 2 };
            let count = loop { break 3; };
            selected
        }
        fn concrete() -> i32 { loop {}; 42 }
        fn forever() -> i32 { loop {} }
        fn main() {}
    )");
    auto result = analyze_fixture(*crate);
    const auto &function = checked_ast<rx::Function>(crate->items.at(0));
    const auto &body = *function.body;
    const auto i32 = rx::SemanticType::int_(rx::SemanticType::IntKind::I32);
    const auto &shared = checked_ast<rx::LetStatement>(body.statements.at(0));
    const auto &borrow = checked_ast<rx::UnaryExpression>(shared.initializer);
    const auto original = result.expressions.at(&borrow);
    check(original.type.same_type(rx::SemanticType::reference(true, i32)) &&
              result.locals.at(result.let_bindings.at(&shared).index).type.same_type(
                  rx::SemanticType::reference(false, i32)) && !original.place.is_place,
          "fixed-target reference coercion must not overwrite the original &mut fact");
    // 当前阶段不推导具体调整；在测试中写入记录，验证与原始表达式结果分开。
    check(result.use_adjustments.empty(), "semantic analysis must not invent adjustments");
    const rx::UseSite initializer{&shared, rx::UseKind::Initializer, 0};
    const rx::UseSite second{&shared, rx::UseKind::Argument, 0};
    const rx::UseSite third{&shared, rx::UseKind::Argument, 1};
    const rx::UseSite other_parent{&borrow, rx::UseKind::Initializer, 0};
    result.use_adjustments.emplace(initializer,
                                   rx::UseAdjustment{rx::SemanticType::reference(false, i32)});
    result.use_adjustments.emplace(second, rx::UseAdjustment{i32});
    result.use_adjustments.emplace(third, rx::UseAdjustment{rx::SemanticType::bool_()});
    result.use_adjustments.emplace(other_parent, rx::UseAdjustment{i32});
    check(result.use_adjustments.size() == 4 &&
              result.use_adjustments.at(initializer).target_type.same_type(
                  rx::SemanticType::reference(false, i32)) &&
              result.use_adjustments.at(second).target_type.same_type(i32) &&
              result.use_adjustments.at(third).target_type.same_type(rx::SemanticType::bool_()) &&
              result.use_adjustments.at(other_parent).target_type.same_type(i32) &&
              result.expressions.at(&borrow).type.same_type(
                  rx::SemanticType::reference(true, i32)),
          "use positions must stay distinct and not overwrite expression facts");
    result.expressions.at(&borrow) = rx::ExpressionResult::value(i32);
    check(result.use_adjustments.at(initializer).target_type.same_type(
              rx::SemanticType::reference(false, i32)),
          "changing expression facts must not overwrite use adjustments");
    result.expressions.at(&borrow) = original;
    const auto &x = result.expressions.at(borrow.operand.get());
    const auto &read = checked_ast<rx::LetStatement>(body.statements.at(1));
    const auto &dereference = checked_ast<rx::UnaryExpression>(read.initializer);
    const auto &read_place = result.expressions.at(&dereference);
    const auto &assignment = checked_ast<rx::AssignmentExpression>(
        &statement_expression(body, 3));
    const auto &write_place = result.expressions.at(assignment.left.get());
    const auto &y = result.expressions.at(assignment.right.get());
    check(x.place.is_place && x.place.is_mutable && !x.place.crossed_shared &&
              read_place.place.is_place && !read_place.place.is_mutable &&
              read_place.place.crossed_shared && write_place.place.is_place &&
              write_place.place.is_mutable && !write_place.place.crossed_shared &&
              y.place.is_place && !y.place.is_mutable && !y.place.crossed_shared,
          "place facts must retain binding and shared/mutable dereference permissions");
    const auto &selected = checked_ast<rx::LetStatement>(body.statements.at(4));
    const auto &conditional = checked_ast<rx::IfExpression>(selected.initializer);
    const auto &otherwise = checked_ast<rx::BlockExpression>(conditional.else_);
    const auto &inner = checked_ast<rx::LetStatement>(conditional.then->statements.at(0));
    const auto &count = checked_ast<rx::LetStatement>(body.statements.at(5));
    const auto &loop = checked_ast<rx::LoopExpression>(count.initializer);
    const auto &jump = checked_ast<rx::BreakExpression>(
        &statement_expression(*loop.body, 0));
    for (const auto *expression : std::array<const rx::Expression *, 8>{
             &body, &conditional, conditional.then.get(), &otherwise,
             inner.initializer.get(), conditional.then->tail.get(),
             otherwise.tail.get(), &loop}) {
        const auto &fact = result.expressions.at(expression);
        check(fact.type.same_type(i32) && !fact.diverges,
              "function body, nested blocks, if branches and value loop need typed facts");
    }
    check(!result.expressions.at(&body).place.is_place &&
              !result.expressions.at(conditional.then.get()).place.is_place &&
              result.expressions.at(conditional.then->tail.get()).place.is_place &&
              result.expressions.at(conditional.condition.get()).type.same_type(
                  rx::SemanticType::bool_()) &&
              result.expressions.at(jump.value.get()).type.same_type(i32),
          "blocks produce values rather than inheriting tail place permissions");
    for (const auto *expression : std::array<const rx::Expression *, 2>{loop.body.get(), &jump}) {
        const auto &fact = result.expressions.at(expression);
        check(fact.type.is_never() && fact.diverges && !fact.place.is_place,
              "break and its containing block must record never and divergence");
    }
    const auto &concrete = checked_ast<rx::Function>(crate->items.at(1));
    const auto &forever = checked_ast<rx::Function>(crate->items.at(2));
    const auto &infinite = checked_ast<rx::LoopExpression>(
        &statement_expression(*concrete.body, 0));
    const auto &forever_loop = checked_ast<rx::LoopExpression>(forever.body->tail);
    check(result.expressions.at(concrete.body.get()).type.same_type(i32) &&
              result.expressions.at(concrete.body.get()).diverges &&
              result.expressions.at(concrete.body->tail.get()).type.same_type(i32) &&
              !result.expressions.at(concrete.body->tail.get()).diverges,
          "a concrete typed divergent block differs from its reachable-agnostic tail fact");
    for (const auto *expression : std::array<const rx::Expression *, 3>{
             &infinite, forever.body.get(), &forever_loop})
        check(result.expressions.at(expression).type.is_never() &&
                  result.expressions.at(expression).diverges,
              "infinite loops and never function bodies must retain their original facts");
    for (const auto *block : {infinite.body.get(), forever_loop.body.get()})
        check(result.expressions.at(block).type.same_type(rx::SemanticType::unit()) &&
                  !result.expressions.at(block).diverges,
              "empty loop bodies still need unit facts, independently of the loop");
    const auto &main = checked_ast<rx::Function>(crate->items.at(3));
    const auto &writable = checked_ast<rx::LetStatement>(body.statements.at(2));
    const auto &mutable_borrow = checked_ast<rx::UnaryExpression>(writable.initializer);
    const auto &write = checked_ast<rx::UnaryExpression>(assignment.left);
    for (const auto *expression : std::array<const rx::Expression *, 4>{
             dereference.operand.get(), writable.initializer.get(),
             write.operand.get(), body.tail.get()})
        check(!result.expressions.at(expression).diverges,
              "reference operands and function tails must also have expression facts");
    check(result.expressions.at(mutable_borrow.operand.get()).type.same_type(i32) &&
              result.expressions.at(&assignment).type.same_type(rx::SemanticType::unit()) &&
              !result.expressions.at(&assignment).place.is_place &&
              !result.expressions.at(&assignment).diverges,
          "assignment and mutable borrow operands need their own unadjusted facts");
    check(result.expressions.at(main.body.get()).type.same_type(rx::SemanticType::unit()) &&
              result.locals.size() == 8 && result.let_bindings.size() == 6 &&
              result.function_locals.size() == 4 && result.expressions.size() == 31,
          "all function bodies need facts without duplicate checks or local allocations");
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
    // 查询结果之前，分析器及其名字表已经销毁。
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
             "struct S {}", // 缺少 main。
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
    first_symbol.id = rx::FunctionId{0};
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
    second_symbol.id = rx::FunctionId{1};
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
    check_binding_and_call_queries();
    check_symbol_and_constant_queries();
    check_integer_queries();
    check_expression_queries();
    check_semantic_result_ownership();
    check_failed_semantic_result();
    std::cout << "semantic invariants: passed\n";
}
