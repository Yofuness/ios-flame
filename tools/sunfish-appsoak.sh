#!/bin/bash
# App soak: opens stock apps in turn, swipes inside each, snaps the webcam after each app.
# usage: tools/sunfish-appsoak.sh TAG SECONDS [APPS...]   (APPS: Name=bundle.id, default 6 stock apps)
# Stops (exit 3) as soon as the phone stops answering ping, and says whether it fell back to fastboot;
# stops (exit 2) when an app fails to open. Writes "=== TAG ..." markers into the live log.
cd "$(dirname "$0")/.." || exit 1
TAG=${1:?tag} SECS=${2:-3600}; shift 2
L=../logs/usb-live.log
PHONE=192.168.7.2
APPS=("$@")
[ ${#APPS[@]} -eq 0 ] && APPS=(Preferences=com.apple.Preferences Safari=com.apple.mobilesafari Notes=com.apple.mobilenotes
	Calendar=com.apple.mobilecal Clock=com.apple.mobiletimer Photos=com.apple.mobileslideshow)
C() { timeout 20 python3 -I tools/flame-cmd.py "$@" >/dev/null 2>&1; }
mark() { echo "=== $TAG $* $(date +%T)" >> "$L"; echo "$TAG $* $(date +%T)"; }

# Three missed pings in a row (~10 s) = gone. Then report whether fastboot answers.
alive() {
	for i in 1 2 3; do ping -c1 -W2 $PHONE >/dev/null 2>&1 && return 0; sleep 1; done
	return 1
}
gone() {
	if timeout 10 fastboot getvar battery-voltage 2>&1 | grep -q battery-voltage; then
		mark "STOP unreachable, phone is in fastboot ($1)"
	else
		mark "STOP unreachable, no fastboot either ($1)"
	fi
	exit 3
}

alive || gone "before start"
mark start
END=$(( $(date +%s) + SECS ))
n=0
while [ $(date +%s) -lt $END ]; do
	n=$((n+1))
	for app in "${APPS[@]}"; do
		name=${app%%=*} bid=${app#*=}
		alive || gone "c$n before $name"
		out=$(timeout 25 tools/sunfish-ssh "openapp $bid" 2>&1 | tr -d '\r' | head -c 150)
		case "$out" in *rror*|*ail*|*nknown*) mark "openfail c$n $name: $out"; exit 2;; esac
		mark "app $name c$n"
		t=$(date +%s)
		while [ $(( $(date +%s) - t )) -lt 10 ]; do
			for j in 1 2 3; do C swipe 540 1800 540 1000 300; sleep 0.8; done
		done
		ffmpeg -hide_banner -loglevel error -y -f v4l2 -i /dev/video0 -frames:v 1 "../logs/$TAG-$name.jpg"
		C swipe 540 2330 540 1100 300; sleep 2	# home gesture
	done
done
mark end
