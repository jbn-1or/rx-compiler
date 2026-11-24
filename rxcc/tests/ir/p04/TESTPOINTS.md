# P4：标量运算、移位与 cast

对应 P4.1–P4.3；规范 §10、§11 算术/比较/cast。种子已接入，完整矩阵的缺口见下文；E 均要求 O0/O2 相同。
种子使用显式整数后缀；除零、MIN/-1 和 MIN%-1 不属于测试域。

| 编号/步骤 | 输入/变体 | 预期与关键断言 | 层次 |
| --- | --- | --- | --- |
| P4-01 / .1 | `wrapping.rx`：MAX+1、MIN-1、65536×65536、-MIN；u32 MAX+1/0-1 | `-2147483648\n2147483647\n0\n-2147483648\n0\n-1\n` | I/E |
| P4-02 / .1 | `division.rx`：-7/3、-7%3、7/-3、7%-3；u32 MAX/2、%2 | `-2\n-1\n-2\n1\n2147483647\n1\n`；分别 sdiv/srem 与 udiv/urem | U/I/E |
| P4-03 / .1 | `comparison.rx`：i32 -1<0、u32 MAX<1、false<true、true>false | `1\n0\n1\n1\n`；bool 使用无符号比较 | I/E |
| P4-04 / .1 | 四种整数的六类比较，equal/less/greater 各一组 | 结果矩阵精确；不得从 LLVM i32 猜回源符号性 | U/I/E |
| P4-05 / .1 | `bitwise.rx`：整数 &/\|/^/!；bool 的 &,\|,^,! | 两侧函数标记均打印，即使左侧已决定结果；整数 ! 按全位取反 | I/E |
| P4-06 / .1 | `mark(1)+mark(2)`、两边嵌套调用 | 输出先 1 后 2，最终标量结果正确；每侧一次 | I/E |
| P4-07 / .2 | `shifts.rx`：count=0/31/32/33/-1；左侧有/无符号，右侧不同整数类型 | 32≈0、33≈1、-1≈31；-8>>1=-4，0xfffffff8u32>>1=2147483644 | U/I/E |
| P4-08 / .2 | get 得到 count 后移位，与常量 count 对照 | 非常量同样 `&31`；不依赖优化常量折叠获得正确行为 | I/E；stdin 多组 |
| P4-09 / .2 | `casts.rx`：-1i32→u32→i32；0x80000000u32→i32；true/false 到四种整数 | 位模式不变；bool 转整数恰为 1/0，使用 zext 而非 sext | U/I/E |
| P4-10 / .2 | i32/u32/isize/usize 的 16 种整数 cast | 0、最高位、全 1 位模式往返不变；源类型改变后除法/比较指令随之改变 | U/I/E |
| P4-11 / .3 | 检查 wrapping 与 shifts 的初版 `.ll` | add/sub/mul 不附 nsw/nuw，除/移位不附未经证明的 exact；移位掩码存在 | I |
| P4-12 / .3 | 手工二元/比较/cast 模型的错误操作数或结果类型 | builder/checker 拒绝 i1+i32、错误比较结果、非法转换组合；printer 输出合法文本 | U/I |
| P4-13 / .1–.3 | get 输入边界后执行同类运算 | O0/O2 stdout 相同；防止只对编译期已知字面量正确 | E |

排序的引用变体到 P5，相等的结构化变体到 P10；短路逻辑到 P6。

## 接入证据与缺口

以下只记录断言已接线，不报告本轮执行成绩。`entry`、`model`、`runner` 分别指
`../entry_test.cpp`、`../model_test.cpp`、`../../run_ir.py`；runner 加 `--reimu`
才执行 O0/O2。源码生成、手工驱动和 semantic 检查的证据不得混同。

| 编号 | 当前接入证据 | 未补齐或不能据此推断的部分 |
| --- | --- | --- |
| P4-01 | 已接入：runner `p04-wrapping` 执行种子；entry `check_scalar_operations` 检查回绕指令和无附加承诺 | 无额外矩阵缺口记录 |
| P4-02 | 已接入：runner `p04-division`；entry 检查 sdiv/srem/udiv/urem，model 检查相应指令 | 无额外矩阵缺口记录 |
| P4-03 | 已接入：runner `p04-comparison`；entry 检查整数符号性和 bool 无符号谓词 | 无额外矩阵缺口记录 |
| P4-04 | 已接入：entry `make_scalar_operation_driver` 从源码生成四种整数和 bool 的六类比较；runner `p41-operation-results` 独立列出期望值 | 入口调用和 bool 打印扩展由手工 IR 驱动补入，不能描述为完整源码 I/O 程序 |
| P4-05 | 已接入：runner `p04-bitwise`、`p41-bool-bitwise-order`；entry/model 检查整数及 bool 运算 | 无额外矩阵缺口记录 |
| P4-06 | 部分：entry `check_scalar_operations` 和 runner `p41-evaluation-order` 检查简单左右标记调用顺序 | 两侧各含嵌套调用的完整变体待补 |
| P4-07 | 已接入：runner `p04-shifts`；entry `check_shifts_and_casts` 追踪每条 shift 对 `count & 31` 的依赖，并用损坏模型检验该断言 | 更多左右整数类型组合另由 P5 引用移位矩阵联合覆盖 |
| P4-08 | 部分：runner `p42-dynamic-shift-32`、`-33`、`-negative` 以 stdin 提供计数 | 运行期 0/31 与常量计数逐项对照矩阵未补齐 |
| P4-09 | 已接入：runner `p04-casts`；entry 检查位模式与 zext，runner 检查八次 bool 扩展且无 sext | 无额外矩阵缺口记录 |
| P4-10 | 部分：现有 cast 种子覆盖 i32/u32 位模式和 bool 到四种整数 | 四种整数间 16 种 cast × 三种位模式，以及 cast 后除法/比较变化的完整矩阵待补 |
| P4-11 | 已接入：runner 的全模块 `verify_module` 拒绝 nsw/nuw/exact 等承诺；entry 检查移位掩码数据依赖 | runner 的指令计数检查不能单独代替 entry 的数据依赖断言 |
| P4-12 | 已接入：model `add_operations`、`check_invalid_models`、转换构建反例及 `--emit-model` 外部验证 | 无额外矩阵缺口记录 |
| P4-13 | 部分：runner `p43-dynamic-overflow` 覆盖运行期加/乘；动态移位及 P5 `p54-runtime-input` 覆盖部分其他运算 | 所有标量运算的运行期输入边界矩阵未补齐 |
