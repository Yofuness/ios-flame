#!/bin/sh
# Build wland, adspboot, aprtest, apr_tone, i2cprobe, camid, btup, btbridge and bluetool-stub for iOS 16 arm64 into phone/wifi/out (signed ad hoc).
set -e
cd "$(dirname "$0")"
mkdir -p out
../../work/userland/bin/ios-cc -O2 -Wall -Wextra -Wno-unused-parameter -o out/wland wland.c smem.c pmem.c glink.c link.c qmi.c rmtfs.c pdmapper.c tftp.c scm.c wlfw.c radio.c smmu.c ipnet.c bridge.c
../../tools/linux-ios-sign out/wland 2>/dev/null
../../work/userland/bin/ios-cc -O2 -Wall -Wextra -Wno-unused-parameter -o out/adspboot adspboot.c fastrpc.c smem.c pmem.c glink.c smmu.c pdmapper.c qmi.c geni_i2c.c cs35l41.c -lm
../../tools/linux-ios-sign out/adspboot 2>/dev/null
../../work/userland/bin/ios-cc -O2 -Wall -Wextra -Wno-unused-parameter -o out/aprtest aprtest.c smem.c pmem.c glink.c
../../tools/linux-ios-sign out/aprtest 2>/dev/null
../../work/userland/bin/ios-cc -O2 -Wall -Wextra -Wno-unused-parameter -o out/apr_tone apr_tone.c apr.c geni_i2c.c cs35l41.c smem.c pmem.c glink.c -lm
../../tools/linux-ios-sign out/apr_tone 2>/dev/null
../../work/userland/bin/ios-cc -O2 -Wall -Wextra -Wno-unused-parameter -o out/i2cprobe i2cprobe.c geni_i2c.c smem.c pmem.c
../../tools/linux-ios-sign out/i2cprobe 2>/dev/null
../../work/userland/bin/ios-cc -O2 -Wall -Wextra -Wno-unused-parameter -o out/camid camid.c geni_i2c.c smem.c pmem.c
../../tools/linux-ios-sign out/camid 2>/dev/null
../../work/userland/bin/ios-cc -O2 -Wall -Wextra -Wno-unused-parameter -o out/btup btup.c bt_uart.c pmem.c
../../tools/linux-ios-sign out/btup 2>/dev/null
../../work/userland/bin/ios-cc -O2 -Wall -Wextra -Wno-unused-parameter -o out/btbridge btbridge.c bt_uart.c pmem.c
../../tools/linux-ios-sign out/btbridge 2>/dev/null
../../work/userland/bin/ios-cc -O2 -Wall -Wextra -Wno-unused-parameter -o out/bluetool-stub bluetool_stub.c
../../tools/linux-ios-sign out/bluetool-stub 2>/dev/null
ls -la out/btup out/wland out/adspboot out/aprtest out/apr_tone out/i2cprobe out/camid out/btbridge out/bluetool-stub
