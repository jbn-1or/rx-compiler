# P7：布局、构造、投影、值快照与聚合 ABI

对应 P7.1–P7.4；规范 §8、§10、§11、§16。布局、种子和多项 ABI/快照断言已接入，完整矩阵边界见下文。
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

## 接入证据与缺口

以下记录当前测试代码，不是本轮执行结果。`entry`、`model`、`runner` 分别指
`../entry_test.cpp`、`../model_test.cpp`、`../../run_ir.py`；runner 加 `--reimu`
才执行 O0/O2。独立 `layout_probe.ll` 是 LLVM 布局对照，不计为源码生成能力。

| 编号 | 当前接入证据 | 未补齐或不能据此推断的部分 |
| --- | --- | --- |
| P7-01 | 已接入：entry `check_named_storage_layouts` 精确检查 Mixed 字段、size/align、数组 stride 及非 packed 文本 | LLVM 对照由 P7-03 单独提供 |
| P7-02 | 已接入：entry 检查嵌套 bool 数组、引用字段、Box/Vec 和尾填充；model `check_layout_limits` 检查标量及 Vec 布局 | 无额外矩阵缺口记录 |
| P7-03 | 已接入：runner `p71-layout-probe` 对独立 GEP 偏移/stride/对齐探针检查固定输出，与 entry 的同组期望对照 | 必须执行探针才有 E，单独 verifier 通过不够 |
| P7-04 | 已接入：entry 检查 Node 和 Left/Right 经 Box/Vec 的有限布局及命名类型文本；runner `forward-struct-layout` 外部验证 | 递归拥有值操作是 P9/P10 联合覆盖，不计入单纯布局证据 |
| P7-05 | 已接入：model `check_layout_limits`、`check_invalid_named_layouts` 覆盖乘法/字段累加/字段及尾对齐溢出；runner 四种 oversized 源码检查诊断和输出保护 | 不分配或执行巨型对象 |
| P7-06 | 已接入：runner `p72-construction-order`；`p72-places` 补混排字段读写 | 无额外矩阵缺口记录 |
| P7-07 | 已接入：runner `p72-repeat-coercion`、`p72-repeat-large`；entry `check_repeat_reference_conversion` 检查转换在循环前且仅一次 | 不仅凭两个元素打印相同值判断转换时机 |
| P7-08 | 部分：runner `p72-repeat-large` 覆盖非 Copy Token 的 N=1 和 Copy 聚合的 513 元素，并限制 GEP 数量、要求动态索引 | 含拥有容器的非 Copy 重复元素专例及明确无 Clone 的结构断言待补 |
| P7-09 | 已接入：runner `p72-places` 覆盖嵌套投影/借用/自动解引用和 base/index 标记；model `check_aggregate_gep` 检查显式类型及动态索引 | GEP 模型自检与源码外部验证是不同证据 |
| P7-10 | 已接入：runner `p72-array-lub` 覆盖 &mut/& 顺序交换、引用链和显式期望数组 | 不由单个 `repeat_coercion.rx` 推断整个 LUB 矩阵 |
| P7-11 | 已接入：runner `p73-copy-values` 覆盖独立副本、整体及自赋值，`p73-snapshot-order` 补重叠来源的时机 | 不要求填充字节保持原值 |
| P7-12 | 已接入：entry `check_shallow_reference_copy` 检查复制只读 ptr、不读 pointee；runner `p73-reference-fields-and-move` | 无额外矩阵缺口记录 |
| P7-13 | 部分：runner `p73-snapshot-order`、`p73-assignment-early-exit` 覆盖 RHS 快照先于 LHS 副作用及提前退出 | 拥有字段 move 不 clone 的对应赋值/结果槽结构矩阵未补齐，不以普通 Ticket 移动替代 |
| P7-14 | 已接入：runner `p74-aggregate-calls` 检查 mut 数组/结构体参数独立；`p74-recursive-results` 补嵌套聚合返回；entry `check_aggregate_call_abi` 检查结果槽写满再返回 | 原 manifest 种子本身不包含嵌套返回，需引用额外回归 |
| P7-15 | 已接入：runner `p74-temporary-array`、`p74-temporary-slots` 检查构造次数、不同临时及后续写入 | 无额外矩阵缺口记录 |
| P7-16 | 已接入：entry `check_aggregate_call_abi` 追踪隐藏结果、两个快照、普通引用和 bool/i32 位置以及副作用前后写满的元素；runner `p74-mixed-arguments` | entry `check_parameter_snapshot_reuse` 另检查 callee 直接投影传入快照，不额外搬运 |
| P7-17 | 部分：runner `p73-results`、`p74-recursive-results` 覆盖 if/break/return 及多次递归返回；entry 检查聚合返回写满结果槽 | 尚无对所有嵌套聚合结果正常边逐一追踪写入的完整结构矩阵 |
| P7-18 | 已接入：model `check_aggregate_abi_validation`、`check_aggregate_gep` 检查错误隐藏参数/布局/字段/索引；`add_aggregate_calls` 已接入 `--emit-model` 外部验证；runner 全局禁止 inbounds/sret/byval | 外部正例覆盖命名结构体/数组 GEP、按值参数和隐藏结果调用；非法模型仍只在 U 层拒绝，不作为执行程序 |
