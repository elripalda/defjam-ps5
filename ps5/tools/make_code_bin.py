#!/usr/bin/env python3
"""[ps5 port] Write code.bin: a copy of the program's code segment, for the
MMIO trap's instruction decoder (the PS5 maps code execute-only, so the
running program cannot read its own instructions).

    make_code_bin.py <linked elf> <out code.bin>

Layout: "DJCB", 4 bytes of padding, then little-endian u64s: the segment's
link address, the link address of mmio_trap_code, the length; then the bytes.
"""
import struct
import subprocess
import sys

PF_X = 1


def main():
    elf, out = sys.argv[1], sys.argv[2]
    data = open(elf, 'rb').read()
    assert data[:4] == b'\x7fELF' and data[4] == 2, 'not a 64-bit ELF'
    phoff, = struct.unpack_from('<Q', data, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', data, 0x36)
    seg = None
    for i in range(phnum):
        p_type, p_flags, p_offset, p_vaddr, _, p_filesz, _, _ = \
            struct.unpack_from('<IIQQQQQQ', data, phoff + i * phentsize)
        if p_type == 1 and p_flags & PF_X:
            seg = (p_offset, p_vaddr, p_filesz)
            break
    assert seg, 'no executable segment'
    anchor = None
    for line in subprocess.run(['llvm-nm', '--defined-only', elf], capture_output=True,
                               text=True, check=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == 'mmio_trap_code':
            anchor = int(parts[0], 16)
    assert anchor is not None, 'mmio_trap_code not found'
    off, vaddr, size = seg
    with open(out, 'wb') as f:
        f.write(b'DJCB' + b'\0' * 4 + struct.pack('<QQQ', vaddr, anchor, size))
        f.write(data[off:off + size])
    print(f'code.bin: {size} bytes from 0x{vaddr:x}, anchor 0x{anchor:x}')


if __name__ == '__main__':
    main()
