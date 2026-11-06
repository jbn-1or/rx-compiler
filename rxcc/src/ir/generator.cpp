// 根据语义结果生成源函数、局部存取、普通调用和返回路径。
// 尚未实现的运算与控制流会在生成前明确拒绝，不输出部分模块。

#include "ir/generator.hpp"
#include "ast/ast.hpp"
#include "ir/storage.hpp"
#include "ir/scalar.hpp"
#include "ir/value.hpp"
#include "sema/result.hpp"
#include <llvm/IR/Argument.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace rx {
namespace {

struct FunctionPlan {
    const FunctionSymbol *symbol;
    std::vector<LocalId> local_ids;
    std::vector<std::pair<const LetStatement *, LocalId>> lets;
};

bool is_top_level_function(const Crate &crate, const Function *function) {
    for (const auto &item : crate.items)
        if (item.get() == function)
            return true;
    return false;
}

bool is_supported_value_type(const SemanticType &type) {
    return type.kind == SemanticType::Kind::Int ||
           type.kind == SemanticType::Kind::Bool;
}

bool is_signed_integer(const SemanticType &type) {
    return type.kind == SemanticType::Kind::Int &&
           (type.int_kind == SemanticType::IntKind::I32 ||
            type.int_kind == SemanticType::IntKind::Isize);
}

bool add_local_id(const SemanticResult &semantics, LocalId id,
                  std::set<std::size_t> &seen, llvm::raw_ostream &diagnostics,
                  bool parameter = false) {
    if (id.index >= semantics.locals.size() ||
        semantics.locals[id.index].id != id || !seen.insert(id.index).second) {
        diagnostics << "IR 内部错误：LocalId 无效、重复或超出语义结果范围\n";
        return false;
    }
    const auto &type = semantics.locals[id.index].type;
    if (!is_supported_value_type(type) &&
        (parameter || (!type.is_unit() && !type.is_never()))) {
        diagnostics << "IR 生成尚未支持为该参数或局部绑定分配存储\n";
        return false;
    }
    return true;
}

bool validate_expression(const Expression &expression, const SemanticResult &semantics,
                         const FunctionSymbol &function,
                         std::set<std::size_t> &visible_locals,
                         std::vector<std::pair<const LetStatement *, LocalId>> &lets,
                         llvm::raw_ostream &diagnostics);

bool validate_block(const BlockExpression &block, const SemanticResult &semantics,
                    const FunctionSymbol &function,
                    std::set<std::size_t> &visible_locals,
                    std::vector<std::pair<const LetStatement *, LocalId>> &lets,
                    llvm::raw_ostream &diagnostics) {
    for (const auto &statement : block.statements) {
        if (const auto *let = dynamic_cast<const LetStatement *>(statement.get())) {
            if (!let->initializer ||
                !validate_expression(*let->initializer, semantics, function,
                                     visible_locals, lets, diagnostics))
                return false;
            const auto binding = semantics.let_bindings.find(let);
            if (binding == semantics.let_bindings.end() ||
                !add_local_id(semantics, binding->second, visible_locals,
                              diagnostics))
                return false;
            lets.emplace_back(let, binding->second);
            continue;
        }
        if (const auto *statement_expression =
                dynamic_cast<const ExpressionStatement *>(statement.get())) {
            if (!statement_expression->expression ||
                !validate_expression(*statement_expression->expression, semantics,
                                     function, visible_locals, lets, diagnostics))
                return false;
            continue;
        }
        diagnostics << "IR 生成尚未支持此类语句\n";
        return false;
    }
    return !block.tail || validate_expression(*block.tail, semantics, function,
                                               visible_locals, lets, diagnostics);
}

bool validate_expression(const Expression &expression, const SemanticResult &semantics,
                         const FunctionSymbol &function,
                         std::set<std::size_t> &visible_locals,
                         std::vector<std::pair<const LetStatement *, LocalId>> &lets,
                         llvm::raw_ostream &diagnostics) {
    const auto fact = semantics.expressions.find(&expression);
    if (fact == semantics.expressions.end()) {
        diagnostics << "IR 内部错误：表达式缺少语义结果\n";
        return false;
    }
    if (dynamic_cast<const IntegerLiteral *>(&expression)) {
        const auto *literal = static_cast<const IntegerLiteral *>(&expression);
        if (!is_supported_value_type(fact->second.type) ||
            fact->second.type.kind != SemanticType::Kind::Int ||
            !semantics.integer_literals.contains(literal)) {
            diagnostics << "IR 内部错误：整数字面量缺少有效的定型结果\n";
            return false;
        }
        return true;
    }
    if (dynamic_cast<const BooleanLiteral *>(&expression))
        return fact->second.type.kind == SemanticType::Kind::Bool;
    if (dynamic_cast<const UnitExpression *>(&expression))
        return fact->second.type.kind == SemanticType::Kind::Unit;

    if (const auto *binary = dynamic_cast<const BinaryExpression *>(&expression)) {
        if (binary->operation == BinaryOperator::LogicalAnd ||
            binary->operation == BinaryOperator::LogicalOr) {
            diagnostics << "IR 生成尚未支持短路逻辑运算\n";
            return false;
        }
        if (!binary->left || !binary->right ||
            !validate_expression(*binary->left, semantics, function, visible_locals,
                                 lets, diagnostics) ||
            !validate_expression(*binary->right, semantics, function, visible_locals,
                                 lets, diagnostics))
            return false;
        const auto &left = semantics.expressions.at(binary->left.get()).type;
        const auto &right = semantics.expressions.at(binary->right.get()).type;
        const bool arithmetic = binary->operation == BinaryOperator::Add ||
                                binary->operation == BinaryOperator::Subtract ||
                                binary->operation == BinaryOperator::Multiply ||
                                binary->operation == BinaryOperator::Divide ||
                                binary->operation == BinaryOperator::Remainder;
        const bool bitwise = binary->operation == BinaryOperator::BitwiseAnd ||
                             binary->operation == BinaryOperator::BitwiseOr ||
                             binary->operation == BinaryOperator::BitwiseXor;
        const bool shift = binary->operation == BinaryOperator::ShiftLeft ||
                           binary->operation == BinaryOperator::ShiftRight;
        const bool comparison = binary->operation == BinaryOperator::Equal ||
                                binary->operation == BinaryOperator::NotEqual ||
                                binary->operation == BinaryOperator::LessThan ||
                                binary->operation == BinaryOperator::LessThanOrEqual ||
                                binary->operation == BinaryOperator::GreaterThan ||
                                binary->operation == BinaryOperator::GreaterThanOrEqual;
        if ((arithmetic && (left.kind != SemanticType::Kind::Int ||
                            !left.same_type(right) ||
                            !fact->second.type.same_type(left))) ||
            (bitwise && ((!left.same_type(right) || !is_supported_value_type(left)) ||
                         !fact->second.type.same_type(left))) ||
            (shift && (left.kind != SemanticType::Kind::Int ||
                       right.kind != SemanticType::Kind::Int ||
                       !fact->second.type.same_type(left))) ||
            (comparison &&
             (!left.same_type(right) || !is_supported_value_type(left) ||
              fact->second.type.kind != SemanticType::Kind::Bool))) {
            diagnostics << "IR 内部错误：标量运算的语义类型记录不一致\n";
            return false;
        }
        return arithmetic || bitwise || shift || comparison;
    }

    if (const auto *unary = dynamic_cast<const UnaryExpression *>(&expression)) {
        if (unary->operation != UnaryOperator::Negative &&
            unary->operation != UnaryOperator::Not) {
            diagnostics << "IR 生成尚未支持借用或解引用运算\n";
            return false;
        }
        if (!unary->operand ||
            !validate_expression(*unary->operand, semantics, function, visible_locals,
                                 lets, diagnostics))
            return false;
        const auto &operand = semantics.expressions.at(unary->operand.get()).type;
        const bool valid = unary->operation == UnaryOperator::Negative
                               ? is_signed_integer(operand)
                               : is_supported_value_type(operand);
        if (!valid || !fact->second.type.same_type(operand)) {
            diagnostics << "IR 内部错误：一元标量运算的语义类型记录不一致\n";
            return false;
        }
        return true;
    }

    if (const auto *cast = dynamic_cast<const CastExpression *>(&expression)) {
        if (!cast->operand || fact->second.type.kind != SemanticType::Kind::Int ||
            !validate_expression(*cast->operand, semantics, function, visible_locals,
                                 lets, diagnostics))
            return false;
        const auto &source = semantics.expressions.at(cast->operand.get()).type;
        if (!is_supported_value_type(source)) {
            diagnostics << "IR 生成尚未支持此源类型的 cast\n";
            return false;
        }
        return true;
    }

    if (const auto *path = dynamic_cast<const PathExpression *>(&expression)) {
        const auto target = semantics.paths.find(path);
        if (target == semantics.paths.end()) {
            diagnostics << "IR 内部错误：值路径缺少语义目标\n";
            return false;
        }
        if (const auto *local = std::get_if<LocalId>(&target->second)) {
            if (!visible_locals.contains(local->index)) {
                diagnostics << "IR 内部错误：局部路径引用当前函数中不可见的绑定\n";
                return false;
            }
            return local->index < semantics.locals.size() &&
                   semantics.locals[local->index].id == *local &&
                   semantics.locals[local->index].type.same_type(fact->second.type);
        }
        if (const auto *constant = std::get_if<ConstantId>(&target->second))
            return constant->index < semantics.constants.size() &&
                   semantics.constants[constant->index]->id == *constant &&
                   semantics.constants[constant->index]->evaluated.ok &&
                   semantics.constants[constant->index]->evaluated.type.same_type(
                       fact->second.type);
        diagnostics << "IR 生成尚未支持把函数路径作为值读取\n";
        return false;
    }

    if (const auto *call = dynamic_cast<const CallExpression *>(&expression)) {
        const auto target = semantics.ordinary_calls.find(call);
        if (target == semantics.ordinary_calls.end()) {
            diagnostics << "IR 生成尚未支持此调用目标\n";
            return false;
        }
        if (const auto *function_id = std::get_if<FunctionId>(&target->second)) {
            if (function_id->index >= semantics.functions.size() ||
                semantics.functions[function_id->index]->id != *function_id) {
                diagnostics << "IR 内部错误：普通调用的函数身份无效\n";
                return false;
            }
        } else {
            const auto builtin = std::get<BuiltinIO>(target->second);
            if (!semantics.builtin_signatures.contains(builtin)) {
                diagnostics << "IR 内部错误：内置调用缺少函数签名\n";
                return false;
            }
        }
        if (!call->callee || !dynamic_cast<const PathExpression *>(call->callee.get())) {
            diagnostics << "IR 生成尚未支持此调用形式\n";
            return false;
        }
        for (const auto &argument : call->arguments)
            if (!argument || !validate_expression(*argument, semantics, function,
                                                   visible_locals, lets, diagnostics))
                return false;
        return true;
    }

    if (const auto *block = dynamic_cast<const BlockExpression *>(&expression))
        return validate_block(*block, semantics, function, visible_locals, lets, diagnostics);

    if (const auto *ret = dynamic_cast<const ReturnExpression *>(&expression))
        return !ret->value || validate_expression(*ret->value, semantics, function,
                                                 visible_locals, lets, diagnostics);

    diagnostics << "IR 生成尚未支持此类表达式\n";
    return false;
}

std::optional<std::vector<FunctionPlan>> plan_functions(
    const Crate &crate, const SemanticResult &semantics,
    llvm::raw_ostream &diagnostics) {
    if (!semantics.main_function ||
        semantics.main_function->index >= semantics.functions.size()) {
        diagnostics << "IR 内部错误：缺少有效的源 main 语义记录\n";
        return std::nullopt;
    }

    std::vector<FunctionPlan> plans;
    std::set<std::size_t> all_locals;
    for (std::size_t i = 0; i < semantics.functions.size(); ++i) {
        const auto &owned = semantics.functions[i];
        if (!owned || owned->id.index != i || !owned->node) {
            diagnostics << "IR 内部错误：无效的源函数语义记录\n";
            return std::nullopt;
        }
        if (owned->owner) {
            diagnostics << "IR 生成尚未支持方法\n";
            return std::nullopt;
        }
        if (!is_top_level_function(crate, owned->node)) {
            diagnostics << "IR 内部错误：语义结果中的函数不属于当前 Crate\n";
            return std::nullopt;
        }
        const auto declaration = semantics.function_declarations.find(owned->node);
        if (declaration == semantics.function_declarations.end() ||
            declaration->second != owned->id) {
            diagnostics << "IR 内部错误：源函数声明与身份不一致\n";
            return std::nullopt;
        }
        if (owned->node->has_generic_parameters || !owned->node->body ||
            owned->parameters.size() != owned->node->parameters.size() ||
            (owned->return_type.kind != SemanticType::Kind::Unit &&
             !is_supported_value_type(owned->return_type))) {
            diagnostics << "IR 生成尚未支持泛型、缺少函数体或此返回类型\n";
            return std::nullopt;
        }

        const auto function_locals = semantics.function_locals.find(owned->id);
        if (function_locals == semantics.function_locals.end() || function_locals->second.self ||
            function_locals->second.parameters.size() != owned->parameters.size()) {
            diagnostics << "IR 内部错误：函数参数 LocalId 记录不完整\n";
            return std::nullopt;
        }
        FunctionPlan plan{owned.get(), {}, {}};
        std::set<std::size_t> visible_locals;
        for (std::size_t index = 0; index < function_locals->second.parameters.size(); ++index) {
            const auto id = function_locals->second.parameters[index];
            if (!add_local_id(semantics, id, visible_locals, diagnostics, true))
                return std::nullopt;
            if (!is_supported_value_type(owned->parameters[index].type) ||
                !semantics.locals[id.index].type.same_type(owned->parameters[index].type)) {
                diagnostics << "IR 生成尚未支持此参数类型\n";
                return std::nullopt;
            }
            plan.local_ids.push_back(id);
        }
        if (!validate_block(*owned->node->body, semantics, *owned, visible_locals,
                            plan.lets, diagnostics))
            return std::nullopt;
        for (const auto &[let, id] : plan.lets) {
            (void)let;
            plan.local_ids.push_back(id);
        }
        for (const auto id : plan.local_ids)
            if (!all_locals.insert(id.index).second) {
                diagnostics << "IR 内部错误：LocalId 被多个函数重复引用\n";
                return std::nullopt;
            }
        plans.push_back(std::move(plan));
    }

    if (all_locals.size() != semantics.locals.size()) {
        diagnostics << "IR 内部错误：存在未归属到函数的 LocalId\n";
        return std::nullopt;
    }
    const auto &main = *semantics.functions[semantics.main_function->index];
    if (main.id != *semantics.main_function || main.name != "main" || main.owner ||
        !main.parameters.empty() || main.receiver || !main.return_type.is_unit()) {
        diagnostics << "IR 内部错误：源 main 与语义结果不一致\n";
        return std::nullopt;
    }
    return plans;
}

llvm::Type *return_type(llvm::LLVMContext &context, const SemanticType &type,
                        llvm::raw_ostream &diagnostics) {
    if (type.kind == SemanticType::Kind::Unit)
        return llvm::Type::getVoidTy(context);
    return ir::value_type(context, type, diagnostics);
}

const char *builtin_name(BuiltinIO builtin) {
    switch (builtin) {
        case BuiltinIO::GetI32: return "get_i32";
        case BuiltinIO::PrintI32: return "print_i32";
        case BuiltinIO::PrintlnI32: return "println_i32";
    }
    return "";
}

struct FunctionEmitter {
    const SemanticResult &semantics;
    const std::map<FunctionId, llvm::Function *> &functions;
    llvm::Module &module;
    llvm::IRBuilder<> &builder;
    ir::FunctionStorage &storage;
    llvm::Function &function;
    llvm::raw_ostream &diagnostics;

    ir::EmitResult emit_binary(const BinaryExpression &binary) {
        auto left_result = emit_expression(*binary.left);
        if (std::holds_alternative<ir::NoContinuation>(left_result))
            return ir::NoContinuation{};
        auto *left = std::get_if<ir::IRValue>(&left_result);
        if (!left) {
            diagnostics << "IR 生成尚未支持 unit 二元操作数\n";
            return ir::NoContinuation{};
        }

        auto right_result = emit_expression(*binary.right);
        if (std::holds_alternative<ir::NoContinuation>(right_result))
            return ir::NoContinuation{};
        auto *right = std::get_if<ir::IRValue>(&right_result);
        if (!right) {
            diagnostics << "IR 生成尚未支持 unit 二元操作数\n";
            return ir::NoContinuation{};
        }

        const auto result = ir::emit_scalar_binary(builder, binary.operation,
                                                   *left, *right, diagnostics);
        return result ? ir::EmitResult{*result} : ir::EmitResult{ir::NoContinuation{}};
    }

    ir::EmitResult emit_unary(const UnaryExpression &unary) {
        auto operand_result = emit_expression(*unary.operand);
        if (std::holds_alternative<ir::NoContinuation>(operand_result))
            return ir::NoContinuation{};
        auto *operand = std::get_if<ir::IRValue>(&operand_result);
        if (!operand) {
            diagnostics << "IR 生成尚未支持 unit 一元操作数\n";
            return ir::NoContinuation{};
        }

        const auto result = ir::emit_scalar_unary(builder, unary.operation,
                                                  *operand, diagnostics);
        return result ? ir::EmitResult{*result} : ir::EmitResult{ir::NoContinuation{}};
    }

    ir::EmitResult emit_cast(const CastExpression &cast) {
        auto operand_result = emit_expression(*cast.operand);
        if (std::holds_alternative<ir::NoContinuation>(operand_result))
            return ir::NoContinuation{};
        auto *operand = std::get_if<ir::IRValue>(&operand_result);
        if (!operand) {
            diagnostics << "IR 生成尚未支持 unit cast 操作数\n";
            return ir::NoContinuation{};
        }
        const auto result = ir::emit_scalar_cast(
            builder, *operand, semantics.expressions.at(&cast).type, diagnostics);
        return result ? ir::EmitResult{*result} : ir::EmitResult{ir::NoContinuation{}};
    }

    ir::EmitResult emit_expression(const Expression &expression) {
        if (const auto *integer = dynamic_cast<const IntegerLiteral *>(&expression)) {
            const auto &fact = semantics.integer_literals.at(integer);
            return ir::IRValue{llvm::ConstantInt::get(builder.getInt32Ty(), fact.bits),
                               fact.type};
        }
        if (const auto *boolean = dynamic_cast<const BooleanLiteral *>(&expression))
            return ir::IRValue{llvm::ConstantInt::get(builder.getInt1Ty(), boolean->value),
                               semantics.expressions.at(boolean).type};
        if (dynamic_cast<const UnitExpression *>(&expression))
            return ir::Unit{};
        if (const auto *binary = dynamic_cast<const BinaryExpression *>(&expression))
            return emit_binary(*binary);
        if (const auto *unary = dynamic_cast<const UnaryExpression *>(&expression))
            return emit_unary(*unary);
        if (const auto *cast = dynamic_cast<const CastExpression *>(&expression))
            return emit_cast(*cast);
        if (const auto *path = dynamic_cast<const PathExpression *>(&expression))
            return emit_path(*path);
        if (const auto *call = dynamic_cast<const CallExpression *>(&expression))
            return emit_call(*call);
        if (const auto *block = dynamic_cast<const BlockExpression *>(&expression))
            return emit_block(*block);
        if (const auto *ret = dynamic_cast<const ReturnExpression *>(&expression))
            return emit_return(*ret);
        diagnostics << "IR 内部错误：未经预检的表达式\n";
        return ir::NoContinuation{};
    }

    ir::EmitResult emit_path(const PathExpression &path) {
        const auto &target = semantics.paths.at(&path);
        const auto &fact = semantics.expressions.at(&path);
        if (const auto *local = std::get_if<LocalId>(&target)) {
            if (fact.type.kind == SemanticType::Kind::Unit)
                return ir::Unit{};
            const auto *place = storage.local(*local);
            if (!place) {
                diagnostics << "IR 内部错误：局部路径缺少 LocalId slot\n";
                return ir::NoContinuation{};
            }
            const auto loaded = ir::load_value(builder, *place, diagnostics);
            return loaded ? ir::EmitResult{*loaded} : ir::EmitResult{ir::NoContinuation{}};
        }
        if (const auto *constant = std::get_if<ConstantId>(&target)) {
            const auto &symbol = *semantics.constants.at(constant->index);
            const auto &value = symbol.evaluated;
            if (value.type.kind == SemanticType::Kind::Int) {
                uint32_t bits = 0;
                if (const auto *signed_value = std::get_if<int64_t>(&value.value))
                    bits = static_cast<uint32_t>(*signed_value);
                else if (const auto *unsigned_value = std::get_if<uint64_t>(&value.value))
                    bits = static_cast<uint32_t>(*unsigned_value);
                else {
                    diagnostics << "IR 内部错误：整数常量缓存类型不匹配\n";
                    return ir::NoContinuation{};
                }
                return ir::IRValue{llvm::ConstantInt::get(builder.getInt32Ty(), bits),
                                   value.type};
            }
            if (value.type.kind == SemanticType::Kind::Bool) {
                const auto *boolean = std::get_if<bool>(&value.value);
                if (boolean)
                    return ir::IRValue{llvm::ConstantInt::get(builder.getInt1Ty(), *boolean),
                                       value.type};
            }
        }
        diagnostics << "IR 内部错误：常量路径目标无效或类型不支持\n";
        return ir::NoContinuation{};
    }

    llvm::Function *get_builtin(BuiltinIO builtin) {
        const auto &signature = semantics.builtin_signatures.at(builtin);
        std::vector<llvm::Type *> parameters;
        for (const auto &parameter : signature.parameters) {
            auto *type = ir::value_type(builder.getContext(), parameter, diagnostics);
            if (!type)
                return nullptr;
            parameters.push_back(type);
        }
        auto *result_type = return_type(builder.getContext(), signature.return_type, diagnostics);
        if (!result_type)
            return nullptr;
        auto *type = llvm::FunctionType::get(result_type, parameters, false);
        const std::string name = builtin_name(builtin);
        if (auto *existing = module.getFunction(name)) {
            if (existing->getFunctionType() != type) {
                diagnostics << "IR 内部错误：内置函数签名冲突\n";
                return nullptr;
            }
            return existing;
        }
        return llvm::Function::Create(type, llvm::Function::ExternalLinkage, name, module);
    }

    ir::EmitResult emit_call(const CallExpression &call) {
        const auto &target = semantics.ordinary_calls.at(&call);
        llvm::Function *callee = nullptr;
        if (const auto *function_id = std::get_if<FunctionId>(&target)) {
            const auto found = functions.find(*function_id);
            if (found == functions.end()) {
                diagnostics << "IR 内部错误：调用目标没有预先登记\n";
                return ir::NoContinuation{};
            }
            callee = found->second;
        } else {
            callee = get_builtin(std::get<BuiltinIO>(target));
            if (!callee)
                return ir::NoContinuation{};
        }

        std::vector<llvm::Value *> arguments;
        arguments.reserve(call.arguments.size());
        for (const auto &argument : call.arguments) {
            auto result = emit_expression(*argument);
            if (std::holds_alternative<ir::NoContinuation>(result))
                return ir::NoContinuation{};
            auto *value = std::get_if<ir::IRValue>(&result);
            if (!value) {
                diagnostics << "IR 生成尚未支持以 unit 作为调用实参\n";
                return ir::NoContinuation{};
            }
            arguments.push_back(value->value);
        }
        auto *result = builder.CreateCall(callee, arguments);
        const auto &type = semantics.expressions.at(&call).type;
        if (type.kind == SemanticType::Kind::Unit)
            return ir::Unit{};
        return ir::IRValue{result, type};
    }

    ir::EmitResult emit_return(const ReturnExpression &ret) {
        if (!ret.value) {
            if (!function.getReturnType()->isVoidTy()) {
                diagnostics << "IR 内部错误：有返回值的函数遇到裸 return\n";
                return ir::NoContinuation{};
            }
            builder.CreateRetVoid();
            return ir::NoContinuation{};
        }
        auto value = emit_expression(*ret.value);
        if (std::holds_alternative<ir::NoContinuation>(value))
            return ir::NoContinuation{};
        if (auto *returned = std::get_if<ir::IRValue>(&value)) {
            if (returned->value->getType() != function.getReturnType()) {
                diagnostics << "IR 内部错误：返回值 LLVM 类型与函数签名不一致\n";
                return ir::NoContinuation{};
            }
            builder.CreateRet(returned->value);
        } else if (function.getReturnType()->isVoidTy()) {
            builder.CreateRetVoid();
        } else {
            diagnostics << "IR 内部错误：非 unit 函数返回了 unit\n";
        }
        return ir::NoContinuation{};
    }

    bool emit_statement(const Statement &statement) {
        if (const auto *let = dynamic_cast<const LetStatement *>(&statement)) {
            const auto id = semantics.let_bindings.at(let);
            auto value = emit_expression(*let->initializer);
            if (std::holds_alternative<ir::NoContinuation>(value))
                return false;
            if (semantics.locals.at(id.index).type.is_never()) {
                diagnostics << "IR 内部错误：Never 绑定的初始化器未终止执行路径\n";
                return false;
            }
            if (semantics.locals.at(id.index).type.kind == SemanticType::Kind::Unit)
                return std::holds_alternative<ir::Unit>(value);
            const auto *emitted = std::get_if<ir::IRValue>(&value);
            const auto *place = storage.local(id);
            return emitted && place && ir::store_value(builder, *place, *emitted, diagnostics);
        }
        if (const auto *expression_statement =
                dynamic_cast<const ExpressionStatement *>(&statement)) {
            const auto result = emit_expression(*expression_statement->expression);
            return !std::holds_alternative<ir::NoContinuation>(result);
        }
        diagnostics << "IR 内部错误：未经预检的语句\n";
        return false;
    }

    ir::EmitResult emit_block(const BlockExpression &block) {
        for (const auto &statement : block.statements)
            if (!emit_statement(*statement))
                return ir::NoContinuation{};
        if (!block.tail)
            return ir::Unit{};
        return emit_expression(*block.tail);
    }

    bool finish_function(ir::EmitResult result) {
        if (std::holds_alternative<ir::NoContinuation>(result))
            return true;
        if (function.getReturnType()->isVoidTy()) {
            if (!std::holds_alternative<ir::Unit>(result)) {
                diagnostics << "IR 内部错误：unit 函数正常结束时产生了数据值\n";
                return false;
            }
            builder.CreateRetVoid();
            return true;
        }
        const auto *value = std::get_if<ir::IRValue>(&result);
        if (!value || value->value->getType() != function.getReturnType()) {
            diagnostics << "IR 内部错误：函数尾值与返回签名不一致\n";
            return false;
        }
        builder.CreateRet(value->value);
        return true;
    }
};

} // namespace

std::optional<std::vector<std::string>> assign_function_names(
    const SemanticResult &semantics, llvm::raw_ostream &diagnostics) {
    std::vector<std::string> names;
    names.reserve(semantics.functions.size());
    for (std::size_t index = 0; index < semantics.functions.size(); ++index) {
        const auto &function = semantics.functions[index];
        if (!function || !function->node || function->id.index != index) {
            diagnostics << "IR 内部错误：无效的源函数身份记录\n";
            return std::nullopt;
        }
        const auto declaration = semantics.function_declarations.find(function->node);
        if (declaration == semantics.function_declarations.end() ||
            declaration->second != function->id) {
            diagnostics << "IR 内部错误：源函数声明与身份不一致\n";
            return std::nullopt;
        }
        names.push_back("__rx_fn_" + std::to_string(function->id.index));
    }
    return names;
}

std::optional<IRArtifact> generate_ir(const Crate &crate,
                                      const SemanticResult &semantics,
                                      llvm::raw_ostream &diagnostics) {
    const auto function_names = assign_function_names(semantics, diagnostics);
    if (!function_names)
        return std::nullopt;
    const auto plans = plan_functions(crate, semantics, diagnostics);
    if (!plans)
        return std::nullopt;

    IRArtifact artifact("rx");
    auto &context = artifact.context();
    auto &module = artifact.module();
    llvm::IRBuilder<> builder(context);
    std::map<FunctionId, llvm::Function *> functions;

    // 先登记全部函数签名，前向调用和相互递归据此按身份解析。
    for (const auto &plan : *plans) {
        std::vector<llvm::Type *> parameters;
        for (const auto &parameter : plan.symbol->parameters) {
            auto *type = ir::value_type(context, parameter.type, diagnostics);
            if (!type)
                return std::nullopt;
            parameters.push_back(type);
        }
        auto *result_type = return_type(context, plan.symbol->return_type, diagnostics);
        if (!result_type)
            return std::nullopt;
        auto *type = llvm::FunctionType::get(result_type, parameters, false);
        auto *function = llvm::Function::Create(
            type, llvm::Function::InternalLinkage,
            function_names->at(plan.symbol->id.index), module);
        functions.emplace(plan.symbol->id, function);
    }

    for (const auto &plan : *plans) {
        auto *function = functions.at(plan.symbol->id);
        auto *entry = llvm::BasicBlock::Create(context, "entry", function);
        ir::FunctionStorage storage(*function);
        for (const auto id : plan.local_ids) {
            const auto &type = semantics.locals.at(id.index).type;
            // Unit 无数据；Never 初始化器不会完成，两者都没有本地存储。
            if (!type.is_unit() && !type.is_never() &&
                !storage.create_local(id, type, diagnostics))
                return std::nullopt;
        }

        builder.SetInsertPoint(entry);
        auto argument = function->arg_begin();
        const auto &locals = semantics.function_locals.at(plan.symbol->id);
        for (const auto id : locals.parameters) {
            argument->setName("arg." + std::to_string(id.index));
            const auto &type = semantics.locals.at(id.index).type;
            if (!ir::store_value(builder, *storage.local(id),
                                 ir::IRValue{&*argument, type}, diagnostics))
                return std::nullopt;
            ++argument;
        }

        FunctionEmitter emitter{semantics, functions, module, builder, storage,
                                *function, diagnostics};
        auto body_result = emitter.emit_block(*plan.symbol->node->body);
        if (!emitter.finish_function(std::move(body_result)))
            return std::nullopt;
        builder.ClearInsertionPoint();
    }

    auto *source_main = functions.at(*semantics.main_function);
    auto *entry_type = llvm::FunctionType::get(builder.getInt32Ty(), false);
    auto *entry = llvm::Function::Create(
        entry_type, llvm::Function::ExternalLinkage, "main", module);
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", entry));
    builder.CreateCall(source_main);
    builder.CreateRet(builder.getInt32(0));
    builder.ClearInsertionPoint();

    if (llvm::verifyModule(module, &diagnostics))
        return std::nullopt;
    return artifact;
}

} // namespace rx
