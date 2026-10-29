# Sensors
**State:** ✅ AUTO-ROTATE WORKS (U10, 2026-10-30): the accelerometer drives SweetMetal2 `R N` (adspboot --sns-motion --sns-rot, sweetmotion=0x37); rotates almost at once. All 6 sensors answer. **Updated:** 2026-10-30.

## Facts
- No motion/light sensor is on an AP bus (../recon/live.dts, every QUP child checked): SE1 has st21nfc@08 (NFC), SE7 the touch controller, SE9 fsa4480 (USB-C audio switch) and slg51000@75 (camera PMIC); no lsm6/bmi/icm/ak09/mmc/tmd/stk/vl53/bmp/lps node anywhere.
- On SM7150 the sensors sit behind the SSC inside the ADSP/LPASS (no separate SLPI): qcom,lpass@62400000 (ssctl-instance 0x14, :4946-4960) and `qcom,fastrpc-adsp-sensors-pdr` (:4251). The SSC QUPs are LPASS-side (probably XPU-protected; not proven). Part numbers are not in the DT.
- iOS side: the shared cache has AppleSPU, AppleSPUHIDDriver/DeviceClass, AppleSPUAccCompassPlugin, AppleSPUTimesync (no AppleALS/AppleProxShim). Live-log matches that return "none": SpringBoard IONameMatch "als" (x474), IOPropertyMatch name "gyro" (x239), IOProviderClass AppleSPUTimesync (x243).

## Plan (after the ADSP boots, see audio.md M0)
1. ADSP up + QRTR lookup lists the SNS client service (QMI 400; nanopb requests, as in postmarketOS libssc/hexagonrpcd), plus the sensor registry/config files from vendor (/vendor/etc/sensors/*.json) served over fastrpc/remotefs. Risk: firmware asserts on missing registry files.
2. Stream accelerometer samples to a kernel log (1-2 weeks).
3. A kernel IOHIDEventService for the accelerometer -> auto-rotation (2-4 days; Apple's vendor usage page/usage to confirm by disassembling BackBoardHIDEventFoundation / CoreMotion).
4. Gyro + magnetometer (~2 days each), prox + ALS (3-5 days). No barometer in the DT.
Fallback without the ADSP: a sysctl that injects rotation events (1-2 days), useful for landscape apps and to test step 3 early.

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

### 2026-10-30 19:15 AUTO-ROTATE WORKS (U10 by the user; kernel108 sweetmotion=0x37, adspboot --sns-motion --sns-rot)
- Raw accel during the user's turns (u10c): upright y~9.2 -> orientation 1, landscape x~9.5 -> 3, posted at once and held: the sensor side was right all along.
- iOS's own path (orientation HID service, sweetmotion=0x77): the oslog (u10e) shows backboardd -> SpringBoard rotating within 0.35 s (SBRotateScenesWorkspaceTransaction Prepare/Animate/Complete, Calculator "will change interface orientation"), plus GraphicsOrientation / display orientation changes, yet the panel only showed it at the next change (user) or not at all (mo3, webcam). Mach-time stamps (u10d) and re-posts (u10b) changed nothing.
- Fix: --sns-rot writes SweetMetal2 "R N" (scene settings, frame kept portrait: the path that draws, rot10/rot12) on each portrait/landscape change, and iOS's orientation service is left unregistered (sweetmotion=0x37) so SpringBoard does not also rotate the system/display. u10g: the user: "rotated basically right away", stays rotated.
- At boot (tools/sunfish-install-sensors.sh): job org.iosonpixel.sensors runs /var/local/bin/sensors-start.sh -> adspboot --sns-motion --sns-rot, --watch 0 --sns-secs 0 (no limit), from uptime 25 s, log /var/local/sensors.log. Off: `touch /var/local/sensors.off` + reboot (needed before running adspboot by hand, e.g. audio: one adspboot owns the ADSP), or `tools/sunfish-install-sensors.sh --undo`. sweetmotion=0x37 is a sunfish-boot.sh default.
- Next: make it permanent (launchd job for the sensor feed + sweetmotion=0x37 default + install script); later: why native rotation does not draw (backboardd display orientation vs our framebuffer).

### 2026-10-30 14:10 U10 auto-rotate by the user: rotates only when the phone is turned back (kernel107 sweetmotion=0x77; adspboot --sns-motion)
- Per-sensor QRTR ports fixed the feed: 1460 samples in 40 s, 0 rejected (prox off by mask).
- U10: Calculator open; upright 5 s, landscape held 5 s / 10 s / 30 s: no rotation while held. Turning back upright: it rotates to landscape and immediately back to portrait. sensd posted 5 -> 1 -> 3 -> 1 -> 5 (accel counter rising ~27/s the whole time, so samples flow while still).
- Theory 1 (each event delivered one step late) tested: re-posting the current orientation every 0.5 s with a fresh mach time (216 orientation events reached the kernel) changed nothing. So: next is the raw accel during a hold (1 Hz "sns: acc x y z orient cur (cand)" log added) to check the 4a's axes and the filter (sweetmotion_axes, sensor.c orientation thresholds).

### 2026-10-30 12:50 iOS ROTATES NATIVELY from the orientation HID service (mo1, mo2; kernel107 sweetmotion=0x77)
- mo1: backboardd (pid 59) opens the orientation service at 24 s (also accel/gyro/compass/pressure/als, interval 0 except als 0.1-0.4 s). Fake orientation events reach it. Real sensors: adspboot --sns-motion posted "orientation 5" (face up, correct: phone flat) but only 5 samples written, 4 rejected: all sensors shared QRTR client port 0x40, so each sensor's queue saw the others' SUIDs ("event 1025 from another SUID ..." x1015) -> fix in progress (per-sensor ports).
- mo1 frames looked unrotated; mo2 explains: the phone was already landscape from an earlier S 3. mo2 oslog on `m fake orient 3`: kernel "orientation #49 (fake) 3" -> backboardd "Received orientation (FaceUp to LandscapeLeft)", "Updating device orientation from CoreMotion to: landscapeLeft (3)", sent to SpringBoard -> SpringBoard "Active interface orientation changing ... to landscapeRight (3)", "Running SBRotateScenesWorkspaceTransaction", "Did set interfaceOrientation[3] on scene" for Calculator; frame landscape. No stale/ignored/locked. Orientation lock keys unset.
- So iOS's own rotation path works from our sensor HID service (no SweetMetal2 R needed). Next: real accel samples (per-sensor port fix), then a tilt test by hand (U10).

### 2026-10-30 12:10 ALL SIX SENSORS ANSWER; accel streams (sn1, kernel106; adspboot e9a349e)
- `--sns-list`: 6 of 6 types: accel + gyro LSM6DSR (STMicro; 26-416 Hz; ±2 g, ±125 dps), mag LIS2MDL (STMicro; 0-100 Hz), pressure BMP380 (Bosch; 1-25 Hz), als + prox TCS3701 (AMS; 10 Hz). So the guessed soc0/registry values were right.
- `--sns-stream accel --sns-rate 50 --sns-secs 20`: 1072 samples (~54 Hz), x -0.04 y -0.57 z 9.86, |a| 9.88 m/s^2 (phone flat, face up). No timeouts or PD crash; rc 0; no reset; 4455 mV.

### 2026-10-28 17:15 orientation without sensors: the path (read-only)
- BackBoardServices BKSAccelerometer (setOrientationEventsEnabled:, currentDeviceOrientation; notify state + a mach port from backboardd); SpringBoard SBDeviceOrientationUpdateManager (_enqueueOrientationUpdateToDeviceOrientation:), SBOrientationLockManager; backboardd's _handleDeviceOrientationChangedHIDEvent: takes kIOHIDEventTypeOrientation (10), subtype CMUsage (1) (backboardd itself is not in the shared cache: its HID matching keys are unknown).
- AccessibilityUtilities `-[AXBackBoardServer setDeviceOrientation:]` (AssistiveTouch's rotate) sends AXIPCMessage key 0x81b {deviceOrientation: n} to backboardd.
- Plan: from SweetMetal2.bundle in SpringBoard, watch /var/local/sweet-orient (1/2/3/4) and call the AX path (fallback: SBDeviceOrientationUpdateManager). The real path later: a kernel HID orientation service once backboardd is pulled from the root and disassembled.

### 2026-10-28 14:10 survey (read-only)
- Findings above. Also seen: locationd exec'd 223 times in the live log (a respawn loop, not yet checked; plan B19).
