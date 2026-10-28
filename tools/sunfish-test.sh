#!/bin/bash
# One phone test from start to end: reboot to fastboot, battery check, boot, wait for SSH, dim, run CMD.
#   tools/sunfish-test.sh [-b BUNDLEDIR] [-k KERNEL_NN|PATH] [-n] TAG [bootargs...] [-- CMD...]
#   -k  kernel number (work/kernelNN) or a kernel file; default: the newest work/kernelNN
#   -b  install this Metal bundle (tools/sunfish-metal2-install.sh DIR --no-restart) before CMD
#   -n  no reboot: run CMD on the current boot
# CMD runs with TAG exported. Markers go into ../logs/usb-live.log (=== TAG ...).
# Unreachable during CMD or no SSH: rescue boot (TAG-r) with the same kernel, ../logs/TAG-rescue.txt, exit 3.
# Exit: CMD's rc, 2 = setup failed (battery, args), 3 = phone lost.
set -u
cd "$(dirname "$0")/.." || exit 2
ROOT=$PWD LOG=$ROOT/../logs/usb-live.log
bundle="" kern="" noreboot=0
while getopts "b:k:n" o; do
	case $o in
	b) bundle=$OPTARG ;; k) kern=$OPTARG ;; n) noreboot=1 ;;
	*) sed -n '2,9p' "$0"; exit 2 ;;
	esac
done
shift $((OPTIND - 1))
[ $# -ge 1 ] || { sed -n '2,9p' "$0"; exit 2; }
TAG=$1; shift
bootargs=() cmd=()
while [ $# -gt 0 ]; do
	[ "$1" = -- ] && { shift; cmd=("$@"); break; }
	bootargs+=("$1"); shift
done
export TAG

if [ -z "$kern" ]; then
	kern=$(ls -d work/kernel[0-9]* 2>/dev/null | sed 's/.*kernel//' | sort -n | tail -1)
fi
case $kern in
*/*) kpath=$kern ;;
*) kpath=$ROOT/work/kernel$kern/kernel.development.sweet ;;
esac
[ $noreboot = 1 ] || [ -f "$kpath" ] || { echo "test $TAG: no kernel $kpath"; exit 2; }

T0=$(date +%s) tboot=0 tssh=0
say() { echo "$(date +%T) test $TAG: $*"; }
mark() { echo "=== $TAG $* $(date '+%F %T')" >> "$LOG"; }
# sunfish-ssh puts its own options first; a 2 s connect timeout needs ssh directly.
sshok() {
	ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=2 \
		-o BatchMode=yes -i "$ROOT/work/userland/ssh/id_ed25519" root@192.168.7.2 true </dev/null >/dev/null 2>&1
}
infb() { fastboot devices 2>/dev/null | grep -q fastboot; }
waitssh() { # max 180 s, 1 s polls; sets tssh
	local s=$(date +%s)
	for _ in $(seq 180); do
		sshok && { tssh=$(( $(date +%s) - s )); return 0; }
		sleep 1
	done
	return 1
}
summary() { say "rc $1, boot $tboot s, ssh $tssh s, total $(( $(date +%s) - T0 )) s"; }

rescue() { # phone lost: boot TAG-r with the same kernel, save the RAM-log rescue
	say "phone unreachable; rescue"
	mark "lost"
	local i
	for i in $(seq 60); do infb && break; sleep 2; done
	if ! infb; then say "not in fastboot after 120 s; giving up (check by hand)"; summary 3; exit 3; fi
	mark "rescue start"
	"$ROOT/tools/sunfish-boot.sh" "$TAG-r" "$kpath" "${bootargs[@]}" >/dev/null 2>&1
	if ! waitssh; then say "rescue boot: no SSH after 180 s"; summary 3; exit 3; fi
	local out=$ROOT/../logs/$TAG-rescue.txt raw=$ROOT/../logs/$TAG-rescue.hex
	mkdir -p "$ROOT/logs"
	"$ROOT/tools/sunfish-ssh" '/var/local/bin/toybox xxd -s 117670649856 -l 262144 -p /dev/rufsroot0' </dev/null > "$raw"
	python3 -I - "$raw" "$out" <<'EOF'
import struct, sys
raw = bytes.fromhex(open(sys.argv[1]).read().replace('\n', ''))
if raw[:8] != b'SWEETRSC':
    open(sys.argv[2], 'w').write('no SWEETRSC magic\n'); sys.exit(0)
total, n = struct.unpack_from('<QI', raw, 8)
open(sys.argv[2], 'wb').write(raw[4096:4096 + n])
EOF
	mark "rescue saved"
	say "rescue log $out (last 30 lines):"
	tail -n 30 "$out"
	summary 3; exit 3
}

if [ $noreboot = 0 ]; then
	mark "start"
	if ! infb; then
		if sshok; then python3 -I tools/flame-cmd.py reboot >/dev/null 2>&1; fi
		for _ in $(seq 60); do infb && break; sleep 2; done
		infb || { say "phone not in fastboot after 120 s"; summary 2; exit 2; }
	fi
	# Only here: fastboot getvar is never run while iOS is up.
	mv=$(fastboot getvar battery-voltage 2>&1 | sed -n 's/.*battery-voltage: *\([0-9]*\).*/\1/p' | head -1)
	say "battery ${mv:-?} mV"
	mark "battery ${mv:-?} mV"
	if [ -z "$mv" ] || [ "$mv" -lt 3600 ]; then say "battery below 3600 mV or unknown; stop"; summary 2; exit 2; fi
	tb=$(date +%s)
	mark "boot $(basename "$(dirname "$kpath")") ${bootargs[*]}"
	tools/sunfish-boot.sh "$TAG" "$kpath" "${bootargs[@]}" >/dev/null 2>&1 \
		|| { say "sunfish-boot.sh failed"; summary 2; exit 2; }
	tboot=$(( $(date +%s) - tb ))
	say "booted in $tboot s"
	waitssh || rescue
	say "ssh up after $tssh s"
	mark "ssh up $tssh s"
	tools/sunfish-dim.sh ${DIM:-10} >/dev/null 2>&1 && mark "dim ${DIM:-10}"
else
	sshok || { say "-n: no SSH"; rescue; }
fi

if [ -n "$bundle" ]; then
	tools/sunfish-metal2-install.sh "$bundle" --no-restart >/dev/null 2>&1 \
		&& { say "bundle installed"; mark "bundle $bundle"; } || say "bundle install failed"
fi

rc=0
if [ ${#cmd[@]} -gt 0 ]; then
	mark "cmd ${cmd[*]}"
	"${cmd[@]}" & cpid=$!
	lost=0 miss=0
	while kill -0 $cpid 2>/dev/null; do
		sleep 5
		if infb; then lost=1; break; fi
		if sshok; then miss=0; else miss=$((miss + 1)); fi
		[ $miss -ge 6 ] && { lost=1; break; } # ~40 s without SSH
	done
	if [ $lost = 1 ]; then
		kill $cpid 2>/dev/null; wait $cpid 2>/dev/null
		mark "end lost"
		rescue
	fi
	wait $cpid; rc=$?
fi
mark "end $rc"
summary $rc
exit $rc
