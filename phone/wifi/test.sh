#!/bin/sh
# sp23: host test of adspboot --audio-fifo's ring and DC guard (af_ring.h).
set -e
cd "$(dirname "$0")"
mkdir -p out
${CC:-cc} -O1 -g -Wall -Wextra -fsanitize=address,undefined -o out/test_af_ring test_af_ring.c -lm
./out/test_af_ring
