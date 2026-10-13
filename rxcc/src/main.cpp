// 读源文件 → ANTLR 词法 + 语法分析 → 诊断收集 → 退出码。
//   --stage parse   语法分析为止（0 接受 / 1 拒绝）
//   --stage ast     AST 构建并 dump（默认）
//   --dump-tree     打印 ANTLR parse tree
// 退出码契约：0 = 接受；1 = 拒绝（语法错误）；
// 2 = 用法错误或 IO 失败。

// 读文件 → ANTLRInputStream → RxLexer → CommonTokenStream → RxParser::crate。
// 有任何语法错误就不进构建

// tests/custom/run.sh              # 自研验证点：12 个
// python3 tests/custom/run_semantic.py   # official semantic：当前 69/236

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

namespace {

void usage(const char *prog) {
    std::cerr << "usage: " << prog
              << " [--stage parse|ast] [--dump-tree] <source.rx>\n"
              << "options:\n"
              << "  --stage <name>    parse（只做语法分析）| ast（默认）\n"
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

// 把 ANTLR 的 syntaxError 回调收进 Diagnostics（Lexer.g4/Parser.g4
// 的错误监听契约）。 lexer 与 parser 各挂一个实例；先 removeErrorListeners
// 摘掉默认控制台监听器。
class CollectErrors : public antlr4::BaseErrorListener {
public:
    explicit CollectErrors(rx::Diagnostics &diags) : diags_(diags) {}

    void syntaxError(antlr4::Recognizer * /*recognizer*/,
                     antlr4::Token * /*offendingSymbol*/, size_t line,
                     size_t charPositionInLine, const std::string &msg,
                     std::exception_ptr /*e*/) override {
        // 词法错误无 token 可指：报点位上的零宽 span（列 +1 成最小区间）。
        const rx::Position at{static_cast<int>(line),
                              static_cast<int>(charPositionInLine)};
        diags_.error(rx::Span{at, rx::Position{at.line, at.column + 1}},
                     "syntax error: " + msg);
    }

private:
    rx::Diagnostics &diags_;
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
    std::string stage = "ast"; // 设计 §10：--stage 默认 ast
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

    if (stage != "parse" && stage != "ast") {
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

    // 有任何语法错误就不进构建：打印诊断，退出 1。
    if (!diags.ok()) {
        print_diagnostics(diags, source_path, std::cerr);
        return 1;
    }

    if (stage == "parse") {
        return 0; // --stage parse：到此为止
    }

    // --stage ast（默认）：语法干净 → AST 构建器构建 → dump。
    // 构建失败（builder 返回空 / 诊断非空）→ 打印诊断，退出 1。
    rx::AstBuilder builder(diags);
    std::shared_ptr<rx::Crate> crate =
        builder.build(static_cast<RxParser::CrateContext *>(tree));
    if (!crate || !diags.ok()) {
        print_diagnostics(diags, source_path, std::cerr);
        return 1;
    }
    dump(*crate, std::cout);
    return 0;
}
