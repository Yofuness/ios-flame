#!/bin/bash
# Pack kernel + ADT + boot-args into a shim boot image and fastboot-boot it
# on the Pixel 4a (sunfish). RAM only: nothing is flashed.
#
#   tools/sunfish-boot.sh NAME KERNEL [EXTRA BOOT-ARGS...]
#
# Waits for the phone to show up in fastboot (e.g. after flamereboot).
# Image: work/boot/NAME.img; args: work/boot/NAME.cmdline.
set -euo pipefail
cd "$(dirname "$0")/.."
name=$1 kernel=$2; shift 2
P=$PWD/work/boot
base="debug=0xc serial=3 cpus=8 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 ufsrw=1 rootrw=1"
base="$base unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin launchd_unsecure_cache=1"
base="$base wdt=-1 sweetfb_noconsole=1 sweetfb_hz=60 ufslogsecs=2 flamereboot=0 sweetuiwdt=30"
base="$base flametouch=0 sweettouch=2 sweetkeys=1 sweetusb=2 sweetgpu=1 sweetgpufreq=5 sweetgpudelay=0 sweetfb_thread=0 sweetmtl=2 sweetddr=0 sweetgpurecover=2 sweetgpukill=1"
base="$base sweetbattery=1"
# on by default since 2026-10-28: iOS Wi-Fi status (xnu 0199), panel off on sleep (0202-0205)
base="$base sweetwifiev=1 sweetpanelsleep=1"
# present waits for its blit (2026-10-28: 20.3 vs 15.3 panel updates/s, 94% vs 70% of swaps; kpw1c vs kpw0c)
base="$base sweetpresentwait=1"
# Real time: PM6150 RTC + offset (tools/flame-rtc.py LOG calibrates it once).
[ -f work/rtc-offset ] && base="$base sweetrtcoff=$(cat work/rtc-offset)"
# Extra args replace defaults with the same key (XNU would take the first).
for a in "$@"; do
	k=${a%%=*}
	base=$(printf '%s\n' $base | grep -v -x -e "$k" -e "$k=.*" | tr '\n' ' ')
done
cmdline="$base $*"
echo "$cmdline" > "$P/$name.cmdline"
python3 -I tools/mkpayload.py "$kernel" "$P/sunfish-hi.adt" "$P/$name.payload" --cmdline "$cmdline" --gpufw "${GPUFW:-$PWD/../restore/fw/gpu}" >/dev/null
shim/build.sh "$P/$name.payload" "$P/$name.img" >/dev/null
echo "$(date +%T) built $name.img; waiting for fastboot"
until fastboot devices | grep -q fastboot; do sleep 2; done
fastboot boot "$P/$name.img"
echo "$(date +%T) booted $name: $cmdline"
