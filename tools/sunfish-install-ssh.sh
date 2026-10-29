#!/bin/bash
# Install SSH (dropbear) + dash into the running iOS root over the kernel file
# server (TCP 192.168.7.2:2223, tools/flame-files.py), from Linux.
# Binaries: work/userland/stage (userland.yml artifact). Key generated here.
# launchd reads its job cache at boot: reboot afterwards.
#
#   tools/sunfish-install-ssh.sh
set -euo pipefail
cd "$(dirname "$0")/.."
F="python3 -I tools/flame-files.py"
U=work/userland
[ -f $U/stage/dropbearmulti ] && [ -f $U/stage/dash ] || { echo "missing $U/stage binaries" >&2; exit 1; }
mkdir -p $U/ssh
[ -f $U/ssh/id_ed25519 ] || ssh-keygen -q -t ed25519 -N '' -C sunfish -f $U/ssh/id_ed25519

$F mkdir /var/local 755 || true
$F mkdir /var/local/bin 755 || true
$F mkdir /private/etc/dropbear 700 || true
$F mkdir /var/root/.ssh 700 || true
$F put $U/stage/dropbearmulti /var/local/bin/dropbearmulti 755
$F put $U/stage/dash /var/local/bin/dash 755
for l in dropbear dropbearkey scp dbclient; do
	$F rm /var/local/bin/$l 2>/dev/null || true
	$F link /var/local/bin/dropbearmulti /var/local/bin/$l || true
done
$F link /var/local/bin/dash /bin/sh || echo "(/bin/sh exists)"
if [ -f $U/stage/toybox ]; then
	$F put $U/stage/toybox /var/local/bin/toybox 755
	for a in ls cat cp mv rm mkdir rmdir ln chmod chown ps kill grep sed head tail wc find xargs \
		sort uniq cut tr df du mount uname date env id whoami hexdump od dd sleep touch stat \
		which file tee printf echo true false test basename dirname readlink realpath uptime \
		free top hostname nc ping ifconfig netstat dmesg sync md5sum sha256sum tar gzip less vi; do
		$F link /var/local/bin/toybox /var/local/bin/$a >/dev/null 2>&1 || true
	done
fi
$F put $U/ssh/id_ed25519.pub /var/root/.ssh/authorized_keys 600
printf 'export PATH=/var/local/bin:/usr/bin:/bin:/usr/sbin:/sbin\nexport PS1="sunfish# "\n' > $U/profile
$F put $U/profile /var/root/.profile 644

# launchd job cache: keep the original on the phone and here.
$F get /System/Library/xpc/launchd.plist $U/launchd.plist.cur
[ -f $U/launchd.plist.orig ] || cp $U/launchd.plist.cur $U/launchd.plist.orig
python3 -I - "$U/launchd.plist.cur" "$U/launchd.plist.new" <<'PY'
import plistlib, sys
p = plistlib.load(open(sys.argv[1], 'rb'))
p['LaunchDaemons']['/System/Library/LaunchDaemons/org.iosonpixel.dropbear.plist'] = {
    'Label': 'org.iosonpixel.dropbear',
    # -R: generate host keys on first connection (no -E: syslog off; no -s: passwords compiled out)
    'ProgramArguments': ['/var/local/bin/dropbear', '-F', '-R', '-p', '22'],
    'RunAtLoad': True, 'KeepAlive': True, 'UserName': 'root',
    # Interactive like timed/wifid, which exec in launchd's first daemon second (D2, 2026-10-29)
    'ProcessType': 'Interactive', 'POSIXSpawnType': 'Interactive',
    'EnvironmentVariables': {'PATH': '/var/local/bin:/usr/bin:/bin:/usr/sbin:/sbin'},
    # no StandardErrorPath: opening a file on /var/local made launchd start dropbear ~8 s late (2026-10-28)
}
plistlib.dump(p, open(sys.argv[2], 'wb'), fmt=plistlib.FMT_BINARY)
print('launchd jobs:', len(p['LaunchDaemons']))
PY
$F put $U/launchd.plist.orig /System/Library/xpc/launchd.plist.orig 644 || true
$F put $U/launchd.plist.new /System/Library/xpc/launchd.plist 644
python3 -I tools/flame-cmd.py sync || true
echo "installed; reboot and boot again, then: tools/sunfish-ssh"
