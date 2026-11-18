"""Converts the GPU library's raw frame dumps (GOW3_FRAME_DUMP_TRIGGER: name_NNN_WxH.raw, 32-bit
pixels) to PNG, and prints a few pixel statistics (an all-black frame shows max 0).

Usage: python3 raw_to_png.py <dump.raw>... [--bgra]"""
import re
import struct
import sys
import zlib


def png(path, w, h, rgba):
    rows = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


def main():
    bgra = "--bgra" in sys.argv
    for raw in [a for a in sys.argv[1:] if not a.startswith("--")]:
        m = re.search(r"_(\d+)x(\d+)\.raw$", raw)
        w, h = int(m.group(1)), int(m.group(2))
        data = bytearray(open(raw, "rb").read()[:w * h * 4])
        if bgra:
            data[0::4], data[2::4] = data[2::4], data[0::4]
        data[3::4] = b"\xff" * (w * h)  # opaque (alpha is often 0 in display buffers)
        rgb_max = max(max(data[0::4]), max(data[1::4]), max(data[2::4]))
        nonzero = sum(1 for i in range(0, len(data), 4 * 97) if data[i] or data[i + 1] or data[i + 2])
        out = raw[:-4] + ".png"
        png(out, w, h, bytes(data))
        print("%s: %dx%d, max channel %d, sampled non-black pixels %d -> %s" % (raw, w, h, rgb_max, nonzero, out))


if __name__ == "__main__":
    main()
