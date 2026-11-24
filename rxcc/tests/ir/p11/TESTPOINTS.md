# P11：正式接线、资源与联合验收

对应 P11.1–P11.4；规范全部章节，尤其 §16。P11.1–P11.3 的入口已接入并通过本地验证；最终联合验收仍见 P11.4。Rust 参考路径的结果不计入自有 IR 验收。

| 编号/步骤 | 操作/程序 | 必须断言 | 层次/依赖 |
| --- | --- | --- | --- |
| P11-01 / .1 | 检查 config.mk 和实际 compile.command | IR 命令直接调用 rxcc --stage ir，runtime 正式存在；无 rustc/参考 .ll 拷贝 | C；P0–P10 |
| P11-02 / .1 | 正式 runner 对一个含 I/O/分配的程序执行全链路 | 自检、llvm-as/opt、Clang RV32、程序/runtime 集成汇编、REIMU 全部实际执行 | I/E/C |
| P11-03 / .1 | runtime 符号及多模块链接 | main 与内部源入口分离；三个 I/O/allocator 定义各一次，C ABI 一致 | I/E |
| P11-04 / .2 | `make test STAGE=semantic`、`make test STAGE=ir`、`python3 rxcc/tests/run_ir.py --reimu --official` | 前两者覆盖语义清单与从 codegen 派生的正式 IR；第三个命令按源码收集原始 optimization 清单和全部 I/O，单独报告源数与执行次数 | C/E |
| P11-05 / .2 | 开发小样例、官方 IR 全量、官方 optimization 大程序 | 正式路径用自己的 IR；大程序每个源码生成一次 IR，O0/O2 各生成一次汇编，两者逐组核对官方预期输出和退出码；不修改清单、不筛掉失败 | E/C |
| P11-06 / .2 | 开发 runner 的非法源码与合法但未实现的 lowering 探针 | 语义错误须和 semantic 入口诊断相同；两类失败均退出 1，不创建新 IR，也不覆盖既有输出。探针通过不算相应 lowering 已实现 | S/C |
| P11-07 / .3 | `loop_storage.rx`：100000 次循环借用临时标量并累加 | IR 中的固定 `alloca` 只在入口块；O0/O2 在 1 MiB 栈下均正常退出并输出 `100000\n` | I/E/C |
| P11-08 / .3 | `recursive_frames.rx`：递归 64 层，局部在递归前后使用 | O0/O2 均输出 `2080\n`；子调用不破坏调用者的局部值 | I/E |
| P11-09 / .3 | `--official` 复用 `comprehensive-large-frame` 和 `calls-recursion-and-abi` | 大数组、超出 12 位的地址偏移或常量、多参数及活跃局部值，O0/O2 汇编通过并逐组核对原始 I/O | I/E/C |
| P11-10 / .3 | `mixed_pipeline.rx`：结构体+Vec<Box<i32>>+clone+remove+eq | O0/O2 均输出 `1\n3\n1\n`；原拥有值、克隆、删除结果互不混淆 | I/E；P10 |
| P11-11 / .3 | `--official` 中的官方资源用例及 optimization 大程序 | 在 256 MiB 内存、1 MiB 栈下输出正确且退出为 0；保留 IR、汇编和执行日志。未测量精确的累计堆分配字节数 | E/C |
| P11-12 / .3 | 复用 P10 的递归类型 helper 调用图和重复编译检查 | 输出确定；helper 数稳定，无重复符号、未完成定义和无限递归 | U/I/C |
| P11-13 / .3 | 复用开发 runner 的回绕、shift、引用、赋值顺序、聚合快照等关键集 | O0/O2 分别与同一份预期比较，不仅比较两者彼此相同 | I/E |
| P11-14 / .3 | 独立 Release 干净构建、源码与 CMake 检查、编译与链接产物审计 | 使用 C++20，未引用 LLVM API、头文件或链接库；只有外部测试调用 LLVM CLI | C |
| P11-15 / .3 | `runner-failures/`：缺工具、超时、signal、程序非零退出、缺输出/profile、stdout 截断 | 所有探针均被拒绝；每次运行使用新目录并保存命令、工具版本、原始 I/O 路径、实际输出和退出记录 | C |
| P11-16 / .4 | CI 从干净 checkout 执行完整入口 | 清单、夹具、runner 被 Git 跟踪；固定 LLVM22；报告覆盖和实际结果，不把设计文档当通过证据 | C |

P11.2 本地验收：语义 245/245；正式 IR 60 个程序、115 组 I/O；开发夹具 242 个 LLVM 模块、484 次 RV32 执行；官方 optimization 13 个程序、39 组原始 I/O、78 次 RV32 执行。CI 配置已接入 `--reimu --official`；官方大程序每次运行的 `compile.command`、`io.txt`、产物和 profile 保存在 `target/tests/official-ir-*/`。

P11.3 本地验收：`python3 rxcc/tests/run_ir.py --reimu --official` 覆盖 3 个专项夹具（6 次执行）、6 个官方 codegen 资源用例（20 组 I/O、40 次执行），并复跑 242 个开发模块（484 次执行）、13 个官方 optimization 大程序（39 组 I/O、78 次执行）。正式 `make test STAGE=ir` 仍为 60 个用例、115 组 I/O 全通过；独立 Release 构建的 CTest 为 6/6。开发 runner 的 `p11/`、`codegen-boundaries/`、`runner-failures/` 目录均保留可核对的证据。本地验证使用 LLVM/Clang 22.1.8 和 REIMU 1.0.1，未等待远端 CI。

最终检查：P0–P10 的 U/S/I/E/C 都有对应证据；所有源错误仍先经过完整 sema；
所有有效测试输出精确、退出码正确。`make test STAGE=optimization` 仅在明确使用自己的
IR → Clang 路径时才算自有 IR 证据，不能只看阶段名。
