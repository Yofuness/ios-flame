#!/bin/bash
# Scripted UI load + fps readout from the live log (SWEET FB 5 s lines).
#   tools/sunfish-fpsbench.sh TAG [SECONDS]
# Unlocks, then swipes between home pages for SECONDS; prints the fps lines
# logged during that window and their average.
cd "$(dirname "$0")/.." || exit 1
tag=$1 secs=${2:-30} L=../logs/usb-live.log
# BENCH_TIGHT=1: back-to-back 400 ms swipes (continuous motion) instead of 250 ms swipes + 600 ms pauses
sw=250 gap=0.6; [ -n "$BENCH_TIGHT" ] && sw=400 gap=0.05
C() { python3 -I tools/flame-cmd.py --wait 0 "$@" >/dev/null 2>&1; }
C swipe 540 2330 540 1100 300; sleep 3
echo "=== bench $tag start $(date +%T)" >> "$L"
end=$((SECONDS + secs))
while [ $SECONDS -lt $end ]; do
	C swipe 900 1200 150 1200 $sw; sleep $gap
	C swipe 150 1200 900 1200 $sw; sleep $gap
done
sleep 6
echo "=== bench $tag end $(date +%T)" >> "$L"
sed -n "/=== bench $tag start/,/=== bench $tag end/p" "$L" | grep -a 'SWEET FB: 5s' | grep -oE '[0-9]+ swaps|[0-9]+ panel copies' | paste -d' ' - - |
	awk -v t="$tag" '{s+=$1; c+=$3; n++} END {if (n) printf "%s: %.1f swaps/s, %.1f panel updates/s over %d samples\n", t, s/n/5, c/n/5, n}'
