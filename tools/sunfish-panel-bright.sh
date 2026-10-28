#!/bin/bash
# OLED brightness on the Pixel 4a (Samsung SOFEF01, DSI command mode): sends one DCS long write 0x51 (set display
# brightness, 10 bit, big-endian) through the DSI0 command DMA, until the next boot.
#   tools/sunfish-panel-bright.sh [-n] VALUE      (4-511; capped at 0x1ff, ~50%; -n prints the pmem commands only)
# Registers: DSI0 host 0x0ae94000 is a 6G v2.3 controller (VERSION 0x20030000); every Linux dsi.xml offset is
# shifted by +4: CTRL 0x004, CMD_DMA_CTRL 0x03c, DMA_BASE 0x048, DMA_LEN 0x04c, ACK_ERR_STATUS 0x068, TRIG_CTRL
# 0x084, TRIG_DMA 0x090, INTR_CTRL 0x110 (bit0 CMD_DMA_DONE, write 1 to clear; other bits are enables, kept).
# The panel is refreshed by MDP INTF1 autorefresh (0x0ae6bab4 = 0x80000001); it is paused around the DMA so the
# command doesn't collide with a frame, and always restored. The packet (msm dsi_cmd_dma_add format) goes into
# the unused tail of the ABL splash reserve at 0x9d6ff000 (unverified that the DSI DMA can reach it).
# Hazards: never read 0x0ae44000 (hangs the bus); never send 0x53 or any other DCS command.
# All steps run in one remote shell so a dropped link can't leave autorefresh off.
set -e
cd "$(dirname "$0")/.."
dry=0
[ "$1" = -n ] && { dry=1; shift; }
v=${1:?value 4-511}
[[ "$v" =~ ^[0-9]+$ ]] && [ "$v" -ge 4 ] && [ "$v" -le 511 ] || { echo "value must be 4..511" >&2; exit 1; }
hh=$(( v >> 8 )); ll=$(( v & 0xff ))
w0=0xC0390003
w1=$(printf '0x%08X' $(( 0xFF000051 | (hh << 8) | (ll << 16) )))

if [ $dry = 1 ]; then
	cat <<DRY
pmem r 0x0ae94000 2        # VERSION, CTRL
pmem r 0x0ae9403c 1        # CMD_DMA_CTRL
pmem r 0x0ae94084 1        # TRIG_CTRL
pmem r 0x0ae94110 1        # INTR_CTRL
pmem r 0x0ae6bab4 1        # autorefresh (abort unless VERSION=0x20030000, CTRL bit0, autorefresh bit31)
pmem w 0x9d6ff000 $w0
pmem w 0x9d6ff004 $w1   # 51 $(printf %02x $hh) $(printf %02x $ll) ff (read back, abort if different)
pmem w 0x0ae6bab4 0        # autorefresh off; sleep 0.03
pmem w 0x0ae94110 INTR_CTRL|1
pmem w 0x0ae94048 0x9d6ff000
pmem w 0x0ae9404c 8
pmem w 0x0ae94090 1        # trigger
pmem r 0x0ae94110 1        # poll bit0, 20 x 10 ms
pmem r 0x0ae94068 1        # ACK_ERR
pmem w 0x0ae6bab4 AUTOREFRESH_SAVED
pmem w 0x0ae94110 INTR_CTRL_NOW|1
DRY
	exit 0
fi

# Remote script (dash). rd ADDR -> value as 0xXXXXXXXX.
tools/sunfish-ssh "
P=/var/local/bin/pmem
rd() { echo 0x\$(\$P r \$1 1 | cut -d' ' -f2); }
wr() { \$P w \$1 \$2 >/dev/null; }
ver=\$(rd 0x0ae94000); ctrl=\$(rd 0x0ae94004); dmac=\$(rd 0x0ae9403c); trig=\$(rd 0x0ae94084)
intr=\$(rd 0x0ae94110); ar=\$(rd 0x0ae6bab4)
echo \"VERSION \$ver CTRL \$ctrl CMD_DMA_CTRL \$dmac TRIG_CTRL \$trig INTR_CTRL \$intr autorefresh \$ar\"
[ \$((ver)) -eq \$((0x20030000)) ] || { echo 'abort: VERSION'; exit 1; }
[ \$((ar >> 31 & 1)) -eq 1 ] || { echo 'abort: autorefresh off'; exit 1; }
[ \$((ctrl & 1)) -eq 1 ] || { echo 'abort: DSI not enabled'; exit 1; }
wr 0x9d6ff000 $w0; wr 0x9d6ff004 $w1
b0=\$(rd 0x9d6ff000); b1=\$(rd 0x9d6ff004)
[ \$((b0)) -eq \$(($w0)) ] && [ \$((b1)) -eq \$(($w1)) ] || { echo \"abort: packet readback \$b0 \$b1\"; exit 1; }
wr 0x0ae6bab4 0; sleep 0.03
wr 0x0ae94110 \$((\$(rd 0x0ae94110) | 1))
wr 0x0ae94048 0x9d6ff000; wr 0x0ae9404c 8; wr 0x0ae94090 1
done=no; i=0
while [ \$i -lt 20 ]; do
	[ \$((\$(rd 0x0ae94110) & 1)) -eq 1 ] && { done=yes; break; }
	sleep 0.01; i=\$((i + 1))
done
ack=\$(rd 0x0ae94068)
wr 0x0ae6bab4 \$ar
wr 0x0ae94110 \$((\$(rd 0x0ae94110) | 1))
echo \"result: value $v done \$done ack_err \$ack\"
"
