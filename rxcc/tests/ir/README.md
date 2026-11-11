# IR 分阶段测试设计

依据：[IR 实现计划](../../../documents/IR/IR实现计划.md) P0–P11 及
[Rx 规范](../../../documents/Rx编译器规范完整中文翻译.md)。每个 `pNN/TESTPOINTS.md`
按计划的小节列出测试输入、断言、依赖和验收层次；未实现阶段也保留完整目标。
现有 `model_test.cpp`、`entry_test.cpp` 和 `../run_ir.py` 的 P2 基线检查继续保留。

| 阶段 | 测试设计 | 主要风险 |
| --- | --- | --- |
| P0 | [工具链与运行时](p00/TESTPOINTS.md) | 目标、入口、I/O、字节比对 |
| P1 | [语义结果契约](p01/TESTPOINTS.md) | 所有权、身份、事实与使用位置调整 |
| P2 | [模型与 CLI](p02/TESTPOINTS.md) | 非法模型、确定输出、失败保护 |
| P3 | [存储与调用](p03/TESTPOINTS.md) | 固定 slot、求值顺序、Unit/发散、bool |
| P4 | [标量运算](p04/TESTPOINTS.md) | 符号性、回绕、移位掩码、cast |
| P5 | [引用与赋值](p05/TESTPOINTS.md) | 地址、重借用、RHS 优先、延后读取 |
| P6 | [控制流](p06/TESTPOINTS.md) | CFG、支配、短路、循环目标、LUB |
| P7 | [聚合值](p07/TESTPOINTS.md) | RV32 布局、初始化顺序、快照、ABI |
| P8 | [方法](p08/TESTPOINTS.md) | 已选目标、接收者一次求值、数组 len |
| P9 | [Box 与 Vec](p09/TESTPOINTS.md) | 分配、真实头、增长、保序 remove |
| P10 | [Clone 与相等](p10/TESTPOINTS.md) | 深克隆、借用比较、递归 helper |
| P11 | [联合验收](p11/TESTPOINTS.md) | 自有 IR 接线、资源、完整回归 |

## 测试层次与状态

- **S**：语义结果/API 查询或源码接受/拒绝；不证明 lowering 完成。
- **U**：自有 IR 的模型、builder、checker、printer 单元检查。
- **I**：检查自己的 `.ll`，然后执行 `llvm-as-22` 和 `opt-22 -passes=verify -disable-output`。
- **E**：自己的 rxcc → I → Clang 22 RV32IM/ILP32 的 O0/O2 → 程序和运行时汇编经 Clang 集成汇编器检查 → REIMU，逐字节比 stdout，退出 0。
- **C**：CLI、失败原子性、工具链和资源检查。

“已有”仅指相邻测试文件中已有对应断言，运行结果以实际命令为准。
其余测试点为 **待接入**。`.rx`、`.out`、可选 `.in` 是已落盘的源码行为种子，
不是全部测试点都已有自动断言。文档明确列出的 U/I/C 检查应随实现补入现有测试和
`../run_ir.py`，不为尚不存在的 API 写空测试或返回成功的占位测试。

每条种子在标准 `manifest.json` 中同时登记 semantic 项与 codegen 项（负例只有 semantic 项）；
现有 runner 会由 codegen 项派生 ir 项。`metadata.requires` 记录最早联合验收步骤，
`metadata.ir_status` 为设计状态。**runner 不会按 metadata 跳过测试**，尚未实现的源码
IR 测试应失败，不能把当前“尚未支持”的拒绝当成最终预期。
这些清单不加入默认 CTest，不改变正式 `config.mk` 的参考实现配置。

## 运行

仓库根目录先构建并运行现有基线：

```sh
cmake -S rxcc -B rxcc/build
cmake --build rxcc/build -j2
ctest --test-dir rxcc/build --output-on-failure
python3 rxcc/tests/run_ir.py --reimu
make test STAGE=semantic
```

复用项目 runner 检查本目录全部源码种子的语义接受/拒绝（不需要 Rust 参考实现）：

```sh
RX_TEST_SEMANTIC='./rxcc/build/rxcc --stage semantic {source}' \
  python3 scripts/test.py --tests-dir rxcc/tests/ir --stage semantic
```

未来对应 lowering 和共享运行时完成后，可用同一 runner 做单阶段行为冒烟：

```sh
FILTER=p03 \
RX_TEST_IR='./rxcc/build/rxcc --stage ir {source} -o {output}' \
RX_TEST_BUILD='clang-22 --target=riscv32-unknown-elf -march=rv32im -mabi=ilp32 -O2 -S -mllvm -riscv-no-aliases runtime/runtime.c -o {runtime}' \
RX_TEST_RUN='xmake run -P vendor/REIMU reimu --memory=256M --stack=1M -f {output},{runtime} -o {stdout} -p {profile}' \
  python3 scripts/test.py --tests-dir rxcc/tests/ir --stage ir
```

上例 `runtime/runtime.c` 是实现计划 P9/P11 的未来交付文件，目前尚不存在；运行时按规范 §16
提供三个 I/O 和 `__rx_alloc`。当前标准 runner 的 IR 路径只有 Clang O0，不能单独满足 E 的
完整要求；O2、外部 verifier、集成汇编器和退出码 profile 检查须按设计扩展 `../run_ir.py`。
只有 `p02` 的空程序可现在用自己的 IR 路径直接行为冒烟，无需运行时。

## 断言约定

1. 源码正例期望语义和最终 IR 生成成功；负例期望正常诊断退出 1。崩溃、超时、缺工具不算通过。
2. 每次先确认编译成功且产生新模块，再验证/执行；失败不得使用保留下来的旧 `.ll`。
3. 不锁定 SSA 编号、块名或完整 IR 文本；检查类型、操作、CFG、布局和可观察行为。
   确定性测试才比较同一输入的完整输出。
4. 需要区分求值顺序时，函数打印唯一标记；同时检查标记次数和最终结果。
5. 不以除零、MIN/-1、越界、悬垂引用、冲突借用、移动后使用、零大小数据、跨源类型相等、
   歧义方法或无公共目标的 LUB 作为必须执行/拒绝的测试。整数后缀避免依赖被排除的推导。
6. 不要求边界 panic、析构/free、运行时所有权标记或 LLVM API；容量、增长策略、填充字节不可观察。

## 规范覆盖索引

与实现计划附录 B 的编号一一对应；每个阶段文档继续细化对应断言。

| 规范编号 | 测试点所在阶段 |
| --- | --- |
| IR01 / IR02 / IR03 / IR04 | P0、P2、P3、P6、P7、P11 |
| IR05 / IR06 / IR07 | P1、P3、P4、P7、P8 |
| IR08 / IR09 / IR10 | P4、P5 |
| IR11 / IR12 / IR13 / IR14 | P3–P8，分别覆盖普通操作、聚合、赋值、引用读取 |
| IR15 / IR16 / IR17 | P1、P3、P5、P6、P7、P9 |
| IR18 / IR19 / IR20 / IR21 | P1、P3、P6、P11 |
| IR22 / IR23 / IR24 / IR25 | P5、P7、P8、P9 |
| IR26 / IR27 / IR28 / IR29 | P1、P8、P9 |
| IR30 / IR31 / IR32 | P7、P10 |
| IR33 / IR34 / IR35 / IR36 / IR37 | P0、P1、P2、P7、P9、P10、P11；有效执行边界适用于所有阶段 |
