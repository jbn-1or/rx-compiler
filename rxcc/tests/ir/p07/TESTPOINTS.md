# P7：布局、构造、投影、值快照与聚合 ABI

对应 P7.1–P7.4；规范 §8、§10、§11、§16。全部待接入。
布局查询 U、独立 LLVM 布局探针 I/E、源码行为 E 三类证据分别记录。

| 编号/步骤 | 输入/构造 | 预期与关键断言 | 层次/依赖 |
| --- | --- | --- | --- |
| P7-01 / .1 | `{bool,i32,bool}`，两元素结构体数组 | offsets=0/4/8、size=12、align=4、stride=12；bool 存储 i8，非 packed | U/I；P7.1 |
| P7-02 / .1 | 嵌套数组、引用字段、Box、Vec 头 | i32/ptr/Box 4/4，bool 1/1；Vec 12/4；数组 stride 含尾填充，使用目标而非宿主 sizeof | U/I；P7.1 |
| P7-03 / .1 | 独立 `.ll` 用 GEP 的偏移差查询字段和数组步长 | 执行探针与 U 布局结果完全相同；不能只依赖 verifier | I/E；手写探针，不需源码投影 |
| P7-04 / .1 | Node{children:Vec<Node>}，相互经 Box/Vec 引用的类型 | 布局与打印有限；命名类型先登记，不展开 Box/Vec 子布局 | U/I；P7.1，运行到 P9 |
| P7-05 / .1 | 极大数组长度、size×N/align_up/字段累加溢出 | 单元输入导致可诊断失败，不发生宿主溢出/死循环；不尝试分配巨型有效程序 | U；P7.1 |
| P7-06 / .2 | `construction_order.rx`：声明 first/second，源码 second:mark(2),first:mark(1) | 先输出 2、1，字段读取分别为 1、2；求值顺序与存放位置分开 | I/E；P7.2 |
| P7-07 / .2 | `repeat_coercion.rx`：数组列表与重复元素；`[&r;2]` 转 [&i32;2] | mark 列表各一次、重复只一次；解引用转换在复制前完成；两个元素读到 7 | S/I/E；P7.2 |
| P7-08 / .2 | `[noncopy;1]` 与 N>1 Copy 元素；大 N 重复 | N=1 移动允许，无 Clone；大 N 用有界 IR 初始化，不按 N 展开代码 | S/I/E；非 Copy 容器到 P9 |
| P7-09 / .2 | 嵌套字段/索引读写、借用、引用自动解引用；base/index 打印 | 修改原对象；base 在 index 前且均一次；index 为 usize；GEP 携带正确 storage type | U/I/E；P7.2 |
| P7-10 / .2 | 无期望数组的 &mut/& 元素 LUB，交换顺序 | 每项按最终类型调整；与显式期望数组分开验证 | S/I/E；P6.7、P7.2 |
| P7-11 / .3 | `copy_values.rx`：Copy 结构体/数组副本、整体赋值、自赋值 | 副本修改不影响原值；数组不退化为指针；重叠搬运安全 | I/E；P7.3 |
| P7-12 / .3 | Copy 结构体有共享引用字段；复制后读取引用目标 | 复制引用地址，引用仍指原对象；不错误深拷贝引用目标 | I/E；P7.3 |
| P7-13 / .3 | 聚合 RHS 完成后才求 LHS；嵌套聚合目标/结果 slot | 独立完整值在 LHS 副作用前固定；移动拥有字段不 clone | I/E；P7.3，拥有字段 P9 |
| P7-14 / .4 | `aggregate_calls.rx`：mut 数组/结构体参数、返回嵌套聚合 | callee 修改不影响 caller；返回后读到独立完整值，不能返回 callee 栈地址 | I/E；P7.4 |
| P7-15 / .4 | `temporary_array.rx`：`make()[i]` 与 `&mut make()[0]` | 每个 make 恰一次；借用临时元素可后续读写，保持存储期限 | I/E；P7.4 |
| P7-16 / .4 | 多个聚合实参，其间有打印/独立数据写入；混合 bool/i32/引用/聚合 | 左到右立即固定快照；调用前每个值已准备；签名定义/调用/return 一致 | U/I/E；P7.4 |
| P7-17 / .4 | 聚合经 if/loop break/提前 return，多层递归返回 | 所有正常边完整写结果；不同调用的结果和递归帧独立 | I/E；P6、P7.4 |
| P7-18 / .4 | FunctionABI 和 GEP 的错误参数数、结果位置、字段序号、类型 | builder/checker 拒绝；baseline 不自动发 inbounds/sret/byval 等未证明属性 | U/I；P7.4 |

涉及多个实参间快照时，还须在 I 层检查复制时机；不得用违反借用规则的 Rust 程序制造差异。
