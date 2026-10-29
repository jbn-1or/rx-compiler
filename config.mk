# Test commands: C++ rxcc checks semantics; Rust supplies local reference IR/assembly.
# See README-EN.md or README-ZH.md for command placeholders and stage contracts.

# Optional: build your compiler once before testing. Leave empty if prebuilt.
# {runtime} supplies shared assembly to each IR/codegen/optimization execution.
# Write shared runtime assembly there if your IR/CODEGEN output references it.
BUILD = cmake -S rxcc -B rxcc/build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build rxcc/build -j2 && : > {runtime}

# Only local IR/codegen/optimization reference tests need the Rust helper library.
# Keep the C/C++ judge's default BUILD independent of cargo and rustc.
# Example: make test BUILD_REFERENCE=true
BUILD_REFERENCE ?= false
ifeq ($(BUILD_REFERENCE),true)
BUILD := CARGO_PROFILE_RELEASE_LTO=true cargo build --quiet --locked --release \
    --manifest-path crates/rx/Cargo.toml \
    --target-dir target/reference \
    --target riscv32im-unknown-none-elf && \
    $(BUILD)
endif

# Required for semantic tests: exit 0 to accept {source}, 1 to reject it.
SEMANTIC = ./rxcc/build/rxcc --stage semantic {source}

# Required for IR tests: write RV32 LLVM IR to {output} (.ll).
IR = RUST_MIN_STACK=16777216 RX_SOURCE={source} $(REFERENCE_RUSTC) --crate-type=staticlib \
    --emit=llvm-ir={output},link={output}.a -C opt-level=2 -C lto=fat \
    crates/rx/src/entry.rs

# Required for codegen/optimization tests: write RV32IM assembly to {output} (.s).
CODEGEN = RUST_MIN_STACK=16777216 RX_SOURCE={source} $(REFERENCE_RUSTC) --crate-type=staticlib \
    --emit=asm={output},link={output}.a -C opt-level=2 -C lto=fat \
    -C llvm-args=-riscv-no-aliases crates/rx/src/entry.rs && \
    $(PYTHON) scripts/strip_asm_debug.py {output}

# Required alongside IR/CODEGEN: run the assembly with the BUILD runtime in REIMU.
RUN = xmake run -P vendor/REIMU reimu --memory=256M --stack=1M \
    -f {output},{runtime} -o {stdout} -p {profile} 1>&2

# Shared rustc options for the IR and CODEGEN reference commands above.
# v0 symbols avoid quoted section names that REIMU does not recognize.
REFERENCE_RUSTC = rustc \
    --edition=2021 \
    --target=riscv32im-unknown-none-elf \
    --crate-name=rx_test \
    -Awarnings -Aarithmetic_overflow \
    -C overflow-checks=off -C panic=abort -C symbol-mangling-version=v0 \
    --extern rx=target/reference/riscv32im-unknown-none-elf/release/librx.rlib \
    -L dependency=target/reference/riscv32im-unknown-none-elf/release/deps
