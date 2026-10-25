#!/bin/bash
cd "/home/johann/Projects/Claude Code/ios pixel 4a/port" || exit 1
L="/home/johann/Projects/Claude Code/ios pixel 4a/port/../logs/usb-live.log"
S=/tmp/claude-1000/-home-johann-Projects-Claude-Code-ios-pixel-4a/c7e2dba9-2389-440a-ae75-e0a3243a6e75/scratchpad
C() { timeout 20 python3 -I tools/flame-cmd.py "$@" >/dev/null 2>&1; }
MARK=$(wc -c < "$L")
END=$(( $(date +%s) + 1200 ))
n=0
while [ $(date +%s) -lt $END ]; do
	n=$((n+1))
	for app in "Preferences com.apple.Preferences" "Safari com.apple.mobilesafari" "Notes com.apple.mobilenotes" "Calendar com.apple.mobilecal"; do
		set -- $app; name=$1; bid=$2
		out=$(timeout 25 tools/sunfish-ssh "openapp $bid" 2>&1 | tr -d '\r' | head -c 150)
		case "$out" in *rror*|*ail*|*nknown*) echo "OPENFAIL c$n $name $out" >> $S/soak1-status.txt; echo "=== soak1 openfail c$n $name $(date +%T)" >> "$L"; exit 2;; esac
		echo "=== soak1 app $name c$n $(date +%T)" >> "$L"
		t=$(date +%s)
		while [ $(( $(date +%s) - t )) -lt 10 ]; do
			for j in 1 2 3; do C swipe 540 1800 540 1000 300; sleep 0.8; done
		done
		ffmpeg -hide_banner -loglevel error -y -f v4l2 -i /dev/video0 -frames:v 1 "../logs/soak1-cycle${n}-${name}.jpg"
		echo "snap c$n $name $(date +%T)" >> $S/soak1-status.txt
		C swipe 540 1800 540 1000 300; sleep 1
		C tap 540 2200; sleep 2
		C swipe 540 2330 540 1100 300; sleep 2
	done
done
echo "=== soak1 end $(date +%T)" >> "$L"
