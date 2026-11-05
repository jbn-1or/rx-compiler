# rxcc — Rx compiler (C++). Build:
#
#   cmake -S rxcc -B rxcc/build
#   cmake --build rxcc/build -j
#   ./rxcc/build/rxcc --dump-tree rxcc/examples/hello.rx
# LLVM 22 development files are required. If discovery fails, configure with:
#   cmake -S rxcc -B rxcc/build -DLLVM_DIR="$(llvm-config-22 --cmakedir)"
#
# Generated parser sources and the matching ANTLR runtime are committed so normal
# builds work offline, without Java or the local dev/antlr toolchain.
cmake_minimum_required(VERSION 3.16)
# LLVM's dependency checks (for example FindFFI) compile C test programs.
project(rxcc LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(LLVM REQUIRED CONFIG)
if(NOT LLVM_VERSION_MAJOR EQUAL 22)
  message(FATAL_ERROR
    "rxcc requires LLVM 22; found ${LLVM_PACKAGE_VERSION} in ${LLVM_DIR}. "
    "Set LLVM_DIR to the directory reported by llvm-config-22 --cmakedir.")
endif()
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")
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

# --- Frontend shared by the driver and semantic invariant tests ---------------
add_library(rxcc_frontend STATIC
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
target_include_directories(rxcc_frontend PUBLIC "${GEN_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(rxcc_frontend PUBLIC rxcc_antlr_runtime Threads::Threads)

# 本项目源码按 -Wall -Wextra 纪律维护；ANTLR 生成代码不在此列，关掉告警。
target_compile_options(rxcc_frontend PRIVATE -Wall -Wextra)
set_source_files_properties(${GENERATED_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")

# LLVM 模块及其 context 一起持有；不改变前端的 C++20、异常或 RTTI 设置。
add_library(rxcc_ir STATIC src/ir/artifact.cpp src/ir/generator.cpp)
target_include_directories(rxcc_ir PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(rxcc_ir SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
separate_arguments(RX_LLVM_DEFINITIONS NATIVE_COMMAND "${LLVM_DEFINITIONS}")
target_compile_options(rxcc_ir PUBLIC ${RX_LLVM_DEFINITIONS})
target_compile_options(rxcc_ir PRIVATE -Wall -Wextra)
if(LLVM_LINK_LLVM_DYLIB AND TARGET LLVM)
  target_link_libraries(rxcc_ir PUBLIC LLVM)
else()
  llvm_map_components_to_libnames(RX_LLVM_LIBS Core Support)
  target_link_libraries(rxcc_ir PUBLIC ${RX_LLVM_LIBS})
endif()

add_executable(rxcc src/main.cpp)
target_link_libraries(rxcc PRIVATE rxcc_frontend rxcc_ir)
target_compile_options(rxcc PRIVATE -Wall -Wextra)

include(CTest)
if(BUILD_TESTING)
  # This optional local test may be ignored by the developer's Git configuration.
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/semantic_invariants.cpp")
    add_executable(semantic_invariants tests/semantic_invariants.cpp)
    target_link_libraries(semantic_invariants PRIVATE rxcc_frontend)
    target_compile_options(semantic_invariants PRIVATE -Wall -Wextra)
    add_test(NAME semantic_invariants COMMAND semantic_invariants)
  endif()

  add_executable(ir_api_smoke tests/ir/api_smoke.cpp)
  target_link_libraries(ir_api_smoke PRIVATE rxcc_ir)
  target_compile_options(ir_api_smoke PRIVATE -Wall -Wextra)
  add_test(NAME ir_api_invariants COMMAND ir_api_smoke --test)
  add_test(NAME ir_api_smoke COMMAND ir_api_smoke)

  add_executable(ir_entry_test tests/ir/entry_test.cpp)
  target_link_libraries(ir_entry_test PRIVATE rxcc_frontend rxcc_ir)
  target_compile_options(ir_entry_test PRIVATE -Wall -Wextra)
  add_test(NAME ir_entry_test COMMAND ir_entry_test)

  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME ir_cli_smoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_ir.py"
      --compiler "$<TARGET_FILE:rxcc>" --ir-test "$<TARGET_FILE:ir_entry_test>")
  set_tests_properties(ir_cli_smoke PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.." TIMEOUT 180)
endif()
