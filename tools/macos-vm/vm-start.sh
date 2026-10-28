#!/bin/bash
# Start the build VM in the background: resume from state.zst (saved by vm-stop.sh) when there is one, else boot.
# Then set the VNC password, pin the vCPUs, set the guest clock, wait for SSH.
# VM data (disk, state, keys, credentials) lives outside git, in $MACOS_VM_DIR
D=${MACOS_VM_DIR:-$HOME/VMs/macos}
[ -f "$D/qemu.pid" ] && kill -0 "$(cat "$D/qemu.pid")" 2>/dev/null && { echo "already running"; exit 0; }
rm -f "$D/qmp.sock" "$D/mon.sock"
t0=$(date +%s.%N)
mode=boot
[ -f "$D/state.zst" ] && mode=resume
setsid "$D/mac-vm.sh" $([ $mode = resume ] && echo resume) > "$D/vm.log" 2>&1 < /dev/null &
for i in $(seq 300); do [ -S "$D/mon.sock" ] && [ -S "$D/qmp.sock" ] && break; sleep 0.1; done
if [ $mode = resume ]; then
	if "$D/vmstate.py" wait; then
		# the saved state belongs to the disk as it was at save time: never resume from it twice
		rm -f "$D/state.zst"
	else
		echo "resume failed; booting instead"; mv "$D/state.zst" "$D/state.zst.bad"
		kill "$(cat "$D/qemu.pid")" 2>/dev/null; sleep 2; exec "$0" "$@"
	fi
fi
pw=$(sed -n 's/^vnc=//p' "$D/credentials")
printf 'set_password vnc %s\n' "$pw" | socat -t2 - UNIX-CONNECT:"$D/mon.sock" >/dev/null 2>&1
"$D/vm-pin.py" >/dev/null 2>&1 || echo "vCPU pinning failed"
if [ $mode = boot ]; then
	# OpenCore's picker waits on "EFI" (its disk is a snapshot, so no default sticks): pick Macintosh HD
	sleep 12; "$D/vmctl.py" key right >/dev/null; sleep 1; "$D/vmctl.py" key ret >/dev/null
fi
for i in $(seq 600); do
	if "$D/vssh" -o ConnectTimeout=2 true 2>/dev/null </dev/null; then
		# after a resume the guest clock is behind by the time it was saved: set it from the host
		"$D/vssh" "sudo date -u $(date -u +%m%d%H%M%Y.%S) >/dev/null" </dev/null
		awk -v m=$mode -v a="$t0" -v b="$(date +%s.%N)" 'BEGIN{printf "VM up (%s) after %.1f s\n", m, b-a}'; exit 0
	fi
	sleep 1
done
echo "VM started ($mode), ssh not answering yet"
