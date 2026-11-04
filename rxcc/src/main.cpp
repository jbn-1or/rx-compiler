// 读源文件 → ANTLR 词法/语法分析 → AST 构建 → 语义检查或最小 IR 生成。
//   --stage parse    语法分析为止（0 接受 / 1 拒绝）
//   --stage ast      AST 构建并 dump（默认）
//   --stage semantic AST 构建后做静态语义检查，不 dump（0 接受 / 1 拒绝）
//   --stage ir      语义检查后生成 LLVM IR，需要 -o <file.ll>
//   --dump-tree      打印 ANTLR parse tree
// 退出码：0 = 当前阶段接受或显示帮助；1 = 词法、语法、AST 构建或语义错误；
// 2 = 命令行用法错误、源文件无法打开或输出文件无法打开。

// 读文件 → ANTLRInputStream → RxLexer → CommonTokenStream → RxParser::crate。
// 词法或语法检查报错后，不再构建 AST。

// python3 tests/custom/run_semantic.py    # 运行官方语义测试，按退出码判定接受/拒绝
// ./rxcc/build/rxcc path                  # 默认 --stage ast：构建并 dump AST
// ./rxcc/build/rxcc --stage parse path    # 只做语法分析
// ./rxcc/build/rxcc --stage semantic path # AST 之上做语义检查（不 dump）
// ./rxcc/build/rxcc --stage ir path -o path.ll # 生成经过验证的 LLVM IR
// ./rxcc/build/rxcc --dump-tree path      # 附带打印 ANTLR parse tree
// cmake --build rxcc/build -j

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "ir/generator.hpp"
#include "RxLexer.h"
#include "RxParser.h"
#include "antlr4-runtime.h"

#include "ast/ast.hpp"
#include "ast/dump.hpp"
#include "ast_builder.hpp"
#include "diagnostics.hpp"
#include "sema/sema.hpp"
#include <llvm/IR/Module.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Verifier.h>

namespace {

void usage(const char *prog) {
    std::cerr << "usage: " << prog
              << " [--stage parse|ast|semantic|ir] [--dump-tree] <source.rx> [-o <file.ll>]\n"
              << "options:\n"
              << "  --stage <name>    parse（只做语法分析）| ast（默认，dump AST）"
                  "| semantic（语义检查）| ir（生成 LLVM IR）\n"
               << "  -o <file.ll>      IR 输出路径，仅 --stage ir 必须指定\n"
              << "  --dump-tree       打印 ANTLR parse tree\n"
              << "  -h, --help        显示本帮助\n";
}

std::string read_file(const std::string &path, bool &ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ok = false;
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    ok = true;
    return buffer.str();
}

// lexer 和 parser 共用此监听器，将 syntaxError 回调写入 Diagnostics。
// 注册前移除各自的默认监听器，避免诊断同时直接输出到控制台。
class CollectErrors : public antlr4::BaseErrorListener {
public:
    explicit CollectErrors(rx::Diagnostics &diags) : diagnostics_(diags) {}

    void syntaxError(antlr4::Recognizer * /*recognizer*/,
                     antlr4::Token * /*offendingSymbol*/, size_t line,
                     size_t charPositionInLine, const std::string &msg,
                     std::exception_ptr /*e*/) override {
        // 统一将回调位置记为一列宽的 [start, end) 区间，不依赖错误 token。
        const rx::Position at{static_cast<int>(line),
                              static_cast<int>(charPositionInLine)};
        diagnostics_.error(
            rx::Span{at, rx::Position{at.line, at.column + 1}},
            "syntax error: " + msg);
    }

private:
    rx::Diagnostics &diagnostics_;
};

void print_diagnostics(const rx::Diagnostics &diags, const std::string &path,
                       std::ostream &out) {
    for (const auto &d : diags.items) {
        out << path << ':' << d.span.start.line << ':' << d.span.start.column
            << ": " << d.message << '\n';
    }
}

} // namespace

int main(int argc, char **argv) {
    std::string stage = "ast"; // 未指定 --stage 时，构建并打印 AST。
    std::string source_path;
    std::string output_path;
    bool dump_tree = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--stage" && i + 1 < argc) {
            stage = argv[++i];
        } else if (arg == "-o" && i + 1 < argc) {
            if (!output_path.empty()) {
                std::cerr << "rxcc: -o may only be specified once\n";
                usage(argv[0]);
                return 2;
            }
            output_path = argv[++i];
        } else if (arg == "--dump-tree") {
            dump_tree = true;
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "rxcc: unknown option: " << arg << "\n";
            usage(argv[0]);
            return 2;
        } else if (source_path.empty()) {
            source_path = arg;
        } else {
            std::cerr << "rxcc: unexpected extra argument: " << arg << "\n";
            usage(argv[0]);
            return 2;
        }
    }

    if (stage != "parse" && stage != "ast" && stage != "semantic" && stage != "ir") {
        std::cerr << "rxcc: unknown stage: " << stage << "\n";
        usage(argv[0]);
        return 2;
    }
    if (stage == "ir" && output_path.empty()) {
        std::cerr << "rxcc: --stage ir requires -o <file.ll>\n";
        usage(argv[0]);
        return 2;
    }
    if (stage != "ir" && !output_path.empty()) {
        std::cerr << "rxcc: -o is only valid with --stage ir\n";
        usage(argv[0]);
        return 2;
    }
    if (source_path.empty()) {
        usage(argv[0]);
        return 2;
    }

    bool ok = false;
    const std::string source = read_file(source_path, ok);
    if (!ok) {
        std::cerr << "rxcc: cannot open source file: " << source_path << "\n";
        return 2;
    }

    // --- 前端：词法 + 语法 ---------------------------------------------------
    rx::Diagnostics diags;
    CollectErrors collector(diags);

    antlr4::ANTLRInputStream input(source);
    RxLexer lexer(&input);
    lexer.removeErrorListeners();
    lexer.addErrorListener(&collector);

    antlr4::CommonTokenStream tokens(&lexer);
    RxParser parser(&tokens);
    parser.removeErrorListeners();
    parser.addErrorListener(&collector);

    antlr4::tree::ParseTree *tree = parser.crate();

    if (dump_tree) {
        std::cout << antlr4::tree::Trees::toStringTree(tree, &parser) << "\n";
    }

    // 词法或语法检查报错：打印诊断，跳过 AST 构建并退出 1。
    if (!diags.ok()) {
        print_diagnostics(diags, source_path, std::cerr);
        return 1;
    }

    if (stage == "parse") {
        return 0; // --stage parse：到此为止
    }

    // 词法、语法检查成功后，递归构建 AST。
    // 构建失败（builder 返回空 / 诊断非空）→ 打印诊断，退出 1。
    rx::AstBuilder builder(diags);
    std::shared_ptr<rx::Crate> crate =
        builder.build(static_cast<RxParser::CrateContext *>(tree));
    if (!crate || !diags.ok()) {
        print_diagnostics(diags, source_path, std::cerr);
        return 1;
    }

    if (stage == "semantic" || stage == "ir") {
        // 两个阶段都先完成整份源码的语义检查。
        // 临时分析器在本句结束时析构；驱动持有结果，crate 比结果更晚析构。
        auto semantic_result = rx::SemanticAnalyzer(diags).analyze(*crate);
        if (!semantic_result) {
            print_diagnostics(diags, source_path, std::cerr);
            return 1;
        }
        if (stage == "ir") {
            auto artifact = rx::generate_ir(*crate, *semantic_result, llvm::errs());
            if (!artifact)
                return 1;

            // 先验证完整模块，再创建/截断输出文件，避免校验失败破坏旧文件。
            if (llvm::verifyModule(artifact->module(), &llvm::errs()))
                return 1;
            std::error_code error;
            llvm::raw_fd_ostream output(output_path, error, llvm::sys::fs::OF_Text);
            if (error) {
                std::cerr << "rxcc: cannot open IR output '" << output_path
                          << "': " << error.message() << '\n';
                return 2;
            }
            artifact->module().print(output, nullptr);
            output.flush();
            if (output.has_error()) {
                std::cerr << "rxcc: failed writing IR output '" << output_path << "'\n";
                output.clear_error();
                return 1;
            }
            return 0;
        }
        return 0;
    }

    // --stage ast（默认）：dump AST。
    dump(*crate, std::cout);
    return 0;
}
