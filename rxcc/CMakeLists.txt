# rxcc — Rx compiler (C++). Build:
#
#   cmake -S rxcc -B rxcc/build
#   cmake --build rxcc/build -j
#   ./rxcc/build/rxcc --dump-tree rxcc/examples/hello.rx
#
# Generated parser sources and the matching ANTLR runtime are committed so normal
# builds work offline, without Java or the local dev/antlr toolchain.
cmake_minimum_required(VERSION 3.16)
project(rxcc LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../vendor/antlr4-runtime"
                 "${CMAKE_CURRENT_BINARY_DIR}/antlr4-runtime")

# --- Pre-generated parser from the official grammars -------------------------
# After editing grammar/*.g4, run scripts/gen_antlr.sh with ANTLR 4.13.2 and
# commit the updated gen/*.cpp and gen/*.h files alongside the grammar changes.
set(GEN_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/gen")

set(GENERATED_SOURCES
  "${GEN_DIR}/RxLexer.cpp"
  "${GEN_DIR}/RxParser.cpp"
  "${GEN_DIR}/RxParserBaseVisitor.cpp"
  "${GEN_DIR}/RxParserVisitor.cpp")

# --- rxcc executable ---------------------------------------------------------
add_executable(rxcc
  src/main.cpp
  src/ast/ast.cpp
  src/ast/dump.cpp
  src/ast_builder.cpp
  # Semantic pipeline entry and crate-level declaration checks.
  src/sema/sema.cpp
  src/sema/type.cpp
  src/sema/declarations/collect.cpp
  src/sema/declarations/constants.cpp
  src/sema/declarations/resolve_type.cpp
  src/sema/declarations/validate.cpp
  # Recursive body driver, expression rules and shared conversions.
  src/sema/body/checker.cpp
  src/sema/body/values.cpp
  src/sema/body/operators.cpp
  src/sema/body/aggregate.cpp
  src/sema/body/control_flow.cpp
  src/sema/body/conversions.cpp
  # Ordinary/qualified calls and dot-method receiver adjustment.
  src/sema/body/calls/call.cpp
  src/sema/body/calls/builtins.cpp
  src/sema/body/calls/method.cpp
  ${GENERATED_SOURCES})

# ANTLR 运行时头按 SYSTEM 引入：第三方头不按本项目告警纪律维护，
# -Wall -Wextra 只约束本项目源码。
target_include_directories(rxcc PRIVATE "${GEN_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(rxcc PRIVATE rxcc_antlr_runtime Threads::Threads)

# 本项目源码按 -Wall -Wextra 纪律维护；ANTLR 生成代码不在此列，关掉告警。
target_compile_options(rxcc PRIVATE -Wall -Wextra)
set_source_files_properties(${GENERATED_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
