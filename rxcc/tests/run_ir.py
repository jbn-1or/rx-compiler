#!/usr/bin/env python3
"""Validate rxcc IR fixtures and optionally the official optimization corpus."""

import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from strip_asm_debug import strip_debug
from test import RV32_TARGET, TestError, discover, execute, excerpt


def run(command, *, expected=0, timeout=60, stdin_data=None):
    try:
        process = subprocess.Popen(
            command, text=True,
            stdin=subprocess.PIPE if stdin_data is not None else subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
    except FileNotFoundError as error:
        raise RuntimeError(f"required executable not found: {command[0]}") from error
    with process:
        try:
            input_text = stdin_data.decode("utf-8") if stdin_data is not None else None
            stdout, stderr = process.communicate(input=input_text, timeout=timeout)
        except (subprocess.TimeoutExpired, KeyboardInterrupt) as error:
            # xmake may spawn REIMU; terminate the entire group, not just xmake.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            stdout, stderr = process.communicate()
            if isinstance(error, KeyboardInterrupt):
                raise
            raise RuntimeError(
                f"timed out after {timeout}s: {command}\n"
                f"stdout:\n{stdout}\nstderr:\n{stderr}"
            ) from None
    result = subprocess.CompletedProcess(command, process.returncode, stdout, stderr)
    if result.returncode != expected:
        raise RuntimeError(
            f"unexpected exit {result.returncode} (expected {expected}): {command}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def check_p10_helpers(name, text, expected_per_operation, *, recursive=False):
    # 不固定 helper 编号，只检查定义、调用边和操作之间的隔离。
    symbol = r"__rx_(?:clone|eq)_helper_\d+"
    definitions = re.findall(rf"^define internal (void|i1) @({symbol})"
                             r"\(ptr [^,\n]+, ptr [^)\n]+\) \{\n(.*?)^\}",
                             text, re.MULTILINE | re.DOTALL)
    all_symbols = set(re.findall(rf"@({symbol})\b", text))
    defined = {function for _, function, _ in definitions}
    if all_symbols != defined or len(definitions) != len(defined):
        raise RuntimeError(f"{name}: every helper must have exactly one internal definition with its ABI")

    for operation, return_type in (("clone", "void"), ("eq", "i1")):
        functions = {}
        for result, function, body in definitions:
            if not function.startswith(f"__rx_{operation}_helper_"):
                continue
            if result != return_type:
                raise RuntimeError(f"{name}: {function} has the wrong result type")
            calls = set(re.findall(rf"\bcall (?:void|i1) @({symbol})\(", body))
            functions[function] = calls
        if len(functions) != expected_per_operation:
            raise RuntimeError(f"{name}: expected {expected_per_operation} {operation} helpers, got {len(functions)}")
        if any(calls - functions.keys() for calls in functions.values()):
            raise RuntimeError(f"{name}: {operation} helper calls a helper for a different operation")
        if recursive:
            # 两个递归样例的类型依赖各组成一个环；定义数量正确但缺少回边也要失败。
            start = current = next(iter(functions))
            visited = set()
            while current not in visited:
                visited.add(current)
                calls = functions[current]
                if len(calls) != 1:
                    raise RuntimeError(f"{name}: {current} must call exactly one helper in its cycle")
                current = next(iter(calls))
            if current != start or visited != functions.keys():
                raise RuntimeError(f"{name}: {operation} helpers do not form the expected recursive cycle")


# 官方大程序使用正式 runner 的日志和文件 stdin；开发小样例仍使用内存输入。
def run_artifact_tool(command, log=None, stdin=None, *, timeout=60):
    if log is None:
        run(command, stdin_data=stdin, timeout=timeout)
        return
    try:
        code = execute(shlex.join(command), log, timeout, stdin)
    except TestError as error:
        log.with_suffix(".status").write_text(f"{error}\n", encoding="utf-8")
        raise TestError(f"{log}: {error}") from error
    log.with_suffix(".status").write_text(f"Exit code: {code}\n", encoding="utf-8")
    if code != 0:
        raise RuntimeError(f"{log}: unexpected exit {code}\n{excerpt(log.with_suffix('.stderr'))}")


# 只检查栈槽放置与是否静态分配，不依赖块编号、临时变量名或槽数。
def check_loop_storage(text):
    saw_stack_slot = False
    for body in re.findall(r"^define .*?\{\n(.*?)^\}", text, re.MULTILINE | re.DOTALL):
        block = -1
        for line in body.splitlines():
            if re.match(r"^[\w.$-]+:", line):
                block += 1
            if re.match(r"^\s*%\S+\s*=\s*alloca\b", line):
                saw_stack_slot = True
                if block != 0 or not re.search(r"= alloca (?:i\d+|ptr), align \d+$", line):
                    raise RuntimeError(f"loop_storage: expected a fixed entry stack slot, got {line.strip()}")
    if not saw_stack_slot:
        raise RuntimeError("loop_storage must exercise fixed stack slots")


# 小样例和官方大程序都检查同一个自有 IR 基线及 LLVM 合法性。
def verify_module(args, name, ir_path, bitcode, logs=None):
    text = ir_path.read_text(encoding="utf-8")
    if name in ("discarded-if-value", "discarded-loop-and-logic") and re.search(
            r"\b(?:alloca|store|load)\b", text):
        raise RuntimeError(f"{name}: discarded results must not need a storage slot")
    for forbidden in (" nsw ", " nuw ", " exact", "noalias",
                      "inbounds", "nonnull", "sret(", "byval("):
        if forbidden in text:
            raise RuntimeError(f"{name}: baseline IR must not contain {forbidden.strip()}")
    run_artifact_tool([args.llvm_as, str(ir_path), "-o", str(bitcode)],
                      logs / "llvm-as" if logs else None)
    if not bitcode.is_file():
        raise RuntimeError(f"{name}: llvm-as did not create {bitcode}")
    run_artifact_tool([args.opt, "-passes=verify", "-disable-output", str(bitcode)],
                      logs / "opt-verify" if logs else None)


# REIMU 进程成功不代表被模拟程序成功；输出必须是独立产物。
def check_rv32_output(name, stdout, profile, expected_stdout):
    for artifact in (stdout, profile):
        if not artifact.is_file():
            raise RuntimeError(f"{name}: missing REIMU artifact {artifact}")
    actual = stdout.read_bytes()
    exits = [line for line in profile.read_bytes().splitlines() if line.startswith(b"Exit code:")]
    if exits != [b"Exit code: 0"]:
        raise RuntimeError(f"{name}: expected one REIMU exit code 0, got {exits!r}")
    if actual != expected_stdout:
        raise RuntimeError(f"{name}: stdout differs (expected {len(expected_stdout)} bytes "
                           f"{expected_stdout[:200]!r}, got {len(actual)} bytes {actual[:200]!r})")


# 用受控进程和独立产物检查 runner 不会把异常当作通过。
def check_runner_failures(work):
    work.mkdir()

    def expect_failure(description, diagnostic, function, *args, **kwargs):
        try:
            function(*args, **kwargs)
        except (TestError, RuntimeError) as error:
            if diagnostic not in str(error):
                raise RuntimeError(f"{description}: unexpected diagnostic: {error}") from error
            return f"{description}: {error}\n"
        raise RuntimeError(f"{description}: expected the runner to fail")

    results = [
        expect_failure("missing tool", "unexpected exit 127", run_artifact_tool,
                       [str(work.resolve() / "missing-tool")], work / "missing-tool"),
        expect_failure("timeout", "timed out", run_artifact_tool,
                       [sys.executable, "-c", "import time; time.sleep(10)"], work / "timeout", timeout=0.2),
        expect_failure("signal", "unexpected exit", run_artifact_tool,
                       [sys.executable, "-c", "import os, signal; os.kill(os.getpid(), signal.SIGTERM)"],
                       work / "signal"),
    ]
    stdout, profile, expected = work / "program.out", work / "program.profile", b"100000\n"
    results.append(expect_failure("missing output", "missing REIMU artifact", check_rv32_output,
                                  "probe", stdout, profile, b""))
    stdout.write_bytes(expected)
    results.append(expect_failure("missing profile", "missing REIMU artifact", check_rv32_output,
                                  "probe", stdout, profile, expected))
    profile.write_bytes(b"Exit code: 7\n")
    results.append(expect_failure("simulated exit", "expected one REIMU exit code 0", check_rv32_output,
                                  "probe", stdout, profile, expected))
    profile.write_bytes(b"Exit code: 0\n")
    stdout.write_bytes(expected[:-1])
    results.append(expect_failure("truncated stdout", "stdout differs", check_rv32_output,
                                  "probe", stdout, profile, expected))
    (work / "results.txt").write_text("".join(results), encoding="utf-8")


# 每个优化级别只生成和检查一次汇编，再用同一份文本执行所有 I/O。
def run_rv32_module(args, name, ir_path, runtime_assembly, output_dir, pairs, *, keep_logs=False):
    executions = 0
    for optimization in ("O0", "O2"):
        prefix = output_dir / f"{ir_path.stem}-{optimization}"
        assembly = output_dir / f"{prefix.name}.s"
        run_artifact_tool([args.clang, *RV32_TARGET, "-mllvm", "-riscv-no-aliases",
                           f"-{optimization}", "-S", str(ir_path), "-o", str(assembly)],
                          prefix.with_name(f"{prefix.name}-clang") if keep_logs else None)
        assembly.write_text(strip_debug(assembly.read_text(encoding="utf-8")), encoding="utf-8")
        obj = output_dir / f"{prefix.name}.o"
        run_artifact_tool([args.clang, *RV32_TARGET, "-c", str(assembly), "-o", str(obj)],
                          prefix.with_name(f"{prefix.name}-assemble") if keep_logs else None)
        if not obj.is_file():
            raise RuntimeError(f"{name} {optimization}: Clang did not create {obj}")
        for index, (stdin, expected) in enumerate(pairs, 1):
            run_prefix = prefix.with_name(f"{prefix.name}-io-{index}")
            stdout = output_dir / f"{run_prefix.name}.out"
            profile = output_dir / f"{run_prefix.name}.profile"
            run_artifact_tool(["xmake", "run", "-P", "vendor/REIMU", "reimu",
                               "--memory=256M", "--stack=1M", "-f", f"{assembly},{runtime_assembly}",
                               "-o", str(stdout), "-p", str(profile)],
                              run_prefix if keep_logs else None, stdin=stdin)
            expected_stdout = expected.read_bytes() if isinstance(expected, Path) else expected
            check_rv32_output(f"{name} {optimization} I/O {index}", stdout, profile, expected_stdout)
            executions += 1
    return executions


# 从同一清单收集同一源码的全部原始 I/O，避免为每组输入重复编译。
def manifest_programs(root, stage):
    programs = {}
    for case in discover(root):
        if case.stage == stage:
            programs.setdefault(case.source, []).extend(case.io)
    if not programs:
        raise RuntimeError(f"no {stage} cases found under {root}")
    return programs


# 保留源码、输入/预期映射和实际工具日志；三个测试集合共用这条验证链。
def run_manifest_suite(args, programs, directory, runtime_assembly=None, *, resource_fixtures=False):
    directory.mkdir(exist_ok=True)
    executions = 0
    for index, (source, pairs) in enumerate(programs.items(), 1):
        work = directory / f"{index:04d}"
        work.mkdir()
        name = os.path.relpath(source, ROOT)
        (work / "io.txt").write_text("".join(
            f"{pair}: stdin={stdin if stdin is not None else '(empty)'}; expected={expected}\n"
            for pair, (stdin, expected) in enumerate(pairs, 1)), encoding="utf-8")
        if resource_fixtures:
            run_artifact_tool([args.compiler, "--stage", "semantic", str(source)], work / "semantic")
        ir_path = work / "program.ll"
        run_artifact_tool([args.compiler, "--stage", "ir", str(source), "-o", str(ir_path)], work / "compile")
        if resource_fixtures and source.name == "loop_storage.rx":
            check_loop_storage(ir_path.read_text(encoding="utf-8"))
        verify_module(args, name, ir_path, work / "program.bc", work)
        if runtime_assembly is not None:
            executions += run_rv32_module(args, name, ir_path, runtime_assembly, work, pairs, keep_logs=True)
    return executions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="rxcc/build/rxcc")
    parser.add_argument("--ir-test", default="rxcc/build/ir_entry_test")
    parser.add_argument("--model-test", default="rxcc/build/ir_model_smoke")
    parser.add_argument("--llvm-as", default="llvm-as-22")
    parser.add_argument("--opt", default="opt-22")
    parser.add_argument("--clang", default="clang-22")
    parser.add_argument("--reimu", action="store_true",
                        help="also run modules with RV32 Clang and REIMU")
    parser.add_argument("--official", action="store_true",
                        help="also run the official optimization corpus through rxcc IR (requires --reimu)")
    args = parser.parse_args()
    if args.official and not args.reimu:
        parser.error("--official requires --reimu")

    resources = manifest_programs(ROOT / "rxcc" / "tests" / "ir" / "p11", "codegen")
    official = {}
    boundaries = {}
    if args.official:
        official_root = ROOT / "tests" / "official"
        official = manifest_programs(official_root / "optimization", "optimization")
        for group in ("comprehensive-large-frame", "calls-recursion-and-abi", "comprehensive-owned-tree",
                      "comprehensive-quicksort", "comprehensive-shortest-path", "comprehensive-stack-machine"):
            for source, pairs in manifest_programs(official_root / "codegen" / group, "codegen").items():
                boundaries.setdefault(source, []).extend(pairs)
    tools = [args.compiler, args.ir_test, args.model_test, args.llvm_as, args.opt]
    if args.reimu:
        tools.extend([args.clang, "xmake"])
    for tool in tools:
        if shutil.which(tool) is None:
            raise RuntimeError(f"required executable missing or not executable: {tool}")

    run([args.ir_test])
    run([args.model_test, "--test"])
    invalid = run([args.model_test, "--invalid"], expected=1)
    if invalid.stdout or not invalid.stderr.strip():
        raise RuntimeError("invalid model must report diagnostics without partial IR")

    with tempfile.TemporaryDirectory(prefix="rxcc-p2-") as directory:
        temp = Path(directory)
        source = temp / "encoding.rx"
        for data, expected in ((b"fn main() {}\r\n", 0),
                               (b"\xef\xbb\xbffn main() {}\n", 1),
                               (b"// \xff\nfn main() {}\n", 1)):
            source.write_bytes(data)
            for stage in ("parse", "ast", "semantic", "ir"):
                output = ["-o", str(temp / "encoding.ll")] if stage == "ir" else []
                run([args.compiler, "--stage", stage, str(source), *output], expected=expected)
        modules = []
        for name, source_text in (
            ("empty-main", "fn main() {}"),
            ("explicit-unit", "fn main() -> () {}"),
            ("plain-helper", "fn f() {} fn main() {}"),
            ("lifetime-helper", "fn f<'a>() {} fn main() {}"),
            ("internal-names", "fn __rx_fn_1() {} fn main() {} fn helper() {}"),
            ("declarations", "const N: usize = 2; struct Data { values: [i32; N] } "
             "impl Data { const VALUE: i32 = 7; } fn main() {}"),
            ("forward-struct-layout", "struct Outer { pair: [Inner; 2] } "
             "struct Inner { flag: bool, value: i32, tail: bool } "
             "struct Node { children: Vec<Node>, next: Box<Node> } "
             "struct Matrix<'a> { bits: [[bool; 3]; 2], refer: &'a [Inner; 2], word: usize } "
             "fn main() {}"),
        ):
            source = temp / f"{name}.rx"
            output = temp / f"{name}.ll"
            source.write_text(source_text, encoding="utf-8")
            run([args.compiler, "--stage", "semantic", str(source)])
            run([args.compiler, "--stage", "ir", str(source), "-o", str(output)])
            text = output.read_text(encoding="utf-8")
            if "define i32 @main()" not in text or "ret i32 0" not in text:
                raise RuntimeError(f"{name}: missing machine main returning zero")
            if re.search(r"\b(?:alloca|load|store|add|sub|mul|icmp|zext|trunc|br)\b", text):
                raise RuntimeError(f"{name}: contains instructions outside the P2 subset")
            repeated = temp / f"{name}-repeated.ll"
            run([args.compiler, "--stage", "ir", str(source), "-o", str(repeated)])
            if repeated.read_bytes() != output.read_bytes():
                raise RuntimeError("identical source must produce deterministic text")
            modules.append((name, output, b"", b""))

        for name, fields in (
            ("oversized-array", "values: [i32; 1073741824usize]"),
            ("oversized-field-sum", "values: [bool; 4294967295usize], last: bool"),
            ("oversized-field-align", "values: [bool; 4294967295usize], last: i32"),
            ("oversized-tail-align", "first: i32, values: [bool; 4294967291usize]"),
        ):
            source = temp / f"{name}.rx"
            source.write_text(f"struct Huge {{ {fields} }} fn main() {{}}", encoding="utf-8")
            output = temp / f"{name}.ll"
            output.write_bytes(b"existing output\n")
            run([args.compiler, "--stage", "semantic", str(source)])
            rejected = run([args.compiler, "--stage", "ir", str(source), "-o", str(output)],
                           expected=1)
            if "布局" not in rejected.stderr or output.read_bytes() != b"existing output\n":
                raise RuntimeError(f"{name}: layout overflow must be diagnosed without replacing output")

        if (temp / "plain-helper.ll").read_bytes() != (temp / "lifetime-helper.ll").read_bytes():
            raise RuntimeError("a helper's lifetime parameter must erase to the same P2 IR")

        fixture = Path(__file__).resolve().parent / "ir" / "p03" / "println_42.rx"
        for name, source_text, expected in (
            ("println-42", fixture.read_text(encoding="utf-8"), fixture.with_suffix(".out").read_bytes()),
            ("println-7", "fn main() { println_i32(7); }", b"7\n"),
            ("println-0", "fn main() { println_i32(0); }", b"0\n"),
            ("println-sequence", "fn main() { println_i32(7); println_i32(0i32); "
             "println_i32(2147483647); }", b"7\n0\n2147483647\n"),
            ("println-unused", "fn unused() { println_i32(99); } "
             "fn main() { println_i32(42); }", b"42\n"),
        ):
            source = temp / f"{name}.rx"
            output = temp / f"{name}.ll"
            source.write_text(source_text, encoding="utf-8")
            run([args.compiler, "--stage", "ir", str(source), "-o", str(output)])
            text = output.read_text(encoding="utf-8")
            if text.count("declare void @println_i32(i32)") != 1:
                raise RuntimeError(f"{name}: expected exactly one println declaration")
            if re.search(r"%\w+\s*=\s*call void", text):
                raise RuntimeError(f"{name}: void call must not produce a value")
            repeated = temp / f"{name}-repeated.ll"
            run([args.compiler, "--stage", "ir", str(source), "-o", str(repeated)])
            if repeated.read_bytes() != output.read_bytes():
                raise RuntimeError(f"{name}: nondeterministic LLVM text")
            modules.append((name, output, expected, b""))

        unit_fixture = Path(__file__).resolve().parent / "ir" / "p03" / "unit_blocks.rx"
        unit_source = temp / "unit-blocks.rx"
        unit_output = temp / "unit-blocks.ll"
        unit_source.write_bytes(unit_fixture.read_bytes())
        run([args.compiler, "--stage", "ir", str(unit_source), "-o", str(unit_output)])
        modules.append(("unit-blocks", unit_output,
                        unit_fixture.with_suffix(".out").read_bytes(), b""))

        recursive_source = temp / "recursive-main.rx"
        recursive_output = temp / "recursive-main.ll"
        recursive_source.write_text("fn main() { main(); }", encoding="utf-8")
        run([args.compiler, "--stage", "ir", str(recursive_source), "-o",
             str(recursive_output)])
        recursive_ir = recursive_output.read_text(encoding="utf-8")
        if "call void @__rx_fn_0()" not in recursive_ir or "call void @main()" in recursive_ir:
            raise RuntimeError("source main call must target the internal source function")
        run([args.llvm_as, str(recursive_output), "-o", str(temp / "recursive-main.bc")])
        run([args.opt, "-passes=verify", "-disable-output", str(recursive_output)])

        for name, source_text, expected in (
            ("local-shadow", "fn main() { let x = 1; println_i32(x); "
             "let x = 2; println_i32(x); }", b"1\n2\n"),
            ("local-shadow-init", "fn main() { let x = 1; let x = x; "
             "println_i32(x); println_i32(x); }", b"1\n1\n"),
            ("local-order", "fn main() { let a = 7; let b = 42; println_i32(b); "
             "println_i32(a); println_i32(b); }", b"42\n7\n42\n"),
            ("local-constant", "const N: i32 = 42; fn main() { let x = N; "
             "println_i32(N); println_i32(x); }", b"42\n42\n"),
            ("local-associated-constant", "struct S {} impl S { const MIN: i32 = -2147483648; } "
             "fn main() { let x = S::MIN; println_i32(x); println_i32(S::MIN); }",
             b"-2147483648\n-2147483648\n"),
            ("local-unused", "fn unused() { let x = 8; } fn main() { let mut x: i32 = 7; "
             "println_i32(x); }", b"7\n"),
            ("local-integer-types", "fn main() { let a: u32 = 1u32; let b = a; "
             "let c: usize = 2usize; let d = c; let e: isize = 3isize; let f = e; "
             "println_i32(4); }", b"4\n"),
        ):
            source = temp / f"{name}.rx"
            output = temp / f"{name}.ll"
            source.write_text(source_text, encoding="utf-8")
            run([args.compiler, "--stage", "semantic", str(source)])
            run([args.compiler, "--stage", "ir", str(source), "-o", str(output)])
            repeated = temp / f"{name}-repeated.ll"
            run([args.compiler, "--stage", "ir", str(source), "-o", str(repeated)])
            if repeated.read_bytes() != output.read_bytes():
                raise RuntimeError(f"{name}: nondeterministic LLVM text")
            modules.append((name, output, expected, b""))

        p03 = Path(__file__).resolve().parent / "ir" / "p03"
        for name, source_text, expected, stdin_data in (
            ("scalar-calls", (p03 / "scalar_calls.rx").read_text(encoding="utf-8"),
             b"1\n2\n7\n", b""),
            ("literal-io", (p03 / "literal_io.rx").read_text(encoding="utf-8"),
             b"1242\n", b""),
            ("read-io", (p03 / "read_io.rx").read_text(encoding="utf-8"),
             (p03 / "read_io.out").read_bytes(), (p03 / "read_io.in").read_bytes()),
            ("read-single-negative", "fn main() { println_i32(get_i32()); }",
             b"-12\n", b"-12\n"),
        ):
            source = temp / f"{name}.rx"
            output = temp / f"{name}.ll"
            source.write_text(source_text, encoding="utf-8")
            run([args.compiler, "--stage", "semantic", str(source)])
            run([args.compiler, "--stage", "ir", str(source), "-o", str(output)])
            repeated = temp / f"{name}-repeated.ll"
            run([args.compiler, "--stage", "ir", str(source), "-o", str(repeated)])
            if repeated.read_bytes() != output.read_bytes():
                raise RuntimeError(f"{name}: nondeterministic LLVM text")
            modules.append((name, output, expected, stdin_data))

        # Additional P3.4 boundaries: many scalar arguments, nested i32 blocks,
        # and continuing to use a local after a value-returning call.
        for name, source_text, expected in (
            ("scalar-nine-args", "fn main() { println_i32(nine(1, 2, 3, 4, 5, 6, 7, 8, 9)); } "
             "fn nine(a: i32, b: i32, c: i32, d: i32, e: i32, f: i32, g: i32, h: i32, i: i32) -> i32 { i }",
             b"9\n"),
            ("nested-integer-block", "fn main() { println_i32({ let value = 7; { value } }); }",
             b"7\n"),
            ("call-then-continue", "fn main() { let x = 4; println_i32(add_one(x)); println_i32(x); } "
             "fn add_one(value: i32) -> i32 { value }", b"4\n4\n"),
        ):
            source = temp / f"{name}.rx"
            output = temp / f"{name}.ll"
            source.write_text(source_text, encoding="utf-8")
            run([args.compiler, "--stage", "semantic", str(source)])
            run([args.compiler, "--stage", "ir", str(source), "-o", str(output)])
            modules.append((name, output, expected, b""))

        # P3.5：return 结束当前路径，后续语句、实参和尾值都不能再执行。
        for name, source_text, expected, stdin_data in (
            ("early-return", (p03 / "early_return.rx").read_text(encoding="utf-8"),
             (p03 / "early_return.out").read_bytes(), b""),
            ("shadowing", (p03 / "shadowing.rx").read_text(encoding="utf-8"),
             (p03 / "shadowing.out").read_bytes(), b""),
            ("bare-return", "fn main() { return; }", b"", b""),
            ("never-let", "fn main() { let x = return; println_i32(99); }", b"", b""),
            ("annotated-never-let", "fn main() { let x: i32 = return; println_i32(99); }",
             b"", b""),
            ("return-stops-arguments",
             "fn main() { println_i32(pair(mark(1), return)); } "
             "fn pair(a: i32, b: i32) -> i32 { a } "
             "fn mark(value: i32) -> i32 { println_i32(value); value }",
             b"1\n", b"9\n"),
            # return 在第一个实参时，后面的输入调用和最终目标调用都不能生成。
            ("return-stops-later-argument",
             "fn main() { consume(return, get_i32()); println_i32(99); } "
             "fn consume(a: i32, b: i32) { println_i32(a); }",
             b"", b"9\n"),
            ("return-nested-block", "fn main() { { return; } println_i32(99); }", b"", b""),
            ("return-independence",
             "fn first() -> i32 { return 1; } fn second() -> i32 { 2 } "
             "fn main() { println_i32(first()); println_i32(second()); }",
             b"1\n2\n", b""),
            # P3.6：bool 参数/局部/返回用 i1，slot 用 i8；样例没有可见输出。
            ("bool-boundary", (p03 / "bool_boundary.rx").read_text(encoding="utf-8"),
             (p03 / "bool_boundary.out").read_bytes(), b""),
            ("bool-constants",
              "const YES: bool = true; fn main() { let a = YES; let b = { false }; "
              "let c = a; let d = b; }", b"", b""),
            ("unit-return-effects",
             "fn finish() { return { println_i32(1); () }; println_i32(99); } "
             "fn number() -> i32 { { let value: bool = { return 7; }; 99 } } "
             "fn main() { finish(); println_i32(number()); return (); }",
             b"1\n7\n", b""),
            ("scalar-constant-bits",
             "const MIN: i32 = -2147483648; const MAX: u32 = 4294967295u32; "
             "const INDEX: usize = 4294967295usize; const OFFSET: isize = -1isize; "
             "const NO: bool = false; fn main() { println_i32(MIN); "
             "let maximum = MAX; let index = INDEX; let offset = OFFSET; let no = NO; }",
             b"-2147483648\n", b""),
            ("mixed-scalar-storage",
             "fn mixed(number: i32, flag: bool, count: usize) -> bool { flag } "
             "fn main() { let number = 7; let flag = true; "
             "{ let number = false; let flag = 3i32; } "
             "let copy = number; let saved = flag; "
             "let result = mixed(copy, saved, 2usize); println_i32(number); }",
             b"7\n", b""),
        ):
            source = temp / f"{name}.rx"
            output = temp / f"{name}.ll"
            source.write_text(source_text, encoding="utf-8")
            run([args.compiler, "--stage", "semantic", str(source)])
            run([args.compiler, "--stage", "ir", str(source), "-o", str(output)])
            modules.append((name, output, expected, stdin_data))

        early_ir = (temp / "early-return.ll").read_text(encoding="utf-8")
        if early_ir.count("call void @println_i32") != 2 or "i32 99" in early_ir:
            raise RuntimeError("return must skip later statements in the same path")
        never_ir = (temp / "never-let.ll").read_text(encoding="utf-8")
        if "alloca i32" in never_ir or "println_i32" in never_ir:
            raise RuntimeError("a never let must not allocate or emit later effects")
        later_ir = (temp / "return-stops-later-argument.ll").read_text(encoding="utf-8")
        # 只检查调用指令：未执行的源码不能留下 get_i32 或 consume 的调用。
        if "call i32 @get_i32()" in later_ir or "call void @__rx_fn_1()" in later_ir:
            raise RuntimeError("return must skip later arguments and the target call")
        bool_ir = (temp / "bool-boundary.ll").read_text(encoding="utf-8")
        if ("alloca i8, align 1" not in bool_ir or "zext i1" not in bool_ir or
                "trunc i8" not in bool_ir or "call i1" not in bool_ir or
                "alloca i32" in bool_ir):
            raise RuntimeError("bool values must use i1, i8 slots, and zext/trunc")

        # P4.1：按源类型选择有符号/无符号指令；二元运算先左后右求值。
        for name, source_text, expected in (
            ("p41-wrapping",
             "fn main() { let max = 2147483647i32; let min = -2147483648i32; "
             "println_i32(max + 1); println_i32(min - 1); "
             "println_i32(65536i32 * 65536i32); println_i32(-min); }",
             b"-2147483648\n2147483647\n0\n-2147483648\n"),
            ("p41-signed-division",
             "fn main() { println_i32(-7i32 / 3i32); println_i32(-7i32 % 3i32); "
             "println_i32(7i32 / -3i32); println_i32(7i32 % -3i32); }",
             b"-2\n-1\n-2\n1\n"),
            ("p41-integer-bitwise",
             "fn main() { println_i32(6i32 & 3i32); println_i32(6i32 | 3i32); "
             "println_i32(6i32 ^ 3i32); println_i32(!0i32); }",
             b"2\n7\n5\n-1\n"),
            ("p41-evaluation-order",
             "fn mark(tag: i32, value: i32) -> i32 { println_i32(tag); value } "
             "fn main() { println_i32(mark(1, 40) + mark(2, 2)); }",
             b"1\n2\n42\n"),
            # 两侧都必须执行，输出标记证明 bool 位运算没有复用短路逻辑。
            ("p41-bool-bitwise-order",
             "fn mark(tag: i32, value: bool) -> bool { println_i32(tag); value } "
             "fn main() { let a = mark(1, false) & mark(2, true); "
             "let b = mark(3, true) | mark(4, false); "
             "let c = mark(5, true) ^ mark(6, true); "
             "let d = !mark(7, false); }",
             b"1\n2\n3\n4\n5\n6\n7\n"),
            # 无符号运算、bool 位运算和 bool 排序只检查 IR 指令，不借 cast 打印。
            ("p41-type-directed",
             "fn main() { let a = -7i32; let b = 3i32; "
             "let signed_q = a / b; let signed_r = a % b; let signed_less = a < b; "
             "let u = 4294967295u32; let two = 2u32; "
             "let wrapped = u + 1u32; let unsigned_q = u / two; "
             "let unsigned_r = u % two; let unsigned_less = u < two; "
             "let bool_less = false < true; let bool_equal = !false == true; "
             "let bool_and = true & false; }",
             b""),
        ):
            source = temp / f"{name}.rx"
            output = temp / f"{name}.ll"
            source.write_text(source_text, encoding="utf-8")
            run([args.compiler, "--stage", "semantic", str(source)])
            run([args.compiler, "--stage", "ir", str(source), "-o", str(output)])
            modules.append((name, output, expected, b""))

        wrapping_ir = (temp / "p41-wrapping.ll").read_text(encoding="utf-8")
        if (wrapping_ir.find("add i32") == -1 or
                wrapping_ir.find("sub i32") == -1 or
                wrapping_ir.find("mul i32") == -1 or
                " nsw " in wrapping_ir or " nuw " in wrapping_ir or
                " exact" in wrapping_ir):
            raise RuntimeError("P4.1 算术必须回绕且不能附加不溢出/精确性承诺")
        directed_ir = (temp / "p41-type-directed.ll").read_text(encoding="utf-8")
        for instruction in ("sdiv i32", "srem i32", "udiv i32", "urem i32",
                            "icmp slt i32", "icmp ult i32", "icmp ult i1",
                            "and i1", "xor i1"):
            if instruction not in directed_ir:
                raise RuntimeError(f"P4.1 缺少按源类型选择的指令：{instruction}")
        if "icmp slt i1" in directed_ir or "icmp sgt i1" in directed_ir:
            raise RuntimeError("P4.1 的 bool 排序必须使用无符号比较")
        order_ir = (temp / "p41-evaluation-order.ll").read_text(encoding="utf-8")
        if (order_ir.find("i32 1, i32 40)") >
                order_ir.find("i32 2, i32 2)") or
                order_ir.find("i32 2, i32 2)") >
                order_ir.find("add i32")):
            raise RuntimeError("P4.1 二元运算必须先求值左操作数，再求值右操作数")

        # 与 entry_test 的源码运算驱动同序：六类比较、算术、取反和有符号取负。
        # 期望值独立列出，不能只凭出现某条指令或空 stdout 判断运算结果正确。
        expected_values = []
        for signed in (True, False, True, False):
            expected_values.extend((0, 1, 0, 1, 0, 1))  # ==、!=
            expected_values.extend(
                (1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 1, 1) if signed else
                (0, 0, 1, 0, 1, 1, 1, 0, 0, 1, 1, 0))  # <、<=、>、>=
            if signed:
                expected_values.extend((
                    -4, -2147483648, -2147483646,  # +
                    -10, 2147483646, 2147483646,   # -
                    -21, 2147483647, 0,            # *
                    -2, 2147483647, -1073741824,   # /
                    -1, 0, 0,                     # %
                    6, -2147483648, 2147483647,    # !
                    7, -2147483647, -2147483648))  # 一元 -
            else:
                expected_values.extend((
                    1, 1, 131072, -3, -1, 0, -2, 0, 0,
                    2147483647, 0, 1, 1, 0, 0, 0, -1, -65537))
        expected_values.extend((
            1, 0, 0, 1, 0, 1, 1, 0,  # bool ==、!=
            0, 1, 0, 0, 1, 1, 0, 1,  # bool <、<=
            0, 0, 1, 0, 1, 0, 1, 1,  # bool >、>=
            0, 0, 0, 1, 0, 1, 1, 1,  # bool &、|
            0, 1, 1, 0, 1, 1, 0, 0)) # bool ^、!
        operation_output = temp / "p41-operation-results.ll"
        operation_output.write_text(
            run([args.ir_test, "--emit-operations"]).stdout, encoding="utf-8")
        expected_stdout = "".join(f"{value}\n" for value in expected_values).encode("ascii")
        modules.append(("p41-operation-results", operation_output, expected_stdout, b""))

        # P4.2：先掩码低五位再移位；cast 只换源类型或把 bool 零扩展。
        for name, source_text, expected, stdin_data in (
            ("p42-dynamic-shift-32",
             "fn main() { let count = get_i32(); println_i32(1i32 << count); }",
             b"1\n", b"32\n"),
            ("p42-dynamic-shift-33",
             "fn main() { let count = get_i32(); println_i32(1i32 << count); }",
             b"2\n", b"33\n"),
            ("p42-dynamic-shift-negative",
             "fn main() { let count = get_i32(); println_i32(1i32 << count); }",
             b"-2147483648\n", b"-1\n"),
            # P4.3：运行期输入跨过 i32/u32 边界，O0/O2 必须得到相同回绕结果。
            ("p43-dynamic-overflow",
             "fn main() { let x = get_i32(); println_i32(x + 1); "
             "let y = get_i32(); println_i32(y * 2); }",
             b"-2147483648\n-2\n", b"2147483647\n2147483647\n"),
        ):
            source = temp / f"{name}.rx"
            output = temp / f"{name}.ll"
            source.write_text(source_text, encoding="utf-8")
            run([args.compiler, "--stage", "semantic", str(source)])
            run([args.compiler, "--stage", "ir", str(source), "-o", str(output)])
            modules.append((name, output, expected, stdin_data))

        # P4 种子全部进入 O0/O2 执行；cast 让 bool 比较和 u32 结果可用于 I/O。
        p04 = Path(__file__).resolve().parent / "ir" / "p04"
        for name in ("wrapping", "division", "comparison", "bitwise",
                     "shifts", "casts"):
            source = temp / f"p04-{name}.rx"
            output = temp / f"p04-{name}.ll"
            source.write_bytes((p04 / f"{name}.rx").read_bytes())
            run([args.compiler, "--stage", "semantic", str(source)])
            run([args.compiler, "--stage", "ir", str(source), "-o", str(output)])
            modules.append((f"p04-{name}", output,
                            (p04 / f"{name}.out").read_bytes(), b""))

        shifts_ir = (temp / "p04-shifts.ll").read_text(encoding="utf-8")
        if (shifts_ir.count("shl i32") != 5 or
                shifts_ir.count("ashr i32") != 1 or
                shifts_ir.count("lshr i32") != 1 or
                shifts_ir.count("and i32") != 7):
            raise RuntimeError("P4.2 每个移位都必须先与 31 相与")
        if " exact" in shifts_ir or " nsw " in shifts_ir or " nuw " in shifts_ir:
            raise RuntimeError("P4.2 移位不能附加不溢出/精确性承诺")
        casts_ir = (temp / "p04-casts.ll").read_text(encoding="utf-8")
        if casts_ir.count("zext i1") != 8 or "sext" in casts_ir:
            raise RuntimeError("P4.2 bool 到整数必须零扩展")
        if "sub i32 0, 1" not in casts_ir or "i32 -2147483648)" not in casts_ir:
            raise RuntimeError("P4.2 整数 cast 必须保留 32 位位模式")

        # P5.1 显式引用与 P5.2 固定位置转换；均不依赖 P5.3 写入。
        # 每条源码都经过外部 LLVM 校验，并在 --reimu 下检查 O0/O2 的输出和退出码。
        for name, source_text, expected, stdin_data in (
            ("p51-unused-reference", "fn unused(value: &i32) {} fn main() {}", b"", b""),
            ("p51-shadow",
             "fn main() { let x = 1; let old = &x; let x = 2; "
             "println_i32(*old); println_i32(x); }", b"1\n2\n", b""),
            ("p51-nested",
             "fn main() { let x = 7; let r = &x; let rr = &r; let rrr = &rr; "
             "println_i32(*r); println_i32(**rr); println_i32(***rrr); "
             "println_i32(***&&&x); }", b"7\n7\n7\n7\n", b""),
            ("p51-temporary-once",
             "fn make() -> i32 { println_i32(9); 3 } "
             "fn main() { let p = &mut make(); println_i32(*p); println_i32(*p); }",
             b"9\n3\n3\n", b""),
            ("p51-bool",
             "fn id(p: &bool) -> &bool { p } "
             "fn main() { let p = &true; let q = &mut false; "
             "println_i32((*id(p)) as i32); println_i32((*q) as i32); }", b"1\n0\n", b""),
            ("p51-return",
             "fn id(p: &i32) -> &i32 { return p; } fn read(p: &i32) -> i32 { *p } "
             "fn main() { let x = 7; let r = id(&x); println_i32(read(r)); }", b"7\n", b""),
            ("p51-mutable-return",
             "fn id(p: &mut i32) -> &mut i32 { p } "
             "fn main() { let mut x = 7; let p = id(&mut x); println_i32(*p); }", b"7\n", b""),
            ("p51-distinct-temporaries",
             "fn make(x: i32) -> i32 { println_i32(x); x } "
             "fn main() { println_i32(0); let a = &make(3); let b = &make(4); "
             "println_i32(*a); println_i32(*b); }", b"0\n3\n4\n3\n4\n", b""),
            ("p51-function-temporaries",
             "fn first() -> i32 { let p = &3; *p } "
             "fn second() -> i32 { let p = &4; *p } "
             "fn main() { println_i32(first()); println_i32(second()); println_i32(first()); }",
             b"3\n4\n3\n", b""),
            ("p51-constant",
             "const N: i32 = 7; fn main() { let a = &N; let b = &mut N; "
             "println_i32(*a); println_i32(*b); }", b"7\n7\n", b""),
            ("p51-explicit-reborrow",
             "fn id(p: &i32) -> &i32 { println_i32(9); p } "
             "fn main() { let x = 7; let r = &*id(&x); println_i32(*r); }", b"9\n7\n", b""),
            ("p51-reference-temporary",
             "fn id(p: &i32) -> &i32 { println_i32(9); p } "
             "fn main() { let x = 7; let r = &id(&x); println_i32(**r); }", b"9\n7\n", b""),
            ("p51-block-reference",
             "fn main() { let x = 7; let p = { println_i32(1); &x }; println_i32(*p); }",
             b"1\n7\n", b""),
            ("p51-borrow-return",
             "fn main() { let p = &mut { return; 3 }; println_i32(*p); }", b"", b""),
            ("p51-dereference-return",
             "fn main() { let x = 7; println_i32(*{ return; &x }); }", b"", b""),
            ("p51-runtime-input",
             "fn main() { let p = &get_i32(); println_i32(*p); println_i32(*p); "
             "println_i32(get_i32()); }", b"-12\n-12\n34\n", b"-12\n34\n"),
            ("p51-integer-types",
             "fn main() { let a = &-1i32; let b = &4294967295u32; "
             "let c = &-2isize; let d = &4294967294usize; "
             "println_i32(*a); println_i32((*b) as i32); "
             "println_i32((*c) as i32); println_i32((*d) as i32); }",
             b"-1\n-1\n-2\n-2\n", b""),
            # P5.2：零步调整、逐层指针读取和固定位置的重借用。
            ("p52-shared-argument",
             "fn read(p: &i32) -> i32 { *p } "
             "fn main() { let mut x = 7; let p = &mut x; println_i32(read(p)); }",
             b"7\n", b""),
            ("p52-annotated-let",
             "fn main() { let mut x = 7; let p = &mut x; "
             "let r: &i32 = p; println_i32(*r); }", b"7\n", b""),
            ("p52-nested-let",
             "fn main() { let x = 7; let r = &x; let rr = &r; let rrr = &rr; "
             "let flat: &i32 = { { rrr } }; println_i32(*flat); }", b"7\n", b""),
            ("p52-tail-return",
             "fn flatten(p: &&&i32) -> &i32 { { { p } } } "
             "fn main() { let x = 7; let r = &x; let rr = &r; "
             "println_i32(*flatten(&rr)); }", b"7\n", b""),
            ("p52-explicit-return",
             "fn flatten(p: &&i32) -> &i32 { return { { p } }; } "
             "fn main() { let x = 7; let r = &x; println_i32(*flatten(&r)); }", b"7\n", b""),
            ("p52-partial-chain",
             "fn partial(p: &&&i32) -> &&i32 { p } "
             "fn main() { let x = 7; let r = &x; let rr = &r; "
             "println_i32(**partial(&rr)); }", b"7\n", b""),
            ("p52-mutable-chain",
             "fn read(p: &mut i32) -> i32 { *p } "
             "fn main() { let mut x = 7; let mut p = &mut x; "
             "println_i32(read(&mut p)); println_i32(*p); }", b"7\n7\n", b""),
            ("p52-shared-chain",
             "fn read(p: &i32) -> i32 { *p } "
             "fn main() { let mut x = 7; let p = &mut x; "
             "println_i32(read(&p)); println_i32(*p); }", b"7\n7\n", b""),
            ("p52-reborrow-let",
             "fn read(p: &mut i32) -> i32 { *p } "
             "fn main() { let mut x = 7; let p = &mut x; "
             "{ let q: &mut i32 = p; println_i32(read(q)); } println_i32(read(p)); }",
             b"7\n7\n", b""),
            ("p52-reborrow-argument",
             "fn read(p: &mut i32) -> i32 { *p } "
             "fn main() { let mut x = 7; let p = &mut x; "
             "println_i32(read(p)); println_i32(read(p)); }", b"7\n7\n", b""),
            ("p52-call-once",
             "fn source(p: &&i32) -> &&i32 { println_i32(9); p } "
             "fn read(p: &i32) -> i32 { *p } "
             "fn main() { let x = 7; let r = &x; println_i32(read(source(&r))); }",
             b"9\n7\n", b""),
            ("p52-argument-order",
             "fn first(p: &&i32) -> &&i32 { println_i32(1); p } "
             "fn second(p: &i32) -> &i32 { println_i32(2); p } "
             "fn consume(a: &i32, b: &i32) -> i32 { *a + *b } "
             "fn main() { let x = 7; let r = &x; "
             "println_i32(consume(first(&r), second(&x))); }", b"1\n2\n14\n", b""),
            ("p52-bool-chain",
             "fn read(p: &bool) -> bool { *p } "
             "fn main() { let p = &mut true; let rr = &p; "
             "println_i32(read(rr) as i32); }", b"1\n", b""),
            ("p52-runtime-input",
             "fn read(p: &i32) -> i32 { *p } "
             "fn main() { let x = get_i32(); let r = &x; let rr = &r; "
             "let flat: &i32 = rr; println_i32(read(flat)); }", b"-12\n", b"-12\n"),
            ("p52-never-initializer",
             "fn main() { let p: &i32 = { println_i32(1); return; &&7 }; "
             "println_i32(*p); }", b"1\n", b""),
            ("p52-never-argument",
             "fn consume(p: &i32, value: i32) { println_i32(value); } "
             "fn main() { consume({ println_i32(1); return; &&7 }, get_i32()); }",
             b"1\n", b""),
        ):
            source = temp / f"{name}.rx"
            output = temp / f"{name}.ll"
            source.write_text(source_text, encoding="utf-8")
            run([args.compiler, "--stage", "semantic", str(source)])
            run([args.compiler, "--stage", "ir", str(source), "-o", str(output)])
            repeated = temp / f"{name}-repeated.ll"
            run([args.compiler, "--stage", "ir", str(source), "-o", str(repeated)])
            if repeated.read_bytes() != output.read_bytes():
                raise RuntimeError(f"{name}: nondeterministic LLVM text")
            modules.append((name, output, expected, stdin_data))

        # P5.3：把原先依赖写入的 P5 fixture 接入自有 C++ IR 执行链。
        p05 = Path(__file__).resolve().parent / "ir" / "p05"
        assignment_cases = [
            (f"p53-{stem}", (p05 / f"{stem}.rx").read_text(encoding="utf-8"),
             (p05 / f"{stem}.out").read_bytes(), b"")
            for stem in ("references", "borrow_once", "coercions",
                         "assignment_order", "assignment_return")
        ]
        # 期望结果独立列出；避开除零及 INT_MIN / -1 等未定义行为。
        # 每种整数同时验证直接 RHS 和一层共享引用 RHS，覆盖全部十种 op=。
        for kind in ("i32", "u32", "isize", "usize"):
            signed = kind in ("i32", "isize")
            rows = (
                ("2147483647" if signed else "4294967295", "+=", "1", -2147483648 if signed else 0),
                ("-2147483648" if signed else "0", "-=", "1", 2147483647 if signed else -1),
                ("1073741824", "*=", "4", 0),
                ("-7" if signed else "4294967295", "/=", "3" if signed else "2", -2 if signed else 2147483647),
                ("-7" if signed else "4294967295", "%=", "3" if signed else "2", -1 if signed else 1),
                ("15", "&=", "6", 6),
                ("8", "|=", "3", 11),
                ("15", "^=", "6", 9),
                ("1", "<<=", "33", 2),
                ("-8" if signed else "4294967288", ">>=", "33", -4 if signed else 2147483644),
            )
            for reference in (False, True):
                statements = []
                expected = []
                for left, operation, right, result in rows:
                    operand = "&right" if reference else "right"
                    statements.append(
                        f"{{ let mut value = {left}{kind}; let right = {right}{kind}; "
                        f"value {operation} {operand}; println_i32(value as i32); }};")
                    expected.append(f"{result}\n")
                assignment_cases.append((
                    f"p53-{kind}-{'reference' if reference else 'direct'}",
                    "fn main() { " + " ".join(statements) + " }",
                    "".join(expected).encode("ascii"), b""))

        # bool 使用 i1 运算和 i8 存取；直接和引用 RHS 都覆盖完整真值表。
        statements = []
        expected = []
        for operation in ("&=", "|=", "^="):
            results = {"&=": (0, 0, 0, 1), "|=": (0, 1, 1, 1), "^=": (0, 1, 1, 0)}[operation]
            for reference in (False, True):
                for (left, right), result in zip(
                        (("false", "false"), ("false", "true"),
                         ("true", "false"), ("true", "true")), results):
                    operand = "&right" if reference else "right"
                    statements.append(
                        f"{{ let mut value = {left}; let right = {right}; "
                        f"value {operation} {operand}; println_i32(value as i32); }};")
                    expected.append(f"{result}\n")
        assignment_cases.append(("p53-bool", "fn main() { " + " ".join(statements) + " }",
                                 "".join(expected).encode("ascii"), b""))
        # 每一种复合运算都要求 RHS 优先，而不只是 +=；标记函数只操作独立数据。
        statements = []
        expected = []
        for reference in (False, True):
            for operation, result in (("+=", 13), ("-=", 7), ("*=", 30), ("/=", 3),
                                      ("%=", 1), ("&=", 2), ("|=", 11), ("^=", 9),
                                      ("<<=", 80), (">>=", 1)):
                operand = "reference(&y)" if reference else "scalar()"
                statements.append(f"*target(&mut x) {operation} {operand}; println_i32(x);")
                expected.append(f"2\n1\n{result}\n")
        assignment_cases.append((
            "p53-all-operator-order",
            "fn scalar() -> i32 { println_i32(2); 3 } "
            "fn reference(p: &i32) -> &i32 { println_i32(2); p } "
            "fn target(p: &mut i32) -> &mut i32 { println_i32(1); *p = 10; p } "
            "fn main() { let mut x = 0; let y = 3; " + " ".join(statements) + " }",
            "".join(expected).encode("ascii"), b""))
        assignment_cases.extend((
            ("p53-local-assignment", "fn main() { let mut x = 1; x = 2; println_i32(x); }", b"2\n", b""),
            ("p53-bool-assignment",
             "fn set(p: &mut bool) { *p = true } "
             "fn main() { let mut x = false; set(&mut x); println_i32(x as i32); "
             "x = false; println_i32(x as i32); }", b"1\n0\n", b""),
            ("p53-unit-contexts",
             "fn set(p: &mut i32) { *p = 4 } fn add(p: &mut i32) { { *p += 2 } } "
             "fn leave(p: &mut i32) { return *p = 9; } "
             "fn main() { let mut x = 0; set(&mut x); add(&mut x); println_i32(x); "
             "leave(&mut x); println_i32(x); }", b"6\n9\n", b""),
            ("p53-reassign-reference",
             "fn main() { let x = 1; let y = 2; let ry = &y; let mut r: &i32 = &x; "
             "r = &ry; println_i32(*r); let mut p = &mut 3; p = &mut 4; println_i32(*p); }",
             b"2\n4\n", b""),
            ("p53-conversion-order",
             "fn source(p: &&i32) -> &&i32 { println_i32(2); p } "
             "fn slot(p: &mut &i32) -> &mut &i32 { println_i32(1); p } "
             "fn main() { let x = 1; let y = 7; let ry = &y; let mut r = &x; "
             "*slot(&mut r) = source(&ry); println_i32(*r); }", b"2\n1\n7\n", b""),
            ("p53-shared-rhs-order",
             "fn source(p: &i32) -> &i32 { println_i32(2); p } "
             "fn target(p: &mut i32) -> &mut i32 { println_i32(1); *p = 10; p } "
             "fn main() { let mut x = 0; let y = 7; "
             "*target(&mut x) += source(&y); println_i32(x); }", b"2\n1\n17\n", b""),
            ("p53-fixed-scalar-rhs",
             "fn target(p: &mut i32) -> &mut i32 { *p = 10; p } "
             "fn main() { let mut x = 3; *target(&mut x) = x; println_i32(x); "
             "*target(&mut x) += x; println_i32(x); }", b"3\n13\n", b""),
            ("p53-compound-return",
             "fn target(p: &mut i32) -> &mut i32 { println_i32(99); p } "
             "fn right() -> i32 { println_i32(2); 3 } "
             "fn leave_rhs(p: &mut i32) -> i32 { *target(p) += { return 7; 1 }; 0 } "
             "fn leave_target(p: &mut i32) -> i32 { *{ return 8; p } += right(); 0 } "
             "fn main() { let mut x = 10; println_i32(leave_rhs(&mut x)); "
             "println_i32(leave_target(&mut x)); println_i32(x); }", b"7\n2\n8\n10\n", b""),
            ("p53-plain-target-return",
             "fn right() -> i32 { println_i32(2); 3 } "
             "fn leave(p: &mut i32) -> i32 { *{ return 8; p } = right(); 0 } "
             "fn main() { let mut x = 10; println_i32(leave(&mut x)); println_i32(x); }",
             b"2\n8\n10\n", b""),
            ("p53-shared-target-return",
             "fn source(p: &i32) -> &i32 { println_i32(2); p } "
             "fn leave(p: &mut i32, y: &i32) -> i32 { "
             "*{ return 8; p } += source(y); 0 } "
             "fn main() { let mut x = 10; let y = 7; "
             "println_i32(leave(&mut x, &y)); println_i32(x); }", b"2\n8\n10\n", b""),
            ("p53-dynamic-shift",
             "fn main() { let count = get_i32(); let mut x = 1u32; "
             "x <<= &count; println_i32(x as i32); "
             "let mut y = -8isize; y >>= &count; println_i32(y as i32); }",
             b"-2147483648\n-1\n", b"-1\n"),
            ("p53-shift-32",
             "fn main() { let mut x = 7u32; x <<= 32i32; println_i32(x as i32); "
             "x >>= 64usize; println_i32(x as i32); }", b"7\n7\n", b""),
            ("p53-nested-target",
             "fn main() { let mut x = 1; let mut p = &mut x; let pp = &mut p; "
             "**pp = 4; **pp += 2; println_i32(**pp); }", b"6\n", b""),
        ))
        for name, source_text, expected, stdin_data in assignment_cases:
            source = temp / f"{name}.rx"
            output = temp / f"{name}.ll"
            source.write_text(source_text, encoding="utf-8")
            run([args.compiler, "--stage", "semantic", str(source)])
            run([args.compiler, "--stage", "ir", str(source), "-o", str(output)])
            repeated = temp / f"{name}-repeated.ll"
            run([args.compiler, "--stage", "ir", str(source), "-o", str(repeated)])
            if repeated.read_bytes() != output.read_bytes():
                raise RuntimeError(f"{name}: nondeterministic LLVM text")
            modules.append((name, output, expected, stdin_data))
        # P5.4：共享引用先求值，最终运算前才读取目标；排序按引用层级读取。
        p54_cases = [
            ("p54-reference-operands",
             (p05 / "reference_operands.rx").read_text(encoding="utf-8"),
             (p05 / "reference_operands.out").read_bytes(), b""),
            ("p54-reference-ordering",
             "fn main() { let a = 3; let b = 7; let ra = &a; let rb = &b; "
             "println_i32((ra < rb) as i32); let rra = &ra; let rrb = &rb; "
             "println_i32((rra < rrb) as i32); let mut am = 3; let mut bm = 7; "
             "let ram = &mut am; let rbm = &mut bm; "
             "println_i32((ram < rbm) as i32); "
             "println_i32((ra < rbm) as i32); let f = false; let t = true; "
             "let rf = &f; let rt = &t; println_i32((rf < rt) as i32); }",
             b"1\n1\n1\n1\n1\n", b""),
            ("p54-reference-operand-order",
             "fn left(p: &i32) -> &i32 { println_i32(1); p } "
             "fn right(p: &i32) -> &i32 { println_i32(2); p } "
             "fn main() { let a = 3; let b = 4; "
             "println_i32(left(&a) + right(&b)); }",
             b"1\n2\n7\n", b""),
            ("p54-reference-ops",
             "fn main() { let a = 13; let b = 3; "
             "println_i32(a - &b); println_i32(&a * b); "
             "println_i32(&a & &b); println_i32(&a | &b); "
             "println_i32(a ^ &b); println_i32(&a << &b); "
             "println_i32(&a >> &b); println_i32(!&a); }",
             b"10\n39\n1\n15\n14\n104\n1\n-14\n", b""),
        ]
        # 三种引用组合都覆盖整数运算边界；期望值独立给出，不以另一个编译器作答案。
        for kind in ("i32", "u32", "isize", "usize"):
            signed = kind in ("i32", "isize")
            rows = (
                ("2147483647" if signed else "4294967295", "+", "1", -2147483648 if signed else 0),
                ("-2147483648" if signed else "0", "-", "1", 2147483647 if signed else -1),
                ("1073741824", "*", "4", 0),
                ("-7" if signed else "4294967295", "/", "3" if signed else "2", -2 if signed else 2147483647),
                ("-7" if signed else "4294967295", "%", "3" if signed else "2", -1 if signed else 1),
                ("15", "&", "6", 6), ("8", "|", "3", 11), ("15", "^", "6", 9),
                ("1", "<<", "33", 2),
                ("-8" if signed else "4294967288", ">>", "33", -4 if signed else 2147483644),
            )
            for left_ref, right_ref in ((True, False), (False, True), (True, True)):
                statements = []
                expected = []
                for left, operation, right, result in rows:
                    left_operand = "&left" if left_ref else "left"
                    right_operand = "&right" if right_ref else "right"
                    statements.append(
                        f"{{ let left = {left}{kind}; let right = {right}{kind}; "
                        f"println_i32(({left_operand} {operation} {right_operand}) as i32); "
                        "println_i32(left as i32); println_i32(right as i32); };")
                    # 另外打印原操作数，确认运算没有写入被引用位置。
                    left_bits = int(left) if int(left) < 2147483648 else int(left) - 4294967296
                    expected.extend((f"{result}\n", f"{left_bits}\n", f"{right}\n"))
                p54_cases.append((
                    f"p54-{kind}-binary-{int(left_ref)}{int(right_ref)}",
                    "fn main() { " + " ".join(statements) + " }",
                    "".join(expected).encode("ascii"), b""))

            # 一元取反覆盖全零/全一位模式；取负只支持有符号类型，并按32位回绕。
            statements = [f"let zero = 0{kind}; println_i32((!&zero) as i32);",
                          f"let all = {'-1' if signed else '4294967295'}{kind}; "
                          "println_i32((!&all) as i32);"]
            expected = b"-1\n0\n"
            if signed:
                statements.append(f"let minimum = -2147483648{kind}; "
                                  "println_i32((-&minimum) as i32); "
                                  f"let n = -7{kind}; println_i32((-&n) as i32);")
                expected += b"-2147483648\n7\n"
            p54_cases.append((f"p54-{kind}-unary", "fn main() { " + " ".join(statements) + " }",
                              expected, b""))

            # 移位允许左右整数类型不同，引用形式也必须掩码低五位。
            statements = []
            expected = []
            for count_kind in ("i32", "u32", "isize", "usize"):
                all_bits = "-1" if count_kind in ("i32", "isize") else "4294967295"
                for count, shift_left, shift_right in (
                        (all_bits, -2147483648, -1 if signed else 1),
                        ("32", 1, -8),
                        ("33", 2, -4 if signed else 2147483644)):
                    for left_ref, right_ref in ((True, False), (False, True), (True, True)):
                        left_operand = "&left" if left_ref else "left"
                        right_operand = "&count" if right_ref else "count"
                        statements.append(
                            f"{{ let left = 1{kind}; let count = {count}{count_kind}; "
                            f"println_i32(({left_operand} << {right_operand}) as i32); }};")
                        statements.append(
                            f"{{ let left = {'-8' if signed else '4294967288'}{kind}; "
                            f"let count = {count}{count_kind}; "
                            f"println_i32(({left_operand} >> {right_operand}) as i32); }};")
                        expected.extend((f"{shift_left}\n", f"{shift_right}\n"))
            p54_cases.append((f"p54-{kind}-mixed-shifts",
                              "fn main() { " + " ".join(statements) + " }",
                              "".join(expected).encode("ascii"), b""))

        # bool 位运算覆盖所有真值和三种引用组合；非短路，不改变目标。
        statements = []
        expected = []
        for operation, results in (("&", (0, 0, 0, 1)), ("|", (0, 1, 1, 1)),
                                   ("^", (0, 1, 1, 0))):
            for left_ref, right_ref in ((True, False), (False, True), (True, True)):
                for (left, right), result in zip(
                        (("false", "false"), ("false", "true"),
                         ("true", "false"), ("true", "true")), results):
                    left_operand = "&left" if left_ref else "left"
                    right_operand = "&right" if right_ref else "right"
                    statements.append(f"{{ let left = {left}; let right = {right}; "
                                      f"println_i32(({left_operand} {operation} {right_operand}) as i32); }};")
                    expected.append(f"{result}\n")
        statements.append("println_i32((!&false) as i32); println_i32((!&true) as i32);")
        p54_cases.append(("p54-bool-truth-table", "fn main() { " + " ".join(statements) + " }",
                          "".join(expected).encode("ascii") + b"1\n0\n", b""))

        # 排序比较目标内容而非地址：五种标量、三层深度、四种谓词及相等值。
        for kind in ("i32", "u32", "isize", "usize", "bool"):
            signed = kind in ("i32", "isize")
            low = "false" if kind == "bool" else f"-7{kind}" if signed else f"2{kind}"
            high = "true" if kind == "bool" else f"3{kind}" if signed else f"4294967295{kind}"
            statements = []
            expected = []
            for depth in (1, 2, 3):
                for left, right, results in ((low, high, (1, 1, 0, 0)),
                                             (low, low, (0, 1, 0, 1)),
                                             (high, low, (0, 0, 1, 1))):
                    body = [f"let left0 = {left}; let right0 = {right};"]
                    for level in range(1, depth + 1):
                        body.append(f"let left{level} = &left{level - 1}; "
                                    f"let right{level} = &right{level - 1};")
                    for operation, result in zip(("<", "<=", ">", ">="), results):
                        body.append(f"println_i32((left{depth} {operation} right{depth}) as i32);")
                        expected.append(f"{result}\n")
                    statements.append("{ " + " ".join(body) + " };")
            p54_cases.append((f"p54-{kind}-ordering", "fn main() { " + " ".join(statements) + " }",
                              "".join(expected).encode("ascii"), b""))

        p54_cases.extend((
            ("p54-mutable-ordering",
             "fn main() { let a = 3; let mut b = 7; let ra = &a; let rb = &mut b; "
             "println_i32((ra < rb) as i32); println_i32((ra <= rb) as i32); "
             "println_i32((ra > rb) as i32); println_i32((ra >= rb) as i32); "
             "let mut x = 3; let mut y = 7; let rx = &mut x; let ry = &mut y; "
             "println_i32((&rx < &ry) as i32); println_i32((&rx >= &ry) as i32); }",
             b"1\n1\n0\n0\n1\n0\n", b""),
            ("p54-fixed-direct",
             "fn change(p: &mut i32) -> i32 { *p = 10; 7 } "
             "fn main() { let mut x = 3; println_i32(x + change(&mut x)); println_i32(x); }",
             b"10\n10\n", b""),
            ("p54-nonshortcircuit-bool",
             "fn left(p: &bool) -> &bool { println_i32(1); p } "
             "fn right(p: &bool) -> &bool { println_i32(2); p } "
             "fn main() { let a = false; let b = true; "
             "println_i32((left(&a) & right(&b)) as i32); "
             "println_i32((right(&b) | left(&a)) as i32); }", b"1\n2\n0\n2\n1\n1\n", b""),
            ("p54-return-left",
             "fn right(p: &i32) -> &i32 { println_i32(99); p } "
             "fn leave(p: &i32) -> i32 { ({ return 7; p }) + right(p) } "
             "fn main() { let x = 3; println_i32(leave(&x)); }", b"7\n", b""),
            ("p54-return-right",
             "fn left(p: &i32) -> &i32 { println_i32(1); p } "
             "fn leave(p: &i32) -> i32 { left(p) + { return 8; p } } "
             "fn main() { let x = 3; println_i32(leave(&x)); }", b"1\n8\n", b""),
            ("p54-return-unary",
             "fn leave(p: &i32) -> i32 { -{ println_i32(1); return 9; p } } "
             "fn main() { let x = 3; println_i32(leave(&x)); }", b"1\n9\n", b""),
            ("p54-return-ordering",
             "fn left(p: &&i32) -> &&i32 { println_i32(1); p } "
             "fn leave(p: &&i32) -> bool { left(p) < { return true; p } } "
             "fn main() { let x = 3; let r = &x; println_i32(leave(&r) as i32); }",
             b"1\n1\n", b""),
            ("p54-runtime-input",
             "fn main() { let x = get_i32(); let count = get_i32(); "
             "println_i32(&x / &3); println_i32(&x % &3); "
             "println_i32(&x >> &count); println_i32((&x < &3) as i32); }",
             b"-2\n-1\n-4\n1\n", b"-7\n33\n"),
        ))
        for name, source_text, expected, stdin_data in p54_cases:
            source = temp / f"{name}.rx"
            output = temp / f"{name}.ll"
            source.write_text(source_text, encoding="utf-8")
            run([args.compiler, "--stage", "semantic", str(source)])
            run([args.compiler, "--stage", "ir", str(source), "-o", str(output)])
            repeated = temp / f"{name}-repeated.ll"
            run([args.compiler, "--stage", "ir", str(source), "-o", str(repeated)])
            if repeated.read_bytes() != output.read_bytes():
                raise RuntimeError(f"{name}: nondeterministic LLVM text")
            modules.append((name, output, expected, stdin_data))

        p06 = Path(__file__).resolve().parent / "ir" / "p06"
        unit_if = p06 / "unit_if.rx"
        if_values = p06 / "if_values.rx"
        if_return = p06 / "if_return.rx"
        short_circuit = p06 / "short_circuit.rx"
        while_condition = p06 / "while_condition.rx"
        loop_control = p06 / "loop_control.rx"
        break_value = p06 / "break_value.rx"
        reference_lub = p06 / "reference_lub.rx"
        source_main_recursion = p06 / "source_main_recursion.rx"
        for name, source_text, expected, stdin_data in (
            ("p61-unit-if", unit_if.read_text(encoding="utf-8"),
             unit_if.with_suffix(".out").read_bytes(), b""),
            # 三次走不同路径；标记记录条件求值次数、未选中的条件和两层合流顺序。
            ("p61-nested-if", """
                fn marked(marker: i32) -> bool {
                    println_i32(marker);
                    get_i32() != 0
                }
                fn choose() {
                    if marked(10) {
                        println_i32(11);
                        if marked(12) { println_i32(13); } else { println_i32(14); }
                        println_i32(15);
                    } else if marked(20) {
                        println_i32(21);
                    } else {
                        println_i32(22);
                    }
                    println_i32(30);
                }
                fn main() { choose(); choose(); choose(); }
             """, b"10\n11\n12\n14\n15\n30\n10\n20\n21\n30\n10\n20\n22\n30\n",
             b"1\n0\n0\n1\n0\n0\n"),
            ("p62-if-values", if_values.read_text(encoding="utf-8"),
             if_values.with_suffix(".out").read_bytes(), b""),
            ("discarded-if-value", """
                fn run() {
                    if get_i32() == 0 {
                        if get_i32() == 1 { println_i32(10); 1 }
                        else { println_i32(20); 2 }
                    } else { println_i32(30); return; 3 };
                    println_i32(40);
                }
                fn main() { run(); run(); run(); }
             """, b"10\n40\n20\n40\n30\n", b"0 1 0 2 3\n"),
            # 外层 get 的值跨外层合流；then 中读取的 base 跨内层合流。
            ("p62-nested-values", """
                fn choose(flag: bool, inner: bool) -> i32 {
                    let base = get_i32();
                    get_i32() - if flag {
                        println_i32(1);
                        base + if inner { println_i32(2); 10 } else { println_i32(3); 20 }
                    } else {
                        println_i32(4);
                        base + 30
                    }
                }
                fn main() {
                    println_i32(choose(true, true));
                    println_i32(choose(true, false));
                    println_i32(choose(false, true));
                }
             """, b"1\n2\n-9\n1\n3\n-19\n4\n-29\n", b"3\n4\n5\n6\n7\n8\n"),
            ("p62-unsigned-values",
             "fn pick(flag: bool) -> u32 { if flag { 4294967295u32 } else { 1u32 } } "
             "fn main() { println_i32((pick(true) > 2u32) as i32); "
             "println_i32((pick(false) > 2u32) as i32); }",
             b"1\n0\n", b""),
            ("p63-if-return", if_return.read_text(encoding="utf-8"),
             if_return.with_suffix(".out").read_bytes(), b""),
            ("p63-one-normal-edge", """
                fn no_else(flag: bool) -> i32 {
                    if flag { println_i32(1); return 7; }
                    println_i32(2);
                    8
                }
                fn initializer(flag: bool) -> i32 {
                    let n = if flag { println_i32(3); 8 }
                            else { println_i32(4); return 7; };
                    println_i32(5);
                    n + 1
                }
                fn main() {
                    println_i32(no_else(true)); println_i32(no_else(false));
                    println_i32(initializer(true)); println_i32(initializer(false));
                }
             """, b"1\n7\n2\n8\n3\n5\n9\n4\n7\n", b""),
            ("p63-return-operand", """
                fn partial(flag: bool) -> i32 {
                    return if flag { println_i32(1); return 7; }
                           else { println_i32(2); 8 };
                }
                fn both(flag: bool) -> i32 {
                    return if flag { println_i32(3); return 11; }
                           else { println_i32(4); return 12; };
                    println_i32(99);
                    13
                }
                fn main() {
                    println_i32(partial(true)); println_i32(partial(false));
                    println_i32(both(true)); println_i32(both(false));
                }
             """, b"1\n7\n2\n8\n3\n11\n4\n12\n", b""),
            ("p63-return-in-condition", """
                fn always() -> i32 {
                    if { println_i32(1); return 9; true } {
                        println_i32(99); 10
                    } else { println_i32(99); 11 }
                }
                fn maybe(flag: bool) -> i32 {
                    if (if flag { println_i32(2); false }
                        else { println_i32(3); return 7; }) {
                        println_i32(99); 10
                    } else { println_i32(4); 8 }
                }
                fn main() {
                    println_i32(always());
                    println_i32(maybe(true)); println_i32(maybe(false));
                }
             """, b"1\n9\n2\n4\n8\n3\n7\n", b""),
            ("p64-short-circuit", short_circuit.read_text(encoding="utf-8"),
             short_circuit.with_suffix(".out").read_bytes(), b""),
            # 由输入决定真假，标记同时检查调用次数、顺序和嵌套合流。
            ("p64-dynamic-order", """
                fn mark(tag: i32) -> bool { println_i32(tag); get_i32() != 0 }
                fn main() {
                    println_i32((mark(10) && mark(11)) as i32);
                    println_i32((mark(20) || mark(21)) as i32);
                    println_i32((mark(30) && mark(31)) as i32);
                    println_i32((mark(40) || mark(41)) as i32);
                    println_i32(((mark(50) && mark(51)) || (mark(52) && mark(53))) as i32);
                    println_i32((mark(60) && (mark(61) || mark(62))) as i32);
                }
             """, b"10\n0\n20\n1\n30\n31\n0\n40\n41\n0\n50\n51\n52\n53\n1\n60\n61\n1\n",
             b"0\n1\n1\n0\n0\n0\n1\n0\n1\n1\n1\n1\n"),
            ("p64-early-return", """
                fn with_call(flag: bool) {
                    println_i32((flag && { println_i32(10); return; true }) as i32);
                    println_i32(11);
                }
                fn with_assignment(flag: bool, out: &mut bool) {
                    *out = flag || { println_i32(20); return; false };
                    println_i32(21);
                }
                fn with_nested(flag: bool) -> bool {
                    (flag && { println_i32(30); return false; true }) ||
                        { println_i32(31); true }
                }
                fn left_exit() {
                    let a = ({ println_i32(40); return; true }) && { println_i32(99); false };
                    println_i32(99);
                }
                fn main() {
                    with_call(false); with_call(true);
                    let mut first = false;
                    with_assignment(true, &mut first);
                    println_i32(first as i32);
                    let mut second = false;
                    with_assignment(false, &mut second);
                    println_i32(second as i32);
                    println_i32(with_nested(false) as i32);
                    println_i32(with_nested(true) as i32);
                    left_exit();
                }
             """, b"0\n11\n10\n21\n1\n20\n0\n31\n1\n30\n0\n40\n", b""),
            ("p65-while-condition", while_condition.read_text(encoding="utf-8"),
             while_condition.with_suffix(".out").read_bytes(), b""),
            # 回边须回到短路条件的起点；false 路径不能执行右侧或循环体。
            ("p65-while-short-circuit", """
                fn check(x: i32) -> bool { println_i32(10 + x); x < 2 }
                fn step(x: i32) -> bool { println_i32(20 + x); true }
                fn count(start: i32) {
                    let mut x = start;
                    while check(x) && step(x) {
                        let next = x + 1;
                        if x == 0 { println_i32(30); } else { println_i32(31); }
                        x = next;
                    }
                    println_i32(100 + x);
                }
                fn main() { count(0); count(2); }
             """, b"10\n20\n30\n11\n21\n31\n12\n102\n12\n102\n", b""),
            ("p65-while-early-return", """
                fn from_body(early: bool) -> i32 {
                    let mut i = 0;
                    while i < 2 {
                        println_i32(i);
                        if early && i == 1 { return 7; }
                        i += 1;
                    }
                    println_i32(8);
                    9
                }
                fn from_condition() -> i32 {
                    while ({ println_i32(3); return 10; true }) { println_i32(99); }
                    11
                }
                fn main() {
                    println_i32(from_body(true)); println_i32(from_body(false));
                    println_i32(from_condition());
                }
             """, b"0\n1\n7\n0\n1\n8\n9\n3\n10\n", b""),
            ("p6-diverging-unit-contexts", """
                fn late() -> i32 { println_i32(99); 8 }
                fn before_exit() -> i32 { println_i32(13); 1 }
                fn as_statement() -> i32 {
                    { return 5; late() }
                    println_i32(99);
                    6
                }
                fn from_while(flag: bool) -> i32 {
                    while flag { return 7; late() }
                    9
                }
                fn from_loop() -> i32 {
                    let mut count = 0;
                    loop {
                        if count == 1 { break 11; }
                        count += 1;
                        continue;
                        late()
                    }
                }
                fn in_tail(flag: bool) -> i32 {
                    while flag { before_exit() + { return 15; late() } }
                    17
                }
                fn main() {
                    println_i32(as_statement());
                    println_i32(from_while(true)); println_i32(from_while(false));
                    println_i32(from_loop());
                    println_i32(in_tail(true)); println_i32(in_tail(false));
                }
             """, b"5\n7\n9\n11\n13\n15\n17\n", b""),
            ("p66-loop-control", loop_control.read_text(encoding="utf-8"),
             loop_control.with_suffix(".out").read_bytes(), b""),
            ("p66-nested-targets", """
                fn check(i: i32) -> bool { println_i32(10 + i); i < 3 }
                fn main() {
                    let mut i = 0;
                    while ({
                        loop { println_i32(20 + i); break; }
                        check(i)
                    }) {
                        i += 1;
                        let mut j = 0;
                        loop {
                            j += 1;
                            if j == 1 { continue; }
                            println_i32(30 + i);
                            break;
                        }
                        if i == 1 { continue; }
                        if i == 2 { break; }
                        println_i32(99);
                    }
                    println_i32(i);
                }
             """, b"20\n10\n31\n21\n11\n32\n2\n", b""),
            # LLVM 验证整个模块，但 main 不调用这个无出口的函数。
            ("p66-no-loop-exit",
             "fn forever() -> i32 { let x = loop {}; println_i32(99); 8 } fn main() {}",
             b"", b""),
            ("p67-break-value", break_value.read_text(encoding="utf-8"),
             break_value.with_suffix(".out").read_bytes(), b""),
            ("discarded-loop-and-logic", """
                fn run() {
                    loop { break { println_i32(1); 7 }; };
                    (get_i32() != 0) && { println_i32(2); true };
                    (get_i32() != 0) || { println_i32(3); false };
                }
                fn main() { run(); run(); }
             """, b"1\n2\n1\n3\n", b"1 1 0 0\n"),
            ("p67-reference-lub", reference_lub.read_text(encoding="utf-8"),
             reference_lub.with_suffix(".out").read_bytes(), b""),
            ("p67-nested-break-value", """
                fn choose(flag: bool) -> i32 {
                    let result = loop {
                        break (loop {
                            if flag { break { println_i32(1); return 9; }; }
                            println_i32(2);
                            break get_i32();
                        }) + 1;
                    };
                    result + 10
                }
                fn main() {
                    println_i32(choose(true)); println_i32(choose(false));
                    println_i32(get_i32());
                }
             """, b"1\n9\n2\n18\n20\n", b"7\n20\n"),
            ("p67-result-edge-adjustments", """
                fn choose(flag: bool) {
                    let a = 10;
                    let b = 20;
                    let p = &a;
                    let from_if = if flag { &p } else { &b };
                    let from_loop = loop {
                        if flag { break &b; }
                        break &p;
                    };
                    println_i32(*from_if); println_i32(*from_loop);
                    let fixed: &i32 = loop { break &p; };
                    println_i32(*fixed);
                }
                fn main() { choose(true); choose(false); }
             """, b"10\n20\n10\n20\n10\n10\n", b""),
            ("p67-unit-and-bool-break", """
                fn main() {
                    loop { break { println_i32(1); }; }
                    while loop { println_i32(2); break false; } { println_i32(99); }
                    let yes = loop { break true; };
                    let max: u32 = loop { break 4294967295; };
                    println_i32(yes as i32);
                    println_i32((max > 1u32) as i32);
                }
             """, b"1\n2\n1\n1\n", b""),
            ("p68-sum-to", """
                fn sum_to(limit: i32) -> i32 {
                    let mut i = 0;
                    let mut sum = 0;
                    while i < limit {
                        i += 1;
                        if i == 2 { continue; }
                        sum += i;
                    }
                    loop {
                        if sum > 0 { break sum; }
                        return 0;
                    }
                }
                fn main() { println_i32(sum_to(4)); println_i32(sum_to(0)); }
             """, b"8\n0\n", b""),
            ("p68-combined-side-effects", """
                fn mark_positive(sum: &i32) -> bool { println_i32(100 + *sum); true }
                fn sum_to(limit: i32) -> i32 {
                    let mut i = 0;
                    let mut sum = 0;
                    while i < limit {
                        i += 1;
                        if i == 2 { continue; println_i32(99); }
                        sum += i;
                    }
                    loop {
                        if sum > 0 && mark_positive(&sum) { break sum; println_i32(99); }
                        return 0;
                        println_i32(99);
                    }
                }
                fn main() { println_i32(sum_to(4)); println_i32(sum_to(0)); }
             """, b"108\n8\n0\n", b""),
            ("p68-recursive-call-order", """
                fn mark(value: i32) -> i32 { println_i32(value); value }
                fn score(n: i32, base: i32) -> i32 {
                    let saved = base + n * 10;
                    if n == 0 { return saved; }
                    let inner = score(n - 1, base + 1);
                    saved + inner
                }
                fn report(n: i32) {
                    println_i32(score(mark(n), if get_i32() != 0 { mark(10) } else { mark(20) }));
                }
                fn main() { report(2); report(1); }
             """, b"2\n10\n63\n1\n20\n51\n", b"1 0\n"),
            ("p68-source-main-recursion", source_main_recursion.read_text(encoding="utf-8"),
             source_main_recursion.with_suffix(".out").read_bytes(),
             source_main_recursion.with_suffix(".in").read_bytes()),
        ):
            source = temp / f"{name}.rx"
            output = temp / f"{name}.ll"
            source.write_text(source_text, encoding="utf-8")
            run([args.compiler, "--stage", "ir", str(source), "-o", str(output)])
            modules.append((name, output, expected, stdin_data))

        p07 = Path(__file__).resolve().parent / "ir" / "p07"
        for name, contents, expected in (
            ("p72-construction-order", (p07 / "construction_order.rx").read_text(encoding="utf-8"),
             (p07 / "construction_order.out").read_bytes()),
            ("p72-repeat-coercion", (p07 / "repeat_coercion.rx").read_text(encoding="utf-8"),
             (p07 / "repeat_coercion.out").read_bytes()),
            ("p72-diverging-array-element",
             "fn main() { [{ println_i32(7); return; }]; println_i32(99); }", b"7\n"),
            ("p72-diverging-array-branches", """
                fn choose(flag: bool) {
                    let unused = if flag { [{ println_i32(7); return; }] }
                                 else { [{ println_i32(8); return; }; 1] };
                    println_i32(99);
                }
                fn main() { choose(true); choose(false); }
             """, b"7\n8\n"),
            ("p72-places", """
                struct Cell { flag: bool, value: i32, tail: bool }
                struct Board { cells: [Cell; 2] }
                fn mark(x: i32) -> i32 { println_i32(x); x }
                fn index() -> usize { println_i32(20); 1 }
                fn main() {
                    let mut board = Board { cells: [
                        Cell { value: mark(1), flag: true, tail: false },
                        Cell { value: mark(2), flag: false, tail: true },
                    ] };
                    ({ println_i32(10); &mut board }).cells[index()].value = mark(30);
                    let p = &mut board.cells[1].value;
                    *p += 1;
                    let rows = &board.cells;
                    let nested = &rows;
                    println_i32(nested[1].value);
                    println_i32(nested[0].value);
                    println_i32(nested[0].flag as i32);
                    println_i32(nested[1].flag as i32);
                    println_i32(nested[1].tail as i32);

                    let temporary = &mut [mark(40), mark(41)][index()];
                    *temporary += 2;
                    println_i32(*temporary);
                    let from_block = { [[4, 5], [6, 7]] };
                    println_i32(from_block[1][0]);
                }
                fn unused() { println_i32(99); let x = [1, 2]; }
             """, b"1\n2\n30\n10\n20\n31\n1\n1\n0\n1\n40\n41\n20\n43\n6\n"),
            ("p72-array-lub", """
                struct Refs<'a> { left: &'a i32, right: &'a i32 }
                fn main() {
                    let mut a = 11;
                    let mut b = 22;
                    let first = &a;
                    let second = &b;
                    let fixed = Refs { right: &second, left: &first };
                    println_i32(*fixed.left); println_i32(*fixed.right);
                    let inferred = [&first, &b];
                    let reversed = [&a, &second];
                    println_i32(*inferred[0]); println_i32(*inferred[1]);
                    println_i32(*reversed[0]); println_i32(*reversed[1]);
                    let annotated: [&i32; 2] = [&a, &second];
                    println_i32(*annotated[0]); println_i32(*annotated[1]);
                    let shared_first = [&a, &mut b];
                    println_i32(*shared_first[0]); println_i32(*shared_first[1]);
                    let mutable_first = [&mut a, &b];
                    println_i32(*mutable_first[0]); println_i32(*mutable_first[1]);
                }
             """, b"11\n22\n" * 6),
            ("p72-repeat-large", """
                const LENGTH: usize = 513;
                #[derive(Clone, Copy)]
                struct Item { flag: bool, word: i32 }
                struct Token { value: i32 }
                fn mark(x: i32) -> i32 { println_i32(x); x }
                fn main() {
                    let mut items = [Item { word: mark(17), flag: true }; LENGTH];
                    items[256].word = 23;
                    println_i32(items[0].word);
                    println_i32(items[256].word);
                    println_i32(items[512].word);
                    println_i32(items[512].flag as i32);
                    let token = Token { value: mark(29) };
                    let singleton = [token; 1];
                    println_i32(singleton[0].value);
                    let x = 31;
                    let reference = &x;
                    let pointers: [&i32; LENGTH] = [&reference; LENGTH];
                    println_i32(*pointers[0]); println_i32(*pointers[512]);
                }
             """, b"17\n17\n23\n17\n1\n29\n29\n31\n31\n"),
            ("p72-early-exit", """
                struct Pair { first: i32, second: i32 }
                fn mark(x: i32) -> i32 { println_i32(x); x }
                fn from_field() -> i32 {
                    let pair = Pair { second: mark(1), first: { return 2; mark(99) } };
                    pair.first
                }
                fn from_list() -> i32 {
                    let a = [mark(3), { return 4; mark(99) }, mark(99)];
                    a[0]
                }
                fn from_repeat() -> i32 {
                    let a: [i32; 513] = [{ println_i32(5); return 6; mark(99) }; 513];
                    a[0]
                }
                fn from_base() -> i32 {
                    ({ println_i32(7); return 8; [0, 1] })[{ println_i32(99); 0usize }]
                }
                fn from_index() -> i32 {
                    [mark(9), 0][{ println_i32(10); return 11; 0usize }]
                }
                fn main() {
                    println_i32(from_field()); println_i32(from_list());
                    println_i32(from_repeat()); println_i32(from_base()); println_i32(from_index());
                }
             """, b"1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n"),
        ):
            source = temp / f"{name}.rx"
            output = temp / f"{name}.ll"
            source.write_text(contents, encoding="utf-8")
            run([args.compiler, "--stage", "ir", str(source), "-o", str(output)])
            text = output.read_text(encoding="utf-8")
            if name == "p72-repeat-large":
                # 可用循环或少量展开，但不能为几百个元素各发一份寻址指令。
                for storage in ("[513 x %__rx_struct_0]", "[513 x ptr]"):
                    array_geps = re.findall(rf"getelementptr {re.escape(storage)},[^\n]+", text)
                    if not 0 < len(array_geps) <= 16 or not any(
                            re.search(r", i32 0, i32 %\w+$", gep) for gep in array_geps):
                        raise RuntimeError(f"{name}: expected bounded, indexed loop IR for {storage}")
            if name == "p72-early-exit" and re.search(r"\bcall\b[^\n]*\bi32 99\b", text):
                raise RuntimeError(f"{name}: unreachable initializer or index was emitted")
            modules.append((name, output, expected, b""))

        for name, contents, expected in (
            ("p73-copy-values", (p07 / "copy_values.rx").read_text(encoding="utf-8"),
             (p07 / "copy_values.out").read_bytes()),
            ("p73-snapshot-order", """
                #[derive(Clone, Copy)]
                struct Pair { first: i32, second: i32 }
                struct Source { value: Pair }
                fn from_index() {
                    let mut rows = [[1, 2], [3, 4]];
                    rows[{ println_i32(20); rows[0][0] = 99; 1usize }] = {
                        println_i32(10);
                        rows[0]
                    };
                    println_i32(rows[0][0]);
                    println_i32(rows[1][0]); println_i32(rows[1][1]);
                }
                fn from_field() {
                    let mut source = Source { value: Pair { first: 5, second: 6 } };
                    let mut target = Pair { first: 7, second: 8 };
                    *{ println_i32(40); source.value.second = 88; &mut target } = {
                        println_i32(30);
                        source.value
                    };
                    println_i32(source.value.second);
                    println_i32(target.first); println_i32(target.second);
                }
                fn from_dereference() {
                    let mut rows = [[11, 12], [13, 14]];
                    rows[{ rows[0][0] = 77; 1usize }] = {
                        let borrowed = &rows[0];
                        *borrowed
                    };
                    println_i32(rows[0][0]);
                    println_i32(rows[1][0]); println_i32(rows[1][1]);
                }
                fn main() { from_index(); from_field(); from_dereference(); }
             """, b"10\n20\n99\n1\n2\n30\n40\n88\n5\n6\n77\n11\n12\n"),
            ("p73-reference-fields-and-move", """
                #[derive(Clone, Copy)]
                struct View<'a> { reference: &'a i32, mark: bool }
                struct Ticket { code: i32 }
                struct Envelope { payload: Ticket }
                fn main() {
                    let first = 41;
                    let second = 42;
                    let mut view = View { reference: &first, mark: true };
                    let copied = view;
                    view.reference = &second;
                    view.mark = false;
                    println_i32(*copied.reference);
                    println_i32(copied.mark as i32);
                    println_i32(*view.reference);
                    println_i32(view.mark as i32);
                    let envelope = Envelope { payload: Ticket { code: 43 } };
                    let mut ticket = envelope.payload;
                    let next = Ticket { code: ticket.code + 1 };
                    ticket = next;
                    println_i32(ticket.code);
                }
             """, b"41\n1\n42\n0\n44\n"),
            ("p73-results", """
                #[derive(Clone, Copy)]
                struct Pair { first: i32, second: i32 }
                fn mark(n: i32) -> i32 { println_i32(n); n }
                fn choose(flag: bool) {
                    let mut original = Pair { first: 1, second: 2 };
                    let selected = if flag { println_i32(10); original }
                                   else { Pair { first: mark(20), second: 4 } };
                    original.first = 90;
                    let mut i = 0;
                    let result = loop {
                        i += 1;
                        if i == 1 { continue; }
                        if flag { break [selected.first, selected.second]; }
                        break [selected.second, selected.first];
                    };
                    println_i32(result[0]); println_i32(result[1]);
                    println_i32(original.first);
                }
                fn early_branch(flag: bool) -> i32 {
                    let value = if flag { [31, 32] } else { return 33; };
                    value[1]
                }
                fn early_break(flag: bool) -> i32 {
                    let value = loop {
                        if flag { break { return 34; [99, 100] }; }
                        break [35, 36];
                    };
                    value[1]
                }
                fn no_exit() -> i32 {
                    let value = loop { return 37; break [99, 100]; };
                    println_i32(99);
                    value[0]
                }
                fn main() {
                    choose(true); choose(false);
                    println_i32(early_branch(true)); println_i32(early_branch(false));
                    println_i32(early_break(true)); println_i32(early_break(false));
                    println_i32(no_exit());
                }
             """, b"10\n1\n2\n90\n20\n4\n20\n90\n32\n33\n34\n36\n37\n"),
            ("p73-assignment-early-exit", """
                fn rhs_exit() -> i32 {
                    let mut rows = [[1, 2], [3, 4]];
                    rows[{ println_i32(99); 1usize }] = { return 51; [5, 6] };
                    rows[1][0]
                }
                fn target_exit() -> i32 {
                    let mut rows = [[1, 2], [3, 4]];
                    rows[{ println_i32(53); return 54; 1usize }] = { println_i32(52); rows[0] };
                    println_i32(99);
                    rows[1][0]
                }
                fn main() { println_i32(rhs_exit()); println_i32(target_exit()); }
             """, b"51\n52\n53\n54\n"),
        ):
            source = temp / f"{name}.rx"
            output = temp / f"{name}.ll"
            source.write_text(contents, encoding="utf-8")
            run([args.compiler, "--stage", "ir", str(source), "-o", str(output)])
            if name in ("p73-results", "p73-assignment-early-exit") and re.search(
                    r"\bcall\b[^\n]*\bi32 99\b", output.read_text(encoding="utf-8")):
                raise RuntimeError(f"{name}: unreachable aggregate continuation was emitted")
            modules.append((name, output, expected, b""))

        for name, contents, expected in (
            ("p74-aggregate-calls", (p07 / "aggregate_calls.rx").read_text(encoding="utf-8"),
             (p07 / "aggregate_calls.out").read_bytes()),
            ("p74-temporary-array", (p07 / "temporary_array.rx").read_text(encoding="utf-8"),
             (p07 / "temporary_array.out").read_bytes()),
            ("p74-deref-array-reference",
             "fn make() -> [i32; 1] { [17] } fn read(p: &[i32; 1]) -> i32 { p[0] } "
             "fn main() { let a = make(); let r = &a; let rr = &r; println_i32(read(*rr)); }",
             b"17\n"),
            ("p74-mixed-arguments", """
                fn combine(mut first: [i32; 2], stamp: i32, second: [i32; 2],
                           flag: bool, reference: &[i32; 2]) -> [i32; 2] {
                    println_i32(stamp);
                    if flag { first[1] = second[0] + reference[1]; }
                    first
                }
                fn direct(reference: &[i32; 2]) -> &[i32; 2] { reference }
                fn consume(mut value: [i32; 2]) { value[1] = 80; println_i32(value[1]); }
                fn main() {
                    let mut original = [1, 2];
                    let other = [3, 4];
                    let returned = combine(original, { println_i32(10); original[0] = 9; 20 },
                                           original, true, direct(&other));
                    println_i32(returned[0]); println_i32(returned[1]);
                    consume(original);
                    println_i32(original[0]); println_i32(original[1]);
                }
             """, b"10\n20\n1\n13\n80\n9\n2\n"),
            ("p74-recursive-results", """
                #[derive(Clone, Copy)]
                struct Packet { rows: [[i32; 2]; 2], flag: bool }
                fn main() {
                    let a = produce(2, true);
                    let b = produce(3, false);
                    println_i32(a.rows[0][0]); println_i32(a.rows[0][1]);
                    println_i32(a.rows[1][0]); println_i32(a.rows[1][1]);
                    println_i32(a.flag as i32);
                    println_i32(b.rows[0][0]); println_i32(b.rows[0][1]);
                    println_i32(b.rows[1][0]); println_i32(b.rows[1][1]);
                    println_i32(b.flag as i32);
                }
                fn produce(depth: i32, flag: bool) -> Packet {
                    if depth == 0 { return Packet { rows: [[1, 2], [3, 4]], flag: false }; }
                    let mut previous = produce(depth - 1, flag);
                    previous.rows[0][0] += depth;
                    loop {
                        break if flag { previous.flag = true; previous }
                              else { previous.rows[1][1] += depth; previous };
                    }
                }
             """, b"4\n2\n3\n4\n1\n7\n2\n3\n10\n0\n"),
            ("p74-temporary-slots", """
                fn make(value: i32) -> [i32; 2] {
                    println_i32(value);
                    [value, value + 1]
                }
                fn main() {
                    let first = &mut make(31)[0];
                    let second = &mut make(41)[1];
                    *first = 51;
                    println_i32(*first); println_i32(*second);
                }
             """, b"31\n41\n51\n42\n"),
            ("p74-argument-early-exit", """
                fn never_called(a: [i32; 2], mark: i32, b: [i32; 2]) -> [i32; 2] {
                    println_i32(99);
                    [a[0] + mark, b[1]]
                }
                fn early() -> i32 {
                    let value = never_called({ println_i32(61); [1, 2] },
                                             { return 62; 0 },
                                             { println_i32(98); [3, 4] });
                    value[0]
                }
                fn main() { println_i32(early()); }
             """, b"61\n62\n"),
        ):
            source = temp / f"{name}.rx"
            output = temp / f"{name}.ll"
            source.write_text(contents, encoding="utf-8")
            run([args.compiler, "--stage", "ir", str(source), "-o", str(output)])
            modules.append((name, output, expected, b""))

        # 方法用例与前几阶段共用 LLVM 校验和 RV32 O0/O2 执行回路。
        p08 = Path(__file__).resolve().parent / "ir" / "p08"
        for stem in ("methods", "inherent_clone", "qualified_calls", "receiver_order",
                     "self_snapshot", "receiver_places", "receiver_early_exit",
                     "array_len", "array_len_places"):
            name = f"p8-{stem}"
            output = temp / f"{name}.ll"
            run([args.compiler, "--stage", "ir", str(p08 / f"{stem}.rx"), "-o", str(output)])
            modules.append((name, output, (p08 / f"{stem}.out").read_bytes(), b""))

        p09 = p08.parent / "p09"
        for stem in ("boxes", "box_places", "empty_vec", "vec_places",
                     "vec_growth", "vec_two_phase", "vec_push_places",
                     "vec_remove", "remove_owned", "vec_remove_order"):
            name = f"p9-{stem}"
            output = temp / f"{name}.ll"
            run([args.compiler, "--stage", "ir", str(p09 / f"{stem}.rx"), "-o", str(output)])
            modules.append((name, output, (p09 / f"{stem}.out").read_bytes(), b""))

        source = temp / "runtime-smoke.rx"
        output = temp / "runtime-smoke.ll"
        source.write_text("fn main() { let boxed = Box::<i32>::new(get_i32()); "
                          "print_i32(*boxed); println_i32(*boxed + 1); }", encoding="utf-8")
        run([args.compiler, "--stage", "ir", str(source), "-o", str(output)])
        modules.append(("runtime-smoke", output, b"-42-41\n", b"-42\n"))

        p10 = p08.parent / "p10"
        helper_counts = {"recursive_tree": 2, "mutual_recursion": 4,
                         "helper_cache": 4, "recursive_declaration": 0}
        for stem in ("deep_clone", "container_clone", "shared_ref_clone", "clone_dispatch",
                     "equality", "eq_order", "equality_nested", *helper_counts):
            name = f"p10-{stem}"
            source = p10 / f"{stem}.rx"
            output = temp / f"{name}.ll"
            run([args.compiler, "--stage", "semantic", str(source)])
            run([args.compiler, "--stage", "ir", str(source), "-o", str(output)])
            text = output.read_text(encoding="utf-8")
            if re.search(r"\bicmp (?:eq|ne) ptr\b|@(?:memcmp|bcmp)\b", text):
                raise RuntimeError(f"{name}: equality must compare values, not addresses or raw bytes")
            if stem in helper_counts:
                check_p10_helpers(name, text, helper_counts[stem],
                                  recursive=stem in ("recursive_tree", "mutual_recursion"))
                repeated = temp / f"{name}-repeated.ll"
                run([args.compiler, "--stage", "ir", str(source), "-o", str(repeated)])
                if repeated.read_bytes() != output.read_bytes():
                    raise RuntimeError(f"{name}: helper definitions must be deterministic")
            modules.append((name, output, (p10 / f"{stem}.out").read_bytes(), b""))

        for name, command, expected in (
            ("model-smoke", [args.model_test], b""),
            ("model-values", [args.model_test, "--emit-model"], b""),
            # 手工把 bool 往返结果扩展成 i32 再打印，观察 true/false 为 1/0。
            ("model-bool", [args.model_test, "--emit-bool"], b"1\n0\n"),
        ):
            output = temp / f"{name}.ll"
            output.write_text(run(command).stdout, encoding="utf-8")
            modules.append((name, output, expected, b""))
        # 由 LLVM 独立计算，与 C++ 的 P7.1 布局断言使用同一组期望值。
        probe = Path(__file__).resolve().parent / "ir" / "p07" / "layout_probe.ll"
        modules.append(("p71-layout-probe", probe,
                        b"0\n4\n8\n12\n4\n"     # Inner: offsets, stride, alignment
                        b"4\n28\n40\n44\n"    # Outer: offsets and size
                        b"4\n8\n12\n16\n"     # Node: Vec fields, Box and size
                        b"3\n8\n12\n16\n",    # Matrix: row stride, reference, word and size
                        b""))
        for name, output, _, _ in modules:
            verify_module(args, name, output, temp / f"{name}.bc")

        source = temp / "empty-main.rx"
        output = temp / "output.ll"
        for command in (
            [args.compiler, "--stage", "ir", str(source)],
            [args.compiler, "--stage", "semantic", str(source), "-o", str(output)],
            [args.compiler, "--stage", "unknown", str(source)],
            [args.compiler, "--stage", "ir", str(source), "-o"],
            [args.compiler, "--stage", "ir", str(source), "-o", str(output), "-o", str(output)],
            [args.compiler, "--stage", "ir", str(temp / "missing.rx"), "-o", str(output)],
            [args.compiler, "--stage", "ir", str(source), "-o", str(temp / "missing" / "out.ll")],
        ):
            run(command, expected=2)
        debug = run([args.compiler, "--stage", "ir", "--dump-tree", str(source), "-o", str(output)])
        if not debug.stdout.strip() or debug.stdout.strip() in output.read_text(encoding="utf-8"):
            raise RuntimeError("parse tree output leaked into LLVM text")

        # All are semantically valid. Reject unsupported lowering even in unused bodies.
        for index, contents in enumerate((
            "fn main() { let x = { (); }; }",
        )):
            unsupported = temp / f"unsupported-{index}.rx"
            unsupported.write_text(contents, encoding="utf-8")
            run([args.compiler, "--stage", "semantic", str(unsupported)])
            for prior in (None, b"preserve on unsupported lowering\n"):
                target = temp / f"unsupported-{index}-{prior is None}.ll"
                if prior is not None:
                    target.write_bytes(prior)
                result = run([args.compiler, "--stage", "ir", str(unsupported), "-o", str(target)], expected=1)
                if "尚未支持" not in result.stderr:
                    raise RuntimeError("unsupported valid input must explain the lowering limit")
                if prior is None and target.exists():
                    raise RuntimeError("unsupported lowering created an output file")
                if prior is not None and target.read_bytes() != prior:
                    raise RuntimeError("unsupported lowering truncated existing output")

        # P8.2：用户方法作为普通函数生成，即使未被调用也不再要求拒绝。
        supported_method = temp / "supported-method.rx"
        supported_method.write_text(
            "struct Data {} impl Data { fn unused(&self) {} } fn main() {}",
            encoding="utf-8")
        supported_method_ir = temp / "supported-method.ll"
        run([args.compiler, "--stage", "semantic", str(supported_method)])
        run([args.compiler, "--stage", "ir", str(supported_method), "-o", str(supported_method_ir)])
        run([args.llvm_as, str(supported_method_ir), "-o", str(temp / "supported-method.bc")])
        run([args.opt, "-passes=verify", "-disable-output", str(supported_method_ir)])

        # 检查特定诊断，避免把解析失败或后端不支持误认为语义规则正确。
        expected_diagnostics = {
            "struct Data { n: i32 } impl Data { fn get(&self) -> i32 { self.n } } "
            "fn main() { let x = Data { n: 1 }; Data::get(x); }":
                "type mismatch: expected &Data, found Data",
            # Box 的 &self clone 先命中；内部按值 clone 虽接收 i32，也不能回退到它。
            "#[derive(Clone)] struct Item { n: i32 } "
            "impl Item { fn clone(self, delta: i32) -> i32 { self.n + delta } } "
            "fn bad(x: Box<Item>) { x.clone(2); } fn main() {}":
                "call expects 0 argument(s), found 1",
            "struct Data { n: i32 } impl Data { fn set(&mut self) { self.n = 1; } } "
            "fn main() { let x = Data { n: 0 }; x.set(); }":
                "cannot mutably borrow an immutable or shared place",
            "struct Data { n: i32 } impl Data { fn set(&mut self) { self.n = 1; } } "
            "fn bad(x: &&mut Data) { x.set(); } fn main() {}":
                "cannot mutably borrow an immutable or shared place",
            "fn main() { let values = [1, 2]; values.len(1); }":
                "call expects 0 argument(s), found 1",
            "fn main() { let values = [1, 2]; let size: i32 = values.len(); }":
                "type mismatch: expected i32, found usize",
            "fn main() { let b = Box::<i32>::new(1); *b = 2; }":
                "assignment target is not mutable",
            "fn bad(b: &Box<&mut i32>) { ***b = 1; } fn main() {}":
                "assignment target is not mutable",
            "struct Data { n: i32 } impl Data { fn set(&mut self) { self.n = 1; } } "
            "fn bad(b: &Box<Data>) { b.set(); } fn main() {}":
                "cannot mutably borrow an immutable or shared place",
            (p09 / "immutable_vec_write.rx").read_text(encoding="utf-8"):
                "assignment target is not mutable",
            "fn bad(values: Box<Vec<&mut i32>>) { *values[0] = 1; } fn main() {}":
                "assignment target is not mutable",
            "fn bad(values: Vec<&mut i32>) { let item: &mut i32 = values[0]; } fn main() {}":
                "cannot mutably reborrow through shared access",
            "fn bad(values: Vec<Vec<i32>>) { values[0].push(1); } fn main() {}":
                "cannot mutably borrow an immutable or shared place",
            "fn bad(values: &Vec<i32>) { values.remove(0); } fn main() {}":
                "cannot mutably borrow an immutable or shared place",
            "fn bad(values: &mut Vec<i32>, index: i32) { values.remove(index); } fn main() {}":
                "type mismatch: expected usize, found i32",
            (p10 / "clone_mutable_reference.rx").read_text(encoding="utf-8"):
                "no method named clone for type &mut Item",
            (p10 / "clone_nonclone_vec.rx").read_text(encoding="utf-8"):
                "type Vec<Item> does not support clone",
            (p10 / "equality_requires_partial_eq.rx").read_text(encoding="utf-8"):
                "equality requires PartialEq on the operand type",
        }
        for index, contents in enumerate((
            "fn main(value: i32) {}",
            "fn main<'a>() {}",
            "fn main() { println_i32(true); }",
            "fn main() { println_i32(7u32); }",
            "fn println_i32() {} fn main() { println_i32(); }",
            # 借用已有位置必须保留权限，不能复制成可写临时值绕过 sema。
            "fn main() { let x = 7; let p = &mut x; }",
            "fn main() { let mut x = 7; let p = &x; let q = &mut *p; }",
            "fn main() { let mut x = 7; let p = &mut x; let r = &p; let q = &mut **r; }",
            # P5.2 的隐式转换不能恢复可变权限、改变内部引用类型或借用拥有值。
            "fn main() { let x = 7; let q: &mut i32 = &x; }",
            "fn main() { let mut x = 7; let p = &mut x; let q: &mut i32 = &p; }",
            "fn main() { let mut x = 7; let mut p = &mut x; let q: &mut &i32 = &mut p; }",
            "fn bad(p: &&mut i32) { let q: &mut i32 = { { *p } }; } fn main() {}",
            "fn main() { let x = 7; let q: &i32 = x; }",
            "fn main() { let x = 7i32; let q: u32 = x; }",
            "fn bad(p: Box<i32>) -> &i32 { p } fn main() {}",
            "fn bad(p: &Vec<i32>) -> &i32 { p } fn main() {}",
            # P5.3 不应绕过 sema 的 place、权限和 RHS 类型规则。
            "fn main() { let x = 1; x = 2; }",
            "fn main() { let x = 1; let p = &x; *p += 2; }",
            "fn main() { 1 = 2; }",
            "fn main() { let mut x = 1; x = true; }",
            "fn main() { let mut x = 1u32; x += 2i32; }",
            "fn main() { let mut x = 1; x += &mut 2; }",
            "fn main() { let mut x = 1; x += &&2; }",
            "fn main() { let mut x = true; x += false; }",
            "fn main() { let mut x = 1; let mut p = &mut x; p += 2; }",
            # P5.4 只放开规定的引用层级，不能套用通用 coercion 或直接比较指针。
            "fn main() { let a = 1; let b = 2; let result = &&a + &b; }",
            "fn main() { let mut a = 1; let b = 2; let result = &mut a + &b; }",
            "fn main() { let a = 1; let result = -&&a; }",
            "fn main() { let mut a = 1; let result = !&mut a; }",
            "fn main() { let a = 1u32; let b = 2i32; let result = &a + &b; }",
            "fn main() { let mut a = 1; let b = 2; let result = &mut a < &b; }",
            "fn main() { let a = 1; let mut b = 2; let result = &&a < &&mut b; }",
            "fn main() { let a = 1; let b = 2; let result = a < &b; }",
            "fn main() { let a = 1; let b = 2; let result = &&a < &b; }",
            "fn bad(a: &Box<i32>, b: &Box<i32>) -> bool { a < b } fn main() {}",
            # P6.1：条件必须为 bool，且运行时不执行的分支仍须通过完整语义检查。
            "fn main() { if 1 { println_i32(1); } }",
            "fn main() { if true { println_i32(1); } else { println_i32(missing); } }",
            # 同为 LLVM i32 不代表同一种源整数类型。
            "fn main() { let value = if true { 1i32 } else { 1u32 }; }",
            # P6.3：两支都返回或条件直接返回时，不可达源码仍须检查名称和类型。
            "fn main() { if true { return; } else { return; }; println_i32(missing); }",
            "fn main() { if { return; true } { println_i32(true); } }",
            # P6.4：短路不改变 bool 操作数要求，也不能跳过对右侧的语义检查。
            "fn main() { let result = &false && true; }",
            "fn main() { let result = true || 1; }",
            "fn main() { let result = false && missing; }",
            # P6.5：条件必须为 bool，且零次循环的体仍需通过名称和类型检查。
            "fn main() { while 1 {} }",
            "fn main() { while false { 1 } }",
            "fn main() { while false { println_i32(missing); } }",
            # 只允许保证发散的非 unit 块；仍要检查不可达尾表达式本身。
            "fn f(flag: bool) { { if flag { return; } 1 } println_i32(2); } fn main() {}",
            "fn f(flag: bool) { loop { if flag { return; } 1 } } fn main() {}",
            "fn main() { while true { return; missing } }",
            "fn main() { { return; 1 + true } println_i32(2); }",
            "fn main() { let n: i32 = { return; true }; }",
            # P6.6：不能跳到循环外，也不能越过 while 条件屏障。
            "fn main() { break; }",
            "fn main() { continue; }",
            "fn main() { while ({ break; true }) {} }",
            "fn main() { loop { while ({ continue; true }) {} break; } }",
            "fn main() { while true { break 1; } }",
            # P6.7：不可达的 break 也要满足固定目标，不能隐式改变整数类型或恢复可变权限。
            "fn main() { let number: u32 = loop { break 1; break 2i32; }; }",
            "fn main() { let x = 1; let r: &mut i32 = loop { break &x; }; }",
            # P6.8：未调用的函数和被 return/break/continue 跳过的源码也必须检查。
            "fn main() {} fn unused() { let x: i32 = true; }",
            "fn main() { return; let x = missing; }",
            "fn main() { loop { break; println_i32(missing); } }",
            "fn main() {} fn unused() { loop { continue; println_i32(true); } }",
            # P7.2：非法构造、错误下标和共享路径写入必须在语义阶段拒绝。
            "struct Pair { a: i32, b: i32 } fn main() { let p = Pair { a: 1, a: 2 }; }",
            "struct Token { n: i32 } fn main() { let a = [Token { n: 1 }; 2]; }",
            "fn main() { let a = [1, 2]; println_i32(a[1u32]); }",
            "struct Data { a: [i32; 1] } "
            "fn main() { let mut d = Data { a: [0] }; let p = &d; p.a[0] = 1; }",
            # P7.3：整体赋值仍要求目标可写、源类型一致。
            "fn main() { let a = [1, 2]; a = [3, 4]; }",
            "fn main() { let mut a = [1, 2]; a = [3]; }",
            "struct A { n: i32 } struct B { n: i32 } "
            "fn main() { let mut a = A { n: 1 }; a = B { n: 2 }; }",
            *expected_diagnostics,
        )):
            invalid_source = temp / f"semantic-error-{index}.rx"
            invalid_source.write_text(contents, encoding="utf-8")
            semantic = run([args.compiler, "--stage", "semantic", str(invalid_source)], expected=1)
            diagnostic = expected_diagnostics.get(contents)
            if diagnostic is not None and diagnostic not in semantic.stderr:
                raise RuntimeError(f"{invalid_source.name}: expected {diagnostic!r}, got {semantic.stderr!r}")
            fresh_output = invalid_source.with_suffix(".ll")
            sentinel = b"preserve on semantic failure\n"
            output.write_bytes(sentinel)
            for target in (fresh_output, output):
                result = run([args.compiler, "--stage", "ir", str(invalid_source), "-o", str(target)], expected=1)
                if not semantic.stderr.strip() or result.stderr != semantic.stderr:
                    raise RuntimeError("IR must perform complete semantic checks before lowering")
            if fresh_output.exists() or output.read_bytes() != sentinel:
                raise RuntimeError(f"{invalid_source.name}: semantic failure created or changed IR output")
        if Path("/dev/full").exists():
            run([args.compiler, "--stage", "ir", str(source), "-o", "/dev/full"], expected=2)

        fixture_executions = 0
        runtime_assembly = None
        evidence = temp
        if args.reimu:
            log_root = ROOT / "target" / "tests"
            log_root.mkdir(parents=True, exist_ok=True)
            prefix = "official-ir-" if args.official else "p11-resources-"
            evidence = Path(tempfile.mkdtemp(prefix=prefix, dir=log_root))
            print(f"IR resource logs: {evidence.relative_to(ROOT)}", flush=True)
            for tool, command in (("clang", [args.clang]), ("llvm-as", [args.llvm_as]), ("opt", [args.opt]),
                                  ("reimu", ["xmake", "run", "-P", "vendor/REIMU", "reimu"])):
                run_artifact_tool([*command, "--version"], evidence / f"{tool}-version")
            runtime = ROOT / "rxcc" / "runtime" / "runtime.c"
            runtime_assembly = evidence / "runtime.s"
            run_artifact_tool([args.clang, *RV32_TARGET, "-mllvm", "-riscv-no-aliases", "-O2",
                               "-ffreestanding", "-S", str(runtime), "-o", str(runtime_assembly)],
                              evidence / "runtime-clang")
            runtime_assembly.write_text(strip_debug(runtime_assembly.read_text(encoding="utf-8")),
                                        encoding="utf-8")
            runtime_obj = runtime_assembly.with_suffix(".o")
            run_artifact_tool([args.clang, *RV32_TARGET, "-c", str(runtime_assembly), "-o", str(runtime_obj)],
                              evidence / "runtime-assemble")
            if not runtime_obj.is_file():
                raise RuntimeError(f"Clang did not create {runtime_obj}")
            for name, ir_path, expected_stdout, stdin_data in modules:
                fixture_executions += run_rv32_module(args, name, ir_path, runtime_assembly, temp,
                                                      [(stdin_data, expected_stdout)])

        check_runner_failures(evidence / "runner-failures")
        resource_executions = run_manifest_suite(args, resources, evidence / "p11", runtime_assembly,
                                                 resource_fixtures=True)
        if args.official:
            boundary_executions = run_manifest_suite(args, boundaries, evidence / "codegen-boundaries", runtime_assembly)
            official_executions = run_manifest_suite(args, official, evidence, runtime_assembly)
    print(f"P2-P10 IR fixtures passed: {len(modules)} modules verified" +
          (f", {fixture_executions} RV32 O0/O2 executions" if args.reimu else ""))
    print(f"P11 resource fixtures passed: {len(resources)} modules verified" +
          (f", {resource_executions} RV32 O0/O2 executions" if args.reimu else "") +
          "; runner failure probes passed")
    if args.official:
        print(f"Official codegen resource cases via rxcc IR passed: {len(boundaries)} programs, "
              f"{sum(len(pairs) for pairs in boundaries.values())} I/O pairs, "
              f"{boundary_executions} RV32 O0/O2 executions")
        print(f"Official optimization via rxcc IR passed: {len(official)} programs, "
              f"{sum(len(pairs) for pairs in official.values())} I/O pairs, "
              f"{official_executions} RV32 O0/O2 executions")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, TestError) as error:
        print(error, file=os.sys.stderr)
        raise SystemExit(1)
