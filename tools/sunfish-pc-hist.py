#!/usr/bin/env python3
"""sunfish-pc-hist.py BOOTLOG [--sym IMAGE=FILE ...] [--jobs N]: histogram of the SWEET PC: lines of kernel patch 0249
(last capture in the log): top 40 functions and top 15 images. Shared-cache PCs are named with ipsw (research/dsc, as
pstack-sym.py; ~3 s per new address, kept in work/pc-sym.json). Other images print as image+offset, or through
llvm-symbolizer with --sym NAME=FILE (FILE with DWARF or symbols, __TEXT at 0). Run from anywhere."""
import argparse, bisect, collections, concurrent.futures, json, os, re, subprocess
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DSC = ROOT + '/research/dsc/dyld_shared_cache_arm64'
MEMO = ROOT + '/work/pc-sym.json'
BASE, SIZE = 0x180000000, 0x100000000
ap = argparse.ArgumentParser()
ap.add_argument('log')
ap.add_argument('--sym', action='append', default=[])
ap.add_argument('--jobs', type=int, default=6)
args = ap.parse_args()

pcs, images, slide, kslide, done, seen, expect, gaps = [], [], None, 0, '', set(), None, 0
for line in open(args.log, errors='replace'):
    m = re.search(r'SWEET PC: (.*)', line)
    if not m:
        continue
    t = m.group(1).strip()
    if t.startswith('armed'):           # a new capture: keep only the last
        pcs, images, slide, kslide, done, seen, expect, gaps = [], [], None, 0, '', set(), None, 0
    elif t.startswith('meta'):
        s = re.search(r' slide 0x([0-9a-f]+)', t)
        k = re.search(r' kslide 0x([0-9a-f]+)', t)
        if s:
            slide, kslide = int(s.group(1), 16), int(k.group(1), 16) if k else 0
    elif t.startswith('img'):
        f = t.split(None, 2)
        if len(f) >= 2 and re.fullmatch(r'0x[0-9a-f]+', f[1]):
            images.append((int(f[1], 16), f[2] if len(f) > 2 else '?'))
    elif t.startswith('done'):
        done = t
    else:
        m = re.fullmatch(r'tid \d+ @(\d+)((?: [0-9a-f]+)+)', t)
        if not m:
            continue                    # mangled by the log channel
        i, batch = int(m.group(1)), [int(x, 16) for x in m.group(2).split()]
        if i in seen:
            continue
        seen.add(i)
        if expect is not None and i != expect:
            gaps += 1
        expect = i + len(batch)
        pcs += [x for x in batch if x]
if not pcs:
    raise SystemExit('no SWEET PC samples in ' + args.log)
if slide is None:
    print('warning: no meta line, assuming slide 0')
    slide = 0
images.sort()
bases = [b for b, _ in images]
syms = dict(s.split('=', 1) for s in args.sym)

def in_cache(a):
    return BASE + slide <= a < BASE + slide + SIZE

try:
    memo = json.load(open(MEMO))
except (OSError, ValueError):
    memo = {}

def a2s(u):
    r = subprocess.run(['ipsw', 'dyld', 'a2s', DSC, hex(u), '-i', '--no-color', '--cache', ROOT + '/work/a2s.cache'],
                       capture_output=True, text=True)
    out = r.stdout + r.stderr
    img = re.search(r'^\s*>\s*(\S+)', out, re.M)
    fn = re.findall(r'^0x[0-9a-f]+: (.*)$', out, re.M)
    return [img.group(1) if img else '?', re.sub(r' \+ \d+$', '', fn[-1]) if fn else '?']

todo = sorted({a - slide for a in pcs if a >> 63 == 0 and in_cache(a)} - {int(k, 16) for k in memo})
if todo:
    print('naming %d new addresses with ipsw (%d jobs)...' % (len(todo), args.jobs), flush=True)
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        for u, r in zip(todo, ex.map(a2s, todo)):
            memo[hex(u)] = r
    os.makedirs(os.path.dirname(MEMO), exist_ok=True)
    json.dump(memo, open(MEMO, 'w'))

# Images outside the cache: llvm-symbolizer for those given with --sym
other = collections.defaultdict(set)
def owner(a):
    i = bisect.bisect_right(bases, a) - 1
    return images[i] if i >= 0 else (0, '?')
for a in pcs:
    if a >> 63 == 0 and not in_cache(a):
        b, p = owner(a)
        other[(b, p)].add(a - b)
named = {}
for (b, p), offs in other.items():
    obj = syms.get(os.path.basename(p))
    if not obj:
        continue
    offs = sorted(offs)
    r = subprocess.run(['llvm-symbolizer', '--obj=' + obj, '--functions=linkage', '--no-inlines', '-C'],
                       input='\n'.join(hex(o) for o in offs) + '\n', capture_output=True, text=True)
    blocks = [x.split('\n') for x in r.stdout.strip().split('\n\n')]
    for o, x in zip(offs, blocks):
        if x[0] != '??':
            named[(b, o)] = x[0]

func, img = collections.Counter(), collections.Counter()
kern = collections.Counter()
for a in pcs:
    if a >> 63:
        u = a - kslide
        kern[u] += 1
        i, f = '[kernel]', '[kernel] %#x' % u
    elif in_cache(a):
        i, f = memo.get(hex(a - slide), ['?', '?'])
        i = os.path.basename(i)
    else:
        b, p = owner(a)
        i = os.path.basename(p)
        f = named.get((b, a - b), '%s+%#x' % (i, (a - b) & ~0xfff))     # per page when unnamed
    img[i] += 1
    func[(f, i)] += 1

n = len(pcs)
print('%d samples%s, slide %#x, %d images outside the cache' % (n, ', %d line gaps' % gaps if gaps else '', slide, len(images)))
if done:
    print(done)
print('-- top 40 functions')
for (f, i), c in func.most_common(40):
    print('%5d %5.1f%%  %s  (%s)' % (c, c * 100 / n, f[:110], i))
print('-- top 15 images')
for i, c in img.most_common(15):
    print('%5d %5.1f%%  %s' % (c, c * 100 / n, i))
kt = sum(kern.values())
print('-- kernel: %d samples (%.1f%%), top unslid PCs (kslide %#x)' % (kt, kt * 100 / n, kslide))
for u, c in kern.most_common(10):
    print('%5d %#x' % (c, u))
