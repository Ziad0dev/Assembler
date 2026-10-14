# Assembler
ASM to machine code. Assembler implementation in C.

`rvasm` is a two-pass assembler for **RISC-V RV32I** that outputs a flat little-endian binary.

- All 37 RV32I base instructions (plus `ecall`, `ebreak`, `fence`)
- Pseudo instructions: `nop li la mv not neg seqz snez j jr ret call tail beqz bnez bltz bgez blez bgtz bgt ble bgtu bleu`
- Labels, `offset(reg)` addressing, `x0`–`x31` and ABI register names, `+`/`-` expressions (`msg_end - msg`)
- Directives: `.word .half .byte .ascii .asciz/.string .align .zero/.space .equ/.set` (section directives are accepted and ignored)
- Range checks on every immediate/offset, with `file:line` errors

## Usage
```sh
make
./rvasm examples/fib.s                 # listing: address, encoding, source
./rvasm -o fib.bin examples/fib.s      # raw binary
./rvasm -f hex -o fib.hex examples/fib.s   # one word per line for Verilog $readmemh
```

## Tests
`make test` assembles `tests/*.s` with rvasm and with `clang --target=riscv32` and checks the bytes are identical, then checks that malformed input is rejected.

How it works: pass 1 walks the file and records the address of every label (pseudo instructions have a fixed size, so addresses are known before encoding); pass 2 encodes each line into R/I/S/B/U/J formats. Everything lives in `src/rvasm.c`.

The `Assembler/` directory is an earlier cmake-init C++ scaffold and is not used by the build.
