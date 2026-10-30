#!/bin/sh
# dm10: build and run the offline compass replay: sunfish-dm-replay.sh [--cal FILE] dump.csv (env knobs: dm_replay.c)
set -e
d="$(cd "$(dirname "$0")/../phone/sensors" && pwd)"
mkdir -p "$d/out"
${CC:-cc} -O2 -g -Wall -Wextra -o "$d/out/dm_replay" "$d/dm_replay.c" "$d/dm_fusion.c" -lm
exec "$d/out/dm_replay" "$@"
