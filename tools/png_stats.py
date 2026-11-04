#!/usr/bin/env python3
"""Pixel statistics of an 8-bit, non-interlaced PNG (standard library only).

Used by tests/gui/screenshot_smoke.sh: a screenshot must not be blank. Prints a JSON object
{"width":W,"height":H,"unique_colors":N,"nonbackground_fraction":F,"stddev":S}.
usage: png_stats.py FILE.png
"""
import json
import struct
import sys
import zlib


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, header = 8, [], None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    w, h, depth, ctype, _comp, _filt, interlace = header
    if depth != 8 or interlace != 0 or ctype not in (2, 6):
        raise ValueError("unsupported PNG (need 8-bit RGB/RGBA, not interlaced): depth=%d type=%d interlace=%d" % (depth, ctype, interlace))
    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(b"".join(idat))
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    p = 0
    for _ in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        rows.append(line)
        prev = line
    return w, h, bpp, rows


def main(path):
    w, h, bpp, rows = read_png(path)
    counts = {}
    total = w * h
    for line in rows:
        for x in range(0, w * bpp, bpp):
            px = bytes(line[x:x + 3])
            counts[px] = counts.get(px, 0) + 1
    bg = max(counts, key=counts.get)
    mean = [sum(k[c] * v for k, v in counts.items()) / total for c in range(3)]
    var = sum(((k[c] - mean[c]) ** 2) * v for k, v in counts.items() for c in range(3)) / (3 * total)
    print(json.dumps({"width": w, "height": h, "unique_colors": len(counts),
                      "nonbackground_fraction": 1.0 - counts[bg] / total, "stddev": var ** 0.5}))


if __name__ == "__main__":
    main(sys.argv[1])
