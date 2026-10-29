#!/bin/sh
# Sensors step 7: fake /sys/devices/soc0 for the ADSP sensors PD. fastrpc maps /sys/devices/soc0/<name> to
# /var/local/fw/soc0/<name>; the registry matches config/*.json against hw_platform and soc_id.
# Values as Linux socinfo sysfs prints them. Sources:
#   soc_id 365            recon/live.dts qcom,msm-id = <0x16d 0x00> (SM7150 / sdmmagpie); 10 sensor configs list "365"
#   hw_platform OEM       guess: every Google s5_*.json (the 4a's sensor set) lists "OEM" first; Pixel SMEM reports OEM
#   platform_subtype_id 14  guess: bits 16-23 of qcom,board-id = <0xe2805 0x00>
#   platform_subtype Unknown  guess: Linux prints "Unknown" for subtypes out of its table (no config matches on it)
#   platform_version 2621440  guess: bits 8-15 of board-id (0x28) as major, printed as major<<16 | minor
#   revision 1.0          model "Google Inc. MSM sm7150 Sunfish MP1.0" (guess for the silicon revision)
#   g_* = the same values
#   tools/sunfish-soc0-files.sh
cd "$(dirname "$0")/.." || exit 1
exec tools/sunfish-ssh 'd=/var/local/fw/soc0; mkdir -p $d && cd $d &&
for p in "" g_; do echo OEM >${p}hw_platform; echo 14 >${p}platform_subtype_id; echo 2621440 >${p}platform_version; done &&
echo Unknown >platform_subtype; echo 365 >soc_id; echo 1.0 >revision; for f in *; do echo "$f: $(cat $f)"; done'
