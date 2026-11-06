#!/usr/bin/env python3
"""Run the local C++ IR smoke tests without requiring tracked test fixtures."""

import argparse
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile


def run(command, *, expected=0, timeout=60):
    try:
        process = subprocess.Popen(
            command, text=True, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
    except FileNotFoundError as error:
        raise RuntimeError(f"required executable not found: {command[0]}") from error
    with process:
        try:
            stdout, stderr = process.communicate(timeout=timeout)
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
    parser.add_argument("--llvm-as", default="llvm-as-22")
    parser.add_argument("--opt", default="opt-22")
    parser.add_argument("--clang", default="clang-22")
    parser.add_argument("--reimu", action="store_true",
                        help="also run the generated RV32 program with REIMU")
    args = parser.parse_args()

    compiler = args.compiler
    ir_test = args.ir_test
    tools = [compiler, ir_test, args.llvm_as, args.opt]
    if args.reimu:
        tools.extend([args.clang, "xmake"])
    for tool in tools:
        if shutil.which(tool) is None:
            raise RuntimeError(
                f"required executable missing or not executable: {tool}; "
                "build the local test targets or set the corresponding tool option/PATH")
    run([ir_test])
    with tempfile.TemporaryDirectory(prefix="rxcc-ir-") as temp:
        temp = Path(temp)
        source = temp / "empty_main.rx"
        output = temp / "empty_main.ll"
        source.write_text("fn main() {}\n", encoding="utf-8")

        run([compiler, "--stage", "ir", str(source), "-o", str(output)])
        text = output.read_text(encoding="utf-8")
        valid_ir = temp / "validated.ll"
        shutil.copyfile(output, valid_ir)
        if "define internal void @__rx_fn_0()" not in text or "define i32 @main()" not in text:
            raise RuntimeError("generated module does not contain both entry functions")
        run([args.llvm_as, str(output), "-o", str(temp / "empty_main.bc")])
        run([args.opt, "-passes=verify", "-disable-output", str(output)])

        locals_source = temp / "locals.rx"
        locals_source.write_text(
            "fn helper(flag: bool, count: u32) { let local = false; }\n"
            "fn main() { let x = 1; let x = 2; let flag = true; }\n",
            encoding="utf-8")
        locals_output = temp / "locals.ll"
        run([compiler, "--stage", "ir", str(locals_source), "-o", str(locals_output)])
        locals_ir = locals_output.read_text(encoding="utf-8")
        if len(re.findall(r"\balloca (?:i8|i32), align [14]", locals_ir)) != 6:
            raise RuntimeError("each parameter and let must have its own fixed slot")
        if not re.search(r"store i32 1, ptr %local\.\d+\.slot, align 4", locals_ir) or \
                not re.search(r"store i32 2, ptr %local\.\d+\.slot, align 4", locals_ir):
            raise RuntimeError("shadowed integer lets must initialize distinct typed slots")
        run([args.llvm_as, str(locals_output), "-o", str(temp / "locals.bc")])
        run([args.opt, "-passes=verify", "-disable-output", str(locals_output)])

        calls_source = temp / "calls.rx"
        calls_source.write_text(
            "fn main() { let n = answer(); println_i32(n); }\n"
            "fn answer() -> i32 { return 7; println_i32(99); }\n",
            encoding="utf-8")
        calls_output = temp / "calls.ll"
        run([compiler, "--stage", "ir", str(calls_source), "-o", str(calls_output)])
        calls_ir = calls_output.read_text(encoding="utf-8")
        if "call i32 @__rx_fn_1()" not in calls_ir or \
                "call void @println_i32(i32 %" not in calls_ir:
            raise RuntimeError("forward call, return value, or local read was not lowered")
        if "i32 99" in calls_ir:
            raise RuntimeError("instructions after return must not be emitted")
        run([args.llvm_as, str(calls_output), "-o", str(temp / "calls.bc")])
        run([args.opt, "-passes=verify", "-disable-output", str(calls_output)])

        modules = [
            ("empty_main", valid_ir, b"", b""),
            ("locals", locals_output, b"", b""),
            ("calls", calls_output, b"7\n", b""),
        ]
        for name, contents, expected_stdout, stdin in (
            ("never-let-unit",
             "fn main() { let x = return; println_i32(99); }\n", b"", b""),
            ("never-let-value",
             "fn main() { println_i32(answer()); }\n"
             "fn answer() -> i32 { let x = return 7; println_i32(99); }\n",
             b"7\n", b""),
            ("never-let-typed",
             "fn main() { println_i32(answer()); }\n"
             "fn answer() -> i32 { let x: i32 = return 7; println_i32(99); }\n",
             b"7\n", b""),
            ("input-i32",
             "fn main() { println_i32(get_i32()); println_i32(get_i32()); "
             "println_i32(get_i32()); println_i32(get_i32()); }\n",
             b"0\n-7\n-2147483648\n2147483647\n",
             b"0 -7 -2147483648 2147483647\n"),
            ("ninth-argument",
             "fn main() { println_i32(ninth(1, 2, 3, 4, 5, 6, 7, 8, 9)); }\n"
             "fn ninth(a: i32, b: i32, c: i32, d: i32, e: i32, f: i32, "
             "g: i32, h: i32, i: i32) -> i32 { i }\n", b"9\n", b""),
            ("old-slot-after-call",
             "fn main() { let old = 7; effect(); println_i32(old); }\n"
             "fn effect() { print_i32(8); }\n", b"87\n", b""),
            ("argument-order",
             "fn main() { consume(mark(1), mark(2), mark(3)); }\n"
             "fn consume(a: i32, b: i32, c: i32) { "
             "print_i32(a); print_i32(b); println_i32(c); }\n"
             "fn mark(value: i32) -> i32 { print_i32(value); value }\n",
             b"123123\n", b""),
            ("diverging-argument",
             "fn main() { consume(mark(1), return, get_i32(), "
             "{ print_i32(99); 4 }); println_i32(99); }\n"
             "fn consume(a: i32, b: i32, c: i32, d: i32) { print_i32(88); }\n"
             "fn mark(value: i32) -> i32 { print_i32(value); value }\n",
             b"1", b""),
        ):
            case_source = temp / f"{name}.rx"
            case_output = temp / f"{name}.ll"
            case_source.write_text(contents, encoding="utf-8")
            run([compiler, "--stage", "semantic", str(case_source)])
            run([compiler, "--stage", "ir", str(case_source), "-o", str(case_output)])
            case_ir = case_output.read_text(encoding="utf-8")
            run([args.llvm_as, str(case_output), "-o", str(temp / f"{name}.bc")])
            run([args.opt, "-passes=verify", "-disable-output", str(case_output)])
            if name.startswith("never-let-"):
                if name != "never-let-typed" and re.search(r"\balloca\b", case_ir):
                    raise RuntimeError(f"{name}: Never binding must not allocate a slot")
                if re.search(r"\bcall\s+void\s+@println_i32\(i32 99\)", case_ir):
                    raise RuntimeError(f"{name}: print after diverging let was emitted")
            if name == "old-slot-after-call":
                if not re.search(
                        r"store i32 7, ptr (%local\.\d+\.slot), align 4"
                        r".*?call void @__rx_fn_1\(\)"
                        r".*?load i32, ptr \1, align 4", case_ir, re.S):
                    raise RuntimeError("old local slot must be read after the effect call")
            if name == "diverging-argument":
                main_body = re.search(
                    r"define internal void @__rx_fn_0\(\)\s*\{(.*?)\n\}",
                    case_ir, re.S)
                if main_body is None:
                    raise RuntimeError("diverging-argument: missing source main")
                body = main_body.group(1)
                if not re.search(r"call i32 @__rx_fn_2\(i32 1\)", body) or \
                        re.search(r"\bcall\b[^\n]*@(?:get_i32|print_i32|println_i32|__rx_fn_1)\(",
                                  body):
                    raise RuntimeError(
                        "diverging argument must retain earlier effects and truncate "
                        "later input/output arguments and the final call")
            modules.append((name, case_output, expected_stdout, stdin))

        debug_output = temp / "debug.ll"
        debug = run([compiler, "--stage", "ir", "--dump-tree", str(source),
                     "-o", str(debug_output)])
        if not debug.stdout.strip() or debug.stdout.strip() in debug_output.read_text(
                encoding="utf-8"):
            raise RuntimeError("parse-tree output leaked into the LLVM IR file")

        for command in (
            [compiler, "--stage", "ir", str(source)],
            [compiler, "--stage", "semantic", str(source), "-o", str(output)],
            [compiler, "--stage", "unknown", str(source)],
            [compiler, "--stage", "ir", str(source), "-o"],
            [compiler, "--stage", "ir", str(source), "-o", str(output),
             "-o", str(temp / "duplicate.ll")],
            [compiler, "--stage", "ir", str(temp / "missing.rx"), "-o", str(output)],
        ):
            run(command, expected=2)

        missing_parent_output = temp / "missing" / "out.ll"
        run([compiler, "--stage", "ir", str(source), "-o", str(missing_parent_output)],
            expected=2)
        if missing_parent_output.exists():
            raise RuntimeError("failed output open unexpectedly created a file")

        for name, contents in (
            ("invalid-main", "fn main(x: i32) {}\n"),
            ("invalid-unused-body",
             "fn main() {}\nfn unused() { let value: i32 = true; }\n"),
            ("invalid-after-never-let",
             "fn main() { let x = return; let y = missing; }\n"),
        ):
            bad_source = temp / f"{name}.rx"
            bad_source.write_text(contents, encoding="utf-8")
            run([compiler, "--stage", "parse", str(bad_source)])
            semantic = run([compiler, "--stage", "semantic", str(bad_source)], expected=1)
            if not semantic.stderr.strip() or "syntax error" in semantic.stderr.lower():
                raise RuntimeError(f"{name}: expected a non-syntax semantic diagnostic")
            sema_failure_output = temp / f"{name}.ll"
            for previous in (None, b"preserve on semantic failure\n"):
                if previous is not None:
                    sema_failure_output.write_bytes(previous)
                rejected = run([compiler, "--stage", "ir", str(bad_source),
                                "-o", str(sema_failure_output)], expected=1)
                if rejected.stderr != semantic.stderr:
                    raise RuntimeError(
                        f"{name}: IR stage did not report the same semantic rejection\n"
                        f"semantic stderr:\n{semantic.stderr}\nIR stderr:\n{rejected.stderr}")
                if previous is None:
                    if sema_failure_output.exists():
                        raise RuntimeError("semantic failure unexpectedly created an IR file")
                elif sema_failure_output.read_bytes() != previous:
                    raise RuntimeError("semantic failure modified the existing IR file")

        unsupported_source = temp / "unsupported.rx"
        unsupported_source.write_text("fn main() { let x = 1 + 2; }\n", encoding="utf-8")
        output.write_text("preserve on generation failure\n", encoding="utf-8")
        run([compiler, "--stage", "ir", str(unsupported_source), "-o", str(output)],
            expected=1)
        if output.read_text(encoding="utf-8") != "preserve on generation failure\n":
            raise RuntimeError("unsupported lowering modified the output file")

        if Path("/dev/full").exists():
            run([compiler, "--stage", "ir", str(source), "-o", "/dev/full"], expected=2)

        if args.reimu:
            runtime_c = temp / "runtime.c"
            runtime_c.write_text(
                "typedef int int32_t;\n"
                "extern int printf(const char *, ...);\n"
                "extern int scanf(const char *, ...);\n"
                "int32_t get_i32(void) { int32_t value; "
                "(void)scanf(\"%d\", &value); return value; }\n"
                "void print_i32(int32_t value) { (void)printf(\"%d\", value); }\n"
                "void println_i32(int32_t value) { (void)printf(\"%d\\n\", value); }\n",
                encoding="utf-8")
            runtime = temp / "runtime.s"
            run([args.clang, "--target=riscv32-unknown-elf", "-march=rv32im",
                 "-mabi=ilp32", "-O2", "-fno-builtin", "-S", str(runtime_c),
                 "-o", str(runtime)])
            for module_name, ir_path, expected_stdout, stdin in modules:
                input_file = temp / f"{module_name}.in"
                input_file.write_bytes(stdin)
                for optimization in ("O0", "O2"):
                    assembly = temp / f"{module_name}-{optimization}.s"
                    stdout = temp / f"{module_name}-{optimization}.out"
                    profile = temp / f"{module_name}-{optimization}.profile"
                    run([args.clang, "--target=riscv32-unknown-elf", "-march=rv32im",
                         "-mabi=ilp32", f"-{optimization}", "-S", str(ir_path),
                         "-o", str(assembly)])
                    run(["xmake", "run", "-P", "vendor/REIMU", "reimu", "--memory=256M",
                          "--stack=1M", "-f", f"{assembly},{runtime}", "-o", str(stdout),
                         "-p", str(profile), "-i", str(input_file)])
                    if stdout.read_bytes() != expected_stdout:
                        raise RuntimeError(
                            f"{module_name} {optimization}: expected stdout "
                            f"{expected_stdout!r}, got {stdout.read_bytes()!r}")
                    # register.cpp prints a0 here; config.cpp routes it to the -p file.
                    exit_lines = [line for line in profile.read_bytes().splitlines()
                                  if line.startswith(b"Exit code:")]
                    if exit_lines != [b"Exit code: 0"]:
                        raise RuntimeError(
                            f"{module_name} {optimization}: expected exactly one "
                            f"REIMU 'Exit code: 0' line, got {exit_lines!r}")

    print("IR smoke tests passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(error, file=os.sys.stderr)
        raise SystemExit(1)
