#!/bin/sh
# camisp (camera stage e3): ios-redfin's camera_isp.c on saved RAW10 frames. Phone: out/camisp;
# PC: ../../work/cam/camisp-host (when cc is there).
set -e
cd "$(dirname "$0")"
mkdir -p out
../../work/userland/bin/ios-cc -O2 -Wall -o out/camisp camisp.c camera_isp.c
../../tools/linux-ios-sign out/camisp
# camera_avtest (ios-redfin, MIT): lists AVCaptureDevices and formats only
../../work/userland/bin/ios-cc -O2 -Wall -fobjc-arc -framework Foundation -framework AVFoundation -o out/camera_avtest camera_avtest.m
../../tools/linux-ios-sign out/camera_avtest
if command -v cc >/dev/null; then
	mkdir -p ../../work/cam
	cc -O2 -Wall -o ../../work/cam/camisp-host camisp.c camera_isp.c -lm
fi
