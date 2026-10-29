#!/bin/sh
# Host test of btbridge's legacy -> extended LE translation (phone/wifi/bt_lext.c): cc + ASan/UBSan, seconds.
set -e
cd "$(dirname "$0")"
out=${TMPDIR:-/tmp}/bt_lext_test.$$
cc -std=c11 -g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -I../../phone/wifi \
	-o "$out" bt_lext_test.c ../../phone/wifi/bt_lext.c
"$out"; rc=$?; rm -f "$out"; exit $rc
