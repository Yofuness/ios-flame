#!/bin/bash
# Build the on-phone tools into out/ with work/userland/bin/ios-cc, signed ad hoc (with NAME-ents.plist when
# there is one). Install one: python3 tools/flame-files.py put phone/tools/out/NAME /var/local/bin/NAME 755
# (usage is at the top of each source):
#   psx      processes without /proc; psx -k NAME kills     pstack   PID: thread backtraces (tools/flame-profile.py)
#   openapp  BUNDLE_ID [URL]: launch an app                   lsreg    LaunchServices registration (no uicache)
#   netsvc   SystemConfiguration network service for an IF    nettest  DNS, TCP, then HTTPS
#   oslog    [-p PID|-n NAME] [-t S] [-g STR] [-l]: unified log (ActivityStream SPI); oslogls = live-store reader
#   prefs    CFPreferences read/write (no defaults)
#   imgtest  does ImageIO decode real pixels (HEIC: no)       membw    CPU memory bandwidth
#   vtdecode FILE [SEC]: AVAssetReader/VideoToolbox decode fps, hardware or not
#   vcopy    SRC DST: tree copy keeping xattrs, ACLs, flags; -l PATH, -c SRC DST compare
#   sctl     NAME[=INT]: read or set a sysctl (no sysctl binary on the phone)
#   fcopy    [-n] SRC DST: fcopyfile(COPYFILE_DATA) data copy (-n: F_NOCACHE), xnu 0198 test
#   jackset  0|1|3: headphone jack state for SunfishAudio.driver (bit 0 plugged, bit 1 mic)
#   mgq      KEY...: MobileGestalt answers                    autobright [0|1]: Auto-Brightness (CoreBrightness)
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
		../../tools/linux-ios-sign "out/$name" "$name-ents.plist"
	else
		../../tools/linux-ios-sign "out/$name"
	fi
}
for c in membw pstack psx vcopy sctl fcopy oslog; do build $c $c.c; done
for m in expt exq nettest openapp oslogls prefs runmigrator; do build $m $m.m -fobjc-arc -framework Foundation; done
build imgtest imgtest.m -fobjc-arc -framework Foundation -framework ImageIO -framework CoreGraphics
build lsreg lsreg.m -fobjc-arc -framework Foundation -framework MobileCoreServices
build netsvc netsvc.m -fobjc-arc -framework Foundation -framework SystemConfiguration
build vtdecode vtdecode.m -fobjc-arc -framework Foundation -framework AVFoundation -framework VideoToolbox -framework CoreMedia -framework CoreVideo
build memwarn memwarn.c
build pmem pmem.c
build pinsample pinsample.c	# sp15: fast read-only register sampler
build camclk camclk.c ../wifi/pmem.c	# camera a2: camcc PLL2 + 24 MHz MCLK
build camrx camrx.c camrx_lib.c ../wifi/pmem.c ../wifi/smmu.c	# camera c1/c2/d: CSID-lite TPG, CSIPHY0 + RX, IFE-lite WM0 capture
build camd camd.c camrx_lib.c ../wifi/pmem.c ../wifi/smmu.c	# camera f1: camera daemon (shm + socket)
build camclient camclient.c	# camera f1: camd test client
build a2s a2s.c
build jackset jackset.c	# headphone jack S4: org.iosonpixel.jack state + post
build mgq mgq.c -framework CoreFoundation	# MobileGestalt answers (ambient light)
build autobright autobright.m -fobjc-arc -framework Foundation	# Auto-Brightness via CoreBrightness's client
ls -l out
