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
base="$base wdt=-1 sweetfb_noconsole=1 sweetfb_hz=60 ufslogsecs=2 sweetuiwdt=30"
base="$base flametouch=0 sweettouch=2 sweetkeys=1 sweetusb=2 sweetgpu=1 sweetgpufreq=5 sweetfb_thread=0 sweetmtl=2 sweetddr=0 sweetgpurecover=2 sweetgpukill=1"
base="$base sweetbattery=1"
# on by default since 2026-10-28: iOS Wi-Fi status (xnu 0199), panel off on sleep (0202-0205)
base="$base sweetwifiev=1 sweetpanelsleep=1"
# present waits for its blit (2026-10-28: 20.3 vs 15.3 panel updates/s, 94% vs 70% of swaps; kpw1c vs kpw0c)
base="$base sweetpresentwait=1"
# on by default since 2026-10-28: Control Center brightness slider (xnu 0208-0216) and flashlight (0190/0209; tile needs /var/local/sweet-flashlight)
base="$base sweetbl=2 sweettorch=1"
# on by default since 2026-10-30: Bluetooth kernel bridge for bluetoothd (xnu 0226, kernel105+; bt7: devices listed)
base="$base btbridge=1"
# on by default since 2026-10-30: iOS orientation HID service off, auto-rotate comes from adspboot --sns-rot (sensors.md u10g; kernel107+, older kernels ignore it)
base="$base sweetmotion=0x37"
# NOSERIAL=1: no serial console (boot-time A/B, plan D2)
if [ "${NOSERIAL:-0}" = 1 ]; then
	base=$(printf '%s\n' $base | grep -v -x -e 'serial=3' -e '-enable_kprintf_spam' | tr '\n' ' ')
fi
# Real time: PM6150 RTC + offset (tools/flame-rtc.py LOG calibrates it once).
[ -f work/rtc-offset ] && base="$base sweetrtcoff=$(cat work/rtc-offset)"
# Extra args replace defaults with the same key (XNU would take the first).
for a in "$@"; do
	k=${a%%=*}
	base=$(printf '%s\n' $base | grep -v -x -e "$k" -e "$k=.*" | tr '\n' ' ')
done
cmdline=$(echo $base "$@")	# flamereboot=0 left out (the kernel default): the line is capped at 591 bytes (B38)
if [ ${#cmdline} -ge 592 ]; then
	echo "boot-args ${#cmdline} bytes, over 591 (mkpayload limit): drop an extra arg or move work/rtc-offset aside" >&2
	exit 1
fi
echo "$cmdline" > "$P/$name.cmdline"
# CAMERA=1: ADT with /arm-io/isp + /product/camera flash (mkadt.py --camera; Camera.app prerequisites,
# camera.md); default the usual work/boot/sunfish-hi.adt. ADT=PATH picks any other tree.
adt=${ADT:-$P/sunfish-hi.adt}
if [ "${CAMERA:-0}" = 1 ]; then
	adt=$P/sunfish-hi-camera.adt
	python3 -I tools/mkadt.py "$adt" --highmem --camera 2>/dev/null
fi
python3 -I tools/mkpayload.py "$kernel" "$adt" "$P/$name.payload" --cmdline "$cmdline" --gpufw "${GPUFW:-$PWD/../restore/fw/gpu}" >/dev/null
shim/build.sh "$P/$name.payload" "$P/$name.img" >/dev/null
echo "$(date +%T) built $name.img; waiting for fastboot"
until fastboot devices | grep -q fastboot; do sleep 2; done
fastboot boot "$P/$name.img"
echo "$(date +%T) booted $name: $cmdline"
