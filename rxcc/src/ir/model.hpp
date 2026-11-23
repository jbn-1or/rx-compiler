// 定义保存 IR 所需的模块、函数、基本块、操作数和指令，不依赖 LLVM 开发库。
// 一个模块保存多个函数，每个函数保存自己的基本块，以及参数和指令结果的类型。
// 当前支持整数、布尔和指针，以及调用、存储、二元运算、比较、
// 整数扩展/截断和返回指令，不推导源程序类型。
#pragma once

#include "ir/layout.hpp"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace rx::ir {

class Module;

// Void 表示没有返回数据；I1 表示布尔计算值；I8 是 bool 的存储单元；
// I32 表示 32 位整数；Ptr 表示不透明地址。
enum class Type { Void, I1, I8, I32, Ptr };
// 返回类型在 LLVM 文本中的名称；不支持的类型返回空串。
const char *type_name(Type type);
// 判断类型是否是已定义的 Type 枚举值。
bool is_known_type(Type type);
// 返回整数类型的位宽；非整数类型返回 0，供扩展/截断检查位宽方向。
unsigned integer_width(Type type);

// 表示尚未分配下标，不能用它访问函数、值或基本块表。
inline constexpr std::size_t invalid_index =
    std::numeric_limits<std::size_t>::max();

// 通过模块编号和函数下标找到一个 IR 函数，不是语义分析中的 FunctionId。
// 同时保存模块编号，防止把另一个模块中下标相同的函数当成当前模块的函数。
struct IRFunctionId {
    uint64_t module = 0; // 所属模块的唯一编号；0 表示还没有指定有效模块。
    std::size_t index = invalid_index; // 函数在模块函数表中的下标；invalid_index 表示尚未分配。
    bool operator==(const IRFunctionId &) const = default;
};
// 标识函数中的一个 SSA 值，即只定义一次的参数或指令结果，不是源变量的 LocalId。
struct ValueId {
    IRFunctionId function; // 定义这个值的函数 ID，其中也记录了所属模块。
    std::size_t index = invalid_index; // 在所属函数 value_types 中的下标；invalid_index 表示尚未分配。
    bool operator==(const ValueId &) const = default;
};
// 通过函数 ID 和块下标找到一个基本块；块编号与值编号分别计算。
struct BlockId {
    IRFunctionId function; // 所属函数的 ID，用来检查是否误用了其他函数的块。
    std::size_t index = invalid_index; // 在所属函数 blocks 中的下标；invalid_index 表示尚未分配。
    bool operator==(const BlockId &) const = default;
};

// 保存一条指令使用的输入，可以是整数常量，也可以是参数或前面指令的结果。
struct Operand {
    Type type = Type::Void; // 这个操作数的 IR 类型；默认 Void 表示还没有可用的数据值。
    std::variant<std::monostate, uint32_t, ValueId> value; // 保存空值、常量位模式或 SSA 值 ID。

    // 用类型和常量的二进制位创建操作数，不分配值 ID，也不截断位模式。
    // 调用方应提供符合类型范围的位模式；i1 为 0/1，i8 不超过 0xff。
    static Operand constant(Type type, uint32_t bits);
    // 用类型和已有的值 ID 创建操作数，不在这里检查值是否已经定义。
    static Operand local(Type type, ValueId value_id);
    // 常量操作数返回其二进制位的地址，其他操作数返回 nullptr。
    const uint32_t *constant_bits() const {
        return std::get_if<uint32_t>(&value);
    }
    // SSA 操作数返回其值 ID 的地址，其他操作数返回 nullptr。
    const ValueId *value_id() const { return std::get_if<ValueId>(&value); }
    bool operator==(const Operand &) const = default;
};
// 判断操作数是否保存了当前支持且范围有效的常量（i1/i8/i32）。
bool valid_constant(const Operand &operand);

// Internal 表示函数只在当前模块可见，External 表示函数符号可以跨模块链接。
enum class Linkage { Internal, External };
// 保存函数的返回类型和参数类型，用来检查函数定义、调用和返回指令。
struct Signature {
    Type result = Type::Void; // 函数的返回类型；Void 表示没有返回数据。
    std::vector<Type> parameters; // 按声明顺序排列的参数类型，不允许 Void 参数。
    bool operator==(const Signature &) const = default;
};

// 内部调用的共享 ABI：声明、定义、调用和 return 都读取这一份描述。
// parameter_storage 与签名的形参表等长，只有按值聚合形参在对应位置保存自己的存储类型。
// result_storage 有值时，签名返回 Void，结果写入调用者提供的第一个 ptr 形参。
struct FunctionABI {
    Signature signature; // 降级后的 LLVM 返回类型和形参类型，包含隐藏结果指针。
    std::vector<StorageType> parameter_storage; // 每个降级形参的按值聚合存储；非聚合为 Unit。
    std::optional<StorageType> result_storage; // 聚合返回的调用者结果存储类型；直接返回为空。
    bool operator==(const FunctionABI &) const = default;
};
// 检查 ABI 自身的形状是否自洽，不访问模块类型表；具体布局由 check_module 再验证。
bool valid_function_abi(const FunctionABI &abi);

// 保存 call 指令要调用哪个函数、传入哪些参数，不保存调用结果。
struct Call {
    IRFunctionId function; // 被调用函数的 IR ID，通过模块查询函数名、返回类型和参数类型。
    std::vector<Operand> arguments; // 按形参顺序保存的实参，数量和类型必须与被调用函数匹配。
};
// 保存 alloca 指令要分配的存储类型和对齐值。
struct Alloca {
    StorageType storage_type; // 分配出的存储单元保存的存储类型，允许命名结构体和数组。
    uint32_t alignment; // 分配地址的对齐字节数，必须是非零的 2 的幂。
};
// 保存 getelementptr 指令：从基址按存储类型和下标计算字段或元素地址。
// 初版不保存 inbounds 等范围承诺；字段投影使用两个下标 {0, declaration_index}。
struct GetElementPtr {
    StorageType source_type; // 基址指向的存储类型，决定下标如何解释。
    Operand base; // 基址操作数，IR 类型必须为 ptr。
    std::vector<Operand> indices; // 依次为指针下标和字段/数组下标，均为 i32。
};
// 检查 GEP 至少包含指针下标和一次字段/元素投影，且各层下标适合当前存储类型。
bool valid_gep_indices(const Module &module, const StorageType &source_type,
                       const std::vector<Operand> &indices);
// 保存 load 指令从哪个地址读取哪种类型。
struct Load {
    Type storage_type; // 读取的 IR 类型；支持 i32、i8 和 ptr。
    Operand address; // 要读取的地址操作数，类型必须是 ptr。
    uint32_t alignment; // 地址的对齐字节数，必须是非零的 2 的幂。
};
// 保存 store 指令把哪个值写到哪个地址。
struct Store {
    Type storage_type; // 写入的 IR 类型；支持 i32、i8 和 ptr。
    Operand value; // 要写入的值。
    Operand address; // 目标地址操作数，类型必须是 ptr。
    uint32_t alignment; // 地址的对齐字节数，必须是非零的 2 的幂。
};
// 存储类型支持 i32、i8 和 ptr；对齐必须是非零的 2 的幂。
bool valid_storage(Type type, uint32_t alignment);
// 自有存储类型可用于 alloca：标量沿用上面的规则，聚合只要求合法对齐和完整结构。
bool valid_storage(const StorageType &type, uint32_t alignment);
// 保存 zext 指令：把较窄整数的无符号位模式扩展成较宽整数。
struct ZExt {
    Operand value; // 要扩展的输入值，类型必须是整数。
    Type target_type; // 扩展后的结果类型，位宽必须大于输入类型。
};
// 保存 trunc 指令：把较宽整数截断成较窄整数。
struct Trunc {
    Operand value; // 要截断的输入值，类型必须是整数。
    Type target_type; // 截断后的结果类型，位宽必须小于输入类型。
};
// 整数和 bool 的二元运算指令；移位操作数需要先掩码低 5 位。
enum class BinaryOp {
    Add,               // add
    Subtract,          // sub
    Multiply,          // mul
    SignedDivide,      // sdiv
    SignedRemainder,   // srem
    UnsignedDivide,    // udiv
    UnsignedRemainder, // urem
    And,               // and
    Or,                // or
    Xor,               // xor
    ShiftLeft,         // shl
    ArithmeticShiftRight, // ashr
    LogicalShiftRight, // lshr
};
// 二元指令在 LLVM 文本中的拼写。
const char *binary_op_name(BinaryOp operation);
// 判断二元指令允许的操作数和结果类型：算术/移位只接受 i32，位运算接受 i1/i32。
bool valid_binary_type(BinaryOp operation, Type type);

// 比较谓词区分有符号和无符号；选择依据是源类型，而不是 IR 中的 i32。
enum class Comparison {
    Equal,                     // eq
    NotEqual,                  // ne
    SignedLessThan,            // slt
    SignedLessThanOrEqual,     // sle
    SignedGreaterThan,         // sgt
    SignedGreaterThanOrEqual,  // sge
    UnsignedLessThan,          // ult
    UnsignedLessThanOrEqual,   // ule
    UnsignedGreaterThan,       // ugt
    UnsignedGreaterThanOrEqual // uge
};
// 比较谓词在 LLVM icmp 文本中的拼写，例如 slt 或 uge。
const char *comparison_name(Comparison comparison);
// 判断比较谓词能否用于该操作数类型：有符号谓词只用于 i32，bool 只能相等或无符号比较。
bool valid_comparison(Comparison comparison, Type operand_type);

// 保存一条二元运算的两个操作数；结果类型与操作数类型相同。
// 这里故意不保存 nsw/nuw/exact 等承诺：标量 baseline 按回绕执行，
// 没有证明时不能把这些源语义之外的假设写进 IR。
struct BinaryOperation {
    BinaryOp operation; // 要执行的 add/sub/mul/div/rem/and/or/xor/移位指令
    Operand left;       // 先求值的左操作数
    Operand right;      // 后求值的右操作数
};
// 保存一条比较指令；两侧类型相同，结果类型固定为 i1。
struct Compare {
    Comparison predicate; // 决定使用 eq/slt/ult 等哪一种比较
    Operand left;         // 先求值的左操作数
    Operand right;        // 后求值的右操作数
};
// 普通指令的具体操作：调用、入口分配、读写、二元运算、移位、比较和整数扩展/截断。
using Operation = std::variant<Call, Alloca, GetElementPtr, Load, Store, ZExt,
                               Trunc, BinaryOperation, Compare>;
// 保存一条普通指令的操作、结果类型，以及有返回数据时分配的结果 ID。
struct Instruction {
    std::optional<ValueId> result; // 结果值的 ID；无结果指令为空。
    Type type; // 结果的 IR 类型，无结果指令为 Void。
    Operation operation; // 这条指令的具体操作及操作数。
};

// 表示 ret 指令，结束当前函数，并返回一个值或不返回数据。
struct Return {
    std::optional<Operand> value; // 要返回的操作数，类型必须匹配函数返回类型；空值表示 ret void。
};
// 表示无条件跳转，当前块只沿这一条边进入目标块。
struct Branch {
    BlockId target; // 要进入的目标块，必须属于当前函数且不能是入口块。
};
// 表示条件跳转；条件必须是 i1，运行时会从两条边中选择一条。
struct CondBranch {
    Operand condition;  // 用于选择的 i1 条件，可以是常量或前面指令的结果。
    BlockId when_true;  // 条件为 true 时进入的目标块。
    BlockId when_false; // 条件为 false 时进入的目标块。
};
// 表示一条终结指令：ret、br 或 condbr。
using Terminator = std::variant<Return, Branch, CondBranch>;
// 保存一段顺序执行的指令，最后必须有一条结束该块的终结指令。
struct BasicBlock {
    BlockId id; // 这个块的 ID，用来查找块，并生成 bb_<下标> 形式的标签。
    std::vector<Instruction> instructions; // 按执行顺序排列的普通指令，不包含末尾的终结指令。
    std::optional<Terminator> terminator; // 结束这个块的指令；构建时可以暂缺，但模块检查通过前必须补上。
};

// 保存一个函数的名称、参数、返回类型和函数体；没有基本块时只有声明。
// 参数和指令结果使用同一张值类型表，基本块使用另一张表单独编号。
struct Function {
    // 保存函数身份、名称、签名和链接属性，并为参数预分配值 ID。
    Function(IRFunctionId function_id, std::string name, FunctionABI abi,
             Linkage linkage);
    IRFunctionId id; // 这个函数的 IR ID，在添加到模块时分配，不直接使用语义分析的 FunctionId。
    const std::string name; // 这个不可变的 LLVM 全局符号名同时用于函数表的名称索引。
    Linkage linkage; // 符号链接属性，决定是否仅在模块内部可见。
    FunctionABI abi; // 函数签名和按值聚合存储信息。
    std::vector<Operand> parameters; // 按声明顺序保存的参数操作数，它们占用本函数最前面的值编号。
    std::vector<Type> value_types; // 记录每个参数和指令结果的类型，ValueId.index 是这张表的下标。
    std::vector<Instruction> entry_allocas; // 固定分配总是在入口正文之前输出，不改变正文的求值顺序。
    std::vector<BasicBlock> blocks; // 按创建顺序保存的基本块；定义的第一个块为入口块。

    // 判断函数是否只有声明、没有函数体；内部函数不能一直保持声明状态。
    bool is_declaration() const { return blocks.empty(); }
    // 按参数序号返回对应的 IR 操作数，序号从 0 开始。
    Operand argument(std::size_t parameter_index) const {
        return parameters.at(parameter_index);
    }
    // 为新值分配 ID 并保存类型；调用方还需要把它记录为参数或指令结果。
    Operand new_value(Type type);
    // 添加一个空基本块并返回它的 ID，结束指令由调用方稍后添加。
    BlockId add_block();
    // 按 ID 返回基本块；块不属于本函数或下标越界时抛出异常。
    BasicBlock &block(BlockId block_id);
    const BasicBlock &block(BlockId block_id) const;
    // 检查操作数的类型和所属函数，不检查它是否在使用前已经定义。
    bool contains(const Operand &operand) const;
};

// 模块类型表中的一个命名结构体定义。
// 名称不带前导 %；字段是存储类型，bool 已映射为 i8，引用/Box 已映射为 ptr。
struct NamedType {
    std::string name; // LLVM 类型名，例如 __rx_struct_0。
    std::vector<StorageType> fields; // 按声明顺序保存的字段存储类型。
    bool defined = false; // 是否已经补齐字段定义；false 时不能用于布局或输出。
};

// 固定生成 RV32 的 32 位小端代码，不使用运行编译器的机器的数据布局。
inline constexpr const char *rv32_triple = "riscv32-unknown-unknown-elf";
inline constexpr const char *rv32_data_layout = "e-m:e-p:32:32-i64:64-n32-S128";

// 保存一次 IR 生成得到的全部函数和目标配置，供检查器、输出器和后端读取。
// 模块对象不能复制或移动；IRArtifact 通过移动指针来移交整个模块。
class Module {
public:
    // 创建模块，同时为它分配唯一编号。
    Module();
    // 模块不能被复制或移动，避免已有函数 ID 和指针失效。
    Module(const Module &) = delete;
    Module &operator=(const Module &) = delete;
    Module(Module &&) = delete;
    Module &operator=(Module &&) = delete;

    // 返回模块编号，用来检查函数是否属于这个模块。
    uint64_t identity() const { return identity_; }
    // 添加函数并分配函数和参数的 ID；名称不能重复，函数体随后再添加。
    IRFunctionId add_function(std::string name, Signature signature,
                              Linkage linkage);
    // 按共享 ABI 添加函数，保留按值聚合和隐藏结果指针信息。
    IRFunctionId add_function_abi(std::string name, FunctionABI abi,
                                  Linkage linkage);
    // 按函数名查找，找不到时返回 nullptr。
    Function *find_function(const std::string &name);
    const Function *find_function(const std::string &name) const;
    // 按 ID 返回函数；函数不属于本模块或下标无效时抛出 logic_error。
    Function &function(IRFunctionId function_id);
    const Function &function(IRFunctionId function_id) const;
    // 判断这个 ID 是否对应本模块中已经添加的函数。
    bool contains(IRFunctionId function_id) const;
    // 返回按添加顺序保存的函数表，不允许通过这个接口修改函数表本身。
    const auto &functions() const { return functions_; }

    // 登记一个命名结构体类型并返回它在类型表中的下标。
    // 先登记身份、后补字段，允许经 Box/Vec 引用尚未完成定义的结构体。
    std::size_t add_named_type(std::string name);
    // 为一个已登记的类型补齐按声明顺序排列的字段存储类型。
    void define_named_type(std::size_t index, std::vector<StorageType> fields);
    // 按登记顺序返回全部命名结构体类型。
    const std::vector<NamedType> &named_types() const { return named_types_; }
    // 按类型表下标返回命名结构体；下标越界时抛出 logic_error。
    const NamedType &named_type(std::size_t index) const;

    std::string target_triple = rv32_triple; // LLVM 目标三元组，指定生成代码使用的架构和目标环境。
    std::string data_layout = rv32_data_layout; // LLVM 数据布局字符串，描述目标的字节序、指针宽度和对齐规则。

private:
    uint64_t identity_; // 当前进程内唯一的模块编号，用来区分不同模块中下标相同的函数。
    std::vector<std::unique_ptr<Function>> functions_; // 按添加顺序保存并拥有函数；IRFunctionId.index 对应表下标。
                                                     // vector 扩容只移动指针，不移动函数对象，已有函数引用仍有效。
    std::unordered_map<std::string, Function *> functions_by_name_; // 函数名到函数对象的索引，不拥有函数对象。
    std::vector<NamedType> named_types_; // 按登记顺序保存的自有命名结构体类型。
};

// 判断函数名是否可以不加引号，直接写入 LLVM 文本。
bool valid_symbol_name(const std::string &name);

} // namespace rx::ir
