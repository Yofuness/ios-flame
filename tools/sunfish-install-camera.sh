#!/bin/bash
# Camera for iOS (stage f, docs/features/camera.md; feature under work). Installs:
#  - phone tools camid camclk camrx camd camclient camera_avtest camisp -> /var/local/bin, the IMX363 table
#    work/cam/imx363-mode8.txt -> /var/local/cam/ (from Google's camx module, not in git: made by tools/sunfish-camtab.py)
#  - launchd job org.iosonpixel.camd (`camd --log /var/local/camd.log`, root, RunAtLoad, KeepAlive on crash only,
#    ThrottleInterval 10); backup /System/Library/xpc/launchd.plist.pre-camera (first run only)
#  - /var/local/camdev.on (turns on the camera in SunfishAudio.driver) and /var/local/camdev.log (0666)
#  - StartRecorderServer = true in MediaToolbox's {D22,D221,Default}/SubServerConfiguration.plist (backup .before-camera)
#  - the MobileGestalt cache moved aside (copy kept as com.apple.MobileGestalt.plist.pre-camera) so iOS rebuilds it
#    with the camera ADT flags at the next boot
# Start order: camd need not beat mediaserverd. The plug-in's capture device exists without camd; camd is only
# opened when a stream starts (camera_device.c worker()), and a failed camera_client_open is retried (250 ms
# doubling to 5 s). WARNING (redfin r15a/r15b): camera ADT + rebuilt MobileGestalt cache without a capture device
# left SpringBoard at the boot logo: install SunfishAudio.driver (tools/sunfish-install-audio.sh) first.
#   tools/sunfish-install-camera.sh [--undo | --check]
#   --undo: job removed, camdev.on removed, plists and MobileGestalt cache restored (tools left); boot without CAMERA=1
#   --check: prints the state of all of the above, no writes
set -euo pipefail
cd "$(dirname "$0")/.."
F() { python3 -I tools/flame-files.py "$@"; }
S() { tools/sunfish-ssh "$@"; }
U=work/userland O=work/userland-linux/out TAB=work/cam/imx363-mode8.txt
LP=/System/Library/xpc/launchd.plist
KEY=/System/Library/LaunchDaemons/org.iosonpixel.camd.plist
MT=/System/Library/Frameworks/MediaToolbox.framework
MGD=/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches
MG=$MGD/com.apple.MobileGestalt.plist
TOOLS="camid camclk camrx camd camclient camera_avtest camisp"
MODE=${1:-}
mkdir -p $U/camera

# job in/out of the launchd.plist cache (cur -> new); --check: print it
job() {
	python3 -I - "$U/camera/launchd.plist.cur" "$U/camera/launchd.plist.new" "$KEY" "$1" <<'PY'
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
        'Label': 'org.iosonpixel.camd',
        'ProgramArguments': ['/var/local/bin/camd', '--log', '/var/local/camd.log'],
        'RunAtLoad': True, 'KeepAlive': {'SuccessfulExit': False}, 'ThrottleInterval': 10, 'UserName': 'root',
    }
plistlib.dump(p, open(sys.argv[2], 'wb'), fmt=plistlib.FMT_BINARY)
print('launchd jobs:', len(jobs))
PY
}

# StartRecorderServer in one plist (local copy $1): set / print; keeps the file's format
srs() {
	python3 -I - "$1" "$2" <<'PY'
import plistlib, sys
b = open(sys.argv[1], 'rb').read()
p = plistlib.loads(b)
if sys.argv[2] == '--check':
    print('  StartRecorderServer =', p.get('StartRecorderServer', 'absent'))
    sys.exit(0)
p['StartRecorderServer'] = True
fmt = plistlib.FMT_BINARY if b.startswith(b'bplist') else plistlib.FMT_XML
open(sys.argv[1], 'wb').write(plistlib.dumps(p, fmt=fmt))
PY
}

F get $LP $U/camera/launchd.plist.cur >/dev/null

if [ "$MODE" = --check ]; then
	job --check
	S "for t in $TOOLS; do [ -x /var/local/bin/\$t ] && echo \"tool \$t: ok\" || echo \"tool \$t: MISSING\"; done; \
for f in /var/local/cam/imx363-mode8.txt /var/local/camdev.on /var/local/camdev.log /var/local/camd.log $LP.pre-camera \
$MG $MG.pre-camera /var/run/sunfish-camera.shm /var/run/sunfish-camera.sock; do \
[ -e \$f ] && echo \"\$f: present\" || echo \"\$f: absent\"; done; \
pgrep -l camd || echo 'camd: not running'"
	for d in D22 D221 Default; do
		P=$MT/$d/SubServerConfiguration.plist
		if F get $P $U/camera/ssc-$d.plist >/dev/null 2>&1; then
			echo "$P:"; srs $U/camera/ssc-$d.plist --check
			S "[ -e $P.before-camera ] && echo '  backup .before-camera: present' || echo '  backup .before-camera: absent'"
		else echo "$P: absent"; fi
	done
	exit 0
fi

if [ "$MODE" = --undo ]; then
	job --undo
	F put $U/camera/launchd.plist.new $LP 644 >/dev/null
	echo "org.iosonpixel.camd removed"
	S "rm -f /var/local/camdev.on; echo 'removed /var/local/camdev.on'; \
for d in D22 D221 Default; do P=$MT/\$d/SubServerConfiguration.plist; \
[ -e \$P.before-camera ] && cp -p \$P.before-camera \$P && echo \"restored \$P\"; done; \
if [ -e $MG.pre-camera ]; then cp -p $MG.pre-camera $MG && echo 'restored $MG'; else echo 'no $MG.pre-camera'; fi; sync"
	echo "boot WITHOUT CAMERA=1 next (tools left in /var/local/bin)"
	exit 0
fi
[ -z "$MODE" ] || { echo "usage: $0 [--undo | --check]"; exit 1; }

# 1. tools and the table
for t in $TOOLS; do [ -s $O/$t ] || { echo "missing $O/$t: run tools/build-userland-linux.sh"; exit 1; }; done
if [ ! -s $TAB ]; then
	python3 -I tools/sunfish-camtab.py --out $TAB --mode 8
fi
S "mkdir -p /var/local/bin /var/local/cam"
for t in $TOOLS; do F put $O/$t /var/local/bin/$t 755 >/dev/null; done
F put $TAB /var/local/cam/imx363-mode8.txt 644 >/dev/null
echo "installed: $TOOLS, /var/local/cam/imx363-mode8.txt"

# 2. launchd job
if ! F get $LP.pre-camera $U/camera/launchd.plist.pre-camera >/dev/null 2>&1; then
	cp $U/camera/launchd.plist.cur $U/camera/launchd.plist.pre-camera
	F put $U/camera/launchd.plist.pre-camera $LP.pre-camera 644 >/dev/null
	echo "backup: $LP.pre-camera"
fi
job install
F put $U/camera/launchd.plist.new $LP 644 >/dev/null
echo "installed: org.iosonpixel.camd"

# 3. flag and log
S "touch /var/local/camdev.on /var/local/camdev.log && chmod 666 /var/local/camdev.log && echo 'camdev.on, camdev.log (0666)'"

# 4. StartRecorderServer
for d in D22 D221 Default; do
	P=$MT/$d/SubServerConfiguration.plist
	F get $P $U/camera/ssc-$d.plist >/dev/null 2>&1 || { echo "$P: absent, skipped"; continue; }
	S "[ -e $P.before-camera ] || cp -p $P $P.before-camera"
	srs $U/camera/ssc-$d.plist set
	F put $U/camera/ssc-$d.plist $P 644 >/dev/null
	echo "StartRecorderServer = true: $P"
done

# 5. MobileGestalt cache aside (once: rebuilt at boot with the camera ADT)
S "if [ -e $MG.pre-camera ]; then echo 'MobileGestalt cache: already moved aside once'; \
elif [ -e $MG ]; then cp -p $MG $MG.pre-camera && rm -f $MG && echo 'MobileGestalt cache moved aside (copy: .pre-camera)'; \
else echo 'WARNING: no $MG'; fi; sync"

# 6.
echo "next boot must be: CAMERA=1 tools/sunfish-boot.sh NAME work/kernel111/kernel.development.sweet sweetse9=2 sweetcampmic=2"
