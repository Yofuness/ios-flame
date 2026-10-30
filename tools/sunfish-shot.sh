#!/bin/sh
# Screenshot of the running phone: phone/tools/shot (render server picture, iOSPlus10 port) over SSH, fetched
# with flame-files.py to work/shots/NAME-TIME.png; prints the local path. Needs shot in /var/local/bin.
#   tools/sunfish-shot.sh [NAME]      NAME default shot
set -eu
cd "$(dirname "$0")/.."
NAME=${1:-shot}
T=$(date +%Y%m%d-%H%M%S)
R=/var/local/$NAME-$T.png
L=work/shots/$NAME-$T.png
mkdir -p work/shots
tools/sunfish-ssh "shot $R" >&2
python3 -I tools/flame-files.py get "$R" "$L" >&2
tools/sunfish-ssh "rm -f $R"
echo "$PWD/$L"
