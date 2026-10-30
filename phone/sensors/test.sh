#!/bin/sh
# sns1: host tests of the SNS client (ios-redfin's test_sensd.c, MIT): protobuf coder, SEE encoders/decoders and a
# canned SUID/stream exchange against a fake DSP over a socketpair (sns_link's fd framing).
set -e
cd "$(dirname "$0")"
mkdir -p out
${CC:-cc} -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -o out/test_sensd \
	test_sensd.c pb.c sns.c sensor.c sns_link.c ../wifi/qmi.c -lm
./out/test_sensd
# B46 dm2: dm_fusion type-2 heading (flat phone, top north = 0, top east = 90, clockwise)
${CC:-cc} -O1 -g -Wall -Wextra -fsanitize=address,undefined -o out/test_dm test_dm.c dm_fusion.c sns.c pb.c ../wifi/qmi.c -lm
./out/test_dm
