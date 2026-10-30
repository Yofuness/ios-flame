#!/bin/bash
# Camera.app defaults for the port: Live Photo off and kept off. With Live Photo on, mediaserverd piles up ~35 MB
# allocations (33 GB in 30 s, MALLOC_LARGE) until the kernel kills it for low swap: photos fail, the session restarts
# (docs/features/camera.md mem1/lp1). Iris = Live Photo: CAMUserPreferenceDesiredIrisMode 0 = off.
#   tools/sunfish-camera-prefs.sh           writes /var/mobile/Library/Preferences/com.apple.camera.plist (backup .bak-lp)
set -euo pipefail
cd "$(dirname "$0")/.."
F() { python3 -I tools/flame-files.py "$@"; }
P=/var/mobile/Library/Preferences/com.apple.camera.plist
T=work/cam/com.apple.camera.plist
mkdir -p work/cam
tools/sunfish-ssh "c=\$(/bin/ps ax | grep '[/]Camera.app/Camera' | sed 's/^ *//' | cut -d' ' -f1); [ -n \"\$c\" ] && kill -9 \$c; p=\$(/bin/ps ax | grep '[c]fprefsd daemon' | sed 's/^ *//' | cut -d' ' -f1); [ -n \"\$p\" ] && kill \$p; sleep 1; [ -e $P.bak-lp ] || cp $P $P.bak-lp"
F get $P $T >/dev/null
python3 -I - "$T" <<'PY'
import plistlib, sys
p = plistlib.load(open(sys.argv[1], 'rb'))
p['CAMUserPreferenceDesiredIrisMode'] = 0
p['CAMUserPreferencePreserveLivePhoto'] = True
plistlib.dump(p, open(sys.argv[1], 'wb'), fmt=plistlib.FMT_BINARY)
print('Iris', p['CAMUserPreferenceDesiredIrisMode'], 'preserve', p['CAMUserPreferencePreserveLivePhoto'])
PY
F put $T $P 600 >/dev/null
tools/sunfish-ssh "chown mobile:mobile $P; p=\$(/bin/ps ax | grep '[c]fprefsd' | sed 's/^ *//' | cut -d' ' -f1); for x in \$p; do kill \$x; done; true"
echo "Camera prefs written: Live Photo off and preserved"
