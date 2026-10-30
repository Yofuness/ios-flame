#!/bin/bash
# Files "On My iPhone" (B37, docs/features/apps.md; feature under work). fileproviderd refuses to keep its FP
# library on a non-APFS volume (our /private/var is HFSX); phone/tools/fpfix.m swizzles FPDVolume's APFS checks.
# Installs:
#  - phone/tools/out/fpfix.dylib (phone/tools/build.sh) -> /var/local/lib/fpfix.dylib
#  - the launchd job com.apple.FileProvider started through `/bin/sh -c 'DYLD_INSERT_LIBRARIES=/var/local/lib/fpfix.dylib exec
#    fileproviderd'` (launchd drops DYLD_* from a system job's EnvironmentVariables: hp4; exec keeps the pid, so the
#    MachServices check-in still works) (key /System/Library/LaunchDaemons/com.apple.FileProvider.plist, fileproviderd) of the launchd.plist cache;
#    backup /System/Library/xpc/launchd.plist.pre-fpfix (first run only). DYLD_* is allowed by xnu 0114.
#   tools/sunfish-install-fpfix.sh [--undo | --check]
#   --undo: the plain Program back (dylib left); --check: prints the job and the files, no writes
# Takes effect at the next boot (or after fileproviderd restarts).
set -euo pipefail
cd "$(dirname "$0")/.."
F() { python3 -I tools/flame-files.py "$@"; }
S() { tools/sunfish-ssh "$@"; }
U=work/userland LIB=phone/tools/out/fpfix.dylib DST=/var/local/lib/fpfix.dylib
LP=/System/Library/xpc/launchd.plist
KEY=/System/Library/LaunchDaemons/com.apple.FileProvider.plist
MODE=${1:-}
[ -z "$MODE" ] || [ "$MODE" = --undo ] || [ "$MODE" = --check ] || { echo "usage: $0 [--undo | --check]"; exit 1; }
mkdir -p $U/fpfix

# DYLD_INSERT_LIBRARIES in/out of the job (cur -> new); --check: print the job
job() {
	python3 -I - "$U/fpfix/launchd.plist.cur" "$U/fpfix/launchd.plist.new" "$KEY" "$DST" "$1" <<'PY'
import plistlib, sys
cur, new, key, dst, mode = sys.argv[1:]
p = plistlib.load(open(cur, 'rb'))
j = p['LaunchDaemons'].get(key)
if j is None:
    sys.exit('job %s not in the launchd.plist cache' % key)
print('job:', j.get('Label'), j.get('Program', j.get('ProgramArguments')))
env = j.setdefault('EnvironmentVariables', {})
env.pop('DYLD_INSERT_LIBRARIES', None)	# launchd drops DYLD_* for system jobs (hp4): the wrapper sets it instead
args = j.get('ProgramArguments')
wrapped = isinstance(args, list) and args[:2] == ['/bin/sh', '-c'] and ' exec ' in args[2]
if mode == '--check':
    print('EnvironmentVariables:', env, '| wrapped:', wrapped)
    sys.exit(0)
if mode == '--undo':
    if wrapped:
        j['Program'] = args[2].split(' exec ', 1)[1]
        del j['ProgramArguments']
else:
    if not wrapped:
        orig = j.pop('Program')
        j['ProgramArguments'] = ['/bin/sh', '-c', 'DYLD_INSERT_LIBRARIES=%s exec %s' % (dst, orig)]
print('now:', j.get('Program', j.get('ProgramArguments')), env)
plistlib.dump(p, open(new, 'wb'), fmt=plistlib.FMT_BINARY)
PY
}

F get $LP $U/fpfix/launchd.plist.cur >/dev/null

if [ "$MODE" = --check ]; then
	job --check
	S "for f in $DST $LP.pre-fpfix; do [ -e \$f ] && echo \"\$f: present\" || echo \"\$f: absent\"; done"
	exit 0
fi

if [ "$MODE" = --undo ]; then
	job --undo
	F put $U/fpfix/launchd.plist.new $LP 644 >/dev/null
	echo "com.apple.FileProvider back to its plain Program (reboot to take effect)"
	exit 0
fi

[ -s $LIB ] || { echo "missing $LIB: run phone/tools/build.sh"; exit 1; }
S "mkdir -p /var/local/lib"
F put $LIB $DST 755 >/dev/null
echo "installed: $DST"
if ! F get $LP.pre-fpfix $U/fpfix/launchd.plist.pre-fpfix >/dev/null 2>&1; then
	cp $U/fpfix/launchd.plist.cur $U/fpfix/launchd.plist.pre-fpfix
	F put $U/fpfix/launchd.plist.pre-fpfix $LP.pre-fpfix 644 >/dev/null
	echo "backup: $LP.pre-fpfix"
fi
job install
F put $U/fpfix/launchd.plist.new $LP 644 >/dev/null
echo "installed: com.apple.FileProvider through the DYLD_INSERT_LIBRARIES wrapper (reboot to take effect)"
