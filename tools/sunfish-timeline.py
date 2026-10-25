#!/usr/bin/env python3
"""Per-frame timeline from the kernel's "SWEET TL:" lines (xnu 0163, boot-arg sweettrace=1).

  tools/sunfish-timeline.py [--ctl FILE --pid N] LOG [START_MARKER [END_MARKER]]

Reads the live log (default ../logs/usb-live.log); with markers, only the lines between
"=== START" and "=== END" (as written by tools/sunfish-fpsbench.sh). Prints:
  - the swap, vsync-tick and present rates
  - frame intervals between swaps (the 60 Hz metric): share of intervals <= 17 / 34 / 50 ms and p50/p95/max
  - per-submit GPU time by owner (pid 0 = kernel presents)
  - gaps along the chain, per swap: last backboardd GPU end -> swap, swap -> present begin,
    present begin -> present end (tick thread blocked), present submit -> GPU start/end
--ctl FILE --pid N: also read "CTL pid idx C D F0 F1 Q H" lines (hex mach ticks, SWEET_CTL=FILE in the Metal client; see
NativeBridgeMetalFrontend.mm) for backboardd's pid N and split GPU end -> swap into stages (see ctl_report).
Records are ev,t,a,b,c in hex: 1 swap 2 tick 3 notified 4 present begin 5 present end 6 submit 7 gpu span 8 clock.
Mach ticks and the CP_ALWAYS_ON counter both run at 19.2 MHz here (ratio measured 1.0000; 718M ticks in a 37 s bench); the clock pairs give the offset.
"""
import re, sys, bisect

MACH_HZ = 19_200_000.0   # mach_absolute_time on this kernel (CP always-on counter: same rate, ratio measured 1.0000)
CP_HZ = 19_200_000.0

def load(path, start, end):
    on = start is None
    recs, clocks = [], []
    for line in open(path, errors='replace'):
        if line.startswith('=== '):
            if start and ('=== ' + start) in line:
                on = True
            elif end and ('=== ' + end) in line:
                on = False
            continue
        if not on or 'SWEET TL:' not in line:
            continue
        body = line.split('SWEET TL:', 1)[1].strip()
        for tok in body.split():
            parts = tok.split(',')
            if len(parts) != 5:
                continue
            try:
                ev, t, a, b, c = (int(x, 16) for x in parts)
            except ValueError:
                continue
            if ev == 8:
                clocks.append((t, c))
            else:
                recs.append((ev, t, a, b, c))
    return recs, clocks

def pct(v, p):
    return v[min(len(v) - 1, int(len(v) * p))] if v else float('nan')

def stats(name, xs, unit='ms'):
    xs = sorted(xs)
    if not xs:
        print(f'  {name:42s} none')
        return
    print(f'  {name:42s} n={len(xs):5d}  p50 {pct(xs,.5):7.2f}  p95 {pct(xs,.95):7.2f}  max {xs[-1]:7.2f} {unit}')

def load_ctl(path, pid):
    out = []
    for line in open(path, errors='replace'):
        p = line.split()
        if len(p) != 9 or p[0] != 'CTL':
            continue
        try:
            if int(p[1]) != pid:
                continue
            out.append(tuple(int(x, 16) for x in p[2:]))   # idx C D F0 F1 Q H
        except ValueError:
            continue
    return sorted(out, key=lambda r: r[6])

def ctl_report(ctl, sw, spans, pid, ms):
    """Per swap: latest backboardd command buffer with H before the swap; GPU end = latest pid span end before its D."""
    print(f'completion chain (--ctl, pid {pid}), per swap:')
    hs = [r[6] for r in ctl]
    ends = sorted((e, s_) for s_, e, f, p in spans if p == pid)
    endt = [x[0] for x in ends]
    st = {k: [] for k in ('gend_d', 'd_f0', 'f0_f1', 'f1_q', 'q_h', 'h_swap', 'c_gstart', 'gend_swap')}
    for t in sw:
        i = bisect.bisect_right(hs, t) - 1
        if i < 0:
            continue
        idx, c, d, f0, f1, q, h = ctl[i]
        if ms(t - h) > 100 or not (c and d and f0 and f1 and q):
            continue
        j = bisect.bisect_right(endt, d) - 1
        if j >= 0 and ms(d - endt[j]) < 100:
            st['gend_d'].append(ms(d - endt[j]))
            st['gend_swap'].append(ms(t - endt[j]))
            st['c_gstart'].append(ms(ends[j][1] - c))
        st['d_f0'].append(ms(f0 - d)); st['f0_f1'].append(ms(f1 - f0)); st['f1_q'].append(ms(q - f1))
        st['q_h'].append(ms(h - q)); st['h_swap'].append(ms(t - h))
    stats('GPU end -> completed() (waiter wake)', st['gend_d'])
    stats('completed() -> -finish start', st['d_f0'])
    stats('flushSurfaces', st['f0_f1'])
    stats('finish -> completion queue block', st['f1_q'])
    stats('CA completed handlers', st['q_h'])
    stats('handlers done -> swap (CA work)', st['h_swap'])
    stats('GPU end -> swap (same buffers)', st['gend_swap'])
    stats('commit -> GPU start (span start - C)', st['c_gstart'])

def main():
    a = sys.argv[1:]
    ctlf = pid = None
    while '--ctl' in a or '--pid' in a:
        for k in ('--ctl', '--pid'):
            if k in a:
                i = a.index(k)
                v = a[i + 1]
                del a[i:i + 2]
                if k == '--ctl': ctlf = v
                else: pid = int(v)
    path = a[0] if a else '../logs/usb-live.log'
    recs, clocks = load(path, a[1] if len(a) > 1 else None, a[2] if len(a) > 2 else None)
    if not recs or not clocks:
        print('no SWEET TL records (boot with sweettrace=1)')
        return
    # CP ticks -> mach ticks: nearest clock pair, offset only (rates are fixed)
    ck = sorted(clocks)
    def cp2mach(cp):
        i = min(range(len(ck)), key=lambda j: abs(ck[j][1] - cp)) if len(ck) < 64 else \
            bisect.bisect_left([x[1] for x in ck], cp)
        i = min(i, len(ck) - 1)
        m, c = ck[i]
        return m + (cp - c) * (MACH_HZ / CP_HZ)
    ms = lambda ticks: ticks * 1000.0 / MACH_HZ
    ev = {k: sorted(r for r in recs if r[0] == k) for k in range(1, 8)}
    t0 = min(r[1] for r in recs); t1 = max(r[1] for r in recs)
    sw0 = [r[1] for r in ev[1]]
    secs = ((sw0[-1] - sw0[0]) / MACH_HZ) if len(sw0) > 1 else (t1 - t0) / MACH_HZ   # rates over the swap span
    print(f'{len(recs)} records over {secs:.1f} s: swaps {len(ev[1])/secs:.1f}/s, ticks {len(ev[2])/secs:.1f}/s, '
          f'presents {len(ev[4])/secs:.1f}/s ({sum(1 for r in ev[5] if r[2])} put off)')

    sw = [r[1] for r in ev[1]]
    iv = [ms(y - x) for x, y in zip(sw, sw[1:])]
    if iv:
        n = len(iv)
        print('frame intervals between swaps:')
        print(f'  <=17 ms {100*sum(1 for x in iv if x<=17.5)/n:5.1f}%   <=34 ms {100*sum(1 for x in iv if x<=34.5)/n:5.1f}%   '
              f'<=50 ms {100*sum(1 for x in iv if x<=51)/n:5.1f}%   >50 ms {100*sum(1 for x in iv if x>51)/n:5.1f}%')
        stats('swap-to-swap', iv)

    spans = []   # (start_mach, end_mach, fence, pid)
    for r in ev[7]:
        spans.append((cp2mach(r[1]), cp2mach(r[4]), r[2], r[3]))
    byown = {}
    for s, e, f, p in spans:
        byown.setdefault(p, []).append(ms(e - s))
    print('GPU time per submit, by owner pid (0 = kernel present):')
    for p, xs in sorted(byown.items(), key=lambda kv: -sum(kv[1])):
        stats(f'pid {p}', xs)
    sub = {r[2]: r for r in ev[6]}   # fence -> submit rec
    q = []
    for s, e, f, p in spans:
        if f in sub:
            q.append(ms(s - sub[f][1]))
    print('queue wait (submit call -> GPU start), all owners:')
    stats('submit -> GPU start', q)

    print('chain, per swap:')
    ends = sorted((e, p) for s, e, f, p in spans if p != 0)
    endt = [x[0] for x in ends]
    g_to_swap, swap_to_pb, pb_dur, pres_wait, pres_gpu = [], [], [], [], []
    pbs = [r[1] for r in ev[4]]; pes = [r[1] for r in ev[5]]
    for t in sw:
        i = bisect.bisect_right(endt, t) - 1
        if i >= 0 and ms(t - endt[i]) < 100:
            g_to_swap.append(ms(t - endt[i]))
        j = bisect.bisect_left(pbs, t)
        if j < len(pbs):
            swap_to_pb.append(ms(pbs[j] - t))
    for b in pbs:
        k = bisect.bisect_left(pes, b)
        if k < len(pes):
            pb_dur.append(ms(pes[k] - b))
    presub = [r for r in ev[6] if r[3] == 0]
    sp = {f: (s, e) for s, e, f, p in spans}
    for r in presub:
        if r[2] in sp:
            pres_wait.append(ms(sp[r[2]][0] - r[1]))
            pres_gpu.append(ms(sp[r[2]][1] - sp[r[2]][0]))
    stats('backboardd GPU end -> swap', g_to_swap)
    stats('swap -> present call (tick wait)', swap_to_pb)
    stats('present call duration (tick blocked)', pb_dur)
    stats('present submit -> blit GPU start', pres_wait)
    stats('present blit GPU time', pres_gpu)
    if ctlf:
        if pid is None:
            sys.exit('--ctl needs --pid N (backboardd)')
        ctl_report(load_ctl(ctlf, pid), sw, spans, pid, ms)

if __name__ == '__main__':
    main()
