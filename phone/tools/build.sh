#!/bin/bash
# Build the on-phone tools into out/ with work/userland/bin/ios-cc, signed ad hoc (with NAME-ents.plist when
# there is one). Install one: python3 tools/flame-files.py put phone/tools/out/NAME /var/local/bin/NAME 755
# (usage is at the top of each source):
#   psx      processes without /proc; psx -k NAME kills     pstack   PID: thread backtraces (tools/flame-profile.py)
#   openapp  BUNDLE_ID [URL]: launch an app                   lsreg    LaunchServices registration (no uicache)
#   netsvc   SystemConfiguration network service for an IF    nettest  DNS, TCP, then HTTPS
#   oslog    stream the unified log                           prefs    CFPreferences read/write (no defaults)
#   imgtest  does ImageIO decode real pixels (HEIC: no)       membw    CPU memory bandwidth
#   vtdecode FILE [SEC]: AVAssetReader/VideoToolbox decode fps, hardware or not
#   expt, exq, runmigrator: one-off probes of ExtensionFoundation and DataMigration (runmigrator needs
#   com.apple.migrationpluginwrapper's entitlements, not in git)
set -euo pipefail
cd "$(dirname "$0")"
CC=../../work/userland/bin/ios-cc
mkdir -p out
build() {	# NAME SOURCE [FLAGS...]
	local name=$1 src=$2
	shift 2
	$CC -O2 -Wall "$@" -o "out/$name" "$src"
	if [ -f "$name-ents.plist" ]; then
		codesign -f -s - --entitlements "$name-ents.plist" "out/$name"
	else
		codesign -f -s - "out/$name"
	fi
}
for c in membw pstack psx; do build $c $c.c; done
for m in expt exq nettest openapp oslog prefs runmigrator; do build $m $m.m -fobjc-arc -framework Foundation; done
build imgtest imgtest.m -fobjc-arc -framework Foundation -framework ImageIO -framework CoreGraphics
build lsreg lsreg.m -fobjc-arc -framework Foundation -framework MobileCoreServices
build netsvc netsvc.m -fobjc-arc -framework Foundation -framework SystemConfiguration
build vtdecode vtdecode.m -fobjc-arc -framework Foundation -framework AVFoundation -framework VideoToolbox -framework CoreMedia -framework CoreVideo
build memwarn memwarn.c
build pmem pmem.c
build a2s a2s.c
ls -l out
