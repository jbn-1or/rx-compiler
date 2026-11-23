// 检查模块的目标配置、函数和块的 ID、参数和返回类型，以及值是否先定义后使用。
// 跨块使用还要检查定义块是否支配使用块；不修改被检查的模块。

#include "ir/check.hpp"
#include <algorithm>
#include <optional>
#include <ostream>
#include <vector>

namespace rx::ir {
namespace {

// 输出带函数名的诊断信息，并返回 false，供检查函数在失败时直接返回。
bool report_error(std::ostream &diagnostics, const Function &function,
                  const char *message) {
    diagnostics << "IR 内部错误：" << function.name << ": " << message << '\n';
    return false;
}

// 检查值属于当前函数、类型匹配且未定义过，再将它标记为已定义。
bool define_value(const Function &function, std::vector<bool> &defined,
                  ValueId value_id, Type type) {
    // 类型必须已知且不是 void，值必须属于本函数、下标有效、未定义过且类型一致。
    if (type == Type::Void || !is_known_type(type) ||
        value_id.function != function.id || value_id.index >= defined.size() ||
        defined[value_id.index] ||
        function.value_types[value_id.index] != type)
        return false;
    defined[value_id.index] = true;
    return true;
}

// 记录一个 SSA 值在哪里定义：参数和入口分配在入口正文之前，
// 普通指令结果记录所属块及块内顺序。
struct DefinitionLocation {
    std::size_t block = 0; // 定义所在块的下标；入口前定义的值固定为 0。
    std::size_t order = 0; // 指令在块内的下标；入口前定义的值固定为 0。
    bool before_entry = false; // 参数或入口分配，进入任何块之前就已经可用。
};

// 按 ValueId.index 保存定义位置；空值表示这个 ID 还没有对应定义。
using DefinitionTable = std::vector<std::optional<DefinitionLocation>>;

// 保存从入口可达的块，以及每个可达块被哪些块支配。
// 不可达块不参与支配集合的固定点计算，跨块使用时也不会被当作可用路径。
struct DominanceInfo {
    std::vector<bool> reachable; // reachable[b] 表示从入口能否走到块 b。
    std::vector<std::vector<bool>> dominators; // dominators[b][d] 表示 d 支配 b。
};

// 判断值类型表中的某一项是否仍未标记为已定义。
bool is_undefined(bool defined) { return !defined; }

// 跳转目标必须属于当前函数、下标有效，并且不能是入口块。
// 入口块没有前驱，跳回入口会让函数重跑一遍参数存储，不能当作普通循环处理。
bool valid_jump_target(const Function &function, BlockId target) {
    return target.function == function.id &&
           target.index < function.blocks.size() && target.index != 0;
}

// 判断操作数在某个使用点能否使用。
// 常量直接可用；参数和入口分配在整个函数中可用；普通指令结果必须同块先定义，
// 或者其定义块支配当前块。use_order 是当前指令或终结指令在块内的位置。
bool operand_available(const Function &function,
                       const DefinitionTable &definitions,
                       const DominanceInfo &dominance,
                       std::size_t use_block, std::size_t use_order,
                       const Operand &operand) {
    if (!function.contains(operand))
        return false;
    const auto *value_id = operand.value_id();
    if (!value_id)
        return true;
    if (value_id->index >= definitions.size() ||
        !definitions[value_id->index])
        return false;
    const auto &definition = *definitions[value_id->index];
    // 参数和入口分配在进入任何块之前就已完成，所有块都能使用。
    if (definition.before_entry)
        return true;
    // 同块仍要求先定义后使用，不能只因为块相同就放宽顺序。
    if (definition.block == use_block)
        return definition.order < use_order;
    // 跨块使用时，定义块必须支配使用块，保证不会从另一条分支绕过定义。
    return dominance.reachable[use_block] &&
           dominance.reachable[definition.block] &&
           dominance.dominators[use_block][definition.block];
}

// 参数按声明顺序从 0 编号，它们在进入任何基本块之前就已经可用。
bool check_parameters(const Function &function, std::vector<bool> &defined,
                      DefinitionTable &definitions,
                      std::ostream &diagnostics) {
    for (std::size_t parameter_index = 0;
         parameter_index < function.parameters.size(); ++parameter_index) {
        const auto &parameter = function.parameters[parameter_index];
        const auto *value_id = parameter.value_id();
        // 参数必须有值 ID，序号、类型和签名一致，并且是第一次定义。
        if (!value_id || value_id->index != parameter_index ||
            parameter.type !=
                function.signature.parameters[parameter_index] ||
            !define_value(function, defined, *value_id, parameter.type))
            return report_error(diagnostics, function,
                                "函数参数身份或类型无效");
        definitions[value_id->index] = DefinitionLocation{0, 0, true};
    }
    return true;
}

// 没有函数体时，必须是外部函数声明，值类型表中也只能保存参数类型。
bool check_declaration(const Function &function,
                       const std::vector<bool> &defined,
                       std::ostream &diagnostics) {
    if (function.linkage != Linkage::External ||
        defined.size() != function.parameters.size() ||
        !function.entry_allocas.empty())
        return report_error(diagnostics, function,
                            "内部函数未完成定义或声明携带函数体数据");
    return true;
}

// 入口分配必须位于正文之前，存储类型和对齐合法，并且产生 ptr 结果。
bool check_entry_allocas(const Function &function,
                         std::vector<bool> &defined,
                         DefinitionTable &definitions,
                         std::ostream &diagnostics) {
    for (const auto &instruction : function.entry_allocas) {
        const auto *allocation = std::get_if<Alloca>(&instruction.operation);
        if (!allocation ||
            !valid_storage(allocation->storage_type, allocation->alignment) ||
            instruction.type != Type::Ptr || !instruction.result ||
            !define_value(function, defined, *instruction.result, Type::Ptr))
            return report_error(diagnostics, function,
                                "入口 alloca 类型、对齐或结果无效");
        definitions[instruction.result->index] =
            DefinitionLocation{0, 0, true};
    }
    return true;
}

// 检查一条普通指令的操作数、结果类型和使用位置。
// 结果是否重复定义由前面的定义收集统一检查，这里只核对操作数能否使用。
bool check_instruction(const Module &module, const Function &function,
                       const Instruction &instruction,
                       const DefinitionTable &definitions,
                       const DominanceInfo &dominance,
                       std::size_t use_block, std::size_t use_order,
                       std::ostream &diagnostics) {
    // 所有操作数都按同一个使用点查询定义位置，避免每条指令重复展开参数。
    const auto operand_ready = [&](const Operand &operand) {
        return operand_available(function, definitions, dominance, use_block,
                                 use_order, operand);
    };
    if (const auto *call = std::get_if<Call>(&instruction.operation)) {
        if (!module.contains(call->function))
            return report_error(diagnostics, function,
                                "调用目标无效或不属于当前模块");
        const auto &signature = module.function(call->function).signature;
        // 实参数量、结果类型和“是否有结果值”必须与被调用签名一致。
        if (call->arguments.size() != signature.parameters.size() ||
            instruction.type != signature.result ||
            instruction.result.has_value() !=
                (signature.result != Type::Void))
            return report_error(diagnostics, function,
                                "call 的签名或结果不一致");
        for (std::size_t argument_index = 0;
             argument_index < call->arguments.size(); ++argument_index)
            if (!operand_ready(call->arguments[argument_index]) ||
                call->arguments[argument_index].type !=
                    signature.parameters[argument_index])
                return report_error(diagnostics, function,
                                    "call 操作数类型、归属或定义位置无效");
    } else if (const auto *load = std::get_if<Load>(&instruction.operation)) {
        // 读取类型和对齐合法，结果类型匹配，地址必须是可用的 ptr。
        if (!valid_storage(load->storage_type, load->alignment) ||
            instruction.type != load->storage_type ||
            !instruction.result || load->address.type != Type::Ptr ||
            !operand_ready(load->address))
            return report_error(diagnostics, function,
                                "load 类型、地址、对齐或结果无效");
    } else if (const auto *store = std::get_if<Store>(&instruction.operation)) {
        // 写入类型和对齐合法，store 不产生结果，值和地址类型必须匹配。
        if (!valid_storage(store->storage_type, store->alignment) ||
            instruction.type != Type::Void || instruction.result ||
            store->value.type != store->storage_type ||
            !operand_ready(store->value) ||
            store->address.type != Type::Ptr ||
            !operand_ready(store->address))
            return report_error(diagnostics, function,
                                "store 类型、地址、对齐或结果无效");
    } else if (const auto *binary =
                   std::get_if<BinaryOperation>(&instruction.operation)) {
        // 两侧类型必须一致且适合该指令，结果类型与操作数类型相同。
        if (binary->left.type != binary->right.type ||
            !valid_binary_type(binary->operation, binary->left.type) ||
            instruction.type != binary->left.type || !instruction.result ||
            !operand_ready(binary->left) ||
            !operand_ready(binary->right))
            return report_error(diagnostics, function,
                                "二元运算的操作数或结果类型无效");
    } else if (const auto *comparison =
                   std::get_if<Compare>(&instruction.operation)) {
        // 比较的两侧类型一致，结果固定为 i1，有符号谓词不能用于 bool。
        if (comparison->left.type != comparison->right.type ||
            !valid_comparison(comparison->predicate,
                              comparison->left.type) ||
            instruction.type != Type::I1 || !instruction.result ||
            !operand_ready(comparison->left) ||
            !operand_ready(comparison->right))
            return report_error(diagnostics, function,
                                "比较谓词、操作数或结果类型无效");
    } else if (const auto *extension = std::get_if<ZExt>(&instruction.operation)) {
        // zext 只允许较窄整数到较宽整数，输入属于当前块，结果类型等于目标类型。
        if (integer_width(extension->value.type) == 0 ||
            integer_width(extension->target_type) <=
                integer_width(extension->value.type) ||
            instruction.type != extension->target_type || !instruction.result ||
            !operand_ready(extension->value))
            return report_error(diagnostics, function,
                                "zext 类型、操作数或结果无效");
    } else if (const auto *truncation = std::get_if<Trunc>(&instruction.operation)) {
        // 源和目标都必须是整数；非整数的位宽 0 不能当作合法截断目标。
        if (integer_width(truncation->value.type) == 0 ||
            integer_width(truncation->target_type) == 0 ||
            integer_width(truncation->target_type) >=
                integer_width(truncation->value.type) ||
            instruction.type != truncation->target_type || !instruction.result ||
            !operand_ready(truncation->value))
            return report_error(diagnostics, function,
                                "trunc 类型、操作数或结果无效");
    } else {
        return report_error(diagnostics, function,
                            "alloca 只能出现在入口分配区");
    }
    return true;
}

// 检查一个基本块的 ID、终结指令、指令序列和终结指令的操作数。
bool check_block(const Module &module, const Function &function,
                 const BasicBlock &block, std::size_t block_index,
                 const DefinitionTable &definitions,
                 const DominanceInfo &dominance,
                 std::ostream &diagnostics) {
    if (block.id != BlockId{function.id, block_index} || !block.terminator)
        return report_error(diagnostics, function,
                            "基本块身份无效或缺少终结指令");
    // 指令按块内顺序检查；use_order 用来判断同块是否先定义后使用。
    for (std::size_t instruction_index = 0;
         instruction_index < block.instructions.size(); ++instruction_index)
        if (!check_instruction(module, function,
                               block.instructions[instruction_index],
                               definitions, dominance, block_index,
                               instruction_index, diagnostics))
            return false;
    // 终结指令在所有普通指令之后使用操作数。
    const std::size_t terminator_order = block.instructions.size();
    const auto &terminator = *block.terminator;
    bool valid_terminator = false;
    // ret 的值也要先定义，并与函数返回类型相同；没有值时函数必须返回 void。
    if (const auto *return_instruction = std::get_if<Return>(&terminator.instruction)) {
        if (return_instruction->value) {
            valid_terminator =
                operand_available(function, definitions, dominance,
                                  block_index, terminator_order,
                                  *return_instruction->value) &&
                return_instruction->value->type == function.signature.result;
        } else {
            valid_terminator = function.signature.result == Type::Void;
        }
        if (!valid_terminator)
            return report_error(diagnostics, function,
                                "ret 的操作数、定义位置或返回类型无效");
    } else if (const auto *branch =
                   std::get_if<Branch>(&terminator.instruction)) {
        valid_terminator = valid_jump_target(function, branch->target);
        if (!valid_terminator)
            return report_error(diagnostics, function,
                                "br 的跳转目标无效或指向入口块");
    } else {
        const auto &conditional =
            std::get<CondBranch>(terminator.instruction);
        // 条件和两个目标都要检查；条件必须在当前块已经可用且类型为 i1。
        valid_terminator =
            conditional.condition.type == Type::I1 &&
            operand_available(function, definitions, dominance,
                              block_index, terminator_order,
                              conditional.condition) &&
            valid_jump_target(function, conditional.when_true) &&
            valid_jump_target(function, conditional.when_false);
        if (!valid_terminator)
            return report_error(diagnostics, function,
                                "condbr 的条件或跳转目标无效");
    }
    return true;
}

// 收集普通指令结果的唯一定义位置。
// 定义表先完整建立，后面就能检查任意块对任意值的跨块使用，不依赖块遍历顺序。
bool collect_instruction_definitions(const Function &function,
                                     std::vector<bool> &defined,
                                     DefinitionTable &definitions,
                                     std::ostream &diagnostics) {
    for (std::size_t block_index = 0;
         block_index < function.blocks.size(); ++block_index) {
        const auto &instructions = function.blocks[block_index].instructions;
        for (std::size_t instruction_index = 0;
             instruction_index < instructions.size(); ++instruction_index) {
            const auto &instruction = instructions[instruction_index];
            // 无结果指令不占用 ValueId，也不需要登记定义位置。
            if (!instruction.result)
                continue;
            if (!define_value(function, defined, *instruction.result,
                              instruction.type))
                return report_error(diagnostics, function,
                                    "指令结果身份无效或重复定义");
            definitions[instruction.result->index] =
                DefinitionLocation{block_index, instruction_index, false};
        }
    }
    return true;
}

// 从终结指令建立前驱/后继表，求从入口可达的块，再求每个可达块的支配集合。
DominanceInfo analyze_dominance(const Function &function) {
    const std::size_t block_count = function.blocks.size();
    DominanceInfo info;
    info.reachable.assign(block_count, false);
    info.dominators.assign(block_count,
                           std::vector<bool>(block_count, false));
    std::vector<std::vector<std::size_t>> predecessors(block_count);
    std::vector<std::vector<std::size_t>> successors(block_count);

    // 只记录形状有效的同函数目标；无效目标仍由 check_block 负责报告。
    const auto add_edge = [&](std::size_t from, BlockId target) {
        if (target.function != function.id ||
            target.index >= block_count)
            return;
        successors[from].push_back(target.index);
        predecessors[target.index].push_back(from);
    };
    for (std::size_t block_index = 0; block_index < block_count;
         ++block_index) {
        const auto &terminator = function.blocks[block_index].terminator;
        if (!terminator)
            continue;
        if (const auto *branch = std::get_if<Branch>(&terminator->instruction)) {
            add_edge(block_index, branch->target);
        } else if (const auto *conditional =
                       std::get_if<CondBranch>(&terminator->instruction)) {
            add_edge(block_index, conditional->when_true);
            add_edge(block_index, conditional->when_false);
        }
    }

    // 入口块是唯一的起点；不可达块不会进入后面的支配集合计算。
    if (block_count == 0)
        return info;
    std::vector<std::size_t> work{0};
    info.reachable[0] = true;
    while (!work.empty()) {
        const auto current = work.back();
        work.pop_back();
        for (const auto next : successors[current]) {
            if (info.reachable[next])
                continue;
            info.reachable[next] = true;
            work.push_back(next);
        }
    }

    // Dom(entry) = {entry}；其余可达块先初始化为全部可达块，再反复求交。
    for (std::size_t block_index = 0; block_index < block_count;
         ++block_index) {
        if (!info.reachable[block_index])
            continue;
        for (std::size_t candidate = 0; candidate < block_count; ++candidate)
            info.dominators[block_index][candidate] =
                info.reachable[candidate];
    }
    info.dominators[0].assign(block_count, false);
    info.dominators[0][0] = true;

    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t block_index = 1; block_index < block_count;
             ++block_index) {
            if (!info.reachable[block_index])
                continue;
            std::vector<bool> intersection(block_count, false);
            bool first_predecessor = true;
            // 只与可达前驱求交；可达块至少有一个可达前驱。
            for (const auto predecessor : predecessors[block_index]) {
                if (!info.reachable[predecessor])
                    continue;
                if (first_predecessor) {
                    intersection = info.dominators[predecessor];
                    first_predecessor = false;
                    continue;
                }
                for (std::size_t candidate = 0; candidate < block_count;
                     ++candidate)
                    intersection[candidate] =
                        intersection[candidate] &&
                        info.dominators[predecessor][candidate];
            }
            if (first_predecessor)
                continue;
            // 每个块支配自己，即使所有前驱的交集里暂时没有它。
            intersection[block_index] = true;
            if (intersection != info.dominators[block_index]) {
                info.dominators[block_index] = intersection;
                changed = true;
            }
        }
    }
    return info;
}

// 检查一个函数的声明或定义；发现错误时输出函数名和原因，并返回 false。
bool check_function(const Module &module, const Function &function,
                    std::ostream &diagnostics) {
    // 返回类型必须已知，参数表数量必须和签名一致。
    if (!is_known_type(function.signature.result) ||
        function.parameters.size() != function.signature.parameters.size())
        return report_error(diagnostics, function,
                            "函数签名或参数数量无效");

    // defined 防止同一个值被定义两次；definitions 记录每个值在哪里定义。
    std::vector<bool> defined(function.value_types.size(), false);
    DefinitionTable definitions(function.value_types.size());
    if (!check_parameters(function, defined, definitions, diagnostics))
        return false;
    if (function.is_declaration())
        return check_declaration(function, defined, diagnostics);
    if (!check_entry_allocas(function, defined, definitions, diagnostics))
        return false;
    if (!collect_instruction_definitions(function, defined, definitions,
                                         diagnostics))
        return false;
    // 每个记录了类型的值都必须实际出现在参数表或某条指令的结果中。
    if (std::any_of(defined.begin(), defined.end(), is_undefined))
        return report_error(diagnostics, function,
                            "值登记表包含未定义的 SSA 值");

    // 支配关系只依赖控制流结构，建立一次后供所有块的跨块使用检查查询。
    const auto dominance = analyze_dominance(function);
    for (std::size_t block_index = 0;
         block_index < function.blocks.size(); ++block_index)
        if (!check_block(module, function, function.blocks[block_index],
                         block_index, definitions, dominance, diagnostics))
            return false;
    return true;
}

// 检查模块类型表中的命名结构体：字段已经定义，
// 并且按同一布局服务能算出有限且不溢出的目标布局。
// 名称与登记顺序由 Module::add_named_type 保证，这里不重复校验。
bool check_named_types(const Module &module, std::ostream &diagnostics) {
    LayoutService layouts(module);
    for (std::size_t index = 0; index < module.named_types().size(); ++index) {
        try {
            (void)layouts.layout(StorageType::struct_of(index));
        } catch (const std::logic_error &error) {
            diagnostics << "IR 内部错误：命名类型布局失败：" << error.what()
                        << '\n';
            return false;
        }
    }
    return true;
}

} // namespace

// 先检查目标配置和函数表，再逐个检查函数；发现第一个错误就返回 false。
// 这里只检查自有模型，生成的 LLVM 文本仍需由外部 LLVM 工具验证。
bool check_module(const Module &module, std::ostream &diagnostics) {
    if (module.target_triple != rv32_triple ||
        module.data_layout != rv32_data_layout) {
        diagnostics << "IR 内部错误：目标配置必须为固定 RV32IM/ILP32 布局\n";
        return false;
    }
    if (!check_named_types(module, diagnostics))
        return false;
    // 函数 ID 要匹配所属模块和函数表下标，函数名也不能在模块中重复。
    for (std::size_t function_index = 0;
         function_index < module.functions().size(); ++function_index) {
        const auto &function = *module.functions()[function_index];
        if (function.id != IRFunctionId{module.identity(), function_index} ||
            !valid_symbol_name(function.name) ||
            module.find_function(function.name) != &function ||
            (function.linkage != Linkage::Internal &&
             function.linkage != Linkage::External)) {
            diagnostics << "IR 内部错误：函数身份、符号或链接属性无效/重复\n";
            return false;
        }
        if (!check_function(module, function, diagnostics))
            return false;
    }
    return true;
}

} // namespace rx::ir
