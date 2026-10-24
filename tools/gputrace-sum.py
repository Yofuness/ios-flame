#!/usr/bin/env python3
"""gputrace-sum.py TRACE: total GPU time per operation from a Turnip MESA_GPU_TRACES=print log.

  tools/sunfish-ssh 'tail -c 600000 /var/local/turnip-backboardd.log' > tr.txt; tools/gputrace-sum.py tr.txt
Prints command buffers over 0.1 ms (frames) with their mean, then each start_X/end_X pair: count, total, mean.
"""
import collections, re, sys
tot, cnt, starts = collections.Counter(), collections.Counter(), {}
frames = el = 0
for line in open(sys.argv[1], errors='ignore'):
    m = re.match(r'(\d+)\s+\+\d+: (start|end)_(\w+)', line)
    if m:
        t, se, n = int(m[1]), m[2], m[3]
        if se == 'start':
            starts[n] = t
        elif n in starts:
            tot[n] += t - starts.pop(n)
            cnt[n] += 1
    m = re.match(r'ELAPSED: (\d+)', line)
    if m and int(m[1]) > 100000:
        frames += 1
        el += int(m[1])
print(f'command buffers > 0.1 ms: {frames}, mean {el / frames / 1e6 if frames else 0:.2f} ms')
for n, v in tot.most_common(12):
    print(f'{n:28s} n={cnt[n]:5d} total={v / 1e6:8.1f} ms mean={v / cnt[n] / 1e3:8.1f} us')
