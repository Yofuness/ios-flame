#!/bin/bash
# Apply the on-device changes that had no install script (docs/device-changes.md rows 9, 10, 12, 15-18), on a phone
# running iOS with SSH (after tools/sunfish-install-ssh.sh). Idempotent: run it again after a reinstall or to check.
# Reboot afterwards (launchd reads its job list at boot). Order and the other install scripts: docs/setup.md step 6.
#   tools/sunfish-setup-phone.sh [--no-launchd] [--no-tools] [--no-cocoatop]
# TZ=Area/City overrides the timezone (default America/Toronto).
set -euo pipefail
cd "$(dirname "$0")/.."
F() { python3 -I tools/flame-files.py "$@"; }
S=tools/sunfish-ssh
U=work/userland
TZ_NAME=${TZ:-America/Toronto}
do_launchd=1 do_tools=1 do_cocoatop=1
for a in "$@"; do
	case $a in
	--no-launchd) do_launchd=0 ;;
	--no-tools) do_tools=0 ;;
	--no-cocoatop) do_cocoatop=0 ;;
	*) sed -n '2,6p' "$0"; exit 2 ;;
	esac
done
mkdir -p $U

if [ $do_launchd = 1 ]; then
	# row 9: jobs that crash-loop or need hardware/servers we don't have (B19, apps.md, ssh.md)
	tools/sunfish-launchd-drop.sh com.apple.ReportCrash com.apple.accessibility.axassetsd com.apple.assetsd.nebulad \
		com.apple.corespotlightservice com.apple.driverkitd com.apple.fairplayd.H2 com.apple.lskdd com.apple.searchd \
		com.apple.spotlight.IndexAgent com.apple.voicemail.vmd com.apple.wifip2pd
	# row 10: backboardd paces CA with a timer instead of the missing VBL (smoothness.md)
	F get /System/Library/xpc/launchd.plist $U/launchd.plist.cur >/dev/null
	if python3 -I - "$U/launchd.plist.cur" "$U/launchd.plist.new" <<'PY'
import plistlib, sys
p = plistlib.load(open(sys.argv[1], 'rb'))
for j in p['LaunchDaemons'].values():
    if isinstance(j, dict) and j.get('Label') == 'com.apple.backboardd':
        env = j.setdefault('EnvironmentVariables', {})
        if env.get('CA_FORCE_TIMER') == '1':
            print('backboardd CA_FORCE_TIMER already set'); sys.exit(1)
        env['CA_FORCE_TIMER'] = '1'
        print('backboardd CA_FORCE_TIMER=1')
plistlib.dump(p, open(sys.argv[2], 'wb'), fmt=plistlib.FMT_BINARY)
PY
	then
		$S 'cp /System/Library/xpc/launchd.plist /var/local/launchd.plist.pre-setup' </dev/null
		F put $U/launchd.plist.new /System/Library/xpc/launchd.plist 644 >/dev/null
	fi
fi

if [ $do_tools = 1 ]; then
	# row 12: phone tools from tools/build-userland-linux.sh (wland, adspboot, dropbear have their own installers)
	O=work/userland-linux/out
	for b in a2s camid fcopy i2cprobe lsreg memwarn netsvc nettest openapp oslog pmem prefs pstack psx sctl vcopy vtdecode; do
		[ -f $O/$b ] || { echo "missing $O/$b: run tools/build-userland-linux.sh" >&2; exit 1; }
		F put $O/$b /var/local/bin/$b 755 >/dev/null
	done
	echo "phone tools installed"
fi

if [ $do_cocoatop = 1 ]; then
	# row 15: CocoaTop process viewer (tools/build-cocoatop-linux.sh), registered with lsreg -r (apps.md ct1)
	A=work/cocoatop/CocoaTop.app
	if [ -d $A ]; then
		tar --owner=0 --group=0 -czf - -C work/cocoatop CocoaTop.app |
			$S 'rm -rf /Applications/CocoaTop.app && toybox zcat | toybox tar -xpf - -C /Applications && /var/local/bin/lsreg -r'
		echo "CocoaTop installed"
	else
		echo "skip CocoaTop: no $A (run tools/build-cocoatop-linux.sh)"
	fi
fi

# rows 16-18: timezone, flashlight tile flag, control file for rotation / fake taps / Location Services
$S "toybox ln -sfn /var/db/timezone/zoneinfo/$TZ_NAME /var/db/timezone/localtime && : >> /var/local/sweet-flashlight &&
	[ -e /var/local/sweet-orient ] || echo P > /var/local/sweet-orient; ls -l /var/db/timezone/localtime" </dev/null
python3 -I tools/flame-cmd.py sync >/dev/null 2>&1 || true
echo "done; reboot to apply the launchd changes"
