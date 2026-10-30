#!/bin/bash
# Camera stage c test (cam8, 2026-10-30): rear IMX363 mode 8 (2016x1512 RAW10) through CSIPHY0 into CSID-lite RDI0,
# then the CSID counters (expect bytes ping/pong 3810240, ECC/CRC 0). Needs a boot with sweetse9=2 sweetcampmic=2,
# the phone tools camid/camclk/camrx in /var/local/bin (tools/build-userland-linux.sh) and the table
# /var/local/cam/imx363-mode8.txt (tools/sunfish-camtab.py --out work/cam/imx363-mode8.txt --mode 8).
#   tools/sunfish-cam-rx.sh [--front] [--install] [--capture | --stream SECONDS] [SETTLE]     output also goes to work/cam/rx-<time>.log
# --capture (stage d): instead of rx arm + camid --stream + rx report, `camrx capture` (arms the CSID, sets up the
# IFE-lite WM0 + SMMU, runs the stream itself) writes /var/local/cam/frame.raw; it is fetched to
# work/cam/frame-<time>.raw and turned into PNGs (tools/sunfish-raw2png.py). CAPMODE picks the SMMU mode
# (default, cam21: "--bypass --sid 0x8c0 --sid2 0xcc0" = two exact SMRs for the WM's two SIDs (1 KB address
# interleave) -> bypass bank; NOT one 0x8c0/0x400 SMR, which lost the data and reset the phone (cam17/18/20).
# "--trap" sends the IFE SIDs to a faulting bank, nothing lands.
# --stream SECONDS (camera e1): `camrx stream SECONDS /var/local/cam/stream` (2-slot ring, per-second fps, the last
# frame) instead of capture; /var/local/cam/stream.raw is fetched to work/cam/stream-<time>.raw and turned into PNGs.
# --front (cam-f1, first option): the front IMX355 instead: mode 2 (1640x924 RAW10, 4 lanes at 360 Mb/s) through
# CSIPHY2 (camrx --front), MCLK2 (camclk mclk 2 24), camid --front, table /var/local/cam/imx355-mode2.txt
# (tools/sunfish-camtab.py research/notes/camx/com.qti.sensormodule.primax_imx355.bin -> work/cam/imx355-mode2.txt).
# Everything is powered down at the end, also after a failed step.
# SE9 is shared with haptics: touch /var/local/haptics.off and kill hapticd first (it restarts, sees the flag, stays off).
set -uo pipefail
cd "$(dirname "$0")/.."
S=tools/sunfish-ssh
B=/var/local/bin
mkdir -p work/cam
LOG=work/cam/rx-$(date +%Y%m%d-%H%M%S).log
FRONT=""	# cam-f1: camid/camrx flag
MCLK=0
TABLE=imx363-mode8.txt
PNG=""
if [ "${1:-}" = --front ]; then
	FRONT=--front
	MCLK=2
	TABLE=imx355-mode2.txt
	PNG="--width 1640 --height 924"
	shift
fi
if [ "${1:-}" = --install ]; then
	shift
	for t in camid camclk camrx; do
		python3 -I tools/flame-files.py put work/userland-linux/out/$t $B/$t 755 >/dev/null || exit 1
	done
	$S 'mkdir -p /var/local/cam' </dev/null
	python3 -I tools/flame-files.py put work/cam/$TABLE /var/local/cam/$TABLE 644 >/dev/null || exit 1
fi
CAPTURE=0
if [ "${1:-}" = --capture ]; then
	CAPTURE=1
	shift
fi
STREAM=0
if [ "${1:-}" = --stream ] && [ -n "${2:-}" ]; then
	STREAM=$2
	shift 2
fi
SETTLE=${1:-}
TS=$(date +%Y%m%d-%H%M%S)
run() {	# one phone command; stops the sequence (and powers down) on failure
	echo "=== $*" | tee -a "$LOG"
	$S "$* 2>&1" </dev/null | tee -a "$LOG"
	return "${PIPESTATUS[0]}"
}
down() {
	for c in "$B/camrx $FRONT rx stop" "$B/camrx $FRONT phy off" "$B/camrx clocks off" "$B/camclk off" "$B/camid $FRONT --addr 0x1a"; do
		run "$c" >/dev/null
	done
	echo "powered down; log $LOG"
}
trap down EXIT
# PLLs first (they need no GDSC): a camnoc RCG left on PLL0 by an aborted capture then still has its source
run "$B/camclk pll2 on" &&
	run "$B/camclk pll0 on" &&
	run "$B/camid $FRONT --addr 0x1a --keep" &&
	run "$B/camclk mclk $MCLK 24" &&
	run "$B/camrx clocks on" &&
	run "$B/camrx $FRONT phy on $SETTLE" || exit 1
if [ "$STREAM" != 0 ]; then
	run "$B/camrx $FRONT stream $STREAM /var/local/cam/stream" || exit 1
	python3 -I tools/flame-files.py get /var/local/cam/stream.raw work/cam/stream-$TS.raw >/dev/null || exit 1
	python3 -I tools/sunfish-raw2png.py work/cam/stream-$TS.raw $PNG | tee -a "$LOG"
	exit 0
fi
if [ $CAPTURE = 1 ]; then
	run "$B/camrx $FRONT capture /var/local/cam/frame.raw ${CAPMODE:---bypass --sid 0x8c0 --sid2 0xcc0}" || exit 1
	python3 -I tools/flame-files.py get /var/local/cam/frame.raw work/cam/frame-$TS.raw >/dev/null || exit 1
	python3 -I tools/sunfish-raw2png.py work/cam/frame-$TS.raw $PNG | tee -a "$LOG"
	exit 0
fi
run "$B/camrx $FRONT rx arm" &&
	run "$B/camid $FRONT --addr 0x1a --keep --mclk-asis --stream /var/local/cam/$TABLE" &&
	run "$B/camrx $FRONT rx report"
