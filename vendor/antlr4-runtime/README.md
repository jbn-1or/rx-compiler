# ANTLR C++ runtime 4.13.2

The unmodified `.cpp` and `.h` files under `src/` come from `runtime/src/` in
the upstream ANTLR 4.13.2 C++ runtime distribution:

- Source archive: https://www.antlr.org/download/antlr4-cpp-runtime-4.13.2-source.zip
- Upstream: https://github.com/antlr/antlr4/tree/4.13.2/runtime/Cpp/runtime/src
- License: BSD 3-Clause, reproduced in `LICENSE.txt`.

The repository's small `CMakeLists.txt` builds only the static runtime. It does
not run upstream tests or download their GoogleTest dependency. Keeping these
sources alongside the generated parser lets the judge build offline without
depending on the version of its system ANTLR runtime.

When upgrading ANTLR, update these sources and regenerate `rxcc/gen/*.cpp` and
`rxcc/gen/*.h` with the matching tool version.
