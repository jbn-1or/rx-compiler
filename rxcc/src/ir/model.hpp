// 本前端生成的 LLVM IR 子集。结构独立于 AST、sema 和 LLVM 开发库。
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

// P2 仅表示函数基础设施需要的两种类型；其他类型从 P3 开始增加。
enum class Type { Void, I32 };
const char *type_name(Type type);

inline constexpr std::size_t invalid_index = std::numeric_limits<std::size_t>::max();

// 模块身份只用于检查归属，不参与输出或命名；移动所属 artifact 不改变身份。
struct IRFunctionId {
    uint64_t module = 0;
    std::size_t index = invalid_index;
    bool operator==(const IRFunctionId &) const = default;
};
struct ValueId {
    IRFunctionId function;
    std::size_t index = invalid_index;
    bool operator==(const ValueId &) const = default;
};
struct BlockId {
    IRFunctionId function;
    std::size_t index = invalid_index;
    bool operator==(const BlockId &) const = default;
};

struct Operand {
    Type type = Type::Void;
    std::variant<std::monostate, uint32_t, ValueId> value;

    static Operand constant(Type type, uint32_t bits);
    static Operand local(Type type, ValueId id);
    const uint32_t *constant_bits() const { return std::get_if<uint32_t>(&value); }
    const ValueId *value_id() const { return std::get_if<ValueId>(&value); }
    bool operator==(const Operand &) const = default;
};
bool valid_constant(const Operand &operand);

enum class Linkage { Internal, External };
struct Signature {
    Type result = Type::Void;
    std::vector<Type> parameters;
    bool operator==(const Signature &) const = default;
};

struct Call { IRFunctionId function; std::vector<Operand> arguments; };
using Operation = std::variant<Call>;
struct Instruction {
    std::optional<ValueId> result;
    Type type;
    Operation operation;
};

struct Return { std::optional<Operand> value; };
using Terminator = std::variant<Return>;
struct BasicBlock {
    BlockId id;
    std::vector<Instruction> instructions;
    std::optional<Terminator> terminator;
};

struct Function {
    IRFunctionId id;
    std::string name;
    Linkage linkage;
    Signature signature;
    std::vector<Operand> parameters;
    std::vector<Type> value_types;
    std::vector<BasicBlock> blocks;

    bool is_declaration() const { return blocks.empty(); }
    Operand argument(std::size_t index) const { return parameters.at(index); }
    Operand new_value(Type type);
    BlockId add_block();
    BasicBlock &block(BlockId id);
    const BasicBlock &block(BlockId id) const;
    bool contains(const Operand &operand) const;
};

inline constexpr const char *rv32_triple = "riscv32-unknown-unknown-elf";
inline constexpr const char *rv32_data_layout = "e-m:e-p:32:32-i64:64-n32-S128";

class Module {
public:
    explicit Module(std::string name);
    Module(const Module &) = delete;
    Module &operator=(const Module &) = delete;
    Module(Module &&) = delete;
    Module &operator=(Module &&) = delete;

    uint64_t identity() const { return identity_; }
    IRFunctionId add_function(std::string name, Signature signature, Linkage linkage);
    Function *find_function(const std::string &name);
    const Function *find_function(const std::string &name) const;
    Function &function(IRFunctionId id);
    const Function &function(IRFunctionId id) const;
    bool contains(IRFunctionId id) const;
    const auto &functions() const { return functions_; }

    std::string name;
    std::string target_triple = rv32_triple;
    std::string data_layout = rv32_data_layout;

private:
    uint64_t identity_;
    // 函数引用在登记后续函数时仍保持有效；跨层映射使用稳定身份。
    std::vector<std::unique_ptr<Function>> functions_;
};

bool valid_symbol_name(const std::string &name);

} // namespace rx::ir
