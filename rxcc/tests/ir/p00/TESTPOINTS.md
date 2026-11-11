# P0：工具链、入口与运行时

对应计划 P0.1–P0.2；规范 §15、§16。本阶段使用手写 IR，不作为源码 lowering 完成的证据。
已有基础：`../model_test.cpp` 输出空程序，`../../run_ir.py --reimu` 验证 O0/O2 空输出。
下列带 I/O 的模块和故障注入待接入；运行时可先使用规范 C 示例。

| 编号 | 输入/操作 | 断言 | 层次/依赖 |
| --- | --- | --- | --- |
| P0-01 | 手写 internal void 源 main；external i32 main 调用后返回 0 | 两个入口不同；空 stdout；profile 恰有 `Exit code: 0` | I/E；已有空模块基础 |
| P0-02 | 手写调用 `println_i32(42)` | stdout 精确为 `42\n`，不是字面反斜杠 n 或 CRLF | I/E；运行时 |
| P0-03 | 依次 `print_i32(1)`、`print_i32(-2)`、`println_i32(0)` | stdout 为 `1-20\n`，print 不添加分隔符 | I/E；运行时 |
| P0-04 | 连续三次 get；stdin=` -2147483648 0 2147483647\n`；逐个 println | 输出三行原十进制值；正确处理前导空白、符号与全 32 位范围 | I/E；运行时 |
| P0-05 | 核对 triple、layout、Clang 参数 | 指针/usize/isize 均为 32 位，小端，rv32im、ilp32；O0/O2 相同 | I/C |
| P0-06 | 同一 `.ll` 经 llvm-as、opt verify、Clang -S、Clang -c | 任一步失败即停止；程序和 runtime 汇编均能被集成汇编器接受 | C |
| P0-07 | 注入非法 IR、缺失输出、缺工具、REIMU 非零退出或超时 | runner 返回失败；不比较旧文件、不把无 stdout 当成功 | C；隔离临时目录 |
| P0-08 | 期望文件含末尾换行；实际去掉换行/多一个空格/截断 | 三种差异全部失败；保留编译命令、stderr、profile 与输出差异 | C |
| P0-09 | 同一用例两次执行；第二次不写输出文件 | 清理或唯一化执行产物；不能复用第一次的 stdout | C |
| P0-10 | 用 REIMU `--memory=256M --stack=1M` 执行 | 明确资源参数，记录实际工具版本；不得换用宿主执行冒充 RV32 | E/C |

验收：空输出与 I/O 都有实际 RV32 结果；验证与编译器进程分离。工具缺失记录为未执行。
