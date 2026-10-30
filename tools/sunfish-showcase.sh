#!/bin/bash
# Showcase video of what works on the Pixel 4a (2026-10-30): the webcam (Iriun, /dev/video0) films the phone, the
# EM-01 measurement mic records the sound, the script drives a ~3 min tour over SSH and writes captions (.srt).
#   tools/sunfish-showcase.sh            -> work/video/showcase-<time>.mkv (+ .srt) and .mp4 (captions burned in)
# Env: ROTATE=0|1|2 (0 none, 1 = 90 deg clockwise, 2 = 90 deg counter-clockwise; default 1: the webcam frames came out
#   rotated 90 deg), VIDEO=/dev/video0, MIC=<pulse source>, DWELL=3 (seconds per screen), NOREC=1 (tour only).
# Phone: unlocked-able (no passcode), SSH up, hapticd running, proxon/sctl/openapp in /var/local/bin, sweettorch=1.
# Never run during quiet hours (the tour makes haptic/UI sounds) or at night (work/night: brightness rules).
set -u
cd "$(dirname "$0")/.." || exit 1

ROTATE=${ROTATE:-1}
VIDEO=${VIDEO:-/dev/video0}
MIC=${MIC:-alsa_input.usb-EVERSOLO_EM-01_2614211098-01.analog-stereo}
DWELL=${DWELL:-3}

# Tap coordinates (0..1, portrait, 0,0 top-left; docs/methods.md). Known values from features/apps.md.
UNLOCK_SWIPE="0.5 0.99 0.5 0.5 600"   # lock screen -> home (apps.md tap3b)
SET_WIFI="0.5 0.37"                   # Settings rows (apps.md)
SET_BT="0.5 0.42"
SET_SOUNDS="0.5 0.57"
BACK="0.08 0.075"
SCROLL_UP="0.5 0.8 0.5 0.3 300"       # scrolls a Settings pane down
CC_OPEN="0.92 0.005 0.92 0.6 400"     # Control Center: swipe down from top-right (apps.md)
CC_CLOSE="0.5 0.95 0.5 0.2 300"       # confirmed vid1
CC_BRIGHT="0.61 0.52 0.61 0.38 0.52"  # slider x, y_start, y_up, y_back (vid1 flame-shot)
HAPTIC_SWITCH="0.85 0.443"            # "Change with Buttons" switch in Sounds & Haptics
NOTES_NEW="0.92 0.93"                  # Notes compose button (bottom right)
# Keyboard (portrait, iOS QWERTY): row y and the x of the first key; key pitch 0.1. TODO runner: check with a flame-shot.
KB_Y1=0.678; KB_Y2=0.744; KB_Y3=0.81; KB_SPACE="0.5 0.876"; KB_SHIFT="0.063 0.81"
KB_X1=0.05; KB_X2=0.10; KB_X3=0.20
FILES_BROWSE="0.83 0.93"
FILES_ONMYIPHONE="0.5 0.375"
SAFARI_URL=https://example.com

ts=$(date +%Y%m%d-%H%M%S)
out=work/video; mkdir -p "$out"
mkv=$out/showcase-$ts.mkv; srt=$out/showcase-$ts.srt; mp4=$out/showcase-$ts.mp4
: >"$srt"
ffpid=; n=0; t0=$(date +%s.%N)

warn() { echo "showcase: WARNING: $*" >&2; }
ph() { tools/sunfish-ssh "$*" >/dev/null 2>&1 || warn "phone command failed: $*"; }
cmd() { ph "echo '$*' > /var/local/sweet-orient"; sleep 1.3; }   # the control file is read every 1 s
tap() { cmd "T $*"; }
swipe() { cmd "W $*"; }
app() { ph "openapp $*"; sleep 1.5; }
now() { awk -v a="$(date +%s.%N)" -v b="$t0" 'BEGIN { printf "%.3f", a - b }'; }
srtt() { awk -v t="$1" 'BEGIN { if (t < 0) t = 0; ms = int(t * 1000 + 0.5); printf "%02d:%02d:%02d,%03d", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000 }'; }
cap_s=; cap_t=
cap() {   # cap "text": closes the previous caption, opens a new one
	cap_end
	cap_s=$(now); cap_t=$1
	echo "showcase: [$(printf '%6.1f' "$cap_s") s] $1"
}
cap_end() {
	[ -n "$cap_t" ] || return 0
	n=$((n + 1))
	printf '%d\n%s --> %s\n%s\n\n' "$n" "$(srtt "$cap_s")" "$(srtt "$(now)")" "$cap_t" >>"$srt"
	cap_t=
}
dwell() { sleep "${1:-$DWELL}"; }

# Notes typing: one tap per letter (lower case; Shift before an upper-case letter).
key() {
	local c=$1 r1=qwertyuiop r2=asdfghjkl r3=zxcvbnm i
	case $c in
	' ') tap "$KB_SPACE"; return ;;
	[A-Z]) tap "$KB_SHIFT"; c=$(echo "$c" | tr A-Z a-z) ;;
	esac
	i=$(expr index "$r1" "$c"); [ "$i" -gt 0 ] && { tap "$(awk -v x="$KB_X1" -v i="$i" 'BEGIN { printf "%.2f", x + (i - 1) * 0.1 }') $KB_Y1"; return; }
	i=$(expr index "$r2" "$c"); [ "$i" -gt 0 ] && { tap "$(awk -v x="$KB_X2" -v i="$i" 'BEGIN { printf "%.2f", x + (i - 1) * 0.1 }') $KB_Y2"; return; }
	i=$(expr index "$r3" "$c"); [ "$i" -gt 0 ] && { tap "$(awk -v x="$KB_X3" -v i="$i" 'BEGIN { printf "%.2f", x + (i - 1) * 0.1 }') $KB_Y3"; return; }
	warn "no key for '$c'"   # digits: skipped
}
type_text() { local s=$1 k; for ((k = 0; k < ${#s}; k++)); do key "${s:k:1}"; done; }

stop_rec() {
	[ -n "$ffpid" ] || return 0
	kill -INT "$ffpid" 2>/dev/null; wait "$ffpid" 2>/dev/null; ffpid=
}
cleanup() {
	trap - INT TERM
	cap_end
	ph "/var/local/bin/sctl kern.sweet_torch=0"
	ph "pkill proxon"
	stop_rec
	echo "showcase: interrupted; partial recording $mkv, captions $srt" >&2
	exit 130
}
trap cleanup INT TERM

# 1. Recording
case $ROTATE in
0) vf=null ;; 1) vf=transpose=1 ;; 2) vf=transpose=2 ;; *) echo "ROTATE must be 0, 1 or 2" >&2; exit 2 ;;
esac
if [ "${NOREC:-0}" != 1 ]; then
	# Native size/fps of the camera; video is rotated only in the final MP4, the mkv is the raw take.
	ffmpeg -hide_banner -loglevel error -y -f v4l2 -thread_queue_size 512 -i "$VIDEO" \
		-f pulse -thread_queue_size 512 -i "$MIC" -c:v libx264 -preset ultrafast -crf 20 -c:a flac "$mkv" </dev/null &
	ffpid=$!
	sleep 2
	kill -0 "$ffpid" 2>/dev/null || { echo "showcase: ffmpeg did not start" >&2; exit 1; }
fi
t0=$(date +%s.%N)

# 2. The tour
cap "Pixel 4a running iOS 16: wake and unlock"
cmd H; dwell 2; swipe "$UNLOCK_SWIPE"; dwell
cap "Home screen"
dwell

cap "Settings: Wi-Fi joined"
app com.apple.Preferences; tap "$SET_WIFI"; dwell
tap "$BACK"; dwell 1
cap "Settings: Bluetooth on, devices listed"
tap "$SET_BT"; dwell 4
tap "$BACK"; dwell 1

cap "Haptics: switch taps in Sounds & Haptics (vibration motor)"
tap "$SET_SOUNDS"; dwell 2
tap "$HAPTIC_SWITCH"; dwell 1.5; tap "$HAPTIC_SWITCH"; dwell 1.5; tap "$HAPTIC_SWITCH"; dwell 1.5; tap "$HAPTIC_SWITCH"
dwell 1
cmd H; dwell 1

cap "Safari loads a web page"
app com.apple.mobilesafari "$SAFARI_URL"; dwell 6

cap "Notes: typing with the keyboard"
app com.apple.mobilenotes; tap "$NOTES_NEW"; dwell 2
tap "0.5 0.575"; dwell 1   # dismisses the one-off "Enable Dictation?" prompt
type_text "hi from a pixel"; dwell   # first letter is auto-capitalised
cmd H; dwell 1

cap "Calculator: rotation to landscape and back"
app com.apple.calculator; dwell 2
cmd "R 3"; dwell 4
cmd "R 1"; dwell
cmd H; dwell 1

cap "Control Center: brightness slider"
swipe "$CC_OPEN"; dwell 2
read -r bx by_lo by_hi by_end <<<"$CC_BRIGHT"
swipe "$bx $by_lo $bx $by_hi 600"; dwell 1.5
swipe "$bx $by_hi $bx $by_end 600"; dwell 2
swipe "$CC_CLOSE"; dwell 1

cap "Flashlight on and off"
ph "/var/local/bin/sctl kern.sweet_torch=50"; dwell 3
ph "/var/local/bin/sctl kern.sweet_torch=0"; dwell 1.5

cap "Proximity sensor: near -> screen off, far -> on"
tools/sunfish-ssh "/var/local/bin/proxon 3 20" >/dev/null 2>&1 &
pxpid=$!
sleep 2
ph "echo 'm fake prox 1' > /dev/redwood-motion"; dwell 4
ph "echo 'm fake prox 0' > /dev/redwood-motion"; dwell 3
ph "pkill proxon"; wait "$pxpid" 2>/dev/null

cap "Files: On My iPhone"
app com.apple.DocumentsApp; tap "$FILES_BROWSE"; dwell 1.5; tap "$FILES_ONMYIPHONE"; dwell
cmd H; dwell 1

cap "Camera: live preview"
app com.apple.camera; dwell 5
cmd H; dwell 1

cap "Music: the library"
app com.apple.Music; dwell 4
cmd H; dwell 1

cap "Lock"
cmd K; dwell 2
cap_end
trap - INT TERM

# 3. Final MP4
[ "${NOREC:-0}" = 1 ] && { echo "showcase: tour done (no recording), captions $srt"; exit 0; }
stop_rec
sub=$vf
if ffmpeg -hide_banner -filters 2>/dev/null | grep -q ' subtitles '; then
	sub="$vf,subtitles=$srt:force_style='FontSize=22,Outline=2'"
elif ffmpeg -hide_banner -filters 2>/dev/null | grep -q ' drawtext '; then
	# drawtext per caption, shown between its start and end times
	sub=$vf
	python3 -I - "$srt" >"$out/showcase-$ts.dt" <<'PY' && sub="$vf,$(cat "$out/showcase-$ts.dt")"
import sys, re
def s(t):
    h, m, r = t.split(':'); sec, ms = r.split(',')
    return int(h) * 3600 + int(m) * 60 + int(sec) + int(ms) / 1000
parts = []
for b in open(sys.argv[1]).read().strip().split('\n\n'):
    l = b.split('\n'); a, e = l[1].split(' --> ')
    txt = l[2].replace('\\', '').replace("'", '').replace(':', '\\:').replace(',', '\\,')
    parts.append("drawtext=text='%s':fontsize=h/30:fontcolor=white:box=1:boxcolor=black@0.6:x=(w-tw)/2:y=h-th-h/20:enable='between(t\\,%.2f\\,%.2f)'" % (txt, s(a), s(e)))
print(','.join(parts))
PY
	rm -f "$out/showcase-$ts.dt"
else
	warn "ffmpeg has neither subtitles nor drawtext: soft subtitles only (mp4 mov_text track)"
fi
if [ "$sub" = "$vf" ] && ! ffmpeg -hide_banner -filters 2>/dev/null | grep -q ' drawtext '; then
	ffmpeg -hide_banner -loglevel error -y -i "$mkv" -i "$srt" -vf "$vf" -c:v libx264 -crf 22 -pix_fmt yuv420p \
		-c:a aac -b:a 160k -c:s mov_text -movflags +faststart "$mp4" || { warn "mp4 failed; raw take $mkv"; exit 1; }
else
	ffmpeg -hide_banner -loglevel error -y -i "$mkv" -vf "$sub" -c:v libx264 -crf 22 -pix_fmt yuv420p \
		-c:a aac -b:a 160k -movflags +faststart "$mp4" || { warn "mp4 failed; raw take $mkv"; exit 1; }
fi
len=$(ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 "$mp4")
echo "showcase: $mp4  $(printf '%.1f' "$len") s  $(du -h "$mp4" | cut -f1)  (raw $mkv, captions $srt)"
