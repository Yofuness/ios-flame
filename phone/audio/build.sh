#!/bin/sh
# Build SunfishAudio.driver, the CoreAudio HAL plug-in (ios-redfin's RedwoodAudio.driver = HackiOS's
# audio_plugin.c, MIT, LICENSE.HackiOS), into phone/audio/out. Camera stage f2: their camera too (../camera:
# inject_media.c, camera_device.c, camera_isp.c, MIT) with camera_client.c (camd's socket + slots); in mediaserverd it
# does nothing unless /var/local/camdev.on exists (or boot-arg sweetcamdev=1).
# Linux: tools/linux-ios-cc + ldid (like phone/wifi/build.sh). Install: tools/sunfish-install-audio.sh.
set -e
cd "$(dirname "$0")"
CC=${CC:-../../tools/linux-ios-cc}
D=out/SunfishAudio.driver
rm -rf "$D"; mkdir -p "$D"
$CC -O2 -Wall -Wno-unused-parameter -Wno-unused-function -bundle -I../camera -framework CoreFoundation \
	-framework CoreMedia -framework CoreVideo -framework IOSurface -lobjc \
	-o "$D/SunfishAudio" audio_plugin.c ../camera/inject_media.c ../camera/camera_device.c ../camera/camera_isp.c \
	../camera/camera_client.c
cp audio_plugin.plist "$D/Info.plist"
../../tools/linux-ios-sign "$D/SunfishAudio"
llvm-nm -gU "$D/SunfishAudio" | grep -q '_SunfishAudio_Create' || { echo "no SunfishAudio_Create export"; exit 1; }
ls -la "$D"
