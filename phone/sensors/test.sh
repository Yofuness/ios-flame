#!/bin/sh
# sns1: host tests of the SNS client (ios-redfin's test_sensd.c, MIT): protobuf coder, SEE encoders/decoders and a
# canned SUID/stream exchange against a fake DSP over a socketpair (sns_link's fd framing).
set -e
cd "$(dirname "$0")"
mkdir -p out
${CC:-cc} -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -o out/test_sensd \
	test_sensd.c pb.c sns.c sensor.c sns_link.c ../wifi/qmi.c -lm
./out/test_sensd
# B46 dm2: dm_fusion type-2 heading (flat phone, top north = 0, top east = 90, clockwise)
${CC:-cc} -O1 -g -Wall -Wextra -fsanitize=address,undefined -o out/test_dm test_dm.c dm_fusion.c sns.c pb.c ../wifi/qmi.c -lm
./out/test_dm
# dm10: replay and analysis on a synthetic dump: hard + soft iron, gyro bias; still, tumble, flat turns
${CC:-cc} -O1 -g -Wall -Wextra -fsanitize=address,undefined -o out/dm_replay dm_replay.c dm_fusion.c -lm
python3 -I - out/dm_synth.csv out/dm_synth_soft.csv <<'PY'
import math, sys
def rot(th, roll, pitch):
    # device axes in earth (x north, y west, z up): heading th clockwise, then pitch about x, roll about y
    t, r, p = map(math.radians, (th, roll, pitch))
    top = (math.cos(p) * math.cos(t), -math.cos(p) * math.sin(t), math.sin(p))
    h = (math.sin(t), math.cos(t), 0.0)  # right, level
    z0 = (h[1] * top[2] - h[2] * top[1], h[2] * top[0] - h[0] * top[2], h[0] * top[1] - h[1] * top[0])
    c, s = math.cos(r), math.sin(r)
    x = tuple(c * a - s * q for a, q in zip(h, z0))
    z = tuple(s * a + c * q for a, q in zip(h, z0))
    return [[x[i], top[i], z[i]] for i in range(3)]
def traj(t):
    if t < 3: return 0, 0, 0
    if t < 63:
        u = t - 3; k = min(u / 3, 1, (60 - u) / 3)
        return 12 * u, k * 70 * math.sin(u * 0.53), k * 55 * math.sin(u * 0.37)
    u = t - 63
    th0 = 720.0
    if u < 30: return th0 + 12 * u, 0, 0
    if u < 35: return th0 + 360, 0, 0
    return th0 + 360 + 60 * (u - 35), 0, 0
for path, soft in ((sys.argv[1], 1.0), (sys.argv[2], 1.2)):
    b = (12, -30, 5); gb = (0.01, -0.006, 0.004)
    dt = 0.01; end = 47 + 63
    with open(path, 'w') as o:
        for i in range(int(end / dt)):
            t = i * dt + 100
            R = rot(*traj(t - 100)); R2 = rot(*traj(t - 100 + dt))
            mul = lambda A, v: [sum(A[k][j] * v[j] for j in range(3)) for k in range(3)]
            tr = lambda v: [sum(R[k][j] * v[k] for k in range(3)) for j in range(3)]
            a = tr((0, 0, 9.81))
            rel = [[sum(R[k][p] * R2[k][q] for k in range(3)) for q in range(3)] for p in range(3)]
            g = ((rel[2][1] - rel[1][2]) / 2 / dt + gb[0], (rel[0][2] - rel[2][0]) / 2 / dt + gb[1], (rel[1][0] - rel[0][1]) / 2 / dt + gb[2])
            o.write('a,%.4f,%.4f,%.4f,%.4f\n' % (t, *a))
            o.write('g,%.4f,%.5f,%.5f,%.5f\n' % (t, *g))
            if i % 2 == 0:
                m = tr((20, 0, -40))
                m[0] *= soft
                o.write('m,%.4f,%.3f,%.3f,%.3f\n' % (t, m[0] + b[0], m[1] + b[1], m[2] + b[2]))
            if i % 50 == 0:
                o.write('h,%.4f,0.00,-1.00\n' % t)
PY
./out/dm_replay out/dm_synth.csv | grep summary | tee out/dm_synth.txt
# dm12: the sphere stops at the last fit with the tumble in its ring, and the first 5 s flat are the limited catch-up
grep -Eq 'bias gyro 1[12]\.[09][0-9] -(29\.9|30\.0)[0-9] [45]\.[09][0-9]' out/dm_synth.txt
awk '/rms flat settled/ { ok = $(NF - 4) < 3.0 && $(NF - 1) >= 80 } END { exit !ok }' out/dm_synth.txt
python3 -I ../../tools/sunfish-magcal-analyze.py out/dm_synth.csv
python3 -I ../../tools/sunfish-magcal-analyze.py out/dm_synth_soft.csv | tee out/dm_synth.txt
awk '$1 == "ellipsoid" && $2 != "center" { e = $NF } $1 == "sphere" && $2 != "center" { s = $NF } END { exit !(e < 1.5 && s > e + 2) }' out/dm_synth.txt
SNS_HEADING_GRAVITY=lp ./out/dm_replay out/dm_synth.csv | grep 'summary: rms'
SNS_HC_K_SLOW=0 SNS_HC_K_FAST=0 ./out/dm_replay out/dm_synth.csv | grep 'summary: rms flat'
echo 'dm10 replay: all ok'
# dm11: the real recording, when it is here (not in git). "Settled": the first 5 s after a tumble don't count, see dm_replay.c
rec=../../work/cal/motion-rec1.csv
if [ -f "$rec" ]; then
	./out/dm_replay "$rec" | grep summary | tee out/dm_rec1.txt
	awk '/summary: bias/ { dx = $4 + 34.3; dy = $5 + 41.5; ok1 = $3 == "gyro" && dx * dx < 6.25 && dy * dy < 6.25 }
	    /rms flat settled/ { ok2 = $(NF - 4) <= 6.0 } /less the gyro/ { ok3 = $NF <= 8 } END { exit !(ok1 && ok2 && ok3) }' out/dm_rec1.txt
	echo 'dm11 recording: ok'
fi
