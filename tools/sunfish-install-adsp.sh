#!/bin/bash
# Audio M0: install adspboot + the ADSP firmware (adsp.mdt + .bNN from the factory vendor.img,
# extracted into ../restore/fw/vendor/adsp with debugfs; outside git). No launchd change: run by hand,
# `/var/local/bin/adspboot` (boot) or `adspboot status` (read-only). Needs /dev/pmem (sweetpmem=1).
set -euo pipefail
cd "$(dirname "$0")/.."
F() { python3 -I tools/flame-files.py "$@"; }
R=../restore/fw/vendor/adsp
if [ ! -f $R/adsp.mdt ]; then
	mkdir -p $R
	for f in $(debugfs -R "ls /firmware" ../restore/factory/vendor.img 2>/dev/null | tr ' ' '\n' | grep '^adsp'); do
		debugfs -R "dump /firmware/$f $R/$f" ../restore/factory/vendor.img 2>/dev/null
	done
fi
for d in /var/local/fw /var/local/fw/adsp; do F mkdir $d 755 >/dev/null 2>&1 || true; done
for f in $R/adsp.mdt $R/adsp.b[0-9][0-9]; do
	F put "$f" /var/local/fw/adsp/$(basename "$f") 644 >/dev/null
done
F put work/userland-linux/out/adspboot /var/local/bin/adspboot 755
python3 -I tools/flame-cmd.py sync >/dev/null || true
echo "installed: /var/local/bin/adspboot + $(ls $R/adsp.mdt $R/adsp.b[0-9][0-9] | wc -l) firmware files"
