#!/usr/bin/env python3
"""dm10: which magnetometer calibration works on a recorded /var/local/motion-dump.csv?

Usage: sunfish-magcal-analyze.py dump.csv
Sphere, ellipsoid and flat-circle fits over the mag samples (thinned to one per 3 uT of movement, so a still phone
does not dominate), the field they leave and their heading error against gyro-integrated yaw in the flat stretch
with the largest turn. Pure Python: no numpy needed.
"""
import math
import sys

DECIM = 3.0
FLAT_DEG = 15.0
G = 9.80665


def solve(a, b):
    """Gaussian elimination with pivoting; None when singular."""
    n = len(b)
    m = [list(a[i]) + [b[i]] for i in range(n)]
    for i in range(n):
        p = max(range(i, n), key=lambda r: abs(m[r][i]))
        if abs(m[p][i]) < 1e-10:
            return None
        m[i], m[p] = m[p], m[i]
        for r in range(n):
            if r != i and m[r][i]:
                f = m[r][i] / m[i][i]
                m[r] = [x - f * y for x, y in zip(m[r], m[i])]
    return [m[i][n] / m[i][i] for i in range(n)]


def lsq(rows, ys):
    n = len(rows[0])
    a = [[0.0] * n for _ in range(n)]
    b = [0.0] * n
    for r, y in zip(rows, ys):
        for i in range(n):
            ri = r[i]
            b[i] += ri * y
            ai = a[i]
            for j in range(n):
                ai[j] += ri * r[j]
    return solve(a, b)


def jacobi(a):
    """Eigenvalues and eigenvectors (columns) of a symmetric 3x3."""
    a = [list(r) for r in a]
    v = [[float(i == j) for j in range(3)] for i in range(3)]
    for _ in range(50):
        for p, q in ((0, 1), (0, 2), (1, 2)):
            if abs(a[p][q]) < 1e-14:
                continue
            th = 0.5 * math.atan2(2 * a[p][q], a[q][q] - a[p][p])
            c, s = math.cos(th), math.sin(th)
            for k in range(3):
                x, y = a[k][p], a[k][q]
                a[k][p], a[k][q] = c * x - s * y, s * x + c * y
            for k in range(3):
                x, y = a[p][k], a[q][k]
                a[p][k], a[q][k] = c * x - s * y, s * x + c * y
            for k in range(3):
                x, y = v[k][p], v[k][q]
                v[k][p], v[k][q] = c * x - s * y, s * x + c * y
    return [a[i][i] for i in range(3)], v


def load(path):
    out = []
    prev = off = 0.0
    with open(path) as fp:
        for i, line in enumerate(fp):
            p = line.strip().split(",")
            if len(p) != 5 or p[0] not in ("a", "g", "m"):
                continue
            try:
                t = float(p[1])
                v = (float(p[2]), float(p[3]), float(p[4]))
            except ValueError:
                continue
            if out and t + off < prev - 5.0:  # a later boot: the clock restarted
                off = prev + 1.0 - t
            t += off
            prev = t
            out.append((t, i, p[0], v))
    out.sort(key=lambda s: (s[0], s[1]))
    return out


def gyro_bias(samples):
    """Mean over every window >= 2 s with all gyro axes below 0.03 rad/s."""
    tot = 0.0
    sum_ = [0.0] * 3
    wt = 0.0
    win = [0.0] * 3
    last = None
    for t, _, c, v in samples + [(0, 0, "g", (9, 9, 9))]:
        if c != "g":
            continue
        dt = t - last if last is not None else 0.0
        last = t
        if all(abs(x) < 0.03 for x in v) and dt < 0.1:
            wt += dt
            win = [w + x * dt for w, x in zip(win, v)]
            continue
        if wt >= 2.0:
            tot += wt
            sum_ = [a + b for a, b in zip(sum_, win)]
        wt, win = 0.0, [0.0] * 3
    return [x / tot if tot else 0.0 for x in sum_], tot


def walk(samples, bias):
    """For each mag sample: (t, m, up, yaw deg), up from accel low-passed 0.2 s, yaw integrated clockwise."""
    up = None
    yaw = 0.0
    last = None
    lastg = None
    mags = []
    for t, _, c, v in samples:
        if c == "a":
            n = math.sqrt(sum(x * x for x in v))
            if n < 1e-3 or abs(n - G) > 0.1 * G:
                continue
            a = 1 - math.exp(-(t - last) / 0.2) if up and last is not None and t > last else 1.0 if not up else 0.0
            u = [x / n for x in v]
            up = u if not up else [p + a * (q - p) for p, q in zip(up, u)]
            n = math.sqrt(sum(x * x for x in up))
            up = [x / n for x in up]
            last = t
        elif c == "g":
            if up and lastg is not None:
                dt = min(t - lastg, 0.1)
                yaw -= sum(u * (x - b) for u, x, b in zip(up, v, bias)) * dt * 180 / math.pi
            lastg = t
        elif up and math.sqrt(sum(x * x for x in v)) <= 200:
            mags.append((t, v, tuple(up), yaw))
    return mags


def heading(u, m):
    """dm_heading_mag: 0 = top north, clockwise; the back of the device when the top is near vertical."""
    w = (u[1] * m[2] - u[2] * m[1], u[2] * m[0] - u[0] * m[2], u[0] * m[1] - u[1] * m[0])
    n = (w[1] * u[2] - w[2] * u[1], w[2] * u[0] - w[0] * u[2], w[0] * u[1] - w[1] * u[0])
    if abs(u[1]) < math.cos(math.radians(20)):
        h = math.atan2(-w[1], n[1])
    else:
        h = math.atan2(w[2], -n[2])
    return math.degrees(h) % 360


def wrap(d):
    return (d + 180) % 360 - 180


def thin(ms):
    out = []
    for m in ms:
        if not out or sum((a - b) ** 2 for a, b in zip(m, out[-1])) > DECIM * DECIM:
            out.append(m)
    return out


def sphere_fit(pts):
    x = lsq([(2 * p[0], 2 * p[1], 2 * p[2], 1.0) for p in pts], [sum(c * c for c in p) for p in pts])
    if not x:
        return None
    r2 = x[3] + x[0] ** 2 + x[1] ** 2 + x[2] ** 2
    if r2 <= 0:
        return None
    r = math.sqrt(r2)
    res = math.sqrt(sum((math.dist(p, x[:3]) - r) ** 2 for p in pts) / len(pts))
    return x[:3], r, res


def ellipsoid_fit(pts):
    """Hard + soft iron: returns center, axes (uT), W (3x3 unit-gain correction scaled to mean radius), residual."""
    mean = [sum(p[i] for p in pts) / len(pts) for i in range(3)]
    s = 50.0  # conditioning
    q = [[(p[i] - mean[i]) / s for i in range(3)] for p in pts]
    x = lsq([(a * a, b * b, c * c, 2 * a * b, 2 * a * c, 2 * b * c, 2 * a, 2 * b, 2 * c) for a, b, c in q], [1.0] * len(q))
    if not x:
        return None
    A = [[x[0], x[3], x[4]], [x[3], x[1], x[5]], [x[4], x[5], x[2]]]
    c = solve(A, [-x[6], -x[7], -x[8]])
    if not c:
        return None
    k = 1 + sum(c[i] * A[i][j] * c[j] for i in range(3) for j in range(3))
    if k <= 0:
        return None
    ev, v = jacobi([[e / k for e in row] for row in A])
    if min(ev) <= 0:
        return None
    axes = [s / math.sqrt(e) for e in ev]
    r = (axes[0] * axes[1] * axes[2]) ** (1 / 3)
    W = [[sum(v[i][l] * r / axes[l] * v[j][l] for l in range(3)) for j in range(3)] for i in range(3)]
    cen = [mean[i] + s * c[i] for i in range(3)]
    e2 = 0.0
    for p in pts:
        d = [p[i] - cen[i] for i in range(3)]
        e2 += (math.sqrt(sum(sum(W[i][j] * d[j] for j in range(3)) ** 2 for i in range(3))) - r) ** 2
    return cen, axes, W, r, math.sqrt(e2 / len(pts))


def circle_fit(pts):
    x = lsq([(2 * p[0], 2 * p[1], 1.0) for p in pts], [p[0] * p[0] + p[1] * p[1] for p in pts])
    if not x:
        return None
    r2 = x[2] + x[0] ** 2 + x[1] ** 2
    if r2 <= 0:
        return None
    return x[:2], math.sqrt(r2)


def is_flat(u):
    return abs(u[2]) >= math.cos(math.radians(FLAT_DEG))


def flat_turn(mags):
    """The stretch of flat samples (gaps < 1 s) with the largest yaw range."""
    best, cur = [], []
    def score(s):
        return max(x[3] for x in s) - min(x[3] for x in s) if s else -1
    for s in mags:
        if is_flat(s[2]) and (not cur or s[0] - cur[-1][0] < 1.0):
            cur.append(s)
            continue
        if score(cur) > score(best):
            best = cur
        cur = [s] if is_flat(s[2]) else []
    return cur if score(cur) > score(best) else best


def report(name, fix, mags, seg):
    hs, vs = [], []
    for _, m, u, _ in mags:
        c = fix(m)
        v = sum(a * b for a, b in zip(c, u))
        vs.append(v)
        hs.append(math.sqrt(max(sum(a * a for a in c) - v * v, 0.0)))
    def ms(x):
        mu = sum(x) / len(x)
        return mu, math.sqrt(sum((a - mu) ** 2 for a in x) / len(x))
    mh, sh = ms(hs)
    mv, sv = ms(vs)
    err = "no flat turn"
    if len(seg) >= 2:
        ds = [wrap(heading(u, fix(m)) - y) for _, m, u, y in seg]
        mean = math.degrees(math.atan2(sum(math.sin(math.radians(d)) for d in ds), sum(math.cos(math.radians(d)) for d in ds)))
        err = "%.1f" % math.sqrt(sum(wrap(d - mean) ** 2 for d in ds) / len(ds))
    print("%-10s %6.1f +- %5.1f  %7.1f +- %5.1f  %s" % (name, mh, sh, mv, sv, err))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    samples = load(sys.argv[1])
    bias, still = gyro_bias(samples)
    mags = walk(samples, bias)
    if len(mags) < 20:
        sys.exit("too few mag samples (%d)" % len(mags))
    pts = thin([s[1] for s in mags])
    flat = thin([s[1] for s in mags if is_flat(s[2])])
    print("samples %d, mag %d, for the fits %d (flat %d); gyro bias %.4f %.4f %.4f (%.0f s still)" %
          (len(samples), len(mags), len(pts), len(flat), bias[0], bias[1], bias[2], still))
    cals = [("raw", lambda m: m)]
    sph = sphere_fit(pts) if len(pts) >= 10 else None
    if sph:
        c, r, res = sph
        print("sphere:    center %.2f %.2f %.2f  r %.2f  residual %.2f uT" % (c[0], c[1], c[2], r, res))
        cals.append(("sphere", lambda m: [m[i] - c[i] for i in range(3)]))
    else:
        print("sphere:    no fit")
    ell = ellipsoid_fit(pts) if len(pts) >= 30 else None
    if ell:
        ec, ax, W, er, eres = ell
        print("ellipsoid: center %.2f %.2f %.2f  axes %.2f %.2f %.2f  ratios %.3f %.3f  residual %.2f uT" %
              (ec[0], ec[1], ec[2], ax[0], ax[1], ax[2], max(ax) / min(ax), sorted(ax)[1] / min(ax), eres))
        def fix_e(m):
            d = [m[i] - ec[i] for i in range(3)]
            return [sum(W[i][j] * d[j] for j in range(3)) for i in range(3)]
        cals.append(("ellipsoid", fix_e))
    else:
        print("ellipsoid: no fit (coverage too small or not an ellipsoid)")
    cir = circle_fit(flat) if len(flat) >= 5 else None
    if cir:
        cc, cr = cir
        cz = sph[0][2] if sph else 0.0
        print("circle:    center %.2f %.2f  r %.2f (flat samples; z from the sphere: %.2f)" % (cc[0], cc[1], cr, cz))
        cals.append(("circle", lambda m: [m[0] - cc[0], m[1] - cc[1], m[2] - cz]))
    else:
        print("circle:    no fit (too few flat samples)")
    seg = flat_turn(mags)
    if seg:
        ys = [s[3] for s in seg]
        print("flat turn: %.1f s, yaw range %.0f deg, %d samples" % (seg[-1][0] - seg[0][0], max(ys) - min(ys), len(seg)))
    print("%-10s %-15s %-16s %s" % ("cal", "horiz uT", "vert uT", "heading rms vs gyro, deg"))
    for name, fix in cals:
        report(name, fix, mags, seg)


main()
