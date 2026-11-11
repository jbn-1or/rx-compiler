// 定义保存 IR 所需的模块、函数、基本块、操作数和指令，不依赖 LLVM 开发库。
// 一个模块保存多个函数，每个函数保存自己的基本块，以及参数和指令结果的类型。
// 当前只支持 void/i32 类型和 call/ret 指令，不负责推导源程序的类型。
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace rx::ir {

// 当前支持的 IR 类型：Void 表示没有返回数据，I32 表示 32 位整数。
enum class Type { Void, I32 };
// 返回类型在 LLVM 文本中的名称；不支持的类型返回空串。
const char *type_name(Type type);

// 表示尚未分配下标，不能用它访问函数、值或基本块表。
inline constexpr std::size_t invalid_index = std::numeric_limits<std::size_t>::max();

// 通过模块编号和函数下标找到一个 IR 函数，不是语义分析中的 FunctionId。
// 同时保存模块编号，防止把另一个模块中下标相同的函数当成当前模块的函数。
struct IRFunctionId {
    // 所属模块的唯一编号；0 表示还没有指定有效模块。
    uint64_t module = 0;
    // 函数在模块函数表中的下标；invalid_index 表示尚未分配。
    std::size_t index = invalid_index;
    bool operator==(const IRFunctionId &) const = default;
};
// 标识函数中的一个 SSA 值，即只定义一次的参数或指令结果，不是源变量的 LocalId。
struct ValueId {
    // 定义这个值的函数 ID，其中也记录了所属模块。
    IRFunctionId function;
    // 在所属函数 value_types 中的下标；invalid_index 表示尚未分配。
    std::size_t index = invalid_index;
    bool operator==(const ValueId &) const = default;
};
// 通过函数 ID 和块下标找到一个基本块；块编号与值编号分别计算。
struct BlockId {
    // 所属函数的 ID，用来检查是否误用了其他函数的块。
    IRFunctionId function;
    // 在所属函数 blocks 中的下标；invalid_index 表示尚未分配。
    std::size_t index = invalid_index;
    bool operator==(const BlockId &) const = default;
};

// 保存一条指令使用的输入，可以是整数常量，也可以是参数或前面指令的结果。
struct Operand {
    // 这个操作数的 IR 类型；默认 Void 表示还没有可用的数据值。
    Type type = Type::Void;
    // monostate 表示空值，uint32_t 保存常量的 32 个二进制位，ValueId 引用 SSA 值。
    std::variant<std::monostate, uint32_t, ValueId> value;

    // 用类型和常量的二进制位创建操作数，不分配值 ID。
    static Operand constant(Type type, uint32_t bits);
    // 用类型和已有的值 ID 创建操作数，不在这里检查值是否已经定义。
    static Operand local(Type type, ValueId id);
    // 常量操作数返回其二进制位的地址，其他操作数返回 nullptr。
    const uint32_t *constant_bits() const { return std::get_if<uint32_t>(&value); }
    // SSA 操作数返回其值 ID 的地址，其他操作数返回 nullptr。
    const ValueId *value_id() const { return std::get_if<ValueId>(&value); }
    bool operator==(const Operand &) const = default;
};
// 判断操作数是否保存了当前支持的 i32 常量。
bool valid_constant(const Operand &operand);

// Internal 表示函数只在当前模块可见，External 表示函数符号可以跨模块链接。
enum class Linkage { Internal, External };
// 保存函数的返回类型和参数类型，用来检查函数定义、调用和返回指令。
struct Signature {
    // 函数的返回类型；Void 表示没有返回数据。
    Type result = Type::Void;
    // 按声明顺序排列的参数类型，不允许 Void 参数。
    std::vector<Type> parameters;
    bool operator==(const Signature &) const = default;
};

// 保存 call 指令要调用哪个函数、传入哪些参数，不保存调用结果。
struct Call {
    // 被调用函数的 IR ID，通过模块查询函数名、返回类型和参数类型。
    IRFunctionId function;
    // 按形参顺序保存的实参，数量和类型必须与被调用函数匹配。
    std::vector<Operand> arguments;
};
// 普通指令支持的操作种类，当前只有 Call。
using Operation = std::variant<Call>;
// 保存一条普通指令的操作、结果类型，以及有返回数据时分配的结果 ID。
struct Instruction {
    // 结果值的 ID；返回 void 的调用没有结果，因此为空。
    std::optional<ValueId> result;
    // 结果的 IR 类型，当前等于被调用函数的返回类型。
    Type type;
    // 这条指令具体要做什么，当前保存 Call 及其目标函数和实参。
    Operation operation;
};

// 表示 ret 指令，结束当前函数，并返回一个值或不返回数据。
struct Return {
    // 要返回的操作数，类型必须匹配函数返回类型；空值表示 ret void。
    std::optional<Operand> value;
};
// 结束基本块的指令种类，当前只有 Return。
using Terminator = std::variant<Return>;
// 保存一段顺序执行的指令，最后必须有一条结束该块的指令，当前为 ret。
struct BasicBlock {
    // 这个块的 ID，用来查找块，并生成 bb_<下标> 形式的标签。
    BlockId id;
    // 按执行顺序排列的普通指令，不包含末尾的终结指令。
    std::vector<Instruction> instructions;
    // 结束这个块的指令；构建时可以暂缺，但模块检查通过前必须补上。
    std::optional<Terminator> terminator;
};

// 保存一个函数的名称、参数、返回类型和函数体；没有基本块时只有声明。
// 参数和指令结果使用同一张值类型表，基本块使用另一张表单独编号。
struct Function {
    // 这个函数的 IR ID，在添加到模块时分配，不直接使用 sema 的 FunctionId。
    IRFunctionId id;
    // LLVM 全局符号名，不含 @ 前缀；在模块内唯一。
    std::string name;
    // 符号链接属性，决定是否仅在模块内部可见。
    Linkage linkage;
    // 函数的返回类型和参数类型，供调用和返回检查使用。
    Signature signature;
    // 按声明顺序保存的参数操作数，它们占用本函数最前面的值编号。
    std::vector<Operand> parameters;
    // 记录每个参数和指令结果的类型，ValueId.index 是这张表的下标。
    std::vector<Type> value_types;
    // 按创建顺序保存的基本块；定义的第一个块为入口块。
    std::vector<BasicBlock> blocks;

    // 判断函数是否只有声明、没有函数体；内部函数不能一直保持声明状态。
    bool is_declaration() const { return blocks.empty(); }
    // 按参数序号返回对应的 IR 操作数，序号从 0 开始。
    Operand argument(std::size_t index) const { return parameters.at(index); }
    // 为新值分配 ID 并保存类型；调用方还需要把它记录为参数或指令结果。
    Operand new_value(Type type);
    // 添加一个空基本块并返回它的 ID，结束指令由调用方稍后添加。
    BlockId add_block();
    // 按 ID 返回基本块；块不属于本函数或下标越界时抛出异常。
    BasicBlock &block(BlockId id);
    const BasicBlock &block(BlockId id) const;
    // 检查操作数的类型和所属函数，不检查它是否在使用前已经定义。
    bool contains(const Operand &operand) const;
};

// 固定生成 RV32 的 32 位小端代码，不使用运行编译器的机器的数据布局。
inline constexpr const char *rv32_triple = "riscv32-unknown-unknown-elf";
inline constexpr const char *rv32_data_layout = "e-m:e-p:32:32-i64:64-n32-S128";

// 保存一次 IR 生成得到的全部函数和目标配置，供检查器、输出器和后端读取。
// 模块对象不能复制或移动；IRArtifact 通过移动指针来移交整个模块。
class Module {
public:
    // 创建指定名称的模块，同时为它分配唯一编号。
    explicit Module(std::string name);
    Module(const Module &) = delete;
    Module &operator=(const Module &) = delete;
    Module(Module &&) = delete;
    Module &operator=(Module &&) = delete;

    // 返回模块编号，用来检查函数是否属于这个模块。
    uint64_t identity() const { return identity_; }
    // 添加函数并分配函数和参数的 ID；名称不能重复，函数体随后再添加。
    IRFunctionId add_function(std::string name, Signature signature, Linkage linkage);
    // 按函数名查找，找不到时返回 nullptr。
    Function *find_function(const std::string &name);
    const Function *find_function(const std::string &name) const;
    // 按 ID 返回函数；函数不属于本模块或下标无效时抛出 logic_error。
    Function &function(IRFunctionId id);
    const Function &function(IRFunctionId id) const;
    // 判断这个 ID 是否对应本模块中已经添加的函数。
    bool contains(IRFunctionId id) const;
    // 返回按添加顺序保存的函数表，不允许通过这个接口修改函数表本身。
    const auto &functions() const { return functions_; }

    // 模块名称，当前不会将它打印为 LLVM 符号。
    std::string name;
    // LLVM 目标三元组，指定生成代码使用的架构和目标环境。
    std::string target_triple = rv32_triple;
    // LLVM 数据布局字符串，描述目标的字节序、指针宽度和对齐规则。
    std::string data_layout = rv32_data_layout;

private:
    // 当前进程内唯一的模块编号，用来区分不同模块中下标相同的函数。
    uint64_t identity_;
    // 按添加顺序保存函数指针，并负责释放函数；IRFunctionId.index 对应表下标。
    // vector 扩容只会移动指针，不会移动函数对象，因此已有函数引用仍然有效。
    std::vector<std::unique_ptr<Function>> functions_;
};

// 判断函数名是否可以不加引号，直接写入 LLVM 文本。
bool valid_symbol_name(const std::string &name);

} // namespace rx::ir
