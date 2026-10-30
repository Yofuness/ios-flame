#!/bin/bash
# Auto-rotate at boot (docs/features/sensors.md): install adspboot, the wrapper /var/local/bin/sensors-start.sh and
# the launchd job org.iosonpixel.sensors (RunAtLoad, no KeepAlive, root). The wrapper waits until uptime >= 25 s,
# then runs `adspboot boot --rpc --root --sensors --unsafe --sns-motion --sns-rot --sns-secs 0 --watch 0` (0 = no
# limit), log /var/local/sensors.log (previous one -> .prev). adspboot writes "R N" to /var/local/sweet-orient,
# which SweetMetal2 applies. Needs kernel107+ booted with sweetmotion=0x37 (tools/sunfish-boot.sh default).
# Sensor files for the DSP: tools/sunfish-soc0-files.sh is run here; the configs are pushed with the commands
# tools/sunfish-sensors-files.sh prints (setup.md step 8).
# Off: touch /var/local/sensors.off (next boot). Audio tests that run adspboot by hand need
# `touch /var/local/sensors.off` and a reboot first: only one adspboot may own the ADSP.
# Backup /System/Library/xpc/launchd.plist.pre-sensors (first run only). Idempotent. Reboot to apply.
#   tools/sunfish-install-sensors.sh [--undo]    --undo: remove the job from launchd.plist (files left)
set -euo pipefail
cd "$(dirname "$0")/.."
F() { python3 -I tools/flame-files.py "$@"; }
U=work/userland O=work/userland-linux/out
KEY=/System/Library/LaunchDaemons/org.iosonpixel.sensors.plist
mkdir -p $U
F get /System/Library/xpc/launchd.plist $U/launchd.plist.cur >/dev/null
if [ "${1:-}" != --undo ]; then
	[ -s $O/adspboot ] || { echo "missing $O/adspboot: run tools/build-userland-linux.sh"; exit 1; }
	F put $O/adspboot /var/local/bin/adspboot 755 >/dev/null
	cat >$U/sensors-start.sh <<'SH'
#!/bin/sh
# org.iosonpixel.sensors (tools/sunfish-install-sensors.sh): auto-rotate feed. Off: /var/local/sensors.off
[ -e /var/local/sensors.off ] && exit 0
b=$(sysctl -n kern.boottime 2>/dev/null | sed 's/^{ sec = \([0-9]*\).*/\1/')
case "$b" in ''|*[!0-9]*) sleep 25 ;; *) up=$(( $(date +%s) - b )); [ $up -lt 25 ] && sleep $(( 25 - up )) ;; esac
# B46 DeviceMotion fusion (kernel124+ with sweetmotion|0x80): on with /var/local/sensors.devmotion
[ -e /var/local/sensors.devmotion ] && export SNS_DEVMOTION=1
[ -f /var/local/sensors.log ] && mv -f /var/local/sensors.log /var/local/sensors.log.prev
exec /var/local/bin/adspboot boot --rpc --root --sensors --unsafe --sns-motion --sns-rot --sns-secs 0 --watch 0 \
	>/var/local/sensors.log 2>&1
SH
	F put $U/sensors-start.sh /var/local/bin/sensors-start.sh 755 >/dev/null
	tools/sunfish-ssh "touch /var/local/sensors.devmotion"	# B46 Compass fusion on (kernel126+ boots sweetmotion=0x4bf)
	tools/sunfish-soc0-files.sh
	tools/sunfish-sensors-files.sh | sed -n '/^== sensors config/,/^== DSP/p' | grep -v '^== DSP'
	echo "(push the sensors config above once if /var/local/fw/sensors is empty)"
	if ! F get /System/Library/xpc/launchd.plist.pre-sensors $U/launchd.plist.pre-sensors >/dev/null 2>&1; then
		cp $U/launchd.plist.cur $U/launchd.plist.pre-sensors
		F put $U/launchd.plist.pre-sensors /System/Library/xpc/launchd.plist.pre-sensors 644 >/dev/null
		echo "backup: /System/Library/xpc/launchd.plist.pre-sensors"
	fi
fi
python3 -I - "$U/launchd.plist.cur" "$U/launchd.plist.new" "$KEY" "${1:-}" <<'PY'
import plistlib, sys
p = plistlib.load(open(sys.argv[1], 'rb'))
jobs = p['LaunchDaemons']
if sys.argv[4] == '--undo':
    jobs.pop(sys.argv[3], None)
else:
    jobs[sys.argv[3]] = {
        'Label': 'org.iosonpixel.sensors',
        'ProgramArguments': ['/bin/sh', '-c', '/var/local/bin/sensors-start.sh'],
        'RunAtLoad': True, 'KeepAlive': False, 'UserName': 'root',
        'EnvironmentVariables': {'PATH': '/var/local/bin:/usr/bin:/bin:/usr/sbin:/sbin'},
    }
plistlib.dump(p, open(sys.argv[2], 'wb'), fmt=plistlib.FMT_BINARY)
print('launchd jobs:', len(jobs))
PY
F put $U/launchd.plist.new /System/Library/xpc/launchd.plist 644 >/dev/null
python3 -I tools/flame-cmd.py sync >/dev/null 2>&1 || true
[ "${1:-}" = --undo ] && echo "org.iosonpixel.sensors removed (files left); reboot to apply" ||
	echo "installed: org.iosonpixel.sensors; reboot (with sweetmotion=0x37): rotation feed starts ~25 s after boot"
