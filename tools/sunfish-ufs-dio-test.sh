#!/bin/sh
# xnu 0198 test: fcopyfile data copies on /private/var (HFS on /dev/ufsroot1), cached and with
# F_NOCACHE (cluster direct I/O). Scratch files in /private/var/tmp only; deleted at the end.
#   tools/sunfish-ufs-dio-test.sh [MB]   (default 64)
# Needs /var/local/bin/fcopy (phone/tools/fcopy.c). Expect: 3 identical sums and "PASS".
cd "$(dirname "$0")/.." || exit 1
MB=${1:-64}
tools/sunfish-ssh "
d=/private/var/tmp/diotest.\$\$; toybox mkdir -p \$d || exit 1
toybox dd if=/dev/urandom of=\$d/src bs=1048576 count=$MB 2>/dev/null
fcopy \$d/src \$d/cached; echo cached rc=\$?
fcopy -n \$d/src \$d/direct; echo direct rc=\$?
toybox sha1sum \$d/src \$d/cached \$d/direct
n=\$(toybox sha1sum \$d/src \$d/cached \$d/direct | toybox cut -d' ' -f1 | toybox sort -u | toybox wc -l)
[ \"\$n\" = 1 ] && echo PASS || echo FAIL
toybox rm -rf \$d
"
