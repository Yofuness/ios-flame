#!/usr/bin/env python3
"""Camera stage d: a MIPI RAW10 frame from `camrx capture` -> PNGs (stdlib only).

  python3 -I tools/sunfish-raw2png.py FRAME.raw [--width 2016] [--height 1512] [--stride 2520] [--bayer RGGB]
                                     [--gamma 2.2] [--out PREFIX]

RAW10 packed: 5 bytes = 4 pixels, bytes 0-3 the 8 high bits, byte 4 the 4 x 2 low bits (only the high 8 bits are
used here). Writes PREFIX-gray.png (full size) and PREFIX-color.png (half size: one RGB pixel per 2x2 Bayer cell,
the two greens averaged). Auto-levels: 0.5 / 99.5 percentile per channel, then gamma.
"""
import argparse
import struct
import sys
import zlib

ORDERS = {  # colour at (row, col) in the 2x2 cell
    "RGGB": ("R", "G", "G", "B"),
    "BGGR": ("B", "G", "G", "R"),
    "GRBG": ("G", "R", "B", "G"),
    "GBRG": ("G", "B", "R", "G"),
}


def png(path, w, h, rows, channels):
    raw = b"".join(b"\0" + bytes(r) for r in rows)
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    hdr = struct.pack(">IIBBBBB", w, h, 8, 2 if channels == 3 else 0, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", hdr) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def lut(data, gamma):
    """auto-levels + gamma as a 256-entry table for bytes.translate"""
    hist = [data.count(v) for v in range(256)] if isinstance(data, bytes) else [bytes(data).count(v) for v in range(256)]
    n, acc, lo, hi = sum(hist), 0, 0, 255
    for v in range(256):
        acc += hist[v]
        if acc <= n * 0.005:
            lo = v
        if acc < n * 0.995:
            hi = v + 1
    hi = max(hi, lo + 1)
    t = bytes(min(255, max(0, round(255 * (min(max(v - lo, 0) / (hi - lo), 1.0) ** (1 / gamma))))) for v in range(256))
    return t, lo, hi


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("raw")
    ap.add_argument("--width", type=int, default=2016)
    ap.add_argument("--height", type=int, default=1512)
    ap.add_argument("--stride", type=int, default=0, help="bytes per line (default width*10/8)")
    ap.add_argument("--bayer", default="RGGB", choices=sorted(ORDERS))
    ap.add_argument("--gamma", type=float, default=2.2)
    ap.add_argument("--out", default=None, help="output prefix (default: the input without .raw)")
    a = ap.parse_args()
    w, h = a.width & ~3, a.height & ~1
    stride = a.stride or a.width * 10 // 8
    data = open(a.raw, "rb").read()
    if len(data) < stride * (h - 1) + w * 5 // 4:
        h = len(data) // stride & ~1
        print(f"raw2png: file has {len(data)} B, only {h} lines", file=sys.stderr)
    # the 8 high bits of every pixel, line by line
    lines = []
    for y in range(h):
        line = data[y * stride:y * stride + w * 5 // 4]
        hi = bytearray(w)
        for k in range(4):
            hi[k::4] = line[k::5]
        lines.append(bytes(hi))
    prefix = a.out or (a.raw[:-4] if a.raw.endswith(".raw") else a.raw)
    allpix = b"".join(lines)
    t, lo, hi_ = lut(allpix, a.gamma)
    png(prefix + "-gray.png", w, h, [l.translate(t) for l in lines], 1)
    print(f"raw2png: {w}x{h} stride {stride}: gray levels {lo}..{hi_} -> {prefix}-gray.png")
    # half-size colour
    order = ORDERS[a.bayer]
    planes = {"R": [], "G": [], "B": []}
    for y in range(0, h, 2):
        p = [lines[y][0::2], lines[y][1::2], lines[y + 1][0::2], lines[y + 1][1::2]]
        gs = [p[i] for i in range(4) if order[i] == "G"]
        planes["R"].append(p[order.index("R")])
        planes["B"].append(p[order.index("B")])
        planes["G"].append(bytes((x + y2 + 1) >> 1 for x, y2 in zip(gs[0], gs[1])))
    luts = {c: lut(b"".join(planes[c]), a.gamma) for c in "RGB"}
    rows = []
    for i in range(h // 2):
        row = bytearray(3 * (w // 2))
        for k, c in enumerate("RGB"):
            row[k::3] = planes[c][i].translate(luts[c][0])
        rows.append(row)
    png(prefix + "-color.png", w // 2, h // 2, rows, 3)
    print(f"raw2png: colour {w // 2}x{h // 2}, Bayer {a.bayer} (a GUESS: IMX363 order and any mirror/flip in the "
          f"mode table not verified; try --bayer if colours look swapped), levels "
          + " ".join(f"{c} {luts[c][1]}..{luts[c][2]}" for c in "RGB") + f" -> {prefix}-color.png")


if __name__ == "__main__":
    main()
