#!/bin/sh
# Build wland, adspboot, aprtest and apr_tone for iOS 16 arm64 into phone/wifi/out (signed ad hoc).
set -e
cd "$(dirname "$0")"
mkdir -p out
../../work/userland/bin/ios-cc -O2 -Wall -Wextra -Wno-unused-parameter -o out/wland wland.c smem.c pmem.c glink.c link.c qmi.c rmtfs.c pdmapper.c tftp.c scm.c wlfw.c radio.c ipnet.c bridge.c
../../tools/linux-ios-sign out/wland 2>/dev/null
../../work/userland/bin/ios-cc -O2 -Wall -Wextra -Wno-unused-parameter -o out/adspboot adspboot.c smem.c pmem.c glink.c
../../tools/linux-ios-sign out/adspboot 2>/dev/null
../../work/userland/bin/ios-cc -O2 -Wall -Wextra -Wno-unused-parameter -o out/aprtest aprtest.c smem.c pmem.c glink.c
../../tools/linux-ios-sign out/aprtest 2>/dev/null
../../work/userland/bin/ios-cc -O2 -Wall -Wextra -Wno-unused-parameter -o out/apr_tone apr_tone.c apr.c geni_i2c.c cs35l41.c smem.c pmem.c glink.c -lm
../../tools/linux-ios-sign out/apr_tone 2>/dev/null
ls -la out/wland out/adspboot out/aprtest out/apr_tone
