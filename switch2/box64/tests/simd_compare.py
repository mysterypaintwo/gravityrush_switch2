"""Compares two box64e_simdcheck result files (dynarec and interpreter) and prints the forms
whose results differ, disassembled, with the differing registers.

Usage: python3 simd_compare.py <forms.bin> <dynarec.bin> <interpreter.bin>"""
import struct
import sys

import capstone

RESULT = 16 * 16 + 16 * 8 + 8 + 8  # xmm, gpr, flags, memory hash
GPR = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi',
       'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15']


def forms(path):
    d = open(path, 'rb').read()
    n, = struct.unpack_from('<I', d, 0)
    off = 4
    for _ in range(n):
        ln = d[off]
        yield d[off + 1:off + 1 + ln]
        off += 1 + ln


def lanes(b):
    return ' '.join('%08x' % v for v in struct.unpack('<4I', b))


def main():
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    a = open(sys.argv[2], 'rb').read()
    b = open(sys.argv[3], 'rb').read()
    bad = 0
    for k, code in enumerate(forms(sys.argv[1])):
        ra, rb = a[k * RESULT:(k + 1) * RESULT], b[k * RESULT:(k + 1) * RESULT]
        if ra == rb:
            continue
        bad += 1
        ins = next(md.disasm(code, 0), None)
        print('%-50s (%s)' % ('%s %s' % (ins.mnemonic, ins.op_str) if ins else '?', code.hex(' ')))
        for r in range(16):
            xa, xb = ra[16 * r:16 * r + 16], rb[16 * r:16 * r + 16]
            if xa != xb:
                print('    xmm%-2d dynarec %s | interpreter %s' % (r, lanes(xa), lanes(xb)))
        for t in range(16):
            ga, = struct.unpack_from('<Q', ra, 256 + 8 * t)
            gb, = struct.unpack_from('<Q', rb, 256 + 8 * t)
            if ga != gb:
                print('    %-5s dynarec %016x | interpreter %016x' % (GPR[t], ga, gb))
        fa, = struct.unpack_from('<Q', ra, 384)
        fb, = struct.unpack_from('<Q', rb, 384)
        if fa != fb:
            print('    flags dynarec %03x | interpreter %03x' % (fa, fb))
        ha, = struct.unpack_from('<Q', ra, 392)
        hb, = struct.unpack_from('<Q', rb, 392)
        if ha != hb:
            print('    memory written differently')
    print('%d forms differ' % bad)


if __name__ == '__main__':
    main()
