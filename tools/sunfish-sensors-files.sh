#!/bin/bash
# Audio step 6: where the files the DSP asks for through FastRPC apps_std (adspboot --rpc --root) come from.
# Copies nothing: the sensors config lives on the phone's Android vendor partition (/vendor/etc/sensors), which iOS
# doesn't mount, and is Google/Qualcomm data (out of git). This prints what the PC has and the commands to push it.
# fastrpc.c maps: /vendor/etc/sensors -> /var/local/fw/sensors, /mnt/vendor/persist/sensors -> /var/local/sensors-persist,
# /vendor/dsp, /usr/lib/qcom/adsp, /vendor/lib/rfsa/adsp, /dsp -> /var/local/fw/dsp, /sys/devices/soc0 -> /var/local/fw/soc0.
set -uo pipefail
cd "$(dirname "$0")/.."
S=../restore/fw/sensors/sensors
V=../restore/factory/vendor.img
echo "== sensors config (/vendor/etc/sensors)"
if [ -f $S/sns_reg_config ]; then
	echo "on the PC: $S ($(find $S -type f | wc -l) files: sns_reg_config, hals.conf, config/*.json, ...)"
	echo "push (phone only, out of git):"
	echo "  python3 -I tools/flame-files.py mkdir /var/local/fw/sensors 755"
	echo "  for d in \$(cd $S && find . -mindepth 1 -type d); do python3 -I tools/flame-files.py mkdir /var/local/fw/sensors/\${d#./} 755; done"
	echo "  for f in \$(cd $S && find . -type f); do python3 -I tools/flame-files.py put $S/\${f#./} /var/local/fw/sensors/\${f#./} 644; done"
else
	echo "missing on the PC: extract /etc/sensors from $V with debugfs (rdump /etc/sensors $(dirname $S))"
fi
echo "== DSP libraries (/vendor/dsp, /vendor/lib/rfsa/adsp)"
if [ -f $V ]; then
	echo "in $V: /dsp/adsp, /dsp/cdsp and /lib/rfsa/adsp (not extracted yet); only when the log shows"
	echo "  'apps_std ... -> 2' for a library: debugfs -R 'dump /dsp/adsp/NAME /tmp/NAME' $V, push to /var/local/fw/dsp/..."
else
	echo "missing: $V"
fi
echo "== persist registry (/mnt/vendor/persist/sensors)"
echo "no copy on the PC (the persist partition was never dumped); the DSP writes a new one to /var/local/sensors-persist"
echo "== /sys/devices/soc0"
echo "no copy on the PC; if the log asks for it, create /var/local/fw/soc0/soc_id (SM7150: 365, unchecked) and friends by hand"
if [ "${1:-}" = "--phone" ]; then	# read-only: what the phone has now
	tools/sunfish-ssh 'ls -R /var/local/fw/sensors /var/local/fw/dsp /var/local/fw/soc0 /var/local/sensors-persist 2>&1 | head -60'
fi
