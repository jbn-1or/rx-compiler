#!/usr/bin/env python3
"""Validate the P2-only IR baseline with external LLVM 22 tools."""

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
    parser.add_argument("--model-test", default="rxcc/build/ir_model_smoke")
    parser.add_argument("--llvm-as", default="llvm-as-22")
    parser.add_argument("--opt", default="opt-22")
    parser.add_argument("--clang", default="clang-22")
    parser.add_argument("--reimu", action="store_true",
                        help="also run P2 modules with RV32 Clang and REIMU")
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
        modules = []
        for name, source_text in (
            ("empty-main", "fn main() {}"),
            ("explicit-unit", "fn main() -> () {}"),
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
            modules.append((name, output))

        for name, command in (
            ("model-smoke", [args.model_test]),
            ("model-values", [args.model_test, "--emit-model"]),
        ):
            output = temp / f"{name}.ll"
            output.write_text(run(command).stdout, encoding="utf-8")
            modules.append((name, output))
        for name, output in modules:
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

        # All are semantically valid. Reject P3+ in the whole crate, including unused bodies.
        for index, contents in enumerate((
            "fn main() { let x = 1; }",
            "fn main() { println_i32(42); }",
            "fn main() { main(); }",
            "fn main() { return; }",
            "fn main() { (); }",
            "fn main() { true; }",
            "fn main() { 1 + 2; }",
            "fn main() { { } }",
            "fn main() { let x = return; }",
            "fn main() {} fn unused() { println_i32(99); }",
            "fn main() {} fn unused(value: i32) {}",
            "fn main() {} fn unused() -> i32 { 7 }",
            "struct Data {} impl Data { fn unused(&self) {} } fn main() {}",
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
            "fn main() {} fn unused() { let x: i32 = true; }",
            "fn main() { return; let x = missing; }",
        )):
            invalid_source = temp / f"semantic-error-{index}.rx"
            invalid_source.write_text(contents, encoding="utf-8")
            semantic = run([args.compiler, "--stage", "semantic", str(invalid_source)], expected=1)
            output.write_bytes(b"preserve on semantic failure\n")
            result = run([args.compiler, "--stage", "ir", str(invalid_source), "-o", str(output)], expected=1)
            if not semantic.stderr.strip() or result.stderr != semantic.stderr:
                raise RuntimeError("IR must perform complete semantic checks before P2 lowering")
            if output.read_bytes() != b"preserve on semantic failure\n":
                raise RuntimeError("semantic failure changed the previous output")
        if Path("/dev/full").exists():
            run([args.compiler, "--stage", "ir", str(source), "-o", "/dev/full"], expected=2)

        if args.reimu:
            for name, ir_path in modules:
                for optimization in ("O0", "O2"):
                    assembly = temp / f"{name}-{optimization}.s"
                    stdout = temp / f"{name}-{optimization}.out"
                    profile = temp / f"{name}-{optimization}.profile"
                    run([args.clang, "--target=riscv32-unknown-elf", "-march=rv32im", "-mabi=ilp32",
                         "-mllvm", "-riscv-no-aliases", f"-{optimization}", "-S", str(ir_path), "-o", str(assembly)])
                    run([args.clang, "--target=riscv32-unknown-elf", "-march=rv32im", "-mabi=ilp32",
                         "-c", str(assembly), "-o", str(temp / f"{name}-{optimization}.o")])
                    run(["xmake", "run", "-P", "vendor/REIMU", "reimu", "--memory=256M", "--stack=1M",
                         "-f", str(assembly), "-o", str(stdout), "-p", str(profile)])
                    exits = [line for line in profile.read_bytes().splitlines() if line.startswith(b"Exit code:")]
                    if stdout.read_bytes() != b"" or exits != [b"Exit code: 0"]:
                        raise RuntimeError(f"{name} {optimization}: expected empty stdout and exit zero")
    print(f"P2 IR tests passed: {len(modules)} modules verified" +
          (f", {2 * len(modules)} RV32 O0/O2 executions" if args.reimu else ""))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(error, file=os.sys.stderr)
        raise SystemExit(1)
