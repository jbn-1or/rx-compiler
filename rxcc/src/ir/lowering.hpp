// 声明函数体 lowering 共用的标量结果、局部存储映射和生成上下文。
// crate 级的函数登记、签名和机器入口仍由 generator.cpp 负责。
#pragma once

#include "ir/model.hpp"
#include "sema/result.hpp"
#include <map>
#include <optional>
#include <ostream>
#include <variant>
#include <vector>

namespace rx {

struct FunctionSymbol;

namespace ir {
class IRBuilder;
class Module;
} // namespace ir

// 判断源类型是否是当前 lowering 支持的标量（整数或 bool）。
bool is_scalar(const SemanticType &type);

// 计算标量值使用的 IR 类型：bool 为 i1，整数为 i32。
ir::Type lower_scalar_type(const SemanticType &type);

// 保存每个局部绑定在入口分配中的地址；名称解析仍来自语义结果。
using LocalPlaces = std::map<LocalId, ir::Operand>;

// 表达式正常完成但没有数据值，例如 () 或已经产生副作用的 unit 调用。
struct UnitResult {};
// 当前执行路径已经由 return 结束，外层不能再生成任何指令。
struct NoContinuation {};
// 一条表达式或语句的生成结果；生成失败仍用外层 std::nullopt 表示。
using EmitResult = std::variant<ir::Operand, UnitResult, NoContinuation>;

// 判断一次生成结果是否已经结束当前路径。
bool path_ended(const EmitResult &result);

// 保存一次函数体生成过程中不变的环境；局部变量表按函数和块作用域单独传递。
class GenerationContext {
public:
    // 保存这次生成使用的引用，不拥有任何对象。
    GenerationContext(const SemanticResult &semantics,
                      const std::vector<ir::IRFunctionId> &functions,
                      ir::Module &module, ir::IRBuilder &builder,
                      std::ostream &diagnostics);

    // 生成一个完整函数体：参数写入各自 slot，再按语句和尾表达式生成并正常返回。
    // 返回 false 表示遇到尚未支持或内部不一致的情况，原因已写入 diagnostics。
    bool emit_function_body(const FunctionSymbol &function, ir::IRFunctionId id);

private:
    // 生成一条 let 或表达式语句。
    std::optional<EmitResult> emit_statement(const Statement &statement,
                                             LocalPlaces &locals);
    // 计算标量值表达式（整数或 bool）。
    std::optional<EmitResult> emit_value(const Expression &expression,
                                         LocalPlaces &locals);
    // 只生成能正常完成并产生 unit 的表达式。
    std::optional<EmitResult> emit_unit_expression(const Expression &expression,
                                                   LocalPlaces &locals);
    // 计算整数、bool 字面量以及局部变量或常量路径。
    ir::Operand emit_scalar(const Expression &expression,
                            const LocalPlaces &locals);
    // 先求值左操作数，再求值右操作数，最后按源类型选择二元指令。
    std::optional<EmitResult> emit_binary(const BinaryExpression &expression,
                                          LocalPlaces &locals);
    // 生成一元取负或按位取反；借用和解引用留到后续阶段。
    std::optional<EmitResult> emit_unary(const UnaryExpression &expression,
                                         LocalPlaces &locals);
    // 生成 as 转换：整数换源类型，bool 到整数先零扩展。
    std::optional<EmitResult> emit_cast(const CastExpression &expression,
                                        LocalPlaces &locals);
    // 只接收已求好的标量操作数，按源类型选择 add/sdiv/udiv/icmp/移位等指令。
    std::optional<ir::Operand>
    emit_binary_operation(BinaryOperator operation,
                          const SemanticType &operand_type,
                          ir::Operand left, ir::Operand right);
    // 从存储单元读回标量；bool 读取 i8 后再截断成 i1。
    ir::Operand load_scalar(const ir::Operand &address,
                            const SemanticType &type);
    // 把标量写入它的存储单元；bool 先扩展成 i8，整数直接写 i32。
    void store_scalar(const ir::Operand &value, const ir::Operand &address);
    // 根据普通调用目标生成函数或 builtin 调用。
    std::optional<EmitResult> emit_call(const CallExpression &call,
                                        LocalPlaces &locals);
    // 先生成初始化器，再为 LocalId 分配并登记存储位置。
    std::optional<EmitResult> emit_let(const LetStatement &statement,
                                       LocalPlaces &locals);
    // 生成 return：先求值操作数，再发 ret 并结束当前路径。
    std::optional<EmitResult> emit_return(const ReturnExpression &expression,
                                          LocalPlaces &locals);

    const SemanticResult &semantics; // 语义分析结果，提供表达式、路径和调用的记录。
    const std::vector<ir::IRFunctionId> &functions; // 源 FunctionId 下标到已登记 IRFunctionId 的映射。
    ir::Module &module; // 正在构建的 IR 模块。
    ir::IRBuilder &builder; // 向当前基本块添加指令的构建器。
    std::ostream &diagnostics; // 收集 IR 生成期间错误信息的输出流。
};

} // namespace rx
