# P8：方法、接收者调整、关联项和数组 len

对应 P8.1–P8.3；规范 §6、§7 impl/关联项、§11 调用/方法/数组方法。
P8.1 的目标、签名和调整记录由 `semantic_invariants` 检查；P8-04、P8-12 的诊断和输出保护接入 `run_ir.py`。
P8.2 的方法 ABI 由 `ir_entry_test` 检查；`run_ir.py` 校验已接入的源码 IR，`--reimu` 再检查 RV32 O0/O2 的输出和退出码。
`qualified_calls.rx` 不依赖点调用，单独检查限定调用路径。Box、Vec 和内置 Clone 的执行仍留到后续阶段。

| 编号/步骤 | 输入/变体 | 预期与关键断言 | 层次/依赖 |
| --- | --- | --- | --- |
| P8-01 / .1 | `methods.rx`：&self get、&mut self set、Self::new、关联常量 | set 修改原对象；get 和显式关联调用返回一致；Self 按 StructId 解析 | S/I/E；P8.1–.2 |
| P8-02 / .1 | `inherent_clone.rx`：用户 `clone(&self, delta:i32)->i32` | 按真实签名调用且返回标量；不能按 clone 拼写调用 builtin | S/I/E；P8.1 |
| P8-03 / .1 | `qualified_calls.rx`：两种结构体同名方法、全局同名函数、`Self::new`、带接收者的聚合返回 | 使用 sema 已选 FunctionId；函数/类型名字空间、隐藏结果地址和接收者不混淆 | S/I/E；P8.1 |
| P8-04 / .1 | `Type::method(value)` 未显式借用，但签名要求 &self | 作为普通参数不自动借用，正常语义拒绝；点调用正例另测 | S/C；P8.1 |
| P8-05 / .2 | `receiver_order.rx`：make 打印 1，arg 打印 2，method 打印 3 | 顺序 `1\n2\n3\n7\n`；接收者只构造一次，方法体在参数之后执行 | I/E；P8.2 |
| P8-06 / .2 | `self_snapshot.rx`：普通实参修改 Copy 接收者的原对象，方法再修改按值 self 并返回聚合 | 接收者先固定快照；实参和返回槽顺序正确；方法的修改不覆盖原对象 | S/U/I/E；P7.4、P8.2 |
| P8-07 / .2 | `receiver_places.rx`：索引接收者、多层共享/可变引用和重借用；共享路径与不可变位置的负例 | 基址、下标、参数各一次且顺序正确；写回原对象；负例在语义阶段拒绝且保留已有输出 | S/I/E/C；P8.2 |
| P8-08 / .2 | Box 后的方法调用，Box builtin clone 与 pointee 方法区分 | 使用已记录目标及完整调整路径，不重新查找 | S/I/E；P9/P10 联合验收 |
| P8-09 / .3 | `array_len.rx`：make_array 打印 9，再 .len | 输出 `9\n3\n`；长度是 usize/i32 常量，但不删除接收者副作用 | I/E；P8.3 |
| P8-10 / .3 | `array_len.rx`：if 一边提前 return 0，另一边 make_array 后 .len | 提前路径不执行 make/len；正常路径恰一次；输出参见 .out | I/E；P6、P8.3 |
| P8-11 / .3 | `array_len_places.rx`：已有数组、多层引用、投影与丢弃结果；同名用户 `len` | 外层长 2、投影行长 3；基址和下标各一次；不搬运数组或调用内置 `len`，用户方法仍执行 | U/I/E；P8.3 |
| P8-12 / .1–.3 | 更早接收者候选被选中但其参数不匹配 | 报 sema 错误，不在 IR 尝试“更合适”的后续候选；排除歧义方法程序 | S/C；P8.1 |
| P8-13 / .2 | `receiver_early_exit.rx`：值接收者、索引和实参分别提前 return | 保留 return 前的副作用，不执行后续实参或方法体；正常分支仍按顺序执行 | S/I/E；P6、P8.2 |

方法解析记录先验收，再验收发射；Vec 的两阶段接收者行为留到 P9。
P8.3 的两个源码正例接入 `run_ir.py`；`ir_entry_test` 检查数组 `len` 的常量结果与 IR 中没有多余的调用或数据搬运。
