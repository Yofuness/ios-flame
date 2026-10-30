#!/bin/bash
# Camera stage c test (cam8, 2026-10-30): rear IMX363 mode 8 (2016x1512 RAW10) through CSIPHY0 into CSID-lite RDI0,
# then the CSID counters (expect bytes ping/pong 3810240, ECC/CRC 0). Needs a boot with sweetse9=2 sweetcampmic=2,
# the phone tools camid/camclk/camrx in /var/local/bin (tools/build-userland-linux.sh) and the table
# /var/local/cam/imx363-mode8.txt (tools/sunfish-camtab.py --out work/cam/imx363-mode8.txt --mode 8).
#   tools/sunfish-cam-rx.sh [--install] [SETTLE]     output also goes to work/cam/rx-<time>.log
# Everything is powered down at the end, also after a failed step.
set -uo pipefail
cd "$(dirname "$0")/.."
S=tools/sunfish-ssh
B=/var/local/bin
mkdir -p work/cam
LOG=work/cam/rx-$(date +%Y%m%d-%H%M%S).log
if [ "${1:-}" = --install ]; then
	shift
	for t in camid camclk camrx; do
		python3 -I tools/flame-files.py put work/userland-linux/out/$t $B/$t 755 >/dev/null || exit 1
	done
	$S 'mkdir -p /var/local/cam' </dev/null
	python3 -I tools/flame-files.py put work/cam/imx363-mode8.txt /var/local/cam/imx363-mode8.txt 644 >/dev/null || exit 1
fi
SETTLE=${1:-}
run() {	# one phone command; stops the sequence (and powers down) on failure
	echo "=== $*" | tee -a "$LOG"
	$S "$* 2>&1" </dev/null | tee -a "$LOG"
	return "${PIPESTATUS[0]}"
}
down() {
	for c in "$B/camrx rx stop" "$B/camrx phy off" "$B/camrx clocks off" "$B/camclk off" "$B/camid --addr 0x1a"; do
		run "$c" >/dev/null
	done
	echo "powered down; log $LOG"
}
trap down EXIT
run "$B/camid --addr 0x1a --keep" &&
	run "$B/camclk pll2 on" &&
	run "$B/camclk pll0 on" &&
	run "$B/camclk mclk 0 24" &&
	run "$B/camrx clocks on" &&
	run "$B/camrx phy on $SETTLE" &&
	run "$B/camrx rx arm" &&
	run "$B/camid --addr 0x1a --keep --mclk-asis --stream /var/local/cam/imx363-mode8.txt" &&
	run "$B/camrx rx report"
