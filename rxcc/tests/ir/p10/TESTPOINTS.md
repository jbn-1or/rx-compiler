# P10：Clone、PartialEq/Eq 和递归辅助函数

对应 P10.1–P10.3；规范 §8、§10 递归类型、§14 克隆/相等。全部待接入。
P10.1/.2 首轮仅非递归类型；递归运行和缓存完整性在 P10.3 单独验收。

| 编号/步骤 | 输入/构造 | 预期与关键断言 | 层次/依赖 |
| --- | --- | --- | --- |
| P10-01 / .1 | `deep_clone.rx`：派生结构体拥有 Box 与 Vec，clone 后改克隆 | 原拥有数据不变；共享引用字段仍引用相同目标 | I/E；P10.1 |
| P10-02 / .1 | 数组、嵌套 Box/Vec、空 Vec 的 clone | 每个活动元素递归克隆；空 Vec 不请求 0 字节；不克隆 spare capacity | U/I/E；P10.1 |
| P10-03 / .1 | `shared_ref_clone.rx`：r:&NonClone，r.clone() | 返回引用且可读原字段，不要求 NonClone 实现 Clone，不克隆 pointee | S/I/E；P10.1 |
| P10-04 / .1 | 显式 Box::clone 和用户同名 inherent clone | 分别遵循已选 builtin/FunctionId；字段派生不调用 inherent clone | S/I/E；P8、P10.1 |
| P10-05 / .1 | 单独 clone &mut、Vec<NonClone> 的 clone | 普通 sema 拒绝；正例的共享引用 clone 不误拒绝 | S/C；排除歧义点调用 |
| P10-06 / .2 | `equality.rx`：独立 Box、独立 Vec、两个不同地址的 &i32 | 内容相同为 true；不同元素/长度为 false；比较后双方仍可使用 | I/E；P10.2 |
| P10-07 / .2 | `equality.rx`：Vec push 3 次后 remove 到 2，与只 push 2 次比较 | 活动序列相同为 true；不观察容量或要求某种增长策略 | I/E；P10.2 |
| P10-08 / .2 | `{bool,i32,bool}` 派生 PartialEq，赋值历史不同 | 按字段比较、不使用整体 memcmp；填充字节不参与相等 | I/E；P10.2 |
| P10-09 / .2 | `eq_order.rx`：两个值生产函数分别打印 1/2，== 和 != | 每次按 1、2 求值且各一次；!= 只取反一次比较，不重复求值 | I/E；P10.2 |
| P10-10 / .2 | 多层引用、数组、嵌套拥有聚合相等 | 递归比较目标/字段/元素，不比较地址；不能以 lowered ptr 相同允许跨类型相等 | U/I/E；只使用同源类型 |
| P10-11 / .3 | `recursive_tree.rx`：Node{value,children:Vec<Node>} 派生 Clone/PartialEq/Eq | 编译终止；深克隆后先相等，修改子节点后不等，原树内容保持 | S/I/E/C；P10.3 |
| P10-12 / .3 | A 含 Vec<B>，B 含 Vec<A>，构造有限叶值 | 相互递归类型和 helper 有限；clone/eq 正确；每个操作/类型只生成一次 | U/I/E/C；P10.3 |
| P10-13 / .3 | 多次请求同类型 clone/eq；同名字段布局不同结构体、不同长度数组 | TypeKey 含操作种类、StructId、子类型、数组长度；不串用 helper | U/I；P10.3 |
| P10-14 / .3 | 正在生成 helper 时递归请求自身 | 先登记身份与签名，命中 Generating 返回该身份；最终全部 Complete、有定义 | U/I；P10.3 |
| P10-15 / .3 | 分支/循环/函数内首次生成 helper，之后还有局部赋值与跳转 | builder 插入点、当前函数、绑定和循环上下文恢复；未向 helper 尾部误发指令 | U/I/E；P10.3 |
| P10-16 / .3 | `struct Chain{next:Box<Chain>}` 仅声明不用 | 有限命名布局；即使无可构造终止值也不编译期无限展开，不强制生成未使用 helper | S/I/C；P10.3 |

验收同时检查行为和 helper 清单；外部 verifier 合法不代表深克隆与内容相等正确。
