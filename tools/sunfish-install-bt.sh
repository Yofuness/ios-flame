#!/bin/bash
# Bluetooth (B15): install btup + the WCN3990 BT firmware (crbtfw21.tlv, crnv21.bin from the factory vendor.img,
# extracted into ../restore/fw/vendor/bt with debugfs; outside git) into /var/local/fw/bt. No launchd change:
# run by hand, `/var/local/bin/btup status` (read-only) or `btup fw /var/local/fw/bt`. Needs /dev/pmem (sweetpmem=1).
set -euo pipefail
cd "$(dirname "$0")/.."
F() { python3 -I tools/flame-files.py "$@"; }
R=../restore/fw/vendor/bt
mkdir -p $R
for f in crbtfw21.tlv crnv21.bin; do
	[ -s $R/$f ] || debugfs -R "dump /firmware/$f $R/$f" ../restore/factory/vendor.img 2>/dev/null
	[ -s $R/$f ] || { echo "missing $R/$f"; exit 1; }
done
for d in /var/local/fw /var/local/fw/bt; do F mkdir $d 755 >/dev/null 2>&1 || true; done
for f in crbtfw21.tlv crnv21.bin; do F put $R/$f /var/local/fw/bt/$f 644 >/dev/null; done
F put work/userland-linux/out/btup /var/local/bin/btup 755
python3 -I tools/flame-cmd.py sync >/dev/null || true
echo "installed: /var/local/bin/btup + $(ls -l $R/crbtfw21.tlv $R/crnv21.bin | awk '{print $5}' | tr '\n' ' ')bytes of firmware"
