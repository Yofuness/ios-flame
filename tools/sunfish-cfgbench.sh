#!/bin/bash
# Boot a kernel with extra boot-args, then two fpsbench runs (BENCH_TIGHT=1, 20 s): one config of a tuning matrix.
#   tools/sunfish-cfgbench.sh NAME KERNEL [BOOTARGS...]
# Starts from a phone reachable over SSH or in fastboot; screen dimmed to 10% after boot.
cd "$(dirname "$0")/.." || exit 1
name=$1 k=$2; shift 2
if timeout 10 tools/sunfish-ssh true 2>/dev/null; then
	python3 -I tools/flame-cmd.py reboot >/dev/null 2>&1
fi
for i in $(seq 60); do fastboot devices | grep -q fastboot && break; sleep 3; done
v=$(fastboot getvar battery-voltage 2>&1 | head -1 | grep -oE '[0-9]+')
echo "$name: battery $v mV"
[ "${v:-0}" -lt 3600 ] && { echo "battery low: not booting"; exit 1; }
echo "=== $name $(basename "$(dirname "$k")") $* $(date +%T)" >> ../logs/usb-live.log
timeout 150 tools/sunfish-boot.sh "$name" "$k" flamereboot=1800 "$@" >/dev/null 2>&1
for i in $(seq 40); do timeout 10 tools/sunfish-ssh true 2>/dev/null && break; sleep 5; done
sleep 50
[ -n "$DIM" ] && tools/sunfish-dim.sh "$DIM" >/dev/null
BENCH_TIGHT=1 tools/sunfish-fpsbench.sh "$name-a" 20
BENCH_TIGHT=1 tools/sunfish-fpsbench.sh "$name-b" 20
