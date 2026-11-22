#!/usr/bin/env python3
"""Validate the implemented P2-P5.4 IR subset with external LLVM 22 tools."""

import argparse
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile


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
    args = parser.parse_args()
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

        for name, command, expected in (
            ("model-smoke", [args.model_test], b""),
            ("model-values", [args.model_test, "--emit-model"], b""),
            # 手工把 bool 往返结果扩展成 i32 再打印，观察 true/false 为 1/0。
            ("model-bool", [args.model_test, "--emit-bool"], b"1\n0\n"),
        ):
            output = temp / f"{name}.ll"
            output.write_text(run(command).stdout, encoding="utf-8")
            modules.append((name, output, expected, b""))
        for name, output, _, _ in modules:
            # P4.3：自有 baseline 不附加不溢出、精确性或别名承诺。
            # 检查生成文本，而不是依赖 LLVM verifier 事后接受错误承诺。
            text = output.read_text(encoding="utf-8")
            for forbidden in (" nsw ", " nuw ", " exact", "noalias",
                              "inbounds", "nonnull"):
                if forbidden in text:
                    raise RuntimeError(
                        f"{name}: baseline IR must not contain {forbidden.strip()}")
            run([args.llvm_as, str(output), "-o", str(temp / f"{name}.bc")])
            run([args.opt, "-passes=verify", "-disable-output", str(output)])

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
            "fn main() { Box::<i32>::new(1); }",
            "fn main() { Vec::<i32>::new(); }",
            "fn main() { let x = true && false; }",
            "fn main() { let x = false && { return; true }; println_i32(7); }",
            "fn main() { let x = true || { return; false }; println_i32(7); }",
            "fn main() { let x = { return; true } && false; }",
            "fn main() { let x = { return; false } || true; }",
            "fn main() { let x = { (); }; }",
            "fn main() { let x = { [1, 2] }; }",
            "fn main() { let x = loop {}; }",
            "fn unused(value: &[i32; 1]) {} fn main() {}",
            "fn unused() -> [i32; 1] { [1] } fn main() {}",
            "fn main() {} fn unused() { println_i32(99); let x = [1, 2]; }",
            "struct Data {} impl Data { fn unused(&self) {} } fn main() {}",
            # 引用相等需要 P10 的结构化读取，不能比较地址后声称完成。
            "fn main() { let a = 1; let b = 1; let ra = &a; let rb = &b; "
             "println_i32((ra == rb) as i32); }",
            "fn main() { let a = 1; let b = 2; println_i32((&a != &b) as i32); }",
            # Box 转换路径仍需 P9；不能把被擦除为 ptr 的源类型直接混用。
            "fn narrow(p: &Box<i32>) -> &i32 { p } fn main() {}",
            "fn narrow(p: &Box<Box<i32>>) -> &i32 { p } fn main() {}",
            # 普通赋值只放开标量与引用，聚合整体赋值仍留给 P7。
            "fn main() { let mut a = [1, 2]; a = [3, 4]; }",
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

        for index, contents in enumerate((
            "fn main(value: i32) {}",
            "fn main<'a>() {}",
            "fn main() {} fn unused() { let x: i32 = true; }",
            "fn main() { return; let x = missing; }",
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
            "fn main() { let result = &false && true; }",
            "fn bad(a: &Box<i32>, b: &Box<i32>) -> bool { a < b } fn main() {}",
        )):
            invalid_source = temp / f"semantic-error-{index}.rx"
            invalid_source.write_text(contents, encoding="utf-8")
            semantic = run([args.compiler, "--stage", "semantic", str(invalid_source)], expected=1)
            output.write_bytes(b"preserve on semantic failure\n")
            result = run([args.compiler, "--stage", "ir", str(invalid_source), "-o", str(output)], expected=1)
            if not semantic.stderr.strip() or result.stderr != semantic.stderr:
                raise RuntimeError("IR must perform complete semantic checks before lowering")
            if output.read_bytes() != b"preserve on semantic failure\n":
                raise RuntimeError("semantic failure changed the previous output")
        if Path("/dev/full").exists():
            run([args.compiler, "--stage", "ir", str(source), "-o", "/dev/full"], expected=2)

        if args.reimu:
            runtime = temp / "runtime.c"
            runtime.write_text('extern int printf(const char *, ...);\n'
                               'extern int scanf(const char *, ...);\n'
                               'int get_i32(void) { int value; (void)scanf("%d", &value); return value; }\n'
                               'void print_i32(int value) { (void)printf("%d", value); }\n'
                               'void println_i32(int value) { (void)printf("%d\\n", value); }\n',
                               encoding="utf-8")
            runtime_assembly = temp / "runtime.s"
            run([args.clang, "--target=riscv32-unknown-elf", "-march=rv32im", "-mabi=ilp32",
                 "-mllvm", "-riscv-no-aliases", "-O2", "-fno-builtin", "-S", str(runtime),
                 "-o", str(runtime_assembly)])
            run([args.clang, "--target=riscv32-unknown-elf", "-march=rv32im", "-mabi=ilp32",
                 "-c", str(runtime_assembly), "-o", str(temp / "runtime.o")])
            for name, ir_path, expected_stdout, stdin_data in modules:
                for optimization in ("O0", "O2"):
                    assembly = temp / f"{name}-{optimization}.s"
                    stdout = temp / f"{name}-{optimization}.out"
                    profile = temp / f"{name}-{optimization}.profile"
                    run([args.clang, "--target=riscv32-unknown-elf", "-march=rv32im", "-mabi=ilp32",
                         "-mllvm", "-riscv-no-aliases", f"-{optimization}", "-S", str(ir_path), "-o", str(assembly)])
                    run([args.clang, "--target=riscv32-unknown-elf", "-march=rv32im", "-mabi=ilp32",
                         "-c", str(assembly), "-o", str(temp / f"{name}-{optimization}.o")])
                    run(["xmake", "run", "-P", "vendor/REIMU", "reimu", "--memory=256M", "--stack=1M",
                         "-f", f"{assembly},{runtime_assembly}", "-o", str(stdout), "-p", str(profile)],
                        stdin_data=stdin_data)
                    exits = [line for line in profile.read_bytes().splitlines() if line.startswith(b"Exit code:")]
                    if stdout.read_bytes() != expected_stdout or exits != [b"Exit code: 0"]:
                        raise RuntimeError(f"{name} {optimization}: expected stdout {expected_stdout!r} "
                                           f"and exit zero, got {stdout.read_bytes()!r}, {exits!r}")
    print(f"P2-P5.4 IR tests passed: {len(modules)} modules verified" +
          (f", {2 * len(modules)} RV32 O0/O2 executions" if args.reimu else ""))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(error, file=os.sys.stderr)
        raise SystemExit(1)
