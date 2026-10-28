#!/bin/sh
# Build wland and adspboot for iOS 16 arm64 into phone/wifi/out (signed ad hoc).
set -e
cd "$(dirname "$0")"
mkdir -p out
../../work/userland/bin/ios-cc -O2 -Wall -Wextra -Wno-unused-parameter -o out/wland wland.c smem.c pmem.c glink.c link.c qmi.c rmtfs.c pdmapper.c tftp.c scm.c wlfw.c radio.c ipnet.c bridge.c
../../tools/linux-ios-sign out/wland 2>/dev/null
../../work/userland/bin/ios-cc -O2 -Wall -Wextra -Wno-unused-parameter -o out/adspboot adspboot.c smem.c pmem.c glink.c
../../tools/linux-ios-sign out/adspboot 2>/dev/null
ls -la out/wland out/adspboot
