// 将已通过语义检查的程序编排为 IR 模块：检查支持范围、登记函数签名、
// 逐函数调用 lowering，并生成机器入口 main。
// 函数体入口由 lowering.cpp 编排，表达式、引用、语句和调用按职责在 lowering/ 实现。
// 这里只构建模块，输出 LLVM 文本由 artifact/printer 完成。
#include "ir/generator.hpp"
#include "ast/ast.hpp"
#include "ir/builder.hpp"
#include "ir/lowering.hpp"
#include "ir/lowering/value.hpp"
#include "sema/result.hpp"
#include <algorithm>
#include <ostream>
#include <stdexcept>
#include <vector>

namespace rx {
namespace {

// 只检查所有函数是否在当前 lowering 的支持范围内，语义正确性由 sema 保证。
bool validate_functions(const SemanticResult &semantics,
                        std::ostream &diagnostics) {
    for (const auto &owned : semantics.functions) {
        const auto &function = *owned;
        // 签名支持标量、引用链（可指向聚合）和按值聚合参数/返回。
        const bool supported_return =
            function.return_type.is_unit() ||
            is_storable_value(function.return_type);
        // 方法接收者作为首个参数参与同一套 ABI 检查。
        const bool supported_receiver =
            !function.receiver || is_storable_value(function.receiver->type);
        const bool supported_parameters =
            std::all_of(function.parameters.begin(), function.parameters.end(),
                        [](const ResolvedParameter &parameter) {
                             return is_storable_value(parameter.type);
                        });
        if (!supported_receiver || !supported_parameters || !supported_return) {
            diagnostics << "IR 生成尚未支持此函数签名"
                           "（聚合容器操作等仍需后续阶段）\n";
            return false;
        }
        // 函数体的语句列表和尾表达式会在 lowering 时按源码顺序处理。
    }
    return true;
}

// 把源函数签名降级成共享 ABI：按值聚合参数用 ptr 快照，聚合返回用隐藏首参数。
ir::FunctionABI make_function_abi(const FunctionSymbol &function) {
    ir::FunctionABI abi;
    if (is_aggregate_value(function.return_type)) {
        abi.result_storage = ir::storage_type_of(function.return_type);
        abi.signature.parameters.push_back(ir::Type::Ptr);
        abi.parameter_storage.push_back(ir::StorageType{});
    } else {
        abi.signature.result = function.return_type.is_unit()
                         ? ir::Type::Void
                         : lower_value_type(function.return_type);
    }
    // 一个源参数或方法的 self 在这里降级；按值聚合传 ptr 快照。
    const auto append_parameter = [&abi](const SemanticType &type) {
        if (is_aggregate_value(type)) {
            abi.signature.parameters.push_back(ir::Type::Ptr);
            abi.parameter_storage.push_back(ir::storage_type_of(type));
        } else {
            abi.signature.parameters.push_back(lower_value_type(type));
            abi.parameter_storage.push_back(ir::StorageType{});
        }
    };
    // 方法接收者位于形参表首位，显式关联调用和点调用都按此顺序传参。
    if (function.receiver)
        append_parameter(function.receiver->type);
    for (const auto &parameter : function.parameters)
        append_parameter(parameter.type);
    return abi;
}

} // namespace

// 根据源函数的编号生成内部名称，避免与机器入口 main 或运行时函数同名。
std::vector<std::string>
assign_function_names(const SemanticResult &semantics) {
    std::vector<std::string> names;
    names.reserve(semantics.functions.size());
    for (const auto &function : semantics.functions)
        names.push_back("__rx_fn_" + std::to_string(function->id.index));
    return names;
}

// 生成源函数和机器入口 main；遇到不支持的代码或内部构建错误时返回空值。
std::optional<IRArtifact> generate_ir(const SemanticResult &semantics,
                                      std::ostream &diagnostics) {
    // 全 crate 支持边界检查失败时，不构建模块。
    if (!validate_functions(semantics, diagnostics))
        return std::nullopt;
    const auto function_names = assign_function_names(semantics);

    try {
        IRArtifact artifact;
        auto &module = artifact.module();
        ir::IRBuilder builder(module);
        // functions[源 FunctionId.index] 保存对应的 IRFunctionId，不混用两种 ID。
        std::vector<ir::IRFunctionId> functions;
        // 先按稳定 StructId 登记全部命名结构体身份，再补齐字段。
        // 这样 Node { children: Vec<Node> } 等经 Box/Vec 的递归类型也能有限完成。
        for (const auto &structure : semantics.structs)
            module.add_named_type("__rx_struct_" +
                                  std::to_string(structure->id.index));
        for (const auto &structure : semantics.structs) {
            std::vector<ir::StorageType> fields;
            fields.reserve(structure->fields.size());
            for (const auto &field : structure->fields)
                fields.push_back(ir::storage_type_of(
                    structure->field_types.at(field.name)));
            module.define_named_type(structure->id.index, std::move(fields));
        }
        // 先登记所有函数的 ABI，使前向调用可以直接按 FunctionId 解析。
        for (std::size_t function_index = 0;
             function_index < function_names.size(); ++function_index) {
            const auto &source = *semantics.functions[function_index];
            functions.push_back(module.add_function_abi(
                function_names[function_index],
                make_function_abi(source), ir::Linkage::Internal));
        }
        // 按源码顺序生成所有函数体，包括未被调用的函数。
        GenerationContext context(semantics, functions, module, builder, diagnostics);
        for (std::size_t function_index = 0;
             function_index < functions.size(); ++function_index) {
            if (!context.emit_function_body(
                    *semantics.functions[function_index],
                    functions[function_index]))
                return std::nullopt;
        }
        // 源 main 返回 unit，对应内部 void 函数；机器入口按 C 调用约定调用它后返回 0。
        const auto entry = module.add_function("main", {ir::Type::I32, {}},
                                               ir::Linkage::External);
        builder.set_insert_point(module.function(entry).add_block());
        builder.call(functions[semantics.main_function->index]);
        builder.ret(ir::Operand::constant(ir::Type::I32, 0));
        // clone 和相等辅助函数在源函数体之后统一补全，确保内部函数都有定义。
        if (!context.emit_pending_helpers())
            return std::nullopt;
        // 完整模块仍可被调用方修改；在输出边界统一做结构和布局校验。
        return artifact;
    } catch (const std::logic_error &error) {
        // IR 构建失败属于编译器内部错误，不是源程序没有通过类型检查。
        diagnostics << "IR 内部错误：" << error.what() << '\n';
        return std::nullopt;
    }
}

} // namespace rx
