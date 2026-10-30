# Sensors
**State:** ✅ COMPASS WORKS (2026-10-30 dm4: heading number + dial, fake DeviceMotion HID services {0xFF0C,4}/{0xFF0C,8} fed by our 9-axis fusion, kernel126; true direction needs one real calibration turn, U19); AUTO-ROTATE WORKS (U10, 2026-10-30): the accelerometer drives SweetMetal2 `R N` (adspboot --sns-motion --sns-rot, sweetmotion=0x37); rotates almost at once. All 6 sensors answer. **Updated:** 2026-10-30.

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

### 2026-10-30 18:00 dm9: Compass heading steady at rest: complementary filter + gyro bias (port efafe97)
- kernel127, 4458 mV, SSH 19 s. Phone still ~112 s (work/hdg-dm9b.txt, 224 lines): h 113.4..119.4, raw hmag 108.4..126.7, largest step in h 1.3 deg, mean |h-hmag| 2.4 deg, drift +0.1 deg. Status: "cal fit r 48.0 res 2.86 bias -35.1,-42.6,-62.9, mag gaps >200ms 0 >1s 0 max 20 ms, gate rej r 0 dip 0, gbias -0.0097,-0.0052,0.0052 still 0" (0.0052 rad/s = 0.3 deg/s about z; still read 0 in that sample, not understood).
- The heading under the webcam moved to ~116 with the newer fit (r 48, res 2.86 from the walk; dm7's fit r 42, res 1.78 read 97-105), so the fit itself still varies by ~15 deg when the horizontal field is this small. The saved fit should probably not be replaced by a worse one (next idea, not done). The final screenshot showed the home screen (Compass was not in front at the end; not understood).
- Next: the user's walk test; absolute accuracy vs the Pixel 8 Pro.

### 2026-10-30 17:40 dm8b: the user's walk test: the 4a drifts away at rest; the fusion lags the magnetometer
- The user walked the 4a through a circle, then laid it flat beside the Pixel 8 Pro: started ~10 deg apart, then the 4a went "farther and farther out of sync" (higher degrees).
- Trace work/hdg-dm9.txt (755 lines, T 107-487): the fit updated (r 48.0, res 2.86, bias -35.1,-42.6,-62.9), gate rej r 25, dip 446, no mag gaps. Fast turns show steps up to 81 deg per 0.5 s (real motion). After the handling the heading crawls: 113 -> 167 over ~50 s, then 144 -> 117 over ~50 s. Madgwick beta 0.04 corrects yaw too slowly, gyro bias uncorrected (gcal 0).
- Fix in progress (dm9): gyro bias at rest + complementary-filter heading directly from the tilt-compensated magnetometer (settles in ~2 s).

### 2026-10-30 17:20 The user's second comparison: 4a 102-105 deg, Pixel 8 Pro right beside it 92 deg (10-13 deg apart)
- Same place as before (under the webcam), kernel127 + 9f65fe1. In the first comparison (dm7) the 8 Pro read 52 and the 4a 97; so the 8 Pro moved 40 deg between readings while the 4a moved ~6 deg: the earlier 45 deg gap was mostly on the 8 Pro's side (disturbed or pointed differently; not separated).
- The remaining 10-13 deg fits true vs magnetic north if the 8 Pro shows true north and local declination is ~10-13 deg west (guess: location and the 8 Pro's setting not confirmed). The 4a shows magnetic only (no location, B32).
- Still open: behaviour while being turned (jumps), accuracy at other headings, the slow 4-5 deg wander at rest (dm8).

### 2026-10-30 17:15 dm8: Compass heading steady at rest: no gaps, no fallback, steps at most 0.5 deg (port 9f65fe1)
- kernel127, 4459 mV, SSH 17 s; magcal.txt kept; SNS_HEADING_TRACE=1 by hand in sensors-start.sh (gone at the next install). Compass in front ~113 s, phone still: 225 trace lines, heading 101.0..105.6 (slow wander), largest step 0.5 deg, 0 steps > 3 deg; "dm type 3" 0 of 11 status lines; "mag gaps >200ms 0 >1s 0 max 20 ms, gate rej r 0 dip 0". So at rest the mag stream has no gaps at all; the earlier type-3 lines were not shown to be caused by real gaps (not re-tested while moving).
- Screenshot digits torn by flame-shot (~103 not readable). Next: the user's comparison away from electronics, and whether it jumps while being moved.

### 2026-10-30 16:45 dm7: sphere-fit calibration works (r 42 uT, residual 1.8); Compass 97 deg vs the user's Pixel 8 Pro 52 deg, still occasional jumps
- kernel127, port 3528464 + 81d4e82 + e73eeac (4462 mV). sensors.log 66 KB/min (was ~0.5 MB/min). Fusion off until Compass opens ("dm on (devmotion+devmotion8)").
- After the user's calibration turn: "cal fit r 42.0 res 1.78 n 300 bias -32.5,-43.4,-68.4", acc 1; magcal.txt "fit -32.542 -43.397 -68.388 41.976 1.781 1".
- Phone back under the webcam: Pixel 4a 97 deg, the user's Pixel 8 Pro 52 deg. Raw check (kernel samples): mag (-41.6, -43.2, -114.5) uT - bias = (-9.1, 0.2, -46.1): |m| 47, horizontal only ~9 uT (dip ~79 deg), pointing to device -x, so the top faces ~90 deg (east): the fusion agrees with the raw data and with the user's own "it faces east". With 9 uT horizontal, 2-3 uT of local disturbance (circuit board at the camera, laptop, the other phone side by side) moves the heading 15-20 deg; the 8 Pro may show true north (~-10 deg declination here, guess). Not separated yet: needs a comparison away from electronics, one phone at a time in the same spot.
- Jumps: status shows occasional "dm type 3" (no mag sample for 1 s -> 6-axis fallback, no heading to locationd). Fix in progress (keep type 2 through short mag gaps, gate disturbed mag samples, measure gaps).

### 2026-10-30 16:15 the user's Compass test vs a Pixel 8 Pro: backwards / jumpy; the phone faced EAST, not west
- Correction to 19:25/cw1: the phone lay facing east, so the "west" seed made the heading ~180 deg off ("sometimes completely backwards"); "other times it jumps around".
- sensors.log after the session: SEE mag_cal/gyro_cal sent only 2 events each, all zero (bias 0,0,0, field 4 = 0): the DSP calibration never produced a bias, even while the phone was turned. The min/max box grew from the bad seed: min -110.5 -115.0 -187.3 max 24.5 1.8 -11.2 (spans 135/117/176 uT; a clean span is ~2 x 50 uT), bias -43.0,-56.6,-99.2: polluted, cannot shrink. Status: accel 123169, gyro 122925, compass 46957, devmotion 54415, acc 2.
- Fix in progress: least-squares sphere fit over recent samples (old min/max files ignored), bias changes only when two fits agree, heading low-pass.

### 2026-10-30 20:15 dm6: SEE mag_cal/gyro_cal stream next to the motion sensors (e226cad); they report bias 0 while the phone lies still
- kernel126, adspboot e226cad, magcal.txt.cw1 restored; SSH 22 s. Startup tries 1-2 found 0 motion sensors, try 3 found 8 (6 + 2 cal). Status after opening Compass: accel 2061, gyro 2042, compass 748, als 755, magcal 1, gyrocal 1, written 4407, devmotion 881, "cal box st 0 bias -65.5,-43.2,-69.7 ... gcal st 0 0,0,0".
- Raw cal events (msgid 1022, 16 bytes): magcal `[1/2 len 12: 0 0 0] [4/0 0]`, gyrocal the same: bias 0,0,0 and field 4 = 0 (no field 3 seen, so the status field number is still open). accel and gyro also send 1022 events of 54 bytes (their own cal/comp-matrix events; not parsed).
- Compass 264° W (flame-shot) / 275° W (webcam, later) with the box, phone untouched. The DSP calibration needs the phone to be turned (U19) before it reports a bias; then the raw dumps will show the real field layout.

: SEE mag_cal/gyro_cal exist ("ASH_CAL", GOOGLE), but the first integration stopped all motion streams; reverted, fixed (dm6)
- Port bd5669f (kernel126): lookups found mag_cal (suid 0a9b44af...) and gyro_cal (suid 082bf075...), name "ASH_CAL", vendor GOOGLE, available 1, rates 10. But the status line showed only "magcal 0 gyrocal 0, written 0, devmotion 0": accel/gyro/mag never started, Compass had no heading. Reverted (5384410); working build restored (dm5r).
- Root cause (implementer, e226cad): in sns_run.c start() the 6 motion lookups got no answer on the first try (sensors PD still loading) while the 2 cal lookups did, so start() saw 2 sensors as success and never retried; late lookup replies then landed on the cal clients' ports ("magcal: SUID event for mag"). Also msgid 1022 (16 bytes: packed bias 3 x f32 + status) is the SEE cal event and was dropped as "unhandled".
- Fix e226cad: motion sensors are set up first and the cal sensors only when at least one motion sensor was found (else retry as before); cal sensors are enabled only after accel/gyro deliver samples; 1022 goes to the cal parser. Host test reproduces the failure. Not yet on the phone (dm6).

### 2026-10-30 19:45 cw1: corrected seed reads 263-265° W (true heading ~270° W)
- kernel126 (the script adds sweetmotion=0x4bf), work/magcal-west.txt pushed right before the reboot: the first try lost the race to adspboot's 30 s save (old bias), the second took: status "bias -65.5,-43.2,-67.7 span 90,90,90".
- Compass (phone untouched, top pointing west per the user): screenshot 263° W (one digit glitched), webcam 265° W (work/shots/cw1.png, scratchpad cw1-cam.jpg). 5-7° short of 270, similar to dm4's 351 vs ~0 for the old seed, so a small constant offset remains (raw value from an older boot, field-strength guess, or tilt; not separated).
- Next: dm5 = SEE mag_cal/gyro_cal biases (port bd5669f) in place of the box.

### 2026-10-30 19:25 the user: the phone lies flat with its top pointing WEST (dm4 showed 351 deg)
- dm4's 351 deg is the seeded guess (bias assumed the top pointed north), so the heading math and axes look right; only the hard-iron bias was wrong. With the top west and face up, device x = north, y = west, z = up, so the expected field is (Bh, 0, -Bz) ~ (21, 0, -48) uT (assumed east-US field; horizontal magnitude only scales the error when the phone is turned).
- New seed: bias = raw (-44.5, -43.2, -115.7) - (21, 0, -48) = (-65.5, -43.2, -67.7) uT (old -44.5, -64.2, -69.6): box min -110.5 -88.2 -112.7 max -20.5 1.8 -22.7 (work/magcal-west.txt). Expected after a reboot: ~270 deg W (cw1).

### 2026-10-30 19:10 dm4: COMPASS WORKS (kernel126, heading in the DeviceMotion report)
- Port e14aaf6: type-2 reports now carry the magnetic heading (@0x44, degrees clockwise from north, from our north-referenced q), its accuracy (@0x48: 10/20/35 deg at calibration level 2/1/0, -1 uncalibrated) and status bit 0x100 (locationd's CLCompass7 publishes only with it; it does not compute heading from q/mag itself). Host test: heading field 30 for yaw 30, status 0x100, accuracy 10.
- dm4 (kernel126, everyday recipe + sweetmotion=0x4bf, /var/local/sensors.devmotion, seeded /var/local/magcal.txt): Compass shows a heading number and turns its dial. Webcam (yaw 0): "351° N"; yaw knob 90 -> "85° E" (shot mid-turn), yaw 200 -> "192° S" (351+200 = 191: matches), dial turned with N at lower right (work/shots/dm4-b/c.png). Compass logs [CLSensorFusionService] samples ~20/s. Status "dm type 2 acc 2 bias -44.5,-64.2,-69.8 span 90,90,94.1".
- Not verified: the true direction. The calibration box is a seeded guess (phone never turned), so the absolute heading is likely off until the phone has been turned through all orientations once (magcal.txt then fills from real data): user task U19. True north needs a location (none: B32), so Compass shows magnetic heading.
- Made default: tools/sunfish-boot.sh passes sweetmotion=0x4bf for kernel126+; tools/sunfish-install-sensors.sh creates /var/local/sensors.devmotion. Cost: accel+gyro+compass stream at >= 100 Hz while the screen is on (Optimize O13).

### 2026-10-30 18:40 dm3: locationd now takes Compass's heading request and finds {0xFF0C,8}, but no heading comes out (kernel126)
- kernel126 = kernel125 + xnu 0245: service "devmotion8" {0xFF0C,8} (class SweetHIDDeviceMotion8, sweetmotion bit 0x400; bits 0x100/0x200 are Transport-SPU / initial-events flags) gets a copy of every type-2 report with payload bytes 4..11 zeroed (sweetdm8raw=1 keeps them). Boot: everyday recipe + sweetmotion=0x4bf; battery 4454 mV, SSH 17 s.
- Kernel: devmotion8 opened through the event system (backboardd pid 60, as for every HID service), ReportInterval=20000 at 105.6 s; type-2 reports #1000 at 124 s, #3000 at 163 s; 0 rejects.
- locationd (cl3 oslog): "Incoming message kCLConnectionMessageHeading" from Compass; "{65292, 8} not ready" is gone; "{65292, 1} not ready", "Could not set all properties ... Giving up" and "Failed to configure motion coprocessor" remain. Compass: dial at N, no heading number; yaw 90/200 changes nothing (work/shots/dm3-a/b/c.png). Status "dm type 2 acc 2 ...".
- Next: RE of CLCompass7's handler checks (what marks the heading invalid or keeps it from being published).

### 2026-10-30 18:00 dm2: 9-axis (type-2) reports accepted, but the Compass dial ignores them: the heading comes from locationd
- Port 8db68aa: type-2 88-byte reports (9-axis Madgwick, x = magnetic north, min/max hard-iron box in /var/local/magcal.txt, accuracy 0/1/2 at 40/60/80 uT span, yaw knob /var/local/dm-yaw). Host test: north 0, east 90, west 270, yaw +30 -> 30.
- dm2 (kernel125, sweetmotion=0xbf): seeded magcal from compass (-44.5,-43.2,-115.7) uT -> status "dm type 2 acc 2 bias -44.5,-64.2,-69.6 span 90,90,93.8"; kernel: type-2 reports flow, 0 rejects (a few type-3 reports interleaved, probably when a mag sample is >1 s old). Compass: dial at N, no heading number; yaw 90 and 200 change nothing (work/shots/dm2-a2/b/c.png).
- So the dial is driven by locationd's heading (CLHeading), not by in-app device motion (which feeds the level/parallax). locationd opens the compass {0xFF00,10} at 25 ms but waits for {0xFF0C,1} and {0xFF0C,8} ("not ready", "Failed to configure motion coprocessor"). Next: RE locationd's heading path and those two services.

### 2026-10-30 17:10 dm1: CoreMotion accepts our DeviceMotion reports; Compass draws its dial but has no north yet (kernel124)
- Boot: kernel124 (xnu 0243), everyday recipe + sweetmotion=0xbf, /var/local/sensors.devmotion (SNS_DEVMOTION=1), adspboot 13:49 (backup adspboot.pre-dm). Battery 4455 mV; SSH 17 s; no resets.
- Kernel: backboardd opens devmotion at 24.6 s (setProperties DeviceOpenedByEventSystem, MotionEventRestricted=false, PreserveTimestamp=true, ReportInterval=0). Compass sets ReportInterval=16667 at 102 s; type-3 52-byte reports flow from 106 s (#1000 at 124 s), 0 rejects. sensors.log: devmotion 562 -> 1101 -> 1639, accel ~3424, gyro ~3413, compass 1479.
- Compass (cl2 log): "Starting device motion, mode=0x22, useGyro=1, useCompass=0, fUseNorthRef=1", then a decoded [CLSensorFusionService] q/userAccel/rotationRate sample; the old "{65292, 4} not ready" / "Could not set all properties" lines from Compass are gone. Screen (work/shots/dm1-compass.png): full dial N/E/S/W, needle at 0, grey centre, no heading number or coordinates.
- Guess: fUseNorthRef needs the 9-axis report (type 2, with magnetometer + bias + accuracy); type 3 marks the field "none", so no heading. Next: type-2 reports with a hard-iron bias estimate (dm2).
- locationd separately waits for {0xFF0C,1} and {0xFF0C,8} ("not ready", "Failed to configure motion coprocessor"; MotionOdometer/activity services) and sets {0xFF00,10} ReportInterval 25000: later.

### 2026-10-30 16:35 cs4: DeviceMotion report layout decoded; fake service being written (xnu 0243)
- RE (research/notes/devicemotion.md "Report layout"): payload size must match a table at 0x191cd3cb8: type 2 = 88 bytes, types 3/4 = 52 bytes; little-endian float32; timestamp from the HID event (mach time). Type 3/4: q[x,y,z,w] @0x0c, rotationRate @0x1c, userAccel @0x28. Type 2 adds raw magnetometer, bias and accuracy. Units (g, rad/s, uT) and which class wants which type: not confirmed; type 3 is the first try.
- Implementation started: SweetHID service {0xFF0C,4} behind a new sweetmotion bit 0x40 (default off), raw-report write path in /dev/redwood-motion, Madgwick fusion in phone/sensors (SNS_DEVMOTION=1).

### 2026-10-30 16:20 cs3: no switch to CoreMotion's own software fusion; Compass needs a DeviceMotion service (B46)
- Read-only RE (CoreMotion.s, research/notes/devicemotion.md "SPU vs AP fusion selector"): after "Starting device motion" (0x1919c43c4) CoreMotion tests bits 3/4/7 of CLPlatformInfo::motionCaps() (sub_1919c14e0, cached u64). A set bit picks the motion-coprocessor (SPU) class, which opens HID {0xFF0C,4}; clear bits pick the built-in software fusion (AP classes, Mekf filters).
- motionCaps is a fixed mask per hardware type, compiled in (queryMotionCaps sub_1919c4c9c, jump table 0x191ba7234). The hardware type comes from MGIsDeviceOfType on ProductType iPhone10,3, and a real iPhone X has the M11, so we get the SPU bits. No MobileGestalt key, defaults key or IORegistry property changes it. The only preference, OverrideMotionCapEclipseService, touches bit 8 only. The IsSimulator route gives caps 0 (no motion at all). So the cheap "flag" fix is out.
- cl1 (kernel123): locationd log during a 25 s Compass run, 2742 lines. No heading, magnetometer or calibration lines at all; "currently 0 non-system location clients". Compass registers with locationd ("+++ compass.app") but never asks for heading, probably because it is stuck on the device-motion start (guess).
- Next (B46, days): a fake {0xFF0C,4} HID service that sends VendorDefined device-motion reports (types 2/3/4, populateDeviceMotion @~0x1919b1e08) with a quaternion from our own fusion. First step: decode the report byte layout (read-only).

### 2026-10-30 15:00 O12: sensors idle while the screen is blanked (408bcb5): screen-off CPU 2.80 -> 0.05 s per minute
- adspboot reads backboardd's "com.apple.springboard.hasBlankedScreen" state (notify_check each tick): blanked -> the orientation-only accel stream stops and the loop sleeps 20 ms (was 2 ms); "sns: screen blanked: sensors idle" / "sns: screen on: sensors back"; SNS_DARK=0 = off.
- pw2 (kernel122, normal recipe): adspboot screen-off CPU 0:05.73 -> 0:05.78 in 60 s = 0.05 s (old binary: 2.80 s); 3 K/H cycles each logged the blanked/on pair, "sns: orientation 5 posted" after each wake, accel counts rising again. Proximity still fine (fake near -> display off ~1 s, far -> on). Oddity: the first K after boot logged no "blanked" (3.05 s CPU that minute) and the display came back by itself at 96.6 s with a second "lock button: sent": not explained.

### 2026-10-30 13:45 cs1/cs2: Compass waits for a "DeviceMotion" HID service we don't have (page 0xFF0C usage 4)
- Compass (pid 334, kernel122) stays alive in the background but shows no heading; its CoreLocation log: "[CLIoHidInterface] Service for device with usage pair {65292, 4} is not ready" + "Could not set all properties after too many retries. Giving up." x22. 65292/4 = kHIDPage_AppleVendorMotion / DeviceMotion (AppleHIDUsageTables.h:105): the fused motion output a real iPhone's motion coprocessor provides. Accelerometer {0xFF00,3} and gyro {0xFF00,9} were accepted and stream; magnetometer samples flow too (kernel "compass #1000: x -43931 y -42412 z -115256 nT"), iOS asked compass at 25 ms.
- Backlog B46: a DeviceMotion service (host-side fusion of accel+gyro+mag -> quaternion events in Apple's format): RE + new service, days. Not now.

### 2026-10-30 13:30 px3: PROXIMITY WORKS in iOS (kernel122, sweetmotion=0x3f, now the default)
- Two fixes: xnu 0241 (prox service Placement = 1: backboardd's discrete prox wrapper, no NSNull crash loop) and detection mode 3 or 6 (backboardd treats every other mode as inactive: research/notes/proximity.md, backboardd 0x10002d820).
- `proxon 3 60` + `m fake prox 1` / `0`: proxon "active yes, near" during the fake, "active yes, far" around it; kernel "prox #1 (fake): near (mask 0x41)" 106.566 -> "SWEET FB: display off" 107.107 (0.5 s); "far" 118.006 -> "display on" 118.020. Mode 6 the same (off 147.380, on 152.951). backboardd exec count 1 throughout, no reset.
- (flame-shot reads the framebuffer, so it can't show a panel-off state: use the kernel display off/on lines.) The real TCS3701 prox samples flow via the sensors job when iOS asks (SNS follow); a real hand-over-sensor check is with the user (U17).
- tools/sunfish-boot.sh default sweetmotion=0x3f (needs kernel122+; older kernels: pass sweetmotion=0x37).

### 2026-10-30 13:15 px2: kernel122 (xnu 0241 prox Placement = 1): NO MORE backboardd crash loop; detection not active yet
- kernel122 sweetmotion=0x3f: "exec backboardd" 1 in 2.5 min, 0 applyUIMode/NSInvalidArgumentException (px1: 11 restarts); kernel "SWEET MOTION: prox property read Placement: present (pid 60 backboardd)". So the prox HID service is accepted.
- But `proxon 1 40` ("requested proximity mode 1") read "active no, far" every second; `m fake prox 1` ("fake prox 3f800000 ... repeating for 40 s") and flame-shots: screen stayed on, no "SWEET FB: display off". Kernel prox lines show "far (mask 0x0)" (the fake may be parsed as 1.0 -> no mask?). backboardd: "_proxState=kProxNone".
- Open: why the detection request is not active (entitlement for BKSHIDServicesRequestProximityDetectionMode? mode number?) and whether the fake near reaches the event (mask). Normal recipe stays sweetmotion=0x37 for now.

### 2026-10-30 12:45 px1: proximity service on (sweetmotion=0x3f) still crash-loops backboardd
- kernel121 HAPTICS=1 CAMERA=1 sweetse9=2 sweetcampmic=2 sweetmotion=0x3f: "exec backboardd" x11 in ~130 s (every ~11 s from 23.5 s); "-[NSNull applyUIMode:]: unrecognized selector", NSInvalidArgumentException (69 lines), liblog_aop lines; black screen + spinner. backboardd opened SweetHIDProx (1 client), read IOCFPlugInTypes / IOClassNameOverride / QueueSize (absent). The 2026-10-26 fixes (DisplayIntegrated, ProtectedAccess, SensorPropertySupported, initial far event) don't help. Back on 0x37: backboardd stable.
- New tool phone/tools/proxon.m (479fc5f: BKSHIDServicesRequestProximityDetectionMode on/off + active/near getters) for the test once prox loads. Next: backboardd disassembly for the NSNull source (investigator).

### 2026-10-30 09:05 Auto-brightness PARKED (~2 h): lux reaches iOS and AAB ramps, but no path writes the steps to our panel
- bl3 (kernel115, new arch): backboardd "Modules not ready: Caching DisplayBrightnessFadePeriod property" and no later ready line; 0 SWEET BL lines all boot. RE (research/notes/ambient-light.md "modules not ready"): in new-arch mode BLControl builds the container from QuartzCore's CADisplay brightness control (CBBrtControl initWithCABrightnessControl:), and its modules start only when CA reports brightness capabilities or sends brightnessNotificationAttached: CA never does here. Our backlight property types all match what CoreBrightness reads.
- Legacy mode (kernel113 / sweetblnew=0): AAB runs (real ramps after autobright 0/1, "begin ramping L: 105.56 -> 399.41"), but the legacy fade only writes the slider's brightness, never AAB's L (ramp output path).
- So neither mode gets AAB's target to the panel. 0239 default switched to OFF (sweetblnew=1 to opt in). Options later: our own auto-brightness (lux from adspboot -> a curve -> the slider/DisplayBrightness path, respecting the night flag), or giving CA a brightness control. Backlog B42.

### 2026-10-30 08:30 k115 (kernel115 = + xnu 0239 new-backlight-architecture): new path on, but auto-brightness stops entirely
- No reset in two boots (sweetuibig=0, then default). The runner's tests were void: the panel auto-locked at ~67 s and was never woken (all frames black).
- By hand (screen woken with H, backboardd restarted): oslog "New backlight architecture." (0239 works). But then backboardd sets no ALS properties at all (only accel/gyro ReportInterval), no AAB lines, no SWEET BL writes during `m fake als 20000` / `1`, and BrightnessSystemClient DisplayBrightnessAuto reads (none) (was 1); "EDR - transceive_data: mach_msg() => (ipc/send) timed out" x2.
- So the new architecture wants more than the property. Investigator: legacy path + the missing piece vs new-arch requirements. Recipe note: tests must wake the screen (`H`) right before each step.

### 2026-10-30 08:05 hp1: after autobright 0/1 the ramps are real, but no step reaches the kernel
- `autobright 0` then `1` (BrightnessSystemClient DisplayBrightnessAuto): backboardd "begin ramping L: 105.56 -> L: 399.41 Brightness: 0.718309 -> 0.959318 t: 2.710891 rate: 108.40 nits/s 60.62hz" at 20000 lux, "begin ramping L: 399.41 -> L: 2.00 ... t: 57.559097" at 1 lux; "begin virtual ramp" 0. So the runtime auto flag needed the re-toggle (now part of the recipe).
- Kernel: no SWEET BL setProperties/nits and no SWEET FB IOMFBBrightnessLevel line during either ramp (only the periodic "IOMFBBrightnessLevel = 6917855" = 105.56 nits every ~73 s). Webcam: no visible change. Next: where the per-step writes go (investigator).

### 2026-10-30 07:55 Auto-brightness on kernel113: CoreBrightness ramps, but only "virtually" (als1/als2)
- als1 (kernel113 CAMERA=1 sweetse9=2 sweetcampmic=2, 4456 mV): backboardd "Begin ramp ... Lmax=500.000000 Lmin=2.000000" (0237's range is seen); boot brightness now 105 nits ("SWEET BL: nits 105 (raw 698edf)", slider 0x5f58). ("nits range 2..500 published" is not in the live log: printed before the log link was up.)
- als2 (`m fake als 20000 5000` for 60 s, then 1 lux 60 s): "_Esensor_trusted: 20000.00 L: 399.4088", "L_newtarget=399.408783 _L_current=392.568665", but no SWEET BL write and no visible change; 1 lux -> L 2.0.
- CoreBrightness RE (research/notes/ambient-light.md "ramp output path"): the AAB step goes to _DisplaySetVirtualBrightnessWithFade, which checks the display object's runtime auto flag (+0x28); 0 -> "begin virtual ramp" (only Lvirtual moves). With it set, the fade callback writes "brightness-nits" on our backlight service (SweetBacklight.cpp:207 setNits already handles it). Guess: the flag is set only when DisplayBrightnessAuto is (re)applied through DisplaySetProperty (Settings switch / BrightnessSystemClient setProperty). Next: `autobright 0` + `autobright 1`, then the fake lux again (after df1 frees the phone).

### 2026-10-30 07:30 xnu 0237 (kernel113): SweetBacklight publishes a nits range 2..500 for CoreBrightness
- research/notes/ambient-light.md "systemShallUpdateBacklight": with no range CoreBrightness falls back to logical 0..1 (the 20000 lux target 1.0 = _L_current 1.0, no delta, no update). 0237 (from ios-redfin 0973, MIT): MinNits 2, MaxNits/MaxNitsPanel 500, HardwareAccessibleMin/MaxNits, DisplayPanel/ProductLuminanceMin/Mid/Max 2/155/500, backlight-calibration-parameters (16.16; the current values are guesses), user-accessible-max-nits / digital-dimming-min-nits as 4-byte 16.16 OSData; setNits maps < 2 nits to DCS 32. `sweetnits=0` turns it off. Built (37 s), test als1 in progress.

### 2026-10-30 07:10 Ambient light: iOS auto-brightness runs on our lux, but "systemShallUpdateBacklight=false"
- New phone tools: mgq (MobileGestalt answers) and autobright (CoreBrightness BrightnessSystemClient DisplayBrightnessAuto get/set), phone/tools/build.sh.
- mgq: ambient-light-sensor = true, proximity-sensor = true, DeviceClassNumber 1, ProductType iPhone10,3. autobright: DisplayBrightnessAuto = 1. So the MobileGestalt gate passes and Auto-Brightness is on.
- backboardd info log (`oslog -l -t 30 -n backboardd`) during `m fake als 20000 5000`: "_state=kAABStateAutoBrightness", TrustedLux 20000, "_Esensor_trusted: 20000.00 L: 1.0000"; real light 2 lux -> "L: 0.0000". Every cycle "systemShallUpdateBacklight=false L_target=1.000000" (0 at 2 lux), "_L_current=1.000000". So the lux is used, the curve target moves, and something keeps CoreBrightness from writing it. Next: what sets systemShallUpdateBacklight, and the 0..1 nits range (investigator).
- Note: oslog's default run time is 10 s (`-t 0` = forever); earlier short captures (jack7/jack8) came from that, not from dropped lines.

### 2026-10-30 07:15 Ambient light: samples reach backboardd, brightness doesn't follow yet (kernel112, sweetmotion=0x37, sweetbl=2; boot sp16r)
- research/notes/ambient-light.md: the ADSP lists "als + prox TCS3701"; adspboot's SNS follow already handles ALS; the kernel ALS service exists and backboardd opens it like an AppleALS.
- At boot the sensors job finds its sensors only after ~30 s (two 10 s retries) while backboardd asks for ALS at 26 s and sets ReportInterval 0 at 45.8 s together with the screen's brightness 0 (display sleep), so no ALS during the first minute. After `K` (lock) + `H` (wake): "sns: als: start", 254 then 752 ALS samples posted, backboardd ReportInterval 400000 (ALSIntegrationMode 2/3).
- `m fake als 20000 5000` for 15 s, then `m fake als 1 5000`: the kernel logs each fake sample with 1 client, but SweetBacklight gets NO setProperties (slider stays at its minimum, position 0x1). So CoreBrightness ignores the lux or auto-brightness is off. backboardd prefs: BKALSUserPreferences present (S 0, E2 2000), no BKEnableALS key.
- Next: CoreBrightness RE for the enable gate / missing service properties (ALSSensorMax is absent) (investigator, in progress).

### 2026-10-30 19:45 Auto-rotate from the boot job confirmed by the user (u10h, kernel108 defaults)
- Fresh boot with the new defaults (sweetmotion=0x37, launchd org.iosonpixel.sensors via tools/sunfish-install-sensors.sh): the feed starts by itself (adspboot pid 37, "SNS service 400 ... starting in 10 s", orientation posts). The user: Calculator auto-rotates just fine.

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
