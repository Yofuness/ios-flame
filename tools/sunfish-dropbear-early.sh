#!/bin/bash
# Start dropbear early at boot: drop its StandardErrorPath (opening /var/local/dropbear.log made launchd exec it
# ~8 s after wland, which has the same location and keys otherwise; 2026-10-28). Backs the plist up on both
# volumes first: /var/local/launchd.plist.pre-early and /private/etc/launchd.plist.pre-early (root, survives
# sweetnofstab=1). Takes effect at the next boot. Undo: put either backup back to /System/Library/xpc/launchd.plist.
#   tools/sunfish-dropbear-early.sh [--keepalive-off]   (also drop KeepAlive, if the first step is not enough)
set -euo pipefail
cd "$(dirname "$0")/.."
F() { python3 -I tools/flame-files.py "$@"; }
U=work/userland
tools/sunfish-ssh 'cp /System/Library/xpc/launchd.plist /var/local/launchd.plist.pre-early && cp /System/Library/xpc/launchd.plist /private/etc/launchd.plist.pre-early && ls -l /var/local/launchd.plist.pre-early /private/etc/launchd.plist.pre-early' </dev/null
F get /System/Library/xpc/launchd.plist $U/launchd.plist.cur
python3 -I - "$U/launchd.plist.cur" "$U/launchd.plist.new" "${1:-}" <<'PY'
import plistlib, sys
raw = open(sys.argv[1], 'rb').read()
p = plistlib.loads(raw)
j = p['LaunchDaemons']['/System/Library/LaunchDaemons/org.iosonpixel.dropbear.plist']
print('before:', sorted(j))
j.pop('StandardErrorPath', None)
if sys.argv[3] == '--keepalive-off':
    j.pop('KeepAlive', None)
print('after: ', sorted(j))
fmt = plistlib.FMT_BINARY if raw.startswith(b'bplist00') else plistlib.FMT_XML
plistlib.dump(p, open(sys.argv[2], 'wb'), fmt=fmt)
PY
F put $U/launchd.plist.new /System/Library/xpc/launchd.plist 644
python3 -I tools/flame-cmd.py sync >/dev/null || true
echo "done; reboot to apply, then: grep -an 'exec wland\|exec dropbear' ../logs/usb-live.log | tail -2"
