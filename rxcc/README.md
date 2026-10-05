# rxcc — Rx 编译器（C++）

本目录是自研编译器本体。当前为**阶段 1 的第一步**：能词法/语法分析并**打印 parse tree**。
AST、名称解析、类型检查、LLVM IR 生成将在后续挂到同一 driver 上。

## 目录

```
rxcc/
├── CMakeLists.txt          # 生成 ANTLR 解析器 + 编译 rxcc
├── scripts/gen_antlr.sh    # 从 grammar/ 生成 RxLexer/RxParser（改名避免与 antlr4::Lexer 冲突）
├── src/main.cpp            # driver：读 .rx → 词法/语法分析 → 打印 parse tree
├── examples/hello.rx       # 最小示例
├── gen/                    # ANTLR 生成物（gitignore，可重建）
└── build/                  # CMake 构建目录（gitignore）
```

## 依赖

本地 ANTLR 工具链在 `<仓库>/dev/antlr`（见 `dev/antlr/README.md`）。
用 `-DANTLR_HOME=<dir>` 可覆盖。

## 构建与运行

```sh
cmake -S rxcc -B rxcc/build
cmake --build rxcc/build -j
./rxcc/build/rxcc --dump-tree rxcc/examples/hello.rx
```

首次构建会自动运行 `gen_antlr.sh`，把官方 `grammar/Lexer.g4`、`grammar/Parser.g4`
复制并改名为 `RxLexer`/`RxParser`（避免与 C++ runtime 的 `antlr4::Lexer`/`antlr4::Parser`
撞名），生成到 `rxcc/gen/`。**官方 grammar 不会被修改。**

## 命令行

```
rxcc [options] <source.rx>
  --stage <name>   parse（默认）| semantic | ir | codegen   （目前仅 parse 可用）
  --dump-tree      打印 ANTLR parse tree
  -o <file>        输出文件（为后续阶段预留）
  -h, --help
```

退出码符合测试契约：`0` = 接受（无语法错误），`1` = 拒绝（有语法错误），`2` = 用法/IO 错误。
