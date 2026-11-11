# P3：局部存储、普通调用与返回

对应 P3.0–P3.6；规范 §6、§7、§9、§10、§11、§15。
全部 lowering 断言待接入。小步验收只使用此前能力；不要以 cast、赋值或 if 作为 P3 前置条件。

| 编号/步骤 | 源码/构造 | 预期与关键断言 | 层次/依赖 |
| --- | --- | --- | --- |
| P3-01 / .1 | `literal_io.rx`：print 1、print 2、println 42 | 精确输出 `1242\n`；print/println 外部签名正确；void call 无值 | U/I/E；P3.1 |
| P3-02 / .1 | `read_io.rx`：三次 get 后逐次 println；配套 .in | 输出 MIN、0、MAX 三行；每个 get 执行一次，顺序不变 | I/E；P3.1 |
| P3-03 / .2 | `shadowing.rx`：嵌套遮蔽、`let x=x` | 输出 `1\n2\n3\n2\n`；LocalId 各有固定 slot，初始化读旧绑定 | I/E；平坦遮蔽 P3.2，完整种子的块依赖 P3.3 |
| P3-04 / .2 | 手工 alloca/load/store 模型 | alloca 结果 ptr；load/store 显式内容类型及合法 align；错类型/无效地址操作数拒绝 | U/I；P3.2 |
| P3-05 / .2 | let 的初始化器是打印后返回的标量块 | 初始化执行一次；slot 在入口，store 在源码求值位置 | I/E；块调用依赖 P3.3–.4 |
| P3-06 / .3 | `unit_blocks.rx`：前向 ping、unit 尾块、分号语句 | 输出 `7\n8\n`；Unit 正常继续，既非零整数也非 NoContinuation | I/E；P3.3 |
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
