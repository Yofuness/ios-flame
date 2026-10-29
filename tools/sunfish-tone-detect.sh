#!/bin/sh
# Listen for a test tone from the phone's speaker with a PC microphone (U5 without the user; 2026-10-29).
# Iriun gives video only on Linux. Default: the laptop mic (lid closed; a 1 kHz tone from the laptop speakers read
# ~50x above the neighbours). The Sonix webcam mic heard the same tone only ~5x (distorted, weak).
#   tools/sunfish-tone-detect.sh SECONDS [FREQ_HZ] [PULSE_SOURCE]
# Prints per 0.5 s window: rms, energy at FREQ and at FREQ +-150 Hz (Goertzel); "TONE" when FREQ is >= 8x both.
# The recording is kept only in /tmp and deleted at the end.
set -eu
secs=${1:?seconds}
freq=${2:-1000}
src=${3:-alsa_input.pci-0000_00_1f.3-platform-skl_hda_dsp_generic.HiFi__Mic1__source}
wav=$(mktemp /tmp/tone-XXXXXX.wav)
trap 'rm -f "$wav"' EXIT
timeout "$((secs + 1))" parecord --device="$src" --rate=48000 --channels=1 --format=s16le --file-format=wav "$wav" || true
python3 -I - "$wav" "$freq" <<'PY'
import sys, wave, struct, math
w = wave.open(sys.argv[1]); d = w.readframes(w.getnframes()); w.close()
s = struct.unpack('<%dh' % (len(d) // 2), d)
f0 = float(sys.argv[2]); win = 24000
def g(x, f):
    k = 2 * math.cos(2 * math.pi * f / 48000); a = b = 0.0
    for v in x:
        a, b = v + k * a - b, a
    return math.sqrt(max(a * a + b * b - k * a * b, 0)) / len(x)
hits = 0
for i in range(0, len(s) - win + 1, win):
    x = s[i:i + win]
    rms = math.sqrt(sum(v * v for v in x) / len(x))
    t, lo, hi = g(x, f0), g(x, f0 - 150), g(x, f0 + 150)
    tone = t >= 8 * max(lo, hi, 0.01)
    hits += tone
    print('%5.1fs rms %7.1f  %d Hz %8.2f  -150 %6.2f  +150 %6.2f%s' % (i / 48000, rms, f0, t, lo, hi, '  TONE' if tone else ''))
print('windows with tone: %d' % hits)
PY
