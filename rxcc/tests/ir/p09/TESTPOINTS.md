# P9：Box、Vec、增长与 remove

对应 P9.1–P9.4；规范 §14、§16 分配接口。Box、Vec 查询、索引、push 和 remove 用例已接入 `run_ir.py`，`--reimu` 验证 RV32 O0/O2 的输出和退出码。
增长策略/容量不可观察；通过长度、活动元素和 I/U 检查验证实现。

| 编号/步骤 | 输入/构造 | 预期与关键断言 | 层次/依赖 |
| --- | --- | --- | --- |
| P9-01 / .1 | `boxes.rx`：函数返回 Box、改 pointee、移动再读 | 内容跨返回存活，写到正确目标；移动只转移拥有者，不 clone | I/E；P9.1 |
| P9-02 / .1 | `box_places.rx`：Box<Box<i32>>、Box<数组/结构体/引用> | 多层 `*`、非 Copy 内容移动；不可变拥有者中保存的 `&mut` 仍能写入目标 | S/I/E；P9.1 |
| P9-03 / .1 | `boxes.rx`、`box_places.rx` 的引用转换、字段/索引/方法与共享路径负例 | 转换到原目标；基址、下标、实参各一次；共享访问不能放宽可变性 | S/I/E/C；P5/P7/P8、P9.1 |
| P9-04 / .1 | `ir_entry_test` 检查分配器 ABI、分配参数、提前退出和移动 | `ptr __rx_alloc(i32,i32)`；Mixed 12/4、bool 1/1、bool 数组 3/1、指针 4/4；实参提前退出及移动时不分配 | U/I/C；P9.1 |
| P9-05 / .2 | `empty_vec.rx` 和 `ir_entry_test`：Vec::new、len/is_empty | 输出 `0\n1\n`；头字段初始化为 `{null, 0, 0}`，不分配，不访问空缓冲区；查询读取传入头的 `len` | U/I/E；P9.2 |
| P9-06 / .2 | `vec_places.rx`：结构体/数组中的 Vec 头经函数传入/返回；嵌套索引；提前返回 | 查询同一头；基址或下标提前返回不投影；只生成非空索引函数的 IR，不在空 Vec 上执行索引 | U/I/E；非空运行依赖 P9.3 |
| P9-07 / .3 | `vec_growth.rx`：循环 push 0..65，逐项检查，累加输出 | len=65、sum=2080；多次增长保持顺序，不读取备用容量 | I/E；P9.3 |
| P9-08 / .3 | `vec_two_phase.rx`：Vec<usize> 上 `v.push(v.len())` 两次 | 输出 `0\n1\n2\n`；固定头地址→实参→读当前元数据→push | I/E；P9.3 |
| P9-09 / .3 | `vec_push_places.rx`：字段/索引上的 push、非 Clone 结构体、Box、嵌套 Vec、可变引用 | 点调用和限定调用写回真实头；接收者先于实参；实参提前退出不追加 | S/I/E；P9.3 |
| P9-10 / .3 | `ir_vec_capacity_boundary`：用模拟的满头求生成 IR 的分配参数 | 容量或字节数倍增超限时，仍请求足够的可表示字节；只检查分配调用，不实际申请大块内存 | U；P9.3 |
| P9-11 / .4 | `vec_remove.rx`：删首、中、末，删空后再 push | removed 和剩余顺序精确，参见 .out；重叠搬运正确 | I/E；P9.4 |
| P9-12 / .4 | `remove_owned.rx`：非 Clone 结构体和内层 Vec 经函数按值返回 | 外层移位或覆写后，返回的拥有值仍可独立读写 | S/I/E；P9.4 |
| P9-13 / .4 | `vec_remove_order.rx` 与 `ir_entry_test` | 点调用和限定调用按顺序求值；提前退出不删除；后缀为空时不搬运；只更新 len，不分配 | U/I/E；P9.4 |
| P9-14 / .2–.4 | 不可变 `Vec<&mut i32>` 写元素、重借用元素、嵌套 Vec push | `run_ir.py` 核对语义诊断和输出保护；权限贯穿 Box 和后续索引 | S/C |
| P9-15 / .2–.4 | mut Vec、&mut Vec、Box<&mut Vec>，元素含 &mut | 可变访问从 Vec 索引之前获得，修改原目标；数组/Box 的对应正例不误拒绝 | S/I/E |
| P9-16 / .2–.4 | grow/remove 后重新取索引；不保存失效引用 | 只比较活动序列；不把增长后旧地址仍有字节误当有效引用 | I/E |
| P9-17 / .1–.4 | 检查 runtime 与生成 IR 的职责 | runtime 只有 I/O/分配/通用字节搬运；类型布局、len/cap、循环操作在生成代码中 | I/C |

不要求 free、析构或分配失败 panic；不能用“允许泄漏”放宽 move/借用/克隆语义。
P9.2 的 `ir_entry_test` 检查基址和下标各求一次，之后才读取 `data`，并使用实际元素布局计算地址，不复制借用的元素。
