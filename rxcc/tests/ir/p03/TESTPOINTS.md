# P3：局部存储、普通调用与返回

对应 P3.0–P3.6；规范 §6、§7、§9、§10、§11、§15。
源码种子和多项结构断言已接入，逐点边界见下表后的接入矩阵；不等于全部目标变体已补齐。
小步验收只使用此前能力；不要以 cast、赋值或 if 作为 P3 前置条件。

| 编号/步骤 | 源码/构造 | 预期与关键断言 | 层次/依赖 |
| --- | --- | --- | --- |
| P3-17 / .1 | `println_42.rx` 只调用一次 `println_i32(42)` | 精确输出 `42\n`，退出 0；void call 无结果值 | U/I/E；P3.1 |
| P3-03 / .2 | `shadowing.rx`：嵌套遮蔽、`let x=x` | 输出 `1\n2\n3\n2\n`；LocalId 各有固定 slot，初始化读旧绑定 | I/E；平坦遮蔽 P3.2，完整种子的块依赖 P3.3 |
| P3-04 / .2 | 手工 alloca/load/store 模型 | alloca 结果 ptr；load/store 显式内容类型及合法 align；错类型/无效地址操作数拒绝 | U/I；P3.2 |
| P3-05 / .2 | let 的初始化器是打印后返回的标量块 | 初始化执行一次；slot 在入口，store 在源码求值位置 | I/E；块调用依赖 P3.3–.4 |
| P3-06 / .3 | `unit_blocks.rx`：前向 ping、unit 尾块、分号语句 | 输出 `7\n8\n`；Unit 正常继续，既非零整数也非 NoContinuation | I/E；P3.3 |
| P3-01 / .4 | `literal_io.rx` 连续 print 1、print 2、println 42 | 精确输出 `1242\n`，print 和 println 的外部签名正确 | U/I/E；P3.4 |
| P3-02 / .4 | `read_io.rx` 从配套 .in 连续读三个整数并逐个 println | 输出 MIN、0、MAX 三行，每个输入恰好读取一次 | I/E；P3.4 |
| P3-07 / .4 | `scalar_calls.rx`：mark 实参、前向调用、传参与尾返回 | 标记 `1\n2\n7\n`；参数从左到右；结果可立即作为参数 | I/E；P3.4 |
| P3-08 / .4 | 参数被同名 let 遮蔽；mut 标量参数 | 参数有自己的 slot；调用者值不变；遮蔽初始化读参数 | I/E；P3.4；修改依赖 P5 |
| P3-09 / .4 | 10 个标量参数，第 1/8/10 参数返回或打印 | 声明/定义/call 签名完全一致，不按机器寄存器数截断参数 | I/E；P3.4 |
| P3-10 / .5 | `early_return.rx`：return 7 后打印 99；unit return | 输出 `7\n8\n`；ret 后不再发指令，外层调用/写入被跳过 | I/E；P3.5 |
| P3-11 / .5 | `let x=return;`、实参中 return、return 的实参中 return | 不为 never 分配结果；外层表达式停止，其他实参及调用不执行 | U/I/E；P3.5 |
| P3-12 / .5 | 两个独立函数，其中一个提前 return | 当前函数的 NoContinuation 不污染另一个函数插入点 | U/I/E；P3.5 |
| P3-13 / .6 | `bool_boundary.rx`：bool 参数、局部存储、返回 | 空 stdout 且退出 0；计算/参数/结果 i1，slot i8，load 后 trunc，store 前 zext | U/I/E；P3.6 |
| P3-14 / .6 | true/false 经多次 let/call 后 `as i32` 打印 | 输出 1、0；不能把 true 解释为 -1；使用前 store | I/E；P4.2 联合验收 |
| P3-15 / .2–.6 | 计数循环中反复初始化局部，递归函数有同名绑定 | alloca 在函数入口，循环只重新初始化；递归帧互相独立 | I/E/C；P6/P11 联合验收 |
| P3-16 / .4 | 常量项作为参数和尾返回 | 读取 SemanticResult 的位模式，不能生成对未定义全局符号的 load | S/I/E；P3.4 |

验收：新存储/转换指令经独立 U/I 测试，源码 E 验收不以手工模块替代。

## 接入证据与缺口

以下是当前测试代码的静态接入记录，不是本轮执行成绩。`entry` 指 `../entry_test.cpp`，
`model` 指 `../model_test.cpp`，`runner` 指 `../../run_ir.py`；结构自检不单独算外部 I，
runner 的执行断言只有加 `--reimu` 才运行。状态含义见 [README](../README.md#测试层次与状态)。

| 编号 | 当前接入证据 | 未补齐或不能据此推断的部分 |
| --- | --- | --- |
| P3-17 | 已接入：entry `check_println` 检查声明、常量与 void 无结果；runner `println-42` 验证/执行种子 | 无额外矩阵缺口记录；运行结果另报 |
| P3-03 | 已接入：entry `check_locals` 检查独立入口 slot 和初始化读取顺序；runner `shadowing` 执行完整种子 | 不以单独 semantic 接受作为执行证据 |
| P3-04 | 已接入：model `add_storage`、`check_invalid_models` 及 `run_tests` 的存储构建反例；`--emit-model` 经 runner 外部验证 | 手工存储函数未被执行，不替代源码存取 E |
| P3-05 | 部分：runner `nested-integer-block` 覆盖标量块；entry `check_reference_temporaries` 检查调用后的初始化和入口分配 | 打印后返回的标量块作为 let 初始化器的专门一次性/写入时机断言待补 |
| P3-06 | 已接入：entry `check_unit_calls_and_blocks`；runner `unit-blocks` | 无额外矩阵缺口记录 |
| P3-01 | 已接入：runner `literal-io` 精确比较 `1242\n`，外部 LLVM 检查调用类型 | 不等于所有 I/O 声明都有专门的内部结构断言 |
| P3-02 | 已接入：runner `read-io` 使用配套输入/输出；另有 `read-single-negative` | 不只检查生成成功或空 stdout |
| P3-07 | 已接入：entry `check_integer_calls_and_returns` 检查参数顺序、签名和尾返回；runner `scalar-calls` | 无额外矩阵缺口记录 |
| P3-08 | 部分：entry 检查标量参数 slot；runner `call-then-continue` 检查调用后仍读取 caller 局部 | 参数被同名 let 遮蔽、mut 标量参数修改后的 caller 独立性专例待补 |
| P3-09 | 部分：entry 和 runner `scalar-nine-args` 已覆盖九参数及第九参数结果 | 目标表中的十参数及第 1/8/10 参数结果矩阵未补齐 |
| P3-10 | 已接入：entry `check_early_return`；runner `early-return` 及无打印 99 的文本检查 | 无额外矩阵缺口记录 |
| P3-11 | 已接入：entry `check_early_return` 检查 never let、嵌套 return；runner `never-let`、`return-stops-later-argument` 等 | 不把发散路径伪造为 Unit 或常量结果 |
| P3-12 | 已接入：entry 独立函数返回断言；runner `return-independence` | 无额外矩阵缺口记录 |
| P3-13 | 已接入：entry `check_bool_boundary`、model `add_bool_scalar` 及转换反例；runner `bool-boundary`、`model-bool` | 空输出种子本身不区分真值；`model-bool` 是底层驱动，不是源码 cast |
| P3-14 | 部分：runner `p04-casts` 覆盖 true/false 到整数；`model-bool` 覆盖模型往返后打印 | 源码经多次 let/call 再 cast 的完整链路专例待补，不能用两组独立测试代替 |
| P3-15 | 已接入联合回归：entry `check_while` 检查循环内初始化但入口分配；runner `p68-recursive-call-order` 和 P11 `loop_storage`、`recursive_frames` | P11 资源/执行结果须由相应命令单独报告 |
| P3-16 | 已接入：entry `check_scalar_constants` 检查常量尾返回位模式，`check_locals` 检查局部常量；runner `local-constant` 等 | 不将语义查询的成功单独算为 IR 执行通过 |
