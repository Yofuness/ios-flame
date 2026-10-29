#!/bin/bash
# Bluetooth for iOS (B15, docs/features/bluetooth.md): install btbridge + bluetool-stub and point launchd at them.
#   - /var/local/bin/btbridge, /var/local/bin/bluetool-stub (from tools/build-userland-linux.sh)
#   - launchd.plist (backup /System/Library/xpc/launchd.plist.pre-btbridge, first run only):
#     com.apple.BlueTool ProgramArguments -> [/var/local/bin/bluetool-stub] (MachServices kept);
#     com.apple.bluetoothd env BT_HCI_TRANSPORT=H4, BT_BAUDRATE=115200;
#     job org.iosonpixel.btbridge (RunAtLoad, root; log /var/local/btbridge.log, off: /var/local/btbridge.off)
# Idempotent. Needs btup's firmware in /var/local/fw/bt (tools/sunfish-install-bt.sh). Reboot to apply.
#   tools/sunfish-install-btbridge.sh [--undo]    --undo: put the .pre-btbridge launchd.plist back
set -euo pipefail
cd "$(dirname "$0")/.."
F() { python3 -I tools/flame-files.py "$@"; }
U=work/userland O=work/userland-linux/out
mkdir -p $U
if [ "${1:-}" = --undo ]; then
	F get /System/Library/xpc/launchd.plist.pre-btbridge $U/launchd.plist.pre-btbridge >/dev/null
	F put $U/launchd.plist.pre-btbridge /System/Library/xpc/launchd.plist 644 >/dev/null
	python3 -I tools/flame-cmd.py sync >/dev/null 2>&1 || true
	echo "launchd.plist restored from .pre-btbridge (binaries left in /var/local/bin); reboot to apply"
	exit 0
fi
for b in btbridge bluetool-stub; do
	[ -s $O/$b ] || { echo "missing $O/$b: run tools/build-userland-linux.sh"; exit 1; }
	F put $O/$b /var/local/bin/$b 755 >/dev/null
done
F get /System/Library/xpc/launchd.plist $U/launchd.plist.cur >/dev/null
if ! F get /System/Library/xpc/launchd.plist.pre-btbridge $U/launchd.plist.pre-btbridge >/dev/null 2>&1; then
	cp $U/launchd.plist.cur $U/launchd.plist.pre-btbridge
	F put $U/launchd.plist.pre-btbridge /System/Library/xpc/launchd.plist.pre-btbridge 644 >/dev/null
	echo "backup: /System/Library/xpc/launchd.plist.pre-btbridge"
fi
python3 -I - "$U/launchd.plist.cur" "$U/launchd.plist.new" <<'PY'
import plistlib, sys
p = plistlib.load(open(sys.argv[1], 'rb'))
jobs = p['LaunchDaemons']
by_label = {v.get('Label'): v for v in jobs.values() if isinstance(v, dict)}
bt = by_label.get('com.apple.BlueTool')
if bt is None:
    sys.exit('no com.apple.BlueTool job in launchd.plist')
bt['ProgramArguments'] = ['/var/local/bin/bluetool-stub']
bt.pop('Program', None)
print('com.apple.BlueTool:', bt['ProgramArguments'], 'MachServices', list(bt.get('MachServices', {})))
d = by_label.get('com.apple.bluetoothd')
if d is None:
    sys.exit('no com.apple.bluetoothd job in launchd.plist')
d.setdefault('EnvironmentVariables', {}).update({'BT_HCI_TRANSPORT': 'H4', 'BT_BAUDRATE': '115200'})
print('com.apple.bluetoothd env:', d['EnvironmentVariables'])
jobs['/System/Library/LaunchDaemons/org.iosonpixel.btbridge.plist'] = {
    'Label': 'org.iosonpixel.btbridge',
    'ProgramArguments': ['/var/local/bin/btbridge'],
    'RunAtLoad': True, 'UserName': 'root',
    'EnvironmentVariables': {'PATH': '/var/local/bin:/usr/bin:/bin:/usr/sbin:/sbin'},
}
plistlib.dump(p, open(sys.argv[2], 'wb'), fmt=plistlib.FMT_BINARY)
print('launchd jobs:', len(jobs))
PY
F put $U/launchd.plist.new /System/Library/xpc/launchd.plist 644 >/dev/null
python3 -I tools/flame-cmd.py sync >/dev/null 2>&1 || true
echo "installed btbridge + bluetool-stub; reboot to apply"
