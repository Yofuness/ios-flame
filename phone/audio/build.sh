#!/bin/sh
# Build SunfishAudio.driver, the CoreAudio HAL plug-in (ios-redfin's RedwoodAudio.driver = HackiOS's
# audio_plugin.c, MIT, LICENSE.HackiOS; only the plug-in, not their camera code), into phone/audio/out.
# Linux: tools/linux-ios-cc + ldid (like phone/wifi/build.sh). Install: tools/sunfish-install-audio.sh.
set -e
cd "$(dirname "$0")"
CC=${CC:-../../tools/linux-ios-cc}
D=out/SunfishAudio.driver
rm -rf "$D"; mkdir -p "$D"
$CC -O2 -Wall -Wno-unused-parameter -Wno-unused-function -bundle -framework CoreFoundation \
	-o "$D/SunfishAudio" audio_plugin.c
cp audio_plugin.plist "$D/Info.plist"
../../tools/linux-ios-sign "$D/SunfishAudio"
llvm-nm -gU "$D/SunfishAudio" | grep -q '_SunfishAudio_Create' || { echo "no SunfishAudio_Create export"; exit 1; }
ls -la "$D"
