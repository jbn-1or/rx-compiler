// 读源文件 → ANTLR 词法/语法分析 → AST 构建 → 可选语义检查。
//   --stage parse    语法分析为止（0 接受 / 1 拒绝）
//   --stage ast      AST 构建并 dump（默认）
//   --stage semantic AST 构建后做静态语义检查，不 dump（0 接受 / 1 拒绝）
//   --dump-tree      打印 ANTLR parse tree
// 退出码：0 = 当前阶段接受或显示帮助；1 = 词法、语法、AST 构建或语义错误；
// 2 = 命令行用法错误或源文件无法打开。

// 读文件 → ANTLRInputStream → RxLexer → CommonTokenStream → RxParser::crate。
// 词法或语法检查报错后，不再构建 AST。

// python3 tests/custom/run_semantic.py    # 运行官方语义测试，按退出码判定接受/拒绝
// ./rxcc/build/rxcc path                  # 默认 --stage ast：构建并 dump AST
// ./rxcc/build/rxcc --stage parse path    # 只做语法分析
// ./rxcc/build/rxcc --stage semantic path # AST 之上做语义检查（不 dump）
// ./rxcc/build/rxcc --dump-tree path      # 附带打印 ANTLR parse tree
// cmake --build rxcc/build -j

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "RxLexer.h"
#include "RxParser.h"
#include "antlr4-runtime.h"

#include "ast/ast.hpp"
#include "ast/dump.hpp"
#include "ast_builder.hpp"
#include "diagnostics.hpp"
#include "sema/sema.hpp"

namespace {

void usage(const char *prog) {
    std::cerr << "usage: " << prog
              << " [--stage parse|ast|semantic] [--dump-tree] <source.rx>\n"
              << "options:\n"
              << "  --stage <name>    parse（只做语法分析）| ast（默认，dump AST）"
                 "| semantic（语义检查）\n"
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
    bool dump_tree = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--stage" && i + 1 < argc) {
            stage = argv[++i];
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

    if (stage != "parse" && stage != "ast" && stage != "semantic") {
        std::cerr << "rxcc: unknown stage: " << stage << "\n";
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

    if (stage == "semantic") {
        // --stage semantic：在 AST 上做静态语义检查，不 dump。
        rx::SemanticAnalyzer analyzer(diags);
        analyzer.run(*crate);
        if (!diags.ok()) {
            print_diagnostics(diags, source_path, std::cerr);
            return 1;
        }
        return 0;
    }

    // --stage ast（默认）：dump AST。
    dump(*crate, std::cout);
    return 0;
}
