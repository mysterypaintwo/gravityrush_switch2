"""Collects the distinct SSE/AVX instruction forms in an x86-64 ELF (the game's eboot.elf) for
box64e_simdcheck, which runs each one under Box64's dynarec test mode (dynarec vs interpreter).

Register forms are kept as they are; memory forms only with a plain base register (no rsp, no
rip) and a displacement within +-1 MiB, since the checker points every general register at the
middle of a large zeroed buffer. Nothing of the game is stored in the repository: the output is
written next to the build (out/).

Usage: python3 simd_forms.py <eboot.elf> <out.bin>
Output: u32 count, then per form: u8 length, bytes."""
import struct
import sys

import capstone
from capstone import x86

SIMD_GROUPS = {x86.X86_GRP_SSE1, x86.X86_GRP_SSE2, x86.X86_GRP_SSE3, x86.X86_GRP_SSSE3,
               x86.X86_GRP_SSE41, x86.X86_GRP_SSE42, x86.X86_GRP_AVX, x86.X86_GRP_AVX2,
               x86.X86_GRP_FMA, x86.X86_GRP_F16C}


def segments(d):
    phoff, = struct.unpack_from('<Q', d, 32)
    phnum, = struct.unpack_from('<H', d, 56)
    for i in range(phnum):
        t, fl, off, va, pa, fsz, msz, al = struct.unpack_from('<IIQQQQQQ', d, phoff + 56 * i)
        if t == 1 and fl & 1:  # PT_LOAD, executable
            yield va, d[off:off + fsz]


def usable(ins):
    for op in ins.operands:
        if op.type == x86.X86_OP_MEM:
            m = op.mem
            if m.base in (0, x86.X86_REG_RSP, x86.X86_REG_RIP, x86.X86_REG_ESP) or m.segment:
                return False
            if abs(m.disp) > (1 << 20) or m.index:  # every register holds the buffer address
                return False
    # Control flow and anything touching rsp is left out.
    return not ins.group(x86.X86_GRP_JUMP) and 'rsp' not in ins.op_str and 'esp' not in ins.op_str


def main():
    d = open(sys.argv[1], 'rb').read()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    md.skipdata = True
    forms = {}
    for va, code in segments(d):
        for ins in md.disasm(code, va):
            if ins.id == 0:  # skipped data
                continue
            if not ins.groups or not (set(ins.groups) & SIMD_GROUPS) or not usable(ins):
                continue
            # One instance per (mnemonic, operand kinds, immediate): enough to reach every
            # emitter path without thousands of duplicates differing only in registers.
            kinds = tuple((op.type, op.size) for op in ins.operands)
            imm = tuple(op.imm for op in ins.operands if op.type == x86.X86_OP_IMM)
            vexl = ins.bytes[0] in (0xc4, 0xc5)
            key = (ins.mnemonic, kinds, imm, vexl)
            forms.setdefault(key, bytes(ins.bytes))
    with open(sys.argv[2], 'wb') as f:
        f.write(struct.pack('<I', len(forms)))
        for b in forms.values():
            f.write(struct.pack('<B', len(b)) + b)
    print('%d instruction forms' % len(forms))


if __name__ == '__main__':
    main()
