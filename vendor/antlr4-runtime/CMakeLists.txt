# Build only the vendored ANTLR 4.13.2 runtime, without upstream test downloads.
file(GLOB_RECURSE ANTLR4_RUNTIME_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
add_library(rxcc_antlr_runtime STATIC ${ANTLR4_RUNTIME_SOURCES})
target_include_directories(rxcc_antlr_runtime SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(rxcc_antlr_runtime PUBLIC Threads::Threads)
if(WIN32)
  target_compile_definitions(rxcc_antlr_runtime PUBLIC ANTLR4CPP_STATIC)
endif()
