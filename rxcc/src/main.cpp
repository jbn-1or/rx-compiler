// rxcc — the Rx compiler driver (stage-1 skeleton).
//
// Current capability: lex + parse a .rx source file and print the ANTLR parse
// tree. This is the first milestone of stage 1 ("get a walkable parse tree");
// AST construction, name resolution, type checking and LLVM IR emission come
// next and will hang off the same driver.
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "RxLexer.h"
#include "RxParser.h"
#include "antlr4-runtime.h"

namespace {

void usage(const char *prog) {
    std::cerr
        << "usage: " << prog << " [options] <source.rx>\n"
        << "options:\n"
        << "  --stage <name>   one of: parse (default), semantic, ir, codegen\n"
        << "  --dump-tree      print the ANTLR parse tree\n"
        << "  -o <file>        output file (reserved for later stages)\n"
        << "  -h, --help       show this help\n";
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

} // namespace

int main(int argc, char **argv) {
    std::string stage = "parse";
    std::string source_path;
    std::string output_path;
    bool dump_tree = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--stage" && i + 1 < argc) {
            stage = argv[++i];
        } else if (arg == "--dump-tree") {
            dump_tree = true;
        } else if (arg == "-o" && i + 1 < argc) {
            output_path = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "rxcc: unknown option: " << arg << "\n";
            usage(argv[0]);
            return 2;
        } else {
            source_path = arg;
        }
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

    // --- Front end: lex + parse -------------------------------------------
    antlr4::ANTLRInputStream input(source);
    RxLexer lexer(&input);
    antlr4::CommonTokenStream tokens(&lexer);
    RxParser parser(&tokens);

    antlr4::tree::ParseTree *tree = parser.crate();

    const std::size_t lex_errors = lexer.getNumberOfSyntaxErrors();
    const std::size_t parse_errors = parser.getNumberOfSyntaxErrors();
    const bool syntax_ok = (lex_errors == 0 && parse_errors == 0);

    // --- Output -----------------------------------------------------------
    if (stage == "parse" || stage == "tree") {
        if (dump_tree || true) {
            std::cout << antlr4::tree::Trees::toStringTree(tree, &parser, true)
                      << "\n";
        }
    } else {
        std::cerr << "rxcc: stage '" << stage
                  << "' is not implemented yet (only 'parse' works today)\n";
        return 2;
    }

    if (!output_path.empty()) {
        // Reserved: later stages will write IR/assembly here.
    }

    std::cerr << "rxcc: " << (syntax_ok ? "parsed OK" : "syntax errors")
              << " — " << source_path << " (" << parse_errors
              << " parse error(s))\n";

    // Exit-code contract used by the test runner: 0 = accept, 1 = reject.
    return syntax_ok ? 0 : 1;
}
