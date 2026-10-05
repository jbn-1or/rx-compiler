# rxcc — Rx compiler (C++). Build:
#
#   cmake -S rxcc -B rxcc/build
#   cmake --build rxcc/build -j
#   ./rxcc/build/rxcc --dump-tree rxcc/examples/hello.rx
#
# The local ANTLR toolchain is expected at <repo>/dev/antlr (see
# dev/antlr/README.md). Override with -DANTLR_HOME=/path/to/antlr.
cmake_minimum_required(VERSION 3.16)
project(rxcc LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Locate the local (rootless) ANTLR 4.13.2 toolchain ----------------------
if(NOT ANTLR_HOME)
  get_filename_component(ANTLR_HOME "${CMAKE_CURRENT_SOURCE_DIR}/../dev/antlr" ABSOLUTE)
endif()

set(ANTLR4_JAVA     "${ANTLR_HOME}/jre/bin/java")
set(ANTLR4_JAR      "${ANTLR_HOME}/antlr-4.13.2-complete.jar")
set(ANTLR4_INC      "${ANTLR_HOME}/cpp-runtime/include/antlr4-runtime")
set(ANTLR4_LIB      "${ANTLR_HOME}/cpp-runtime/lib/libantlr4-runtime.a")

find_package(Threads REQUIRED)

if(NOT EXISTS "${ANTLR4_JAR}" OR NOT EXISTS "${ANTLR4_LIB}")
  message(FATAL_ERROR
    "ANTLR toolchain not found under ${ANTLR_HOME}.\n"
    "Install it first (see dev/antlr/README.md) or pass -DANTLR_HOME=<dir>.")
endif()

# --- Generate the parser from the official grammars --------------------------
get_filename_component(GRAMMAR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../grammar" ABSOLUTE)
set(GEN_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_antlr.sh")
set(GEN_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/gen")

set(GENERATED_SOURCES
  "${GEN_DIR}/RxLexer.cpp"
  "${GEN_DIR}/RxParser.cpp"
  "${GEN_DIR}/RxParserBaseVisitor.cpp"
  "${GEN_DIR}/RxParserVisitor.cpp")

add_custom_command(
  OUTPUT ${GENERATED_SOURCES}
         "${GEN_DIR}/RxLexer.h" "${GEN_DIR}/RxParser.h"
         "${GEN_DIR}/RxParserBaseVisitor.h" "${GEN_DIR}/RxParserVisitor.h"
  COMMAND "${CMAKE_COMMAND}" -E env
          "ANTLR4_JAVA=${ANTLR4_JAVA}"
          "ANTLR4_JAR=${ANTLR4_JAR}"
          "GRAMMAR_DIR=${GRAMMAR_DIR}"
          "GEN_DIR=${GEN_DIR}"
          sh "${GEN_SCRIPT}"
  DEPENDS "${GRAMMAR_DIR}/Lexer.g4" "${GRAMMAR_DIR}/Parser.g4" "${GEN_SCRIPT}"
  COMMENT "Generating ANTLR C++ parser (RxLexer/RxParser) into gen/"
  VERBATIM)

add_custom_target(rxcc_antlr DEPENDS ${GENERATED_SOURCES})

# --- rxcc executable ---------------------------------------------------------
add_executable(rxcc
  src/main.cpp
  ${GENERATED_SOURCES})

add_dependencies(rxcc rxcc_antlr)

target_include_directories(rxcc PRIVATE "${ANTLR4_INC}" "${GEN_DIR}")
target_link_libraries(rxcc PRIVATE "${ANTLR4_LIB}" Threads::Threads)
