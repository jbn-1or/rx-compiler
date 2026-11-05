#!/usr/bin/env python3
"""Run the local C++ IR smoke tests without requiring tracked test fixtures."""

import argparse
import os
from pathlib import Path
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
        unsupported_source.write_text("fn main() { println_i32(1); }\n", encoding="utf-8")
        output.write_text("preserve on generation failure\n", encoding="utf-8")
        run([compiler, "--stage", "ir", str(unsupported_source), "-o", str(output)],
            expected=1)
        if output.read_text(encoding="utf-8") != "preserve on generation failure\n":
            raise RuntimeError("unsupported lowering modified the output file")

        if Path("/dev/full").exists():
            run([compiler, "--stage", "ir", str(source), "-o", "/dev/full"], expected=2)

        if args.reimu:
            for optimization in ("O0", "O2"):
                assembly = temp / f"empty_main-{optimization}.s"
                stdout = temp / f"stdout-{optimization}"
                profile = temp / f"profile-{optimization}"
                run([args.clang, "--target=riscv32-unknown-elf", "-march=rv32im",
                     "-mabi=ilp32", f"-{optimization}", "-S", str(valid_ir),
                     "-o", str(assembly)])
                run(["xmake", "run", "-P", "vendor/REIMU", "reimu", "--memory=256M",
                     "--stack=1M", "-f", str(assembly), "-o", str(stdout),
                     "-p", str(profile)])
                if stdout.read_bytes() != b"":
                    raise RuntimeError(f"{optimization}: empty main unexpectedly produced stdout")
                # register.cpp prints a0 here; config.cpp routes it to the -p file.
                exit_lines = [line for line in profile.read_bytes().splitlines()
                              if line.startswith(b"Exit code:")]
                if exit_lines != [b"Exit code: 0"]:
                    raise RuntimeError(
                        f"{optimization}: expected exactly one REIMU 'Exit code: 0' line, "
                        f"got {exit_lines!r}")

    print("IR smoke tests passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(error, file=os.sys.stderr)
        raise SystemExit(1)
