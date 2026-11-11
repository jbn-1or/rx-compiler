# P8：方法、接收者调整、关联项和数组 len

对应 P8.1–P8.3；规范 §6、§7 impl/关联项、§11 调用/方法/数组方法。全部待接入。

| 编号/步骤 | 输入/变体 | 预期与关键断言 | 层次/依赖 |
| --- | --- | --- | --- |
| P8-01 / .1 | `methods.rx`：&self get、&mut self set、Self::new、关联常量 | set 修改原对象；get 和显式关联调用返回一致；Self 按 StructId 解析 | S/I/E；P8.1–.2 |
| P8-02 / .1 | `inherent_clone.rx`：用户 `clone(&self, delta:i32)->i32` | 按真实签名调用且返回标量；不能按 clone 拼写调用 builtin | S/I/E；P8.1 |
| P8-03 / .1 | 两种结构体同名方法、全局同名函数，显式 `Type::method` | 使用 sema 已选 FunctionId；函数/类型名字空间不混淆 | S/I/E；P8.1 |
| P8-04 / .1 | `Type::method(value)` 未显式借用，但签名要求 &self | 作为普通参数不自动借用，正常语义拒绝；点调用正例另测 | S/C；P8.1 |
| P8-05 / .2 | `receiver_order.rx`：make 打印 1，arg 打印 2，method 打印 3 | 顺序 `1\n2\n3\n7\n`；接收者只构造一次，方法体在参数之后执行 | I/E；P8.2 |
| P8-06 / .2 | Copy 聚合按值 self，方法内修改 self 字段 | 方法拥有独立值；调用者仍保持原字段 | I/E；P7.4、P8.2 |
| P8-07 / .2 | 多层引用自动解引用后的共享/可变接收者 | 调整到原对象；共享路径不能通过可写临时副本绕过权限 | S/I/E；P8.2 |
| P8-08 / .2 | Box 后的方法调用，Box builtin clone 与 pointee 方法区分 | 使用已记录目标及完整调整路径，不重新查找 | S/I/E；P9/P10 联合验收 |
| P8-09 / .3 | `array_len.rx`：make_array 打印 9，再 .len | 输出 `9\n3\n`；长度是 usize/i32 常量，但不删除接收者副作用 | I/E；P8.3 |
| P8-10 / .3 | `array_len.rx`：if 一边提前 return 0，另一边 make_array 后 .len | 提前路径不执行 make/len；正常路径恰一次；输出参见 .out | I/E；P6、P8.3 |
| P8-11 / .3 | 已有数组 place、多层引用数组的 len | 不复制/移动整个数组来取长度；必须先完成接收者求值 | I/E；P8.3 |
| P8-12 / .1–.3 | 更早接收者候选被选中但其参数不匹配 | 报 sema 错误，不在 IR 尝试“更合适”的后续候选；排除歧义方法程序 | S/C；P8.1 |

方法解析记录先验收，再验收发射；Vec 的两阶段接收者行为留到 P9。
