#!/bin/bash
# Haptics for iOS (docs/features/haptics.md, hp3: works with hapticd started by hand). Installs:
#  - phone tools hapticd and drvbuzz -> /var/local/bin (tools/build-userland-linux.sh)
#  - the wrapper /var/local/bin/hapticd-start.sh and the launchd job org.iosonpixel.hapticd (root, RunAtLoad,
#    KeepAlive on failure only, ThrottleInterval 10; log /var/local/hapticd.log, .prev); backup
#    /System/Library/xpc/launchd.plist.pre-haptics (first run only)
#  - the MobileGestalt cache moved aside once (copy kept as com.apple.MobileGestalt.plist.pre-haptics; skipped if
#    that copy exists) so iOS rebuilds it with the ADT /product/haptics node at the next boot (HAPTICS=1)
# Needs SunfishAudio.driver with the Actuator FIFO (tools/sunfish-install-audio.sh) first.
# Off: touch /var/local/haptics.off (hapticd exits 0 at start, launchd leaves it stopped); do that and
# `launchctl stop org.iosonpixel.hapticd` before other SE9 tools (adspboot amp, camid, i2cprobe, sunfish-cam-rx.sh).
#   tools/sunfish-install-haptics.sh [--undo | --check]
#   --undo: job removed, MobileGestalt cache restored from .pre-haptics if present (tools left); boot without HAPTICS=1
#   --check: prints the state of all of the above, no writes
set -euo pipefail
cd "$(dirname "$0")/.."
F() { python3 -I tools/flame-files.py "$@"; }
S() { tools/sunfish-ssh "$@"; }
U=work/userland O=work/userland-linux/out
LP=/System/Library/xpc/launchd.plist
KEY=/System/Library/LaunchDaemons/org.iosonpixel.hapticd.plist
MGD=/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches
MG=$MGD/com.apple.MobileGestalt.plist
DRV=/System/Library/Audio/Plug-Ins/HAL/SunfishAudio.driver
TOOLS="hapticd drvbuzz"
MODE=${1:-}
mkdir -p $U/haptics

# job in/out of the launchd.plist cache (cur -> new); --check: print it
job() {
	python3 -I - "$U/haptics/launchd.plist.cur" "$U/haptics/launchd.plist.new" "$KEY" "$1" <<'PY'
import plistlib, sys
p = plistlib.load(open(sys.argv[1], 'rb'))
jobs = p['LaunchDaemons']
if sys.argv[4] == '--check':
    print('launchd job:', jobs.get(sys.argv[3], 'absent'))
    sys.exit(0)
if sys.argv[4] == '--undo':
    jobs.pop(sys.argv[3], None)
else:
    jobs[sys.argv[3]] = {
        'Label': 'org.iosonpixel.hapticd',
        'ProgramArguments': ['/bin/sh', '-c', '/var/local/bin/hapticd-start.sh'],
        'RunAtLoad': True, 'KeepAlive': {'SuccessfulExit': False}, 'ThrottleInterval': 10, 'UserName': 'root',
    }
plistlib.dump(p, open(sys.argv[2], 'wb'), fmt=plistlib.FMT_BINARY)
print('launchd jobs:', len(jobs))
PY
}

F get $LP $U/haptics/launchd.plist.cur >/dev/null

if [ "$MODE" = --check ]; then
	job --check
	S "for t in $TOOLS hapticd-start.sh; do [ -x /var/local/bin/\$t ] && echo \"tool \$t: ok\" || echo \"tool \$t: MISSING\"; done; \
for f in $DRV /var/run/sunfish-haptic /var/local/haptics.off /var/local/hapticd.log $LP.pre-haptics $MG $MG.pre-haptics; do \
[ -e \$f ] && echo \"\$f: present\" || echo \"\$f: absent\"; done; \
pgrep -l hapticd || echo 'hapticd: not running'; tail -n 3 /var/local/hapticd.log 2>/dev/null || true"
	exit 0
fi

if [ "$MODE" = --undo ]; then
	job --undo
	F put $U/haptics/launchd.plist.new $LP 644 >/dev/null
	echo "org.iosonpixel.hapticd removed"
	S "if [ -e $MG.pre-haptics ]; then cp -p $MG.pre-haptics $MG && echo 'restored $MG'; else echo 'no $MG.pre-haptics'; fi; \
sync 2>/dev/null || true"
	echo "boot WITHOUT HAPTICS=1 next (tools left in /var/local/bin)"
	exit 0
fi
[ -z "$MODE" ] || { echo "usage: $0 [--undo | --check]"; exit 1; }

# 1. SunfishAudio.driver (Actuator FIFO)
if ! S "[ -d $DRV ]"; then
	echo "SunfishAudio.driver missing: installing it (tools/sunfish-install-audio.sh)"
	tools/sunfish-install-audio.sh || { echo "install SunfishAudio.driver first: phone/audio/build.sh + tools/sunfish-install-audio.sh"; exit 1; }
fi

# 2. tools and the wrapper
for t in $TOOLS; do [ -s $O/$t ] || { echo "missing $O/$t: run tools/build-userland-linux.sh"; exit 1; }; done
S "mkdir -p /var/local/bin"
for t in $TOOLS; do F put $O/$t /var/local/bin/$t 755 >/dev/null; done
cat >$U/haptics/hapticd-start.sh <<'SH'
#!/bin/sh
# org.iosonpixel.hapticd (tools/sunfish-install-haptics.sh): iOS haptics. Off: /var/local/haptics.off (hapticd exits 0)
[ -f /var/local/hapticd.log ] && mv -f /var/local/hapticd.log /var/local/hapticd.log.prev
exec /var/local/bin/hapticd >/var/local/hapticd.log 2>&1
SH
F put $U/haptics/hapticd-start.sh /var/local/bin/hapticd-start.sh 755 >/dev/null
echo "installed: $TOOLS, hapticd-start.sh"

# 3. launchd job
if ! F get $LP.pre-haptics $U/haptics/launchd.plist.pre-haptics >/dev/null 2>&1; then
	cp $U/haptics/launchd.plist.cur $U/haptics/launchd.plist.pre-haptics
	F put $U/haptics/launchd.plist.pre-haptics $LP.pre-haptics 644 >/dev/null
	echo "backup: $LP.pre-haptics"
fi
job install
F put $U/haptics/launchd.plist.new $LP 644 >/dev/null
echo "installed: org.iosonpixel.hapticd"

# 4. MobileGestalt cache aside (once: rebuilt at boot with the ADT /product/haptics)
S "if [ -e $MG.pre-haptics ]; then echo 'MobileGestalt cache: already moved aside once (.pre-haptics exists)'; \
elif [ -e $MG ]; then cp -p $MG $MG.pre-haptics && rm -f $MG && echo 'MobileGestalt cache moved aside (copy: .pre-haptics)'; \
else echo 'WARNING: no $MG'; fi; sync 2>/dev/null || true"

# 5.
C=; S "[ -e /var/local/camdev.on ]" 2>/dev/null && C="CAMERA=1 "
echo "next boot must be: HAPTICS=1 ${C}tools/sunfish-boot.sh NAME KERNEL sweetse9=2 sweetcampmic=2"
