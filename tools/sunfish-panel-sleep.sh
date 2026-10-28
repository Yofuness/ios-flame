#!/bin/bash
# Panel off/on test for the Pixel 4a OLED (Samsung SOFEF01, DSI command mode): DCS short writes through the DSI0
# command DMA, same path as tools/sunfish-panel-bright.sh (read its header for the registers).
#   tools/sunfish-panel-sleep.sh [-n] off|on|cycle      (-n prints the packet words only)
#   off   = pause autorefresh (value saved in /var/local/panel-ar.save), 28 (display off), 20 ms, 10 (sleep in), 120 ms;
#           autorefresh stays paused (a frame to a sleeping panel is pointless).
#   on    = 11 (sleep out), 120 ms, 29 (display on), 20 ms, restore autorefresh. Always sends both and always restores,
#           even after a half-failed off (no save file -> the known boot value 0x80000001).
#   cycle = off, sleep 3, on.
# Packet header (one 32-bit word, little-endian in RAM): the existing long-write word 0xC0390003 follows the
# downstream mdss/msm DSI command-DMA header: bit31 LAST, bit30 LONG_PKT, bits23:22 VC, bits21:16 DTYPE,
# bits15:0 word count (long) or DATA1 in 7:0 / DATA2 in 15:8 (short); the host adds the ECC. So
# 0xC0390003 = LAST|LONG|DTYPE 0x39 (DCS long write)|WC 3. A DCS short write without parameter is DTYPE 0x05 with
# LONG clear (setting it would make the host send a long packet with WC = cmd): 0x80050000 | cmd, e.g. 0x80050028.
# DMA_LEN = 4 (the header alone; msm pads packets to 4 bytes). PANEL_DMA_LEN=8 retries with an 8-byte transfer
# (second word 0xFFFFFFFF filler, like the msm pad bytes) if 4 never completes.
# Hazards: only opcodes 28/10/11/29; never 0x53 or anything else; never touch 0x0ae44000 (hangs the bus).
# Recovery if the screen stays black: tools/sunfish-panel-sleep.sh on; else python3 -I tools/flame-cmd.py reboot
# (the panel re-inits at boot).
set -e
cd "$(dirname "$0")/.."
dry=0
[ "$1" = -n ] && { dry=1; shift; }
mode=${1:?off|on|cycle}
len=${PANEL_DMA_LEN:-4}
[ "$len" = 4 ] || [ "$len" = 8 ] || { echo "PANEL_DMA_LEN must be 4 or 8" >&2; exit 1; }
case $mode in
off|on) ;;
cycle) "$0" off || { "$0" on; exit 1; }; sleep 3; exec "$0" on ;;
*) echo "usage: $0 [-n] off|on|cycle" >&2; exit 1 ;;
esac
if [ $dry = 1 ]; then
	for c in 28 10 11 29; do printf 'DCS %s: header 0x%08X, DMA_LEN %s\n' $c $(( 0x80050000 | 0x$c )) $len; done
	exit 0
fi

# Remote script (dash). send CMD -> "dcs CMD done yes|no t_ms N ack_err 0x..", returns 1 on timeout.
tools/sunfish-ssh "
P=/var/local/bin/pmem
S=/var/local/panel-ar.save
rd() { echo 0x\$(\$P r \$1 1 | cut -d' ' -f2); }
wr() { \$P w \$1 \$2 >/dev/null; }
send() {
	case \$1 in 28|10|11|29) ;; *) echo \"refused opcode \$1\"; return 1 ;; esac
	w=\$(printf '0x%08X' \$((0x80050000 | 0x\$1)))
	wr 0x9d6ff000 \$w; wr 0x9d6ff004 0xFFFFFFFF
	[ \$((\$(rd 0x9d6ff000))) -eq \$((w)) ] || { echo \"dcs \$1 packet readback failed\"; return 1; }
	wr 0x0ae94110 \$((\$(rd 0x0ae94110) | 1))
	wr 0x0ae94048 0x9d6ff000; wr 0x0ae9404c $len; wr 0x0ae94090 1
	done=no; i=0
	while [ \$i -lt 20 ]; do
		[ \$((\$(rd 0x0ae94110) & 1)) -eq 1 ] && { done=yes; break; }
		sleep 0.01; i=\$((i + 1))
	done
	echo \"dcs \$1 header \$w done \$done t_ms <=\$((i * 10)) ack_err \$(rd 0x0ae94068)\"
	wr 0x0ae94110 \$((\$(rd 0x0ae94110) | 1))
	[ \$done = yes ]
}
ver=\$(rd 0x0ae94000); ctrl=\$(rd 0x0ae94004); ar=\$(rd 0x0ae6bab4)
echo \"VERSION \$ver CTRL \$ctrl autorefresh \$ar\"
[ \$((ver)) -eq \$((0x20030000)) ] || { echo 'abort: VERSION'; exit 1; }
[ \$((ctrl & 1)) -eq 1 ] || { echo 'abort: DSI not enabled'; exit 1; }
if [ $mode = off ]; then
	[ \$((ar >> 31 & 1)) -eq 1 ] || { echo 'abort: autorefresh already off (run on first)'; exit 1; }
	echo \$ar > \$S
	wr 0x0ae6bab4 0; sleep 0.03; echo 'autorefresh paused'
	send 28 || { wr 0x0ae6bab4 \$ar; echo 'timeout: autorefresh restored'; exit 1; }
	sleep 0.02
	send 10 || { wr 0x0ae6bab4 \$ar; echo 'timeout: autorefresh restored'; exit 1; }
	sleep 0.12
	echo 'off: done, autorefresh left paused'
else
	sv=\$(cat \$S 2>/dev/null || true)
	[ \$((sv >> 31 & 1)) -eq 1 ] 2>/dev/null || sv=0x80000001
	wr 0x0ae6bab4 0; sleep 0.03
	rc=0
	send 11 || rc=1
	sleep 0.12
	send 29 || rc=1
	sleep 0.02
	wr 0x0ae6bab4 \$sv; rm -f \$S
	echo \"on: autorefresh restored to \$sv (rc \$rc)\"
	exit \$rc
fi
"
