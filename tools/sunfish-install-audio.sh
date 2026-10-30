#!/bin/bash
# SunfishAudio.driver (phone/audio, the CoreAudio HAL plug-in: Speaker, Hawking, Actuator, Codec; feature under work,
# docs/features/audio.md) on the running phone, and VirtualAudio.plugin back on (it needs the plug-in's "Speaker" and
# "Hawking" devices, research/notes/ios-audio-device.md). mediaserverd loads HAL plug-ins once: reboot to apply.
#   tools/sunfish-install-audio.sh [--undo] [DIR]   DIR holds SunfishAudio.driver (default phone/audio/out)
#   --undo: VirtualAudio.plugin back to HAL.disabled, SunfishAudio.driver removed.
# The driver also carries the camera (stage f2, phone/camera): off in mediaserverd unless /var/local/camdev.on exists
# (or boot-arg sweetcamdev=1); its log is /var/local/camdev.log. Frames come from camd.
# Only file moves and a new directory, nothing to back up. Files over flame-files, moves and checks over SSH.
set -euo pipefail
cd "$(dirname "$0")/.."
F() { python3 -I tools/flame-files.py "$@"; }
S() { tools/sunfish-ssh "$@"; }
H=/System/Library/Audio/Plug-Ins/HAL
DRV=$H/SunfishAudio.driver
VA=/Library/Audio/Plug-Ins/HAL VAOFF=/Library/Audio/Plug-Ins/HAL.disabled
if [ "${1:-}" = --undo ]; then
	S "set -e; mkdir -p $VAOFF; if [ -d $VA/VirtualAudio.plugin ]; then mv $VA/VirtualAudio.plugin $VAOFF/; echo 'VirtualAudio.plugin -> $VAOFF'; else echo 'VirtualAudio.plugin not in $VA'; fi; rm -rf $DRV; echo 'removed $DRV'; sync 2>/dev/null || true"
	echo "reboot to apply"
	exit 0
fi
D=${1:-phone/audio/out}/SunfishAudio.driver
[ -f "$D/SunfishAudio" ] && [ -f "$D/Info.plist" ] || { echo "no $D/SunfishAudio + Info.plist: run phone/audio/build.sh"; exit 1; }
S "mkdir -p $DRV"
F put "$D/Info.plist" $DRV/Info.plist 644 >/dev/null
F put "$D/SunfishAudio" $DRV/SunfishAudio 755 >/dev/null
if [ -f "$D/_CodeSignature/CodeResources" ]; then
	S "mkdir -p $DRV/_CodeSignature"
	F put "$D/_CodeSignature/CodeResources" $DRV/_CodeSignature/CodeResources 644 >/dev/null
fi
echo "installed $DRV"
S "if [ -d $VA/VirtualAudio.plugin ]; then echo 'VirtualAudio.plugin already in $VA'; \
elif [ -d $VAOFF/VirtualAudio.plugin ]; then mkdir -p $VA && mv $VAOFF/VirtualAudio.plugin $VA/ && echo 'VirtualAudio.plugin -> $VA'; \
else echo 'WARNING: VirtualAudio.plugin in neither $VA nor $VAOFF'; fi"
S "found=; for d in $H /System/Library/Audio/Plug-Ins/HAL.disabled $VA $VAOFF; do \
[ -d \$d/BTAudioHALPlugin.driver ] && { echo \"BTAudioHALPlugin.driver: \$d\"; found=1; }; done; \
[ -n \"\$found\" ] || echo 'WARNING: BTAudioHALPlugin.driver not found in any HAL dir'; \
[ -d $H/BTAudioHALPlugin.driver ] || echo 'WARNING: BTAudioHALPlugin.driver is not in $H (not loaded)'"
S "if [ -d /Library/Audio/Tunings/D22 ]; then echo '/Library/Audio/Tunings/D22: present'; else echo 'WARNING: /Library/Audio/Tunings/D22 missing'; fi; sync 2>/dev/null || true"
echo "reboot to apply"
