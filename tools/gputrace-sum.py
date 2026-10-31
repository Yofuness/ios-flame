#!/usr/bin/env python3
"""gputrace-sum.py TRACE [TOP]: total GPU time per operation from a Turnip MESA_GPU_TRACES=print log.

  python3 -I tools/flame-files.py get /var/local/turnip/trace-PID.log tr.txt; tools/gputrace-sum.py tr.txt
Prints command buffers over 0.1 ms with their mean, then each start_X/end_X pair: count, total, mean. Then by pass
target size, copy_buffer_to_image and copy_image_to_buffer per frame, and with TU_SWEET_DRAW_TRACE=1 the draw spans by
fragment + vertex shader hash and by target + draw index (TOP rows each, default 12).
A frame is a command buffer with a pass of at least half the largest target's area: Turnip's own frame counter does not
move here (no END OF FRAME lines). Draw spans are per bin, and binning replays them: those are counted apart.
"""
import collections, re, statistics, sys
top = int(sys.argv[2]) if len(sys.argv) > 2 else 12
tot, cnt, starts = collections.Counter(), collections.Counter(), {}
frames = el = 0
copies = ('copy_buffer_to_image', 'copy_image_to_buffer')
passes = collections.defaultdict(collections.Counter)   # target -> pass count, ms and what ran inside
shaders, indexes, where = [collections.defaultdict(lambda: [0, 0]) for _ in range(3)]
batches = []    # largest pass area of a command buffer, its time per copy kind
batch = None
target = '-'
ctx = None      # the bin or binning section being replayed
draw = None

def new_batch():
    return {'area': 0, **{c: 0 for c in copies}, **{c + '_n': 0 for c in copies}}

def arg(rest, name, default='-'):
    m = re.search(name + r'=([^,]*),', rest)
    return m[1] if m and m[1] else default

for line in open(sys.argv[1], errors='ignore'):
    m = re.match(r'(\d+)\s+[+-]\d+:\s+(start|end)_(\w+) ?(.*)', line)
    if m:
        t, se, n, rest = int(m[1]), m[2], m[3], m[4]
        batch = batch or new_batch()
        if se == 'start':
            starts[n] = t
            if n == 'render_pass':
                w, h = int(arg(rest, 'width', 0)), int(arg(rest, 'height', 0))
                target = f'{w}x{h}'
                batch['area'] = max(batch['area'], w * h)
                passes[target]['bins'] += int(arg(rest, 'numberOfBins', 0))
            elif n in ('binning_ib', 'concurrent_binning_ib', 'draw_ib_gmem', 'draw_ib_sysmem'):
                ctx = 'binning' if 'binning' in n else 'bin'
            elif n == 'draw':
                draw = rest
        elif n in starts:
            d = t - starts.pop(n)
            tot[n] += d
            cnt[n] += 1
            if n == 'draw' and draw is not None:
                kind = ctx or 'sysmem'
                where[kind][0] += d
                where[kind][1] += 1
                if kind != 'binning':
                    for table, key in ((shaders, f"fs {arg(draw, 'fs_sha1')[:12]} vs {arg(draw, 'vs_sha1')[:12]}"),
                                       (indexes, f"{target} draw {arg(draw, 'draw_idx')}")):
                        table[key][0] += d
                        table[key][1] += 1
                    passes[target]['draw'] += d
                    passes[target]['drawn'] += 1
                draw = None
            elif n in copies:
                batch[n] += d
                batch[n + '_n'] += 1
            elif n == 'render_pass':
                passes[target]['n'] += 1
                passes[target]['ms'] += d
                target = '-'
            elif n in ('gmem_load', 'gmem_store') or 'binning_ib' in n:
                passes[target]['binning' if 'binning' in n else n] += d
            if n in ('binning_ib', 'concurrent_binning_ib', 'draw_ib_gmem', 'draw_ib_sysmem'):
                ctx = None
        continue
    m = re.match(r'ELAPSED: (\d+)', line)
    if m:
        if int(m[1]) > 100000:
            frames += 1
            el += int(m[1])
        if batch:
            batches.append(batch)
        batch = None
if batch:
    batches.append(batch)   # the file ended inside a command buffer
print(f'command buffers > 0.1 ms: {frames}, mean {el / frames / 1e6 if frames else 0:.2f} ms')
for n, v in tot.most_common(top):
    print(f'{n:28s} n={cnt[n]:5d} total={v / 1e6:8.1f} ms mean={v / cnt[n] / 1e3:8.1f} us')

print('\nby pass target: passes, ms total and per pass, bins per pass; ms per pass: gmem_load gmem_store binning draws (draw spans)')
for k, p in sorted(((k, p) for k, p in passes.items() if p['n']), key=lambda kv: -kv[1]['ms'])[:top]:
    per = lambda key: p[key] / p['n'] / 1e6
    print(f"{k:12s} n={p['n']:5d} total={p['ms'] / 1e6:8.1f} ms mean={per('ms'):7.2f} bins={p['bins'] / p['n']:5.1f} "
          f"load={per('gmem_load'):6.2f} store={per('gmem_store'):6.2f} binning={per('binning'):6.2f} "
          f"draws={per('draw'):7.2f} ({p['drawn'] / p['n']:.0f})")

full = max((b['area'] for b in batches), default=0)
fr = [b for b in batches if full and b['area'] * 2 >= full]
print(f'\nframes (a pass of at least half of {full} px): {len(fr)}')
for c in copies:
    n, ms = sum(b[c + '_n'] for b in batches), sum(b[c] for b in batches) / 1e6
    # the copies of the between-frame command buffers count too; the p50 is of the frame buffers alone
    p50 = statistics.median(b[c] for b in fr) / 1e6 if fr else 0
    print(f'{c:22s} n={n:5d} total={ms:8.1f} ms; per frame n={n / len(fr) if fr else 0:6.1f} ms={ms / len(fr) if fr else 0:7.2f}; '
          f'in the frame buffer p50 ms={p50:7.2f}')

if where:
    print('\ndraw spans (timestamps in every bin: spans serialize the GPU, read them as shares):',
          ', '.join(f'{k} n={v[1]} {v[0] / 1e6:.1f} ms' for k, v in sorted(where.items())))
    for title, table in (('by shader (no pipeline label in the trace)', shaders), ('by target and draw index', indexes)):
        print(title)
        for k, v in sorted(table.items(), key=lambda kv: -kv[1][0])[:top]:
            print(f'  {k:42s} n={v[1]:6d} total={v[0] / 1e6:8.1f} ms mean={v[0] / v[1] / 1e3:8.1f} us')
