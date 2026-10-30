#!/usr/bin/env python3
"""Join two half captures of the IMX363 into one frame (camera stage d, cam18).

The IFE-lite write master interleaves its writes by address bit 10 across SMMU SIDs 0x8c0 (even 1 KB blocks) and
0xcc0 (odd blocks), and only 0x8c0 can pass without resetting the phone. So:
  A = `camrx capture ... --bypass --sid 0x8c0`                      frame at the buffer: its even 1 KB blocks are valid
  B = `camrx capture ... --bypass --sid 0x8c0 --addr-offset 0x400`  frame 1 KB up: its odd 1 KB blocks are valid
This takes the even blocks from A and the odd ones from B (a still scene is assumed) and prints how many blocks of
each half were empty (all zero), which should be ~0.
  tools/sunfish-cam-merge.py A.raw B.raw OUT.raw
"""
import sys

BLOCK = 1024


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    a = open(sys.argv[1], 'rb').read()
    b = open(sys.argv[2], 'rb').read()
    if len(a) != len(b):
        sys.exit(f'size mismatch: {len(a)} vs {len(b)}')
    out = bytearray(len(a))
    empty = [0, 0]
    for o in range(0, len(a), BLOCK):
        odd = (o // BLOCK) & 1
        src = b if odd else a
        blk = src[o:o + BLOCK]
        out[o:o + BLOCK] = blk
        if not any(blk):
            empty[odd] += 1
    open(sys.argv[3], 'wb').write(out)
    n = (len(a) + BLOCK - 1) // BLOCK
    print(f'{sys.argv[3]}: {len(out)} B, {n} blocks of 1 KB; empty: even (from A) {empty[0]}, odd (from B) {empty[1]}')


if __name__ == '__main__':
    main()
