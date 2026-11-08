# IR 生成层：P2 完成基线

使用 C++20 自行表示 LLVM IR，不包含 LLVM 头文件或链接 LLVM 库。
当前保留 P0–P2 的基础闭环，P3 及后续代码由后续开发重新实现。

源码入口只生成**空的、无参、返回 unit 的普通函数**：
内部源 main 返回 void，导出的机器 `i32 @main()` 调用它后返回 0。
其他空普通函数也按 `SemanticResult` 中的稳定身份生成，名称不依赖用户拼写。
全局类型和常量声明继续接受完整语义检查，但尚不生成使用它们的函数体代码。
非空函数体、参数/非 unit 返回、方法等有效程序明确报告“尚未支持”；不丢弃未使用函数中的代码。

| 文件 | 当前职责 |
| --- | --- |
| `model.*` | `void/i32`、模块/函数/基本块、身份、操作数、call 和 ret；固定 RV32 目标 |
| `builder.*` | 检查构建请求，维护插入点；ret 后关闭插入点，void call 不产生结果 |
| `check.*` | 检查名称、身份、签名、块终结、call/ret 类型及结果定义先于使用 |
| `printer.*` | 集中输出确定的 LLVM 文本，不推导源类型或改写指令 |
| `artifact.*` | 独立拥有模块，结构检查成功后才输出文本 |
| `generator.*` | 验证 P2 支持边界、按语义身份命名并生成空函数及机器入口 |

底层模型可以手工构造 i32 参数、返回和带参数 call，用来检查签名与 SSA 身份；
这不表示源码参数、调用或返回表达式已经实现。各块只使用参数与本块此前的 call 结果，
跨块控制流和 SSA 支配关系到 P6 再加入。
`IRArtifact` 的模块不借用 AST/sema；模块归属标记不参与文本命名。
构建接口的无效请求在修改结构前抛出 `std::logic_error`，生成入口将其转为诊断。
自有检查器返回 `true` 表示通过，非法模块不输出部分文本。

局部 slot、Place/EmitResult、bool 存取、普通表达式 lowering、标量运算/cast、
分支及其 CFG 检查均未保留，后续从 P3 开始逐步增加。

在仓库根目录构建与验证：

```sh
cmake -S rxcc -B rxcc/build
cmake --build rxcc/build -j2
ctest --test-dir rxcc/build --output-on-failure
python3 rxcc/tests/run_ir.py --reimu
make test STAGE=semantic
```

测试仅覆盖 P2 基线、P3+ 的明确拒绝和完整 sema/CLI 错误路径。
runner 需要 `llvm-as-22`、`opt-22`；`--reimu` 另需 `clang-22`、xmake 和 REIMU。
生成 `.ll` 本身不启动这些工具，输出文件在完整 sema、自检和文本生成之后才打开。
`config.mk` 的正式 IR/CODEGEN 命令仍走 Rust 参考路径，不能作为 C++ IR 完成的证据。
