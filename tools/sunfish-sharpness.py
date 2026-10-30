#!/usr/bin/env python3
"""Camera af2: compare lens positions without viewing the frames.

  tools/sunfish-sharpness.py [--green-even] FRAME.raw...

FRAME: bare 2016x1512 RAW10 (MIPI packed, stride 2528), as from `camclient -o`. Prints for each frame the variance of
the green channel's Laplacian over the central 50% of the width and of the height (higher = sharper), and the mean
level there, since the variance also grows with brightness: compare frames of the same exposure. Greens are at odd
row + column (RGGB or BGGR; --green-even for GRBG or GBRG). The Laplacian uses the four diagonal neighbours, all green.
"""
import argparse
import sys

import numpy as np

W, H, STRIDE = 2016, 1512, 2528


def unpack(path):
    data = np.fromfile(path, dtype=np.uint8)
    if data.size < STRIDE * H:
        raise ValueError(f"{data.size} B, need {STRIDE * H}")
    rows = data[:STRIDE * H].reshape(H, STRIDE)[:, :W // 4 * 5].astype(np.uint16)
    groups = rows.reshape(H, W // 4, 5)
    low = groups[:, :, 4]
    px = np.empty((H, W), dtype=np.float64)
    for i in range(4):
        px[:, i::4] = groups[:, :, i] << 2 | (low >> (2 * i)) & 3
    return px


def sharpness(px, green_odd=True):
    h, w = px.shape
    y0, y1, x0, x1 = h // 4, 3 * h // 4, w // 4, 3 * w // 4
    c = px[y0:y1, x0:x1]
    lap = 4 * c - px[y0 - 1:y1 - 1, x0 - 1:x1 - 1] - px[y0 - 1:y1 - 1, x0 + 1:x1 + 1] \
        - px[y0 + 1:y1 + 1, x0 - 1:x1 - 1] - px[y0 + 1:y1 + 1, x0 + 1:x1 + 1]
    yy, xx = np.indices(c.shape)
    green = (yy + y0 + xx + x0) % 2 == (1 if green_odd else 0)
    return float(lap[green].var()), float(c[green].mean())


def main():
    ap = argparse.ArgumentParser(description="Sharpness of RAW10 frames (green Laplacian variance, central 50%)")
    ap.add_argument("--green-even", action="store_true", help="greens at even row + column")
    ap.add_argument("frames", nargs="+")
    args = ap.parse_args()
    rc = 0
    for path in args.frames:
        try:
            var, mean = sharpness(unpack(path), not args.green_even)
        except (OSError, ValueError) as e:
            print(f"{path}: {e}", file=sys.stderr)
            rc = 1
            continue
        print(f"{path}: sharpness {var:.1f} mean {mean:.1f}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
