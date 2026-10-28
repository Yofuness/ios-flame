# Sensors
**State:** ⬜ not started; survey done 2026-10-28 (waits for the ADSP work). **Updated:** 2026-10-28.

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

### 2026-10-28 17:15 orientation without sensors: the path (read-only)
- BackBoardServices BKSAccelerometer (setOrientationEventsEnabled:, currentDeviceOrientation; notify state + a mach port from backboardd); SpringBoard SBDeviceOrientationUpdateManager (_enqueueOrientationUpdateToDeviceOrientation:), SBOrientationLockManager; backboardd's _handleDeviceOrientationChangedHIDEvent: takes kIOHIDEventTypeOrientation (10), subtype CMUsage (1) (backboardd itself is not in the shared cache: its HID matching keys are unknown).
- AccessibilityUtilities `-[AXBackBoardServer setDeviceOrientation:]` (AssistiveTouch's rotate) sends AXIPCMessage key 0x81b {deviceOrientation: n} to backboardd.
- Plan: from SweetMetal2.bundle in SpringBoard, watch /var/local/sweet-orient (1/2/3/4) and call the AX path (fallback: SBDeviceOrientationUpdateManager). The real path later: a kernel HID orientation service once backboardd is pulled from the root and disassembled.

### 2026-10-28 14:10 survey (read-only)
- Findings above. Also seen: locationd exec'd 223 times in the live log (a respawn loop, not yet checked; plan B19).
