# P6：分支、短路、循环与结果汇合

对应 P6.1–P6.8；规范 §10 never/LUB、§11 控制流。全部待接入。
每步保留 U/I 的合法及非法 CFG 模型；带引用和聚合结果明确后置。

| 编号/步骤 | 输入/构造 | 预期与关键断言 | 层次/依赖 |
| --- | --- | --- | --- |
| P6-01 / .1 | `unit_if.rx`：if true 与 if false，各分支打印唯一标记 | 只输出 `1\n4\n`；无 else 的 false 不执行 body | I/E；P6.1 |
| P6-02 / .1 | br/condbr 模型，重复 terminator、错误目标、非 i1 条件 | 合法块有且仅有一个终结；目标同函数且存在；非法模型拒绝 | U/I；P6.1 |
| P6-03 / .2 | `if_values.rx`：if 返回 10/20，分支先打印 1/2 | 输出 `1\n10\n2\n20\n`；正常边先写结果再合流 | I/E；P6.2 |
| P6-04 / .2 | 分支读取入口局部；循环回边、菱形 CFG | 跨块定义必须支配使用；同块先用后定义、兄弟块值泄漏拒绝 | U/I；P6.2 |
| P6-05 / .2 | 一条前驱未写结果 slot、phi 缺/多前驱或错误类型 | 采用 slot 时 I 检查各正常边写入；采用 phi 时 U 检查前驱和值/支配 | U/I；不强制某种汇合方案 |
| P6-06 / .3 | `if_return.rx`：一边 return，一边提供值；两边均 return | 输出 `7\n8\n`；发散边不参与结果、不加额外跳转；两边发散无假结果 | I/E；P6.3 |
| P6-07 / .4 | `short_circuit.rx`：false&&、true\|\|、true&&、false\|\| | 输出 `0\n1\n3\n1\n4\n1\n`；前两项不执行 RHS，后两项各执行一次 | I/E；P6.4 |
| P6-08 / .4 | RHS 为 return；逻辑结果用于 call/赋值/外层逻辑 | 只有实际执行 RHS 的路径提前离开；发散时不执行外层调用/写入 | I/E；P6.4 |
| P6-09 / .5 | `while_condition.rx`：条件函数打印计数，body 自增 | 输出 `0\n1\n2\n`；退出前仍检查条件；false 初值不执行 body | I/E；P5、P6.5 |
| P6-10 / .6 | `loop {break;}`；`loop_control.rx` 包含 continue 和 break | 空 loop 正常退出；后者输出 `1\n3\n`；continue 后语句不执行 | I/E；P6.6 |
| P6-11 / .6 | while continue，嵌套 while/loop 的 break/continue | continue 回 while 条件；无标签跳转只作用最近循环；退出内层不丢外层上下文 | U/I/E；P6.6 |
| P6-12 / .7A | `break_value.rx`：同一 loop 两个 i32 break 结果 | 分别输出 `7\n8\n`；每条 break 在跳出前固定结果，仅属于当前 loop | I/E；P6.7A |
| P6-13 / .7B | `reference_lub.rx`：&mut 分支后遇 & 分支；交换顺序 | 输出 `1\n2\n`；所有正常结果边按最终 &i32 调整；never 边忽略 | S/I/E；P5、P6.7B |
| P6-14 / .7B | 三条 loop break，前两条 &mut，最后 &；显式期望 &i32 | 早期结果边被回填；带期望与无期望分别测；内层 break 不参与外层 LUB | S/I/E；P6.7B |
| P6-15 / .7B | 所有结果均 never，或 unit break/分支 | 没有数据 slot/伪造值；Unit 仍有正常边，never 无正常边 | S/U/I；P6.7B |
| P6-16 / .7B | Box 解引用的引用 LUB；数组/结构体 if 与 break 结果 | 转换和快照正确，不把调用者的地址当发散值 | S/I/E；P9、P7 联合验收 |
| P6-17 / .8 | `source_main_recursion.rx`：get 决定是否递归调用源 main | stdin `1 0` 输出 `0\n1\n`；内层返回到源调用者，最后机器 main 返回 0 | I/E；P6.8 |
| P6-18 / .8 | if+while+break 值+短路+提前 return 的组合 | 标记次数精确；无关闭块追加、空正常边读值或错误循环目标 | U/I/E；P6.8 |
| P6-19 / .8 | return 后缺失名称；未使用函数的普通类型错误 | sema 仍拒绝；已通过 sema 的被跳过打印不得出现在执行输出 | S/C；P1/P11 |

验收时分别报告 P6.1–P6.7 小步和 P6.8 组合；单个样例通过不能代替支配检查。
