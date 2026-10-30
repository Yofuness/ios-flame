#!/bin/bash
# Install Filza File Manager (TIGI Software, the user's request 2026-10-30, for its music import) as its own .deb's
# postinst does: /Applications/Filza.app (root:admin), /usr/libexec/filza/{Filza (setuid), FilzaHelper,
# FilzaWebDAVServer}, /usr/bin/Filza -> /usr/libexec/filza/Filza, and the helper job com.tigisoftware.filza.helper
# (Mach service com.tigisoftware.filza.helper.xpc) added to launchd's job cache (the phone reads launchd.plist, not
# /Library/LaunchDaemons). Then lsreg -r registers the app. Reboot for the helper job.
# The package is not in git (third-party): tools/sunfish-install-filza.sh [DEB]; default DEB is downloaded once from
#   https://tigisoftware.com/cydia/com.tigisoftware.filza64bit_4.0.1-4_iphoneos-arm.deb (8701068 bytes,
#   sha256 1a20cd1ca85ac0f5d7069d548438b4efa82ecce5d7bd640b3a7f07af1f5faca0) into research/re/filza/dl/.
#   --undo removes the files and the job (backup of launchd.plist: /System/Library/xpc/launchd.plist.pre-filza).
set -euo pipefail
cd "$(dirname "$0")/.."
F() { python3 -I tools/flame-files.py "$@"; }
S=tools/sunfish-ssh
U=work/userland
URL=https://tigisoftware.com/cydia/com.tigisoftware.filza64bit_4.0.1-4_iphoneos-arm.deb
SHA=1a20cd1ca85ac0f5d7069d548438b4efa82ecce5d7bd640b3a7f07af1f5faca0
mkdir -p $U research/re/filza/dl

edit_jobs() {	# $1 = add|remove
	F get /System/Library/xpc/launchd.plist $U/launchd.plist.cur >/dev/null
	$S '[ -e /System/Library/xpc/launchd.plist.pre-filza ] || cp /System/Library/xpc/launchd.plist /System/Library/xpc/launchd.plist.pre-filza' </dev/null
	python3 -I - "$U/launchd.plist.cur" "$U/launchd.plist.new" "$1" <<'PY'
import plistlib, sys
p = plistlib.load(open(sys.argv[1], 'rb'))
k = '/Library/LaunchDaemons/com.tigisoftware.filza.helper.plist'
if sys.argv[3] == 'add':
    p['LaunchDaemons'][k] = {'Label': 'com.tigisoftware.filza.helper', 'ProgramArguments': ['/usr/libexec/filza/FilzaHelper'],
                             'UserName': 'root', 'MachServices': {'com.tigisoftware.filza.helper.xpc': True}}
else:
    p['LaunchDaemons'].pop(k, None)
plistlib.dump(p, open(sys.argv[2], 'wb'), fmt=plistlib.FMT_BINARY)
print('launchd jobs:', len(p['LaunchDaemons']))
PY
	F put $U/launchd.plist.new /System/Library/xpc/launchd.plist 644 >/dev/null
}

if [ "${1:-}" = --undo ]; then
	edit_jobs remove
	$S 'rm -rf /Applications/Filza.app /usr/libexec/filza /usr/bin/Filza; /var/local/bin/lsreg -r' </dev/null
	python3 -I tools/flame-cmd.py sync >/dev/null 2>&1 || true
	echo "Filza removed; reboot"
	exit 0
fi

DEB=${1:-research/re/filza/dl/filza.deb}
[ -f "$DEB" ] || curl -sSfL -o "$DEB" "$URL"
echo "$SHA  $DEB" | sha256sum -c - >/dev/null || { echo "sha256 mismatch: $DEB" >&2; exit 1; }
X=$(mktemp -d); trap 'rm -rf "$X"' EXIT
(cd "$X" && ar x "$OLDPWD/$DEB" && mkdir root && tar -xzf data.tar.gz -C root)
tar --owner=0 --group=0 -czf - -C "$X/root" Applications usr Library |
	$S 'toybox zcat | toybox tar -xpf - -C / && chown -R root:admin /Applications/Filza.app &&
	    chown root /usr/libexec/filza/FilzaHelper /usr/libexec/filza/Filza && chmod ug+s /usr/libexec/filza/Filza &&
	    rm -rf /usr/bin/Filza && ln -s /usr/libexec/filza/Filza /usr/bin/Filza && /var/local/bin/lsreg -r && ls -la /usr/libexec/filza'
edit_jobs add
python3 -I tools/flame-cmd.py sync >/dev/null 2>&1 || true
echo "Filza installed; reboot so launchd starts com.tigisoftware.filza.helper"
