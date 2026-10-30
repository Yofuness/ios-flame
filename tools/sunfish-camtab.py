#!/usr/bin/env python3
"""Camera stage b: the IMX363's init + mode register tables, as plain text for `camid --stream FILE`.
Adapted from ios-redfin's tools/redfin-camtab.py (DenialOfGravity/ios-redfin, MIT).

  tools/sunfish-camtab.py [MODULE.bin] --list
  tools/sunfish-camtab.py [MODULE.bin] [--out FILE] [--mode N]

MODULE.bin: the camx sensor module file from the vendor image (default
research/notes/camx/com.qti.sensormodule.metric_imx363.bin). It is Google's file, and so is anything made from
it: both stay out of git (default output work/cam/imx363-modeN.txt; work/ is gitignored).
Default mode 8: 2016x1512 @60, 2x2 binned, 4 lanes at 304 Mb/s (the lowest lane rate of the 10 modes).
Front IMX355 (com.qti.sensormodule.primax_imx355.bin, output work/cam/imx355-modeN.txt): default mode 2,
1640x924 @30, 2x2 binned, 4 lanes at 360 Mb/s. Its file leaves the lane rate word at 0; the rate is then taken
from the mode's REQ_LINK_BIT_RATE_MBPS writes (0x0820/0x0821, all lanes together) divided by the lanes.

The file ("Parameter Parser V1.3.2", sunfish; redfin's lito2 files are V2.0.0, same layout): a symbol table of
0x48-byte records { id, name[32], kind, parent, offset, size } and a data area; offsets are into the data area.
Values are u64; arrays are (count, symbol id) pairs. A resolutionData element is 35 words here (37 on redfin):
[0] colour filter, [1],[2] stream configurations, [3] line length, [4] frame length, [8] pixel clock,
[9] lane rate, [10],[11] binning, [12] fps (double), [13] lanes, [20],[21] the mode's register settings.
A stream configuration is 8 words { vc, dt, x, y, width, height, bits, type }. A register setting is 10 words
{ slave present, slave, address, count, data, address type, data type, operation, delay present, delay }.

Output: one write a line, `0xADDR 0xVAL DELAY_US` (16-bit address, 8-bit value); `#` starts a comment.
"""
import argparse
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BIN = os.path.join(ROOT, 'research/notes/camx/com.qti.sensormodule.metric_imx363.bin')
DEFAULT_MODE = 8
DEFAULT_MODES = {'imx363': 8, 'imx355': 2}
RES_WORDS = 35


def load(path):
    d = open(path, 'rb').read()
    if not d.startswith(b'QTI Chromatix Header') or b'Parameter Parser V' not in d[:0x60]:
        sys.exit(f'{path}: not a CamX module file')
    toff, tsz = struct.unpack_from('<QQ', d, 0xa8)
    doff, dsz = struct.unpack_from('<QQ', d, 0xc0)
    syms, order = {}, []
    for o in range(toff, toff + tsz, 0x48):
        sid, name, kind, parent, off, size = struct.unpack_from('<Q32sQQQQ', d, o)
        syms[sid] = (name.split(b'\0')[0].decode('latin1'), off, size)
        order.append(sid)
    return d[doff:doff + dsz], syms, order


def main():
    ap = argparse.ArgumentParser(description='IMX363/IMX355 register tables for camid --stream')
    ap.add_argument('module', nargs='?', default=DEFAULT_BIN)
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--out')
    ap.add_argument('--mode', type=int)
    a = ap.parse_args()
    sensor = next((n for n in DEFAULT_MODES if n in os.path.basename(a.module)), 'imx363')
    if a.mode is None:
        a.mode = DEFAULT_MODES[sensor]
    data, syms, order = load(a.module)

    def words(sid):
        _, off, size = syms[sid]
        return struct.unpack_from('<%dQ' % (size // 8), data, off)

    def regsetting(sid):
        out = []
        _, off, size = syms[sid]
        for e in range(off, off + size, 80):
            _, _, addr, count, dsid, atype, dtype, op, has_delay, delay_sid = struct.unpack_from('<10Q', data, e)
            vals = words(dsid)[:max(count, 1)] if dsid in syms else (0,)
            delay = words(delay_sid)[0] if has_delay and delay_sid in syms and syms[delay_sid][2] >= 8 else 0
            for i, v in enumerate(vals):
                # a burst (count > 1) writes the next addresses
                out.append((addr + i * dtype, v, atype, dtype, op, delay if i == len(vals) - 1 else 0))
        return out

    rsid = next(s for s in order if syms[s][0] == 'resolutionData')
    raw = words(rsid)
    modes = []
    for k in range(len(raw) // RES_WORDS):
        w = raw[RES_WORDS * k:RES_WORDS * k + RES_WORDS]
        fps = struct.unpack('<d', struct.pack('<Q', w[12]))[0]
        sc = words(w[2])[:8]
        modes.append(dict(w=sc[4], h=sc[5], dt=sc[1], lanes=w[13], fps=fps, line=w[3], frame=w[4], pclk=w[8],
                          lrate=w[9], bin=(w[10], w[11]), regs=regsetting(w[21])))
        m = modes[-1]
        if not m['lrate'] and m['lanes']:
            r = {x[0]: x[1] for x in m['regs']}
            if 0x820 in r and 0x821 in r:
                m['lrate'] = ((r[0x820] << 8) | r[0x821]) * 1000000 // m['lanes']
    tables, after, used = [], '', {}
    roles = {'regToRealGain': ['on', 'off', 'holdon', 'holdoff'], 'initSettings': ['init']}
    for sid in order:
        name = syms[sid][0]
        if name != 'regSetting':
            if name not in ('slaveAddr', 'registerData', 'delayUs', '[0]'):
                after = name
            continue
        n = used.get(after, 0)
        used[after] = n + 1
        if n < len(roles.get(after, [])):
            tables.append((roles[after][n], regsetting(sid)))
    tab = dict(tables)

    if a.list:
        print(f'{a.module}: {len(modes)} modes')
        for k, m in enumerate(modes):
            print(f'  mode {k}: {m["w"]}x{m["h"]} @{m["fps"]:.2f} dt {m["dt"]:#x}, {m["lanes"]} lanes, '
                  f'{m["lrate"] / 1e6:.0f} Mb/s/lane, bin {m["bin"][0]}x{m["bin"][1]}, line {m["line"]}, '
                  f'frame {m["frame"]}, {len(m["regs"])} writes' + ('  (default)' if k == a.mode else ''))
        for name, t in tables:
            print(f'  {name}: {len(t)} writes: ' + ' '.join(f'{x[0]:x}={x[1]:x}' + (f'+{x[5]}us' if x[5] else '')
                                                         for x in t[:8]) + (' ...' if len(t) > 8 else ''))
        if not a.out:
            return
    if not 0 <= a.mode < len(modes):
        sys.exit(f'mode {a.mode}: only 0-{len(modes) - 1}')
    if 'init' not in tab:
        sys.exit('no init table found')
    m = modes[a.mode]
    out = a.out or os.path.join(ROOT, 'work/cam/%s-mode%d.txt' % (sensor, a.mode))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    lines = [f'# {sensor.upper()} from {os.path.basename(a.module)} (vendor data: keep out of git)',
             f'# init: {len(tab["init"])} writes']
    regs = list(tab['init'])
    lines += [f'0x{x[0]:04x} 0x{x[1]:02x} {x[5]}' for x in tab['init']]
    lines.append(f'# mode {a.mode}: {m["w"]}x{m["h"]} @{m["fps"]:.2f}, {m["lanes"]} lanes, '
                 f'{m["lrate"] / 1e6:.0f} Mb/s/lane: {len(m["regs"])} writes')
    lines += [f'0x{x[0]:04x} 0x{x[1]:02x} {x[5]}' for x in m['regs']]
    regs += m['regs']
    bad = [x for x in regs if x[0] > 0xffff or x[1] > 0xff or x[2] != 2 or x[3] != 1 or x[4] != 0]
    if bad:
        sys.exit(f'{len(bad)} writes are not 16-bit address / 8-bit value writes, e.g. {bad[0]}')
    open(out, 'w').write('\n'.join(lines) + '\n')
    print(f'{out}: {len(regs)} writes ({len(tab["init"])} init + {len(m["regs"])} mode {a.mode})')


main()
