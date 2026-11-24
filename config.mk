# Test commands: C++ rxcc checks semantics and generates IR; Rust supplies reference assembly.
# See README-EN.md or README-ZH.md for command placeholders and stage contracts.

# Only reference codegen/optimization tests need the Rust helper library.
# Example: make test STAGE=codegen BUILD_REFERENCE=true
BUILD_REFERENCE ?= false
ifeq ($(BUILD_REFERENCE),true)
REFERENCE_BUILD = CARGO_PROFILE_RELEASE_LTO=true cargo build --quiet --locked --release \
    --manifest-path crates/rx/Cargo.toml \
    --target-dir target/reference \
    --target riscv32im-unknown-none-elf &&
endif

# An unrestricted run includes IR; semantic and reference-only runs need no C runtime.
BUILD_IR_RUNTIME = "$(CLANG)" --target=riscv32-unknown-elf -march=rv32im -mabi=ilp32 \
    -mllvm -riscv-no-aliases -O2 -ffreestanding -S rxcc/runtime/runtime.c -o {runtime}
BUILD = $(REFERENCE_BUILD) cmake -S rxcc -B rxcc/build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build rxcc/build -j2 && \
    $(if $(filter-out ir,$(STAGE)),: > {runtime},$(BUILD_IR_RUNTIME))

# Required for semantic tests: exit 0 to accept {source}, 1 to reject it.
SEMANTIC = ./rxcc/build/rxcc --stage semantic {source}

# Required for IR tests: write RV32 LLVM IR to {output} (.ll).
IR = ./rxcc/build/rxcc --stage ir {source} -o {output}

# Required for codegen/optimization tests: write RV32IM assembly to {output} (.s).
CODEGEN = RUST_MIN_STACK=16777216 RX_SOURCE={source} $(REFERENCE_RUSTC) --crate-type=staticlib \
    --emit=asm={output},link={output}.a -C opt-level=2 -C lto=fat \
    -C llvm-args=-riscv-no-aliases crates/rx/src/entry.rs && \
    $(PYTHON) scripts/strip_asm_debug.py {output}

# Required alongside IR/CODEGEN: run the assembly with the BUILD runtime in REIMU.
RUN = xmake run -P vendor/REIMU reimu --memory=256M --stack=1M \
    -f {output},{runtime} -o {stdout} -p {profile} 1>&2

# rustc options for the CODEGEN reference command above.
# v0 symbols avoid quoted section names that REIMU does not recognize.
REFERENCE_RUSTC = rustc \
    --edition=2021 \
    --target=riscv32im-unknown-none-elf \
    --crate-name=rx_test \
    -Awarnings -Aarithmetic_overflow \
    -C overflow-checks=off -C panic=abort -C symbol-mangling-version=v0 \
    --extern rx=target/reference/riscv32im-unknown-none-elf/release/librx.rlib \
    -L dependency=target/reference/riscv32im-unknown-none-elf/release/deps
