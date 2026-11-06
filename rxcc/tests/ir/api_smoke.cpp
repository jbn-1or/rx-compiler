// 独立验证 LLVM API、模块生命周期和输出检查，不从 Rx 源码生成 IR。

#include "ir/artifact.hpp"
#include "ir/storage.hpp"
#include "ir/value.hpp"
#include "ast/ast.hpp"
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

// 只允许在尚未结束的块中插入指令，不把 builder 的插入位置视为保证。
void check_open_block(const llvm::IRBuilder<> &builder) {
    const auto *block = builder.GetInsertBlock();
    check(block && !block->getTerminator(), "当前基本块不能继续插入指令");
}

void check_value_storage(rx::IRArtifact &artifact) {
    auto &context = artifact.context();
    auto &module = artifact.module();
    llvm::IRBuilder<> builder(context);
    auto *type = llvm::FunctionType::get(builder.getVoidTy(), {builder.getInt1Ty()}, false);
    auto *function = llvm::Function::Create(
        type, llvm::Function::InternalLinkage, "storage_boundaries", module);
    auto *block = llvm::BasicBlock::Create(context, "entry", function);
    builder.SetInsertPoint(block);

    const std::array<rx::SemanticType, 4> integer_types = {
        rx::SemanticType::int_(rx::SemanticType::IntKind::I32),
        rx::SemanticType::int_(rx::SemanticType::IntKind::U32),
        rx::SemanticType::int_(rx::SemanticType::IntKind::Isize),
        rx::SemanticType::int_(rx::SemanticType::IntKind::Usize),
    };
    for (const auto &source_type : integer_types) {
        auto *computed = rx::ir::value_type(context, source_type, llvm::errs());
        check(computed && computed->isIntegerTy(32),
              "四种源整数的 LLVM 计算类型都应为 i32");
        auto *address = builder.CreateAlloca(computed);
        auto place = rx::ir::make_place(address, source_type, llvm::errs());
        check(place && place->storage_type->isIntegerTy(32) &&
                  place->alignment == llvm::Align(4),
              "整数位置应使用 i32 存储并按 4 字节对齐");
        check(rx::ir::store_value(
                  builder, *place,
                  rx::ir::IRValue{builder.getInt32(42), source_type}, llvm::errs()),
              "整数 store 应接受与源类型一致的 i32 值");
        auto loaded = rx::ir::load_value(builder, *place, llvm::errs());
        check(loaded && loaded->value->getType()->isIntegerTy(32) &&
                  loaded->source_type.same_type(source_type) &&
                  llvm::cast<llvm::LoadInst>(loaded->value)->getAlign() == llvm::Align(4),
              "整数 load 应返回 i32 并保留具体源类型");
    }

    const auto boolean = rx::SemanticType::bool_();
    auto *computed_bool = rx::ir::value_type(context, boolean, llvm::errs());
    check(computed_bool && computed_bool->isIntegerTy(1), "bool 计算类型应为 i1");
    auto *bool_address = builder.CreateAlloca(llvm::Type::getInt8Ty(context));
    auto bool_place = rx::ir::make_place(bool_address, boolean, llvm::errs());
    check(bool_place && bool_place->storage_type->isIntegerTy(8) &&
              bool_place->alignment == llvm::Align(1),
          "bool 位置应使用 i8 存储并按 1 字节对齐");
    check(rx::ir::store_value(builder, *bool_place,
                              rx::ir::IRValue{&*function->arg_begin(), boolean}, llvm::errs()),
          "bool store 应接受 i1 值");
    auto bool_loaded = rx::ir::load_value(builder, *bool_place, llvm::errs());
    check(bool_loaded && bool_loaded->value->getType()->isIntegerTy(1) &&
              llvm::isa<llvm::TruncInst>(bool_loaded->value),
          "bool load 应从 i8 还原 i1");
    bool saw_bool_extension = false;
    bool saw_aligned_bool_store = false;
    bool saw_aligned_bool_load = false;
    for (const auto &instruction : *block) {
        if (const auto *extension = llvm::dyn_cast<llvm::ZExtInst>(&instruction))
            saw_bool_extension |= extension->getSrcTy()->isIntegerTy(1) &&
                                  extension->getDestTy()->isIntegerTy(8);
        if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
            saw_aligned_bool_store |= store->getPointerOperand() == bool_address &&
                                      store->getAlign() == llvm::Align(1) &&
                                      store->getValueOperand()->getType()->isIntegerTy(8);
        if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
            saw_aligned_bool_load |= load->getPointerOperand() == bool_address &&
                                     load->getAlign() == llvm::Align(1) &&
                                     load->getType()->isIntegerTy(8);
    }
    check(saw_bool_extension && saw_aligned_bool_store && saw_aligned_bool_load,
          "bool 存储应扩展为 i8，load/store 应按 1 字节对齐");

    std::string errors;
    llvm::raw_string_ostream diagnostics(errors);
    check(!rx::ir::store_value(
              builder, *bool_place,
              rx::ir::IRValue{builder.getInt32(1), boolean}, diagnostics),
          "bool 存储不得接收 LLVM i32 值");
    check(!rx::ir::make_place(bool_address, rx::SemanticType::unit(), diagnostics),
          "unit 不应被伪造为有数据的存储位置");
    check(!rx::ir::value_type(context,
                              rx::SemanticType::reference(false, integer_types[0]),
                              diagnostics),
          "引用表示应留待后续阶段");
    check(!errors.empty(), "拒绝不支持或类型不匹配的存取时应说明原因");

    rx::ir::EmitResult unit = rx::ir::Unit{};
    rx::ir::EmitResult no_continuation = rx::ir::NoContinuation{};
    rx::ir::PlaceResult no_place_continuation = rx::ir::NoContinuation{};
    check(std::holds_alternative<rx::ir::Unit>(unit) &&
              !std::holds_alternative<rx::ir::IRValue>(unit) &&
              std::holds_alternative<rx::ir::NoContinuation>(no_continuation) &&
              !std::holds_alternative<rx::ir::IRValue>(no_continuation) &&
              std::holds_alternative<rx::ir::NoContinuation>(no_place_continuation),
          "unit 和已结束路径不得伪造 LLVM 数据值或存储位置");

    builder.CreateRetVoid();
    builder.ClearInsertionPoint();
}

void check_storage_insertion_points(rx::IRArtifact &artifact) {
    auto &context = artifact.context();
    llvm::IRBuilder<> builder(context);
    auto *function = llvm::Function::Create(
        llvm::FunctionType::get(builder.getVoidTy(), {builder.getInt1Ty()}, false),
        llvm::Function::InternalLinkage, "rejected_storage", artifact.module());
    auto *block = llvm::BasicBlock::Create(context, "entry", function);
    builder.SetInsertPoint(block);
    const auto integer = rx::SemanticType::int_(rx::SemanticType::IntKind::I32);
    const auto boolean = rx::SemanticType::bool_();
    const std::array places{
        rx::ir::make_place(builder.CreateAlloca(builder.getInt32Ty()), integer, llvm::errs()),
        rx::ir::make_place(builder.CreateAlloca(builder.getInt8Ty()), boolean, llvm::errs()),
    };
    const std::array values{
        rx::ir::IRValue{builder.getInt32(42), integer},
        rx::ir::IRValue{&*function->arg_begin(), boolean},
    };
    auto *terminator = builder.CreateRetVoid();
    const auto instruction_count = block->size();
    for (bool has_insertion_block : {false, true}) {
        if (has_insertion_block)
            builder.SetInsertPoint(block);
        else
            builder.ClearInsertionPoint();
        const auto *insert_block = builder.GetInsertBlock();
        const auto insert_point = builder.GetInsertPoint();
        for (std::size_t i = 0; i < places.size(); ++i) {
            check(places[i].has_value(), "回归测试必须使用有效的整数和 bool 位置");
            std::string errors;
            llvm::raw_string_ostream diagnostics(errors);
            check(!rx::ir::load_value(builder, *places[i], diagnostics) &&
                      !errors.empty() && block->size() == instruction_count,
                  "无插入块或已终结块中的 load 必须诊断失败且不发射指令");
            errors.clear();
            check(!rx::ir::store_value(builder, *places[i], values[i], diagnostics) &&
                      !errors.empty() && block->size() == instruction_count,
                  "无插入块或已终结块中的 store 不得发射 zext 或 store");
            check(builder.GetInsertBlock() == insert_block &&
                      builder.GetInsertPoint() == insert_point &&
                      block->getTerminator() == terminator,
                  "存取失败必须保留插入点和原来的终结指令");
        }
    }
}

void check_temporary_slots(rx::IRArtifact &artifact) {
    auto &context = artifact.context();
    llvm::IRBuilder<> builder(context);
    auto *function_type = llvm::FunctionType::get(
        builder.getVoidTy(), {builder.getInt1Ty()}, false);
    auto *function = llvm::Function::Create(
        function_type, llvm::Function::InternalLinkage, "temporary_slots", artifact.module());
    auto *entry = llvm::BasicBlock::Create(context, "entry", function);
    auto *body = llvm::BasicBlock::Create(context, "loop.body", function);
    auto *exit = llvm::BasicBlock::Create(context, "exit", function);
    rx::ir::FunctionStorage storage(*function);
    const auto integer = rx::SemanticType::int_(rx::SemanticType::IntKind::I32);
    const auto boolean = rx::SemanticType::bool_();
    builder.SetInsertPoint(entry);
    auto local = storage.create_local(rx::LocalId{0}, integer, llvm::errs());
    check(local && rx::ir::store_value(builder, *local,
                                      {builder.getInt32(7), integer}, llvm::errs()),
          "临时存储测试应先在入口初始化局部绑定");
    auto *initialization = &entry->back();
    builder.CreateBr(body);
    auto *entry_terminator = entry->getTerminator();
    builder.SetInsertPoint(body);
    const auto insertion = builder.GetInsertPoint();

    rx::IntegerLiteral first_site{}, second_site{}, unsupported_site{};
    rx::BooleanLiteral bool_site{};
    auto first = storage.temporary(first_site, integer, llvm::errs());
    auto second = storage.temporary(second_site, integer, llvm::errs());
    auto flag = storage.temporary(bool_site, boolean, llvm::errs());
    check(first && second && flag && first->address != second->address &&
              first->address != local->address && flag->storage_type->isIntegerTy(8) &&
              flag->alignment == llvm::Align(1),
          "不同静态位置和局部绑定应使用独立 slot，bool 临时值使用 i8");
    check(builder.GetInsertBlock() == body && builder.GetInsertPoint() == insertion &&
              body->empty() && entry->getTerminator() == entry_terminator,
          "入口已有执行指令和 terminator 时仍能分配，不移动正文插入点或初始化临时值");
    std::size_t allocations = 0;
    bool execution_seen = false;
    for (const auto &instruction : *entry) {
        if (const auto *allocation = llvm::dyn_cast<llvm::AllocaInst>(&instruction)) {
            check(!execution_seen && allocation->getParent() == entry,
                  "临时 alloca 必须加入入口的连续 alloca 区");
            ++allocations;
        } else {
            execution_seen = true;
            check(&instruction == initialization || &instruction == entry_terminator,
                  "分配临时位置不得添加初始化或移动既有执行指令");
        }
    }
    check(allocations == 4, "局部位置和三个临时位置各分配一个 slot");
    const auto entry_size = entry->size();
    for (int request = 0; request < 3; ++request) {
        const auto reused = storage.temporary(first_site, integer, llvm::errs());
        check(reused && reused->address == first->address && entry->size() == entry_size,
              "重复生成同一静态临时位置必须复用入口 slot");
        check(rx::ir::store_value(builder, *reused,
                                  {builder.getInt32(request), integer}, llvm::errs()),
              "临时值初始化应由正文 builder 在当前执行位置写入");
    }
    for (const auto &instruction : *body)
        check(llvm::isa<llvm::StoreInst>(&instruction),
              "循环体中的临时初始化不得分配 alloca");
    check(storage.local(rx::LocalId{0})->address == local->address &&
              !storage.local(rx::LocalId{1}),
          "已有局部直接返回原位置，不物化副本");

    std::string errors;
    llvm::raw_string_ostream diagnostics(errors);
    check(!storage.temporary(first_site,
                              rx::SemanticType::int_(rx::SemanticType::IntKind::U32),
                              diagnostics) &&
              !storage.temporary(unsupported_site, rx::SemanticType::unit(), diagnostics) &&
              !storage.create_local(rx::LocalId{0}, integer, diagnostics) &&
              !storage.create_local(rx::LocalId{}, integer, diagnostics) &&
              entry->size() == entry_size && !errors.empty(),
          "临时类型改变、unit 和无效/重复局部身份应拒绝，且不添加 slot");
    builder.CreateCondBr(&*function->arg_begin(), body, exit);
    builder.SetInsertPoint(exit);
    builder.CreateRetVoid();
    builder.ClearInsertionPoint();

    auto *other = llvm::Function::Create(
        function_type, llvm::Function::InternalLinkage, "other_temporary_slots",
        artifact.module());
    auto *other_entry = llvm::BasicBlock::Create(context, "entry", other);
    rx::ir::FunctionStorage other_storage(*other);
    auto isolated = other_storage.temporary(first_site, integer, llvm::errs());
    check(isolated && isolated->address != first->address &&
              llvm::cast<llvm::AllocaInst>(isolated->address)->getFunction() == other &&
              !builder.GetInsertBlock(),
          "临时缓存必须属于单一函数且不依赖正文 builder 的插入点");
    builder.SetInsertPoint(other_entry);
    builder.CreateRetVoid();
    builder.ClearInsertionPoint();
}

rx::IRArtifact make_module(bool invalid) {
    rx::IRArtifact artifact("rx-api-smoke");
    auto &context = artifact.context();
    auto &module = artifact.module();
    llvm::IRBuilder<> builder(context);

    auto *source_type = llvm::FunctionType::get(builder.getVoidTy(), false);
    auto *source = llvm::Function::Create(
        source_type, llvm::Function::InternalLinkage, "__rx_fn_0", module);
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", source));
    if (!invalid) {
        check_open_block(builder);
        builder.CreateRetVoid();
        builder.ClearInsertionPoint();
    }

    auto *entry_type = llvm::FunctionType::get(builder.getInt32Ty(), false);
    auto *entry = llvm::Function::Create(
        entry_type, llvm::Function::ExternalLinkage, "main", module);
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", entry));
    check_open_block(builder);
    builder.CreateCall(source);
    check_open_block(builder);
    builder.CreateRet(builder.getInt32(0));
    builder.ClearInsertionPoint();
    return artifact;
}

void run_tests() {
    static_assert(std::is_nothrow_move_constructible_v<rx::IRArtifact>);
    static_assert(!std::is_move_assignable_v<rx::IRArtifact>);
    auto artifact = make_module(false);
    const auto *context = &artifact.context();
    const auto *module = &artifact.module();
    rx::IRArtifact moved(std::move(artifact));
    check(&moved.context() == context && &moved.module() == module,
          "移动后 context 和 module 的地址应保持不变");
    check(moved.module().getTargetTriple().str() == "riscv32-unknown-unknown-elf" &&
              moved.module().getDataLayoutStr() == "e-m:e-p:32:32-i64:64-n32-S128",
          "目标和数据布局应与 RV32 一致");
    check_value_storage(moved);
    check_storage_insertion_points(moved);
    check_temporary_slots(moved);
    std::string text, errors;
    llvm::raw_string_ostream output(text), diagnostics(errors);
    check(moved.verify_and_print(output, diagnostics), "合法模块应通过检查");
    output.flush();
    diagnostics.flush();
    check(errors.empty() && text.find("define i32 @main()") != std::string::npos,
          "合法模块应输出入口且没有检查错误");

    auto invalid = make_module(true);
    std::string bad_text, bad_errors;
    llvm::raw_string_ostream bad_output(bad_text), bad_diagnostics(bad_errors);
    check(!invalid.verify_and_print(bad_output, bad_diagnostics),
          "缺少结束指令的模块必须被拒绝");
    bad_output.flush();
    bad_diagnostics.flush();
    check(bad_text.empty() && !bad_errors.empty(), "非法模块不得输出 IR，且应有诊断");

    llvm::IRBuilder<> builder(moved.context());
    bool rejected = false;
    try {
        check_open_block(builder);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    check(rejected, "没有插入位置时不得发出指令");
    builder.SetInsertPoint(&moved.module().getFunction("main")->getEntryBlock());
    rejected = false;
    try {
        check_open_block(builder);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    check(rejected, "已结束的基本块不得继续插入指令");
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--test") {
        run_tests();
        return 0;
    }
    if (argc > 2 || (argc == 2 && std::string_view(argv[1]) != "--invalid"))
        return 2;
    auto artifact = make_module(argc == 2);
    return artifact.verify_and_print(llvm::outs(), llvm::errs()) ? 0 : 1;
}
