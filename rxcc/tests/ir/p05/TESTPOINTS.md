# P5：引用、临时位置、转换和赋值

对应 P5.1–P5.4；规范 §10 强制转换、§11 place/运算/赋值、§13。
种子、引用读取顺序及赋值矩阵已接入；具体证据与剩余变体见下文。
标记函数只操作独立数据，不通过冲突别名制造“可观察”顺序。

| 编号/步骤 | 输入/变体 | 预期与关键断言 | 层次/依赖 |
| --- | --- | --- | --- |
| P5-01 / .1 | `references.rx`：旧 x 的引用、遮蔽、`&mut 3` 写成 4 | 输出 `1\n2\n4\n`；旧绑定存储不因遮蔽复用 | I/E；写入 P5.3 |
| P5-02 / .1 | `borrow_once.rx`：`&mut make()`，make 打印 9，随后读写 | 输出 `9\n3\n4\n`；调用一次且临时存储活到最后使用 | I/E；P5.3 |
| P5-03 / .1 | `&x`、`&&x`、`&&&x` 与逐层显式解引用 | 引用 slot 的 load 与目标 load 区分；每个 * 只解一层 | U/I/E |
| P5-04 / .1 | 不可变绑定 p 保存 &mut i32，`*p=4` | 更新原目标而非引用的临时副本；不以绑定 mut 代替路径权限 | I/E |
| P5-05 / .2 | `coercions.rx`：&mut→&、&&i32→&i32、带类型重借用后再用原 p | 输出 `7\n7\n8\n9\n`；真正需要指针 load 的转换不能按 ptr 直通 | S/I/E |
| P5-06 / .2 | let、参数、return、赋值四处相同转换 | 使用已存 CoercionPlan；原始表达式类型与各使用位置调整分开 | S/U/I/E |
| P5-07 / .2 | &Box→&T、多层 Box 及 &mut Box→&mut T | 每层指针读取正确，不移动 pointee；共享路径不能变可写 | S/I/E；P9 |
| P5-08 / .3 | `assignment_order.rx`：目标函数打印 1，RHS 打印 2；普通/复合赋值 | 输出 `2\n1\n7\n2\n1\n17\n`；两种赋值 RHS 优先，地址函数各一次 | I/E |
| P5-09 / .3 | 同上：目标函数先将旧值设为 10 再返回地址，RHS=7 | 复合赋值得 17；不能在目标函数前读旧值 | I/E；同一 fixture |
| P5-10 / .3 | `assignment_return.rx`：`*target(&mut x)=return;`，target 打印 99 | 不输出 99；RHS 发散阻止 LHS 求值和 store | I/E |
| P5-11 / .3 | 十种复合赋值：+=、-=、*=、/=、%=、&=、\|=、^=、<<=、>>= | RHS 优先、Unit 继续执行；每种有边界结果；移位同样掩码低 5 位 | U/I/E |
| P5-12 / .4 | `reference_operands.rx`：`&a + &b`、`-&a`、`!&bool`、标量 += &b | 输出 `10\n-3\n1\n10\n1\n`；共享引用读目标，运算不修改 a/b | I/E |
| P5-13 / .4 | 左操作数返回共享引用并打印 1，右侧函数打印 2，操作/复合赋值后打印结果 | IR 中先获得全部操作数，再读引用目标；直接标量在自身求值处固定 | I/E；U 检查发射顺序，不使用冲突别名 |
| P5-14 / .4 | 同层引用排序，另测 &T < &mut T | 比较目标值；&mut T < &T、内部层级不匹配属于普通类型拒绝用例 | S/I/E |
| P5-15 / .1–.4 | ptr 的 load/store/call/ret；错显式存储类型/归属 | builder/checker 按携带类型检查；opaque ptr 不掩盖源类型错误 | U/I |
| P5-16 / .1–.4 | 引用相关函数及局部存取的 baseline IR | 不自动加 noalias 等未证明属性；入口固定临时 slot，循环中只初始化 | I；循环 P6 |

延后读取的语义顺序以 I 层断言为主，不能为制造结果差异而违反共享引用的有效性保证。

## 接入证据与缺口

以下是测试代码的接入状态，不是本轮通过记录。`entry`、`model`、`runner` 分别指
`../entry_test.cpp`、`../model_test.cpp`、`../../run_ir.py`；结构自检与外部 LLVM 验证
分开记录，runner 的 E 断言需要 `--reimu`。源码权限负例是 S/C，不是正例 E。

| 编号 | 当前接入证据 | 未补齐或不能据此推断的部分 |
| --- | --- | --- |
| P5-01 | 已接入：runner `p53-references`；entry `check_reference_places` 检查原地址与逐层读取 | 无额外矩阵缺口记录 |
| P5-02 | 已接入：runner `p53-borrow_once`；entry `check_reference_temporaries` 检查调用一次、不同临时及入口 slot | 无额外矩阵缺口记录 |
| P5-03 | 已接入：entry `check_reference_places` 检查 load ptr 与 load i32 链；runner `p51-nested` | model 的 ptr 往返补充 U，不代替源码 E |
| P5-04 | 已接入：`references.rx`、`borrow_once.rx` 的不可变 p 写原临时目标进入 runner | 绑定自身的赋值权限仍由 S 负例单独检查 |
| P5-05 | 已接入：runner `p53-coercions`；entry `check_reference_coercions` 逐层追踪指针读取 | 无额外矩阵缺口记录 |
| P5-06 | 已接入：entry 的引用转换、实参转换顺序及 `check_assignment_coercion_order`；runner `p52-*`、`p53-conversion-order` 覆盖 let/参数/return/赋值 | 这里列的是实际转换/发射证据，不声称已完整覆盖所有原始语义事实的 S 查询矩阵 |
| P5-07 | 已接入 P9 联合种子：`boxes.rx`、`box_places.rx` 覆盖多层 Box 引用转换与可变目标；entry 另检查 narrow/deep 可生成 | 共享路径的写入拒绝是 S/C，不能作为 Box 正例执行成绩 |
| P5-08 | 已接入：runner `p53-assignment_order`；entry `check_assignment_order` 检查 RHS→目标→读取/写入且调用一次 | 无额外矩阵缺口记录 |
| P5-09 | 已接入：同一赋值种子的目标先改旧值；entry 追踪旧值 load 使用目标调用返回地址 | 不仅凭输出中出现 17 判断顺序 |
| P5-10 | 已接入：runner `p53-assignment_return` 及 RHS/目标各阶段提前退出变体；entry `check_assignment_early_return` | 无额外矩阵缺口记录 |
| P5-11 | 已接入：runner 生成四种整数 × 十种 op= × 直接/引用 RHS，另有 bool 真值表、`p53-all-operator-order`、动态移位和 Unit 上下文 | 这是 runner 的生成矩阵，不应只按 manifest 种子数量统计 |
| P5-12 | 已接入：runner `p54-reference-operands`、整数三种引用组合、bool 真值表及一元边界矩阵；entry 检查一元读取/转换 | 无额外矩阵缺口记录 |
| P5-13 | 已接入：entry `check_reference_binary_order`、`check_mixed_reference_operand_order`、`check_assignment_order` 检查延后读取；runner 有独立数据标记用例 | E 的标记顺序本身不能证明内容 load 的发射时机，需保留结构断言 |
| P5-14 | 已接入：entry `check_reference_ordering_chains`；runner 五种标量 × 三层深度 × 四种排序及可变引用变体 | 反向/内部层级不匹配的拒绝只计 S/C |
| P5-15 | 已接入：model `add_pointer_storage`、`reject_pointer_model` 和 builder 存取反例；`--emit-model` 经 runner 外部验证 | 只检查显式类型、值归属与签名，不从 opaque ptr 反推 pointee 源类型 |
| P5-16 | 已接入联合回归：runner `verify_module` 全局禁止未证明属性；entry 检查引用临时的入口分配；P11 `loop_storage.rx` 循环十万次初始化 `&mut 1`，并由 `check_loop_storage` 检查固定入口 slot | 该循环的结构/执行证据来自 P11 专项，不是单凭 P5 的一次性临时借用推断 |
