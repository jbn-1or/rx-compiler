# P10：Clone、PartialEq/Eq 和递归辅助函数

对应 P10.1–P10.3；规范 §8、§10 递归类型、§14 克隆/相等。源码用例已接入 `run_ir.py`；`--reimu` 验证 RV32 O0/O2 的输出和退出码。
P10.1/.2 使用非递归类型验收，递归行为和 helper 完整性由 P10.3 单独检查。

| 编号/步骤 | 输入/构造 | 预期与关键断言 | 层次/依赖 |
| --- | --- | --- | --- |
| P10-01 / .1 | `deep_clone.rx`：派生结构体拥有 Box 与 Vec，clone 后改克隆 | 原拥有数据不变；共享引用字段仍引用相同目标 | I/E；P10.1 |
| P10-02 / .1 | `container_clone.rx`：嵌套数组/Box/Vec；新建及删空后的 Vec | 独立修改克隆；空路径不分配或访问缓冲区；循环只处理 `len` 个活动元素，不用源 `cap` | U/I/E；P10.1 |
| P10-03 / .1 | `shared_ref_clone.rx`：r:&NonClone，r.clone() | 语义接受；IR 返回同一目标地址，不访问目标内容；运行时可读原字段 | S/U/I/E；P10.1 |
| P10-04 / .1 | `clone_dispatch.rx`：整数、bool、显式 Box::clone、`&Tag` 的内置 Clone、用户同名方法 | 分别遵循已选 builtin/FunctionId；派生不调用同名用户方法；接收者只求值一次，提前返回时跳过克隆 | S/I/E；P8、P10.1 |
| P10-05 / .1 | `clone_mutable_reference.rx`、`clone_nonclone_vec.rx` | 语义阶段给出具体诊断；IR 入口报相同诊断并保留已有输出 | S/C；P10.1 |
| P10-06 / .2 | `equality.rx`：独立 Box、独立 Vec、两个不同地址的 &i32 | 内容相同为 true；不同元素/长度为 false；比较后双方仍可使用 | I/E；P10.2 |
| P10-07 / .2 | `equality.rx`：追加历史不同的 Vec、删空 Vec 和新建空 Vec | 同一活动序列相等；不观察容量或要求某种增长策略；IR 只使用 data/len | U/I/E；P10.2 |
| P10-08 / .2 | `equality.rx` 和 `ir_entry_test`：`{bool,i32,bool}` 赋值历史不同 | 按字段宽度读取两侧，不用整块字节比较；不读取填充 | U/I/E；P10.2 |
| P10-09 / .2 | `eq_order.rx`：分别比较值和索引位置，操作数可提前返回 | 左右各求值一次，左先右后；提前返回跳过尚未发生的求值或比较 | I/E；P10.2 |
| P10-10 / .2 | `equality_nested.rx`、`equality_requires_partial_eq.rx` | 多层引用、可变引用、数组和嵌套拥有值按内容比较；缺少 PartialEq 在语义阶段拒绝 | S/I/E；P10.2 |
| P10-11 / .3 | `recursive_tree.rx`：Node{value,children:Vec<Node>} 派生 Clone/PartialEq/Eq | 编译终止；深克隆后先相等，修改子节点后不等，原树内容保持 | S/I/E/C；P10.3 |
| P10-12 / .3 | `mutual_recursion.rx`：A → Vec<B> → B → Vec<A> → A 叶子 | 深克隆和相等运行正确；每种操作四个 helper 形成同一递归环 | I/E/C；P10.3 |
| P10-13 / .3 | `helper_cache.rx`：多次请求；结构体共享字段名但布局不同；不同长度数组 | 类型或操作不同不串用；每种操作恰好四个 helper；多次编译结果稳定 | I/E；P10.3 |
| P10-14 / .3 | `recursive_tree.rx`、`mutual_recursion.rx` 的调用图 | 回边引用已登记身份；签名正确，每个内部 helper 恰好有一份完整定义 | I；P10.3 |
| P10-15 / .3 | `helper_cache.rx`：在分支/循环里首次请求，之后继续赋值和跳转 | 源函数 IR 继续在正确的块发射，循环及结果正确 | I/E；P10.3 |
| P10-16 / .3 | `recursive_declaration.rx`：`Chain{next:Box<Chain>}` 仅声明不用 | 有限命名布局；无运行时构造，不生成未使用 helper | S/I/E；P10.3 |

验收同时检查行为和 helper 清单；外部 verifier 合法不代表深克隆与内容相等正确。
