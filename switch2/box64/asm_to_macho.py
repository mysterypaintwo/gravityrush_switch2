"""Rewrites one of Box64's ELF arm64 .S files for Apple's assembler (Mach-O).

Mach-O C symbols carry a leading underscore, and page-relative relocations are written
sym@PAGE / sym@PAGEOFF instead of sym / :lo12:sym. Every global defined in the file, every
.extern and every bl/b target outside the file get the underscore.

Usage: python3 asm_to_macho.py <in.S> <out.S>
"""
import re
import sys


def convert(text):
    symbols = set(re.findall(r"^\s*\.(?:global|globl|extern)\s+([A-Za-z_][A-Za-z0-9_]*)", text, re.M))
    local_labels = set(re.findall(r"^([A-Za-z_][A-Za-z0-9_]*):", text, re.M))
    # branch targets that are not labels of this file (LinkNext, ...)
    for target in re.findall(r"^\s*bl?\s+([A-Za-z_][A-Za-z0-9_]*)\s*$", text, re.M):
        if target not in local_labels:
            symbols.add(target)
    out = []
    for line in text.splitlines():
        if re.match(r"^\s*\.(type|size)\b", line):
            continue
        line = re.sub(r"#:lo12:([A-Za-z_][A-Za-z0-9_]*)", r"\1@PAGEOFF", line)
        line = re.sub(r"^(\s*adrp\s+\w+\s*,\s*)([A-Za-z_][A-Za-z0-9_]*)\s*$", r"\1\2@PAGE", line)
        for sym in symbols:
            line = re.sub(r"(?<![A-Za-z0-9_])" + sym + r"(?![A-Za-z0-9_])", "_" + sym, line)
        out.append(line)
    return "\n".join(out) + "\n"


def main():
    src, dst = sys.argv[1], sys.argv[2]
    with open(src) as f:
        text = f.read()
    with open(dst, "w") as f:
        f.write(convert(text))


if __name__ == "__main__":
    main()
