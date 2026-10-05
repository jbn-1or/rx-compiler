#!/usr/bin/env sh
# Generate the C++ Rx lexer/parser into $GEN_DIR using ANTLR 4.13.2.
#
# The official grammars are copied and renamed:
#   lexer  grammar Lexer  -> RxLexer   (avoids clashing with antlr4::Lexer)
#   parser grammar Parser -> RxParser  (avoids clashing with antlr4::Parser)
# and Parser's `tokenVocab=Lexer;` is rewritten to `tokenVocab=RxLexer;`.
# The originals under grammar/ are never modified.
#
# Env (all optional; defaults resolve relative to this script):
#   ANTLR4_JAVA  path to the java executable
#   ANTLR4_JAR   path to antlr-4.13.2-complete.jar
#   GRAMMAR_DIR  directory holding Lexer.g4 / Parser.g4
#   GEN_DIR      output directory for generated sources
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RXCC_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
REPO_ROOT=$(CDPATH= cd -- "$RXCC_DIR/.." && pwd)

: "${ANTLR4_JAVA:=$REPO_ROOT/dev/antlr/jre/bin/java}"
: "${ANTLR4_JAR:=$REPO_ROOT/dev/antlr/antlr-4.13.2-complete.jar}"
: "${GRAMMAR_DIR:=$REPO_ROOT/grammar}"
: "${GEN_DIR:=$RXCC_DIR/gen}"

if [ ! -x "$ANTLR4_JAVA" ]; then
    echo "gen_antlr.sh: java not found: $ANTLR4_JAVA" >&2
    echo "  (install the local ANTLR toolchain; see dev/antlr/README.md)" >&2
    exit 1
fi
if [ ! -f "$ANTLR4_JAR" ]; then
    echo "gen_antlr.sh: ANTLR jar not found: $ANTLR4_JAR" >&2
    exit 1
fi

mkdir -p "$GEN_DIR"

# 1) Copy + rename the grammars into GEN_DIR.
sed 's/^lexer grammar Lexer;/lexer grammar RxLexer;/' \
    "$GRAMMAR_DIR/Lexer.g4" > "$GEN_DIR/RxLexer.g4"
sed -e 's/^parser grammar Parser;/parser grammar RxParser;/' \
    -e 's/tokenVocab=Lexer;/tokenVocab=RxLexer;/' \
    "$GRAMMAR_DIR/Parser.g4" > "$GEN_DIR/RxParser.g4"

# 2) Generate (run with cwd=GEN_DIR so output lands there flat).
#    Parser depends on Lexer.tokens, so Lexer must be generated first.
( cd "$GEN_DIR" && "$ANTLR4_JAVA" -jar "$ANTLR4_JAR" \
    -Dlanguage=Cpp -visitor -no-listener -o . RxLexer.g4 )
echo "gen_antlr.sh: generated $GEN_DIR/RxLexer.*"

( cd "$GEN_DIR" && "$ANTLR4_JAVA" -jar "$ANTLR4_JAR" \
    -Dlanguage=Cpp -visitor -no-listener -lib . -o . RxParser.g4 )
echo "gen_antlr.sh: generated $GEN_DIR/RxParser.*"
