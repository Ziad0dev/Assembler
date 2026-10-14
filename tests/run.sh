#!/usr/bin/env bash
# Differential test: assemble tests/*.s with rvasm and with clang's RISC-V
# assembler, and compare the .text bytes.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

extract_text() { # extract the .text section of a 32-bit little endian ELF
  python3 - "$1" "$2" <<'PY'
import struct, sys
data = open(sys.argv[1], 'rb').read()
shoff, = struct.unpack_from('<I', data, 0x20)
shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x2e)
def sh(i): return struct.unpack_from('<IIIIIIIIII', data, shoff + i * shentsize)
strtab = sh(shstrndx)
for i in range(shnum):
    name, _, _, _, off, size, *_ = sh(i)
    end = data.index(b'\0', strtab[4] + name)
    if data[strtab[4] + name:end] == b'.text':
        open(sys.argv[2], 'wb').write(data[off:off + size])
PY
}

status=0
for src in tests/*.s; do
  name=$(basename "$src" .s)
  ./rvasm -o "$tmp/$name.bin" "$src"
  if ! command -v clang >/dev/null; then
    echo "SKIP $name (no clang)"; continue
  fi
  clang --target=riscv32 -march=rv32i -mno-relax -c -x assembler "$src" -o "$tmp/$name.o" 2>/dev/null
  extract_text "$tmp/$name.o" "$tmp/$name.ref"
  if cmp -s "$tmp/$name.bin" "$tmp/$name.ref"; then
    echo "PASS $name ($(stat -c %s "$tmp/$name.bin") bytes match clang)"
  else
    echo "FAIL $name"
    diff <(od -An -tx4 -w4 -v "$tmp/$name.bin") <(od -An -tx4 -w4 -v "$tmp/$name.ref") | head -20 || true
    status=1
  fi
done

# Error handling: these must be rejected
for bad in 'addi a0, a0, 4096' 'add a0, a1' 'lw a0, 0(q9)' 'j nowhere' 'frob a0'; do
  if echo "$bad" | ./rvasm - >/dev/null 2>&1; then
    echo "FAIL accepted bad input: $bad"; status=1
  fi
done
[ $status = 0 ] && echo "error cases rejected"
exit $status
