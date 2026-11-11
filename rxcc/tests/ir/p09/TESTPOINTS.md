# P9：Box、Vec、增长与 remove

对应 P9.1–P9.4；规范 §14、§16 分配接口。全部待接入，E 需规范运行时。
增长策略/容量不可观察；通过长度、活动元素和 I/U 检查验证实现。

| 编号/步骤 | 输入/构造 | 预期与关键断言 | 层次/依赖 |
| --- | --- | --- | --- |
| P9-01 / .1 | `boxes.rx`：函数返回 Box、改 pointee、移动再读 | 内容跨返回存活，写到正确目标；移动只转移拥有者，不 clone | I/E；P9.1 |
| P9-02 / .1 | Box<Box<i32>>、Box<数组/结构体/引用> | 按类型布局分配；多层 * 每次投影一层；拥有者不可变但内部 &mut 可写的正例 | U/I/E；P9.1 |
| P9-03 / .1 | `boxes.rx` 的 &Box/&mut Box 转到目标引用 | 路径有必要的指针 load；字段/索引/方法自动解引用，base 一次 | S/I/E；P5/P7/P8、P9.1 |
| P9-04 / .1 | 分配器声明/调用及分配尺寸跟踪 | `ptr __rx_alloc(i32,i32)`；参数为 size/align；Mixed 12/4，不用宿主大小 | U/I/C；P9.1 |
| P9-05 / .2 | `empty_vec.rx`：Vec<i32>::new、len/is_empty | 输出 `0\n1\n`；头 12/4；不分配零字节、不解引用 null | U/I/E；P9.2 |
| P9-06 / .2 | Vec 位于结构体/数组/外层 Vec；经函数传入/返回 | 更新真实头，不能只修改头的临时副本；拥有者移动正确 | I/E；非空依赖 P9.3 |
| P9-07 / .3 | `vec_growth.rx`：循环 push 0..65，逐项检查，累加输出 | len=65、sum=2080；多次增长保持顺序，不读取备用容量 | I/E；P9.3 |
| P9-08 / .3 | `vec_two_phase.rx`：Vec<usize> 上 `v.push(v.len())` 两次 | 输出 `0\n1\n2\n`；固定头地址→实参→读当前元数据→push | I/E；P9.3 |
| P9-09 / .3 | push 非 Clone 结构体、Box、嵌套 Vec | 不要求 Copy/Clone；增长搬运仅 len 个已初始化元素，无深克隆 | S/I/E；P9.3；`remove_owned.rx` |
| P9-10 / .3 | 分配字节数、容量计算的目标上限/溢出辅助单元输入 | 检查乘法/倍增后可表示再分配；不使用巨型运行样例耗尽 256MiB | U；P9.3 |
| P9-11 / .4 | `vec_remove.rx`：删首、中、末，删空后再 push | removed 和剩余顺序精确，参见 .out；重叠搬运正确 | I/E；P9.4 |
| P9-12 / .4 | `remove_owned.rx`：remove 非 Clone 拥有结构体、嵌套 Vec | 返回被移出的原值，可继续使用；先保存结果再移动后缀，不深克隆 | S/I/E；P9.4 |
| P9-13 / .4 | remove(index_fn())，接收者/index 函数打印 | 接收者先、index 后且各一次；返回值不被后缀覆盖；最后一个后缀长度 0 | I/E；P9.4 |
| P9-14 / .2–.4 | 不可变 `Vec<&mut i32>` 写元素、重借用元素、嵌套 Vec push | 普通 sema 拒绝；权限贯穿 Box/字段/后续索引；`immutable_vec_write.rx` | S/C |
| P9-15 / .2–.4 | mut Vec、&mut Vec、Box<&mut Vec>，元素含 &mut | 可变访问从 Vec 索引之前获得，修改原目标；数组/Box 的对应正例不误拒绝 | S/I/E |
| P9-16 / .2–.4 | grow/remove 后重新取索引；不保存失效引用 | 只比较活动序列；不把增长后旧地址仍有字节误当有效引用 | I/E |
| P9-17 / .1–.4 | 检查 runtime 与生成 IR 的职责 | runtime 只有 I/O/分配/通用字节搬运；类型布局、len/cap、循环操作在生成代码中 | I/C |

不要求 free、析构或分配失败 panic；不能用“允许泄漏”放宽 move/借用/克隆语义。
