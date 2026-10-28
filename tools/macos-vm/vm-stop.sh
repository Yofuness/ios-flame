#!/bin/bash
# Stop the build VM and free its 8 GB. Default: hibernate (RAM state to state.zst; vm-start.sh resumes from it).
#   vm-stop.sh --shutdown   a real macOS shutdown instead (next start is a full boot)
# VM data (disk, state, keys, credentials) lives outside git, in $MACOS_VM_DIR
D=${MACOS_VM_DIR:-$HOME/VMs/macos}
running() { kill -0 "$(cat "$D/qemu.pid" 2>/dev/null)" 2>/dev/null; }
running || { echo "not running"; exit 0; }
if [ "${1:-}" != --shutdown ] && "$D/vmstate.py" save; then exit 0; fi
rm -f "$D/state.zst.tmp"
"$D/vssh" 'sudo shutdown -h now' </dev/null >/dev/null 2>&1
for i in $(seq 60); do running || { echo stopped; exit 0; }; sleep 2; done
echo "not down after 120 s; sending SIGTERM"; kill "$(cat "$D/qemu.pid")"
