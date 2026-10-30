# Completed

Finished items moved out of docs/plan.md and docs/backlog.md (the user, 2026-10-30: keep those two short and clear).
Newest first. Details stay in the feature Logs (docs/features/<name>.md).

## From the plan (2026-10-28 .. 2026-10-30)

- 2026-10-30 PROXIMITY in iOS (B12 part): xnu 0241 Placement = 1 + detection mode 3/6; near -> panel off in 0.5 s (sensors.md px3). Default boot sweetmotion=0x3f, kernel122.
- 2026-10-30 FILES "ON MY IPHONE" OPENS (B37): fpfix.dylib (fileproviderd accepts our HFSX volume) + xnu 0240 v5 (Sandbox op 4 container path from the exec's HOME), kernel121 (docs/features/apps.md).
- 2026-10-30 HAPTICS (plan item 9, B39): iOS haptics work: Settings switch taps -> SunfishAudio Actuator FIFO -> hapticd (launchd job, SE9 lock) -> DRV2624; boot with HAPTICS=1 (docs/features/haptics.md). Open: the user's feel check (U16).

Done 2026-10-30: VIBRATION MOTOR BUZZES (hp2), camera opens fixed (2ed9d22), daily upstream check. Done 2026-10-30 (night): BOTH CAMERAS STREAM + iOS LISTS THEM (cam1-cam33), Bluetooth boot bring-up fixed (bt12), oslog survives restarts (B31), B36 = touch silent while plugged in (kernel112 status line). Done 2026-10-30: AUTO-ROTATE (accelerometer -> R; sensors via the ADSP), BLUETOOTH IN iOS (Settings on, devices listed; kernel105 + btbridge, iOSPlus10/redfin code), Bluetooth LE scan from btup (bt3, 26 devices), Bluetooth chip answers (bt1), H/K home+lock commands, B34 closed, reproducibility audit (docs/setup.md, docs/device-changes.md, tools/sunfish-setup-phone.sh), zero-copy flip mode works (kernel103 manual, no fps gain; parked, O2/O7). Done 2026-10-29: rotation incl. landscape touch (`R 3`/`R 1`; auto-rotate waits for sensors B12), Notes keyboard typing, fake taps/swipes for tests (`T`/`W`, methods.md), Location Services on (`L 1`), rotation follows app switches (rot12), rotation via the front scene's settings (Calculator landscape), r/hackios survey (research/notes/hackios-survey.md: SM7150 audio/BT/camera references), kernel95 = 0 crashes per boot (AegirPoster, App Store fixed), Photos opens (assetsd re-enabled), Music opens (kernel95, xnu 0219 sandbox extensions), Notes and Wallet open fine (survey black was session state), Maps opens (B24: listed + 3 bridge selector commits), B1 closed (mtl2.log truncates at 8 MB by design; 0.8 MB now), B19 lskdd/fairplayd.H2/wifip2pd dropped (xpcproxy 633 -> 314 per 10 min), app survey (11 of 18 apps fine; B24-B27 in backlog), D2 SSH earlier (dropbear Interactive priority: boot to SSH 16-18 s, was 24-41), VMware bridge pinned (boot to SSH 24-29 s, 0 lost sessions), SMMU streams for the DSP (U7), Wi-Fi auto-join accepted (U2), FastRPC listener written. Done 2026-10-28: camera IDs, brightness slider, flashlight button, SE9 I2C + amps, boot-arg defaults.

## Phone tasks and dev-speed items done

- U9 [DONE 2026-10-30: Xbox controller pairs; AirPods crash bluetoothd] Bluetooth pairing: put headphones or a keyboard in pairing mode near the phone, tap it in Settings > Bluetooth. ~3 min.
- U8 [DONE 2026-10-30: works] home swipe with a finger: from inside any app, swipe up from the bottom edge: does it go home? ~1 min. (B33)
- U5 [DONE 2026-10-30] speaker beep: silent in bypass and boost (audio.md).
- D5 [DONE 2026-10-29] fake taps/swipes (`T`/`W` in /var/local/sweet-orient): tests no longer need a finger.
- D2 [DONE 2026-10-29] SSH earlier at boot: dropbear Interactive, SSH up 16-18 s.
- D4 [DONE 2026-10-29] live log per boot (`../logs/boots/current.log`); the 474 MB log gzipped in `../logs/archive/`.

## Backlog items done

- B43 [DONE 2026-10-30] camera start 11 s -> 0.4 s and AE from the real exposure (2e4f7cc, camera.md).
- B41/D7 [DONE 2026-10-30] screenshots: flame-shot defaults fixed (0x9c000000, 1080x2340, stride 4352), display.md.
- B40 [DONE 2026-10-30] Camera.app pre-checks: all of iOSPlus10's camera fixes are already in our stack (camera.md, research/notes/camera-precheck.md); optional: always send SENSOR (camera_client.c:194).
- B39 [DONE 2026-10-30: iOS haptics work, haptics.md] Haptics (new from the 2026-10-30 survey, hakios-xnu 9ae5bde/06817c6, MIT): iOS drives haptics as waveforms on the "Actuator" audio device only with an ADT /product/haptics node (closed-loop 1, supports-3rd-party-haptics 1); without it FigVibratorInitialize -16490. Our motor = TI DRV2624 on I2C (research/notes/hackios-survey.md:133). ~1-1.5 d. (research/notes/upstream-2026-10-30-ports.md)
- B26 [DONE 2026-10-30] Tap survey: Notes keyboard, Control Center, cover sheet, Settings panes (tap2-tap4).
- B34 [DONE 2026-10-30] `openapp`: 10/10 rc 0 with the right app in front (b33 run); the tap4 misses were while locked or busy.
- B17 Camera frames: [DONE 2026-10-31] both cameras stream into RAM (rear 60 fps, front 30 fps), camd + camera_device in SunfishAudio.driver; iOS lists both (camera.md).
- B20 Screen rotation: [DONE 2026-10-30] auto-rotate from the accelerometer.
- O8 [DONE 2026-10-30] Camera ISP speed: whole-frame renders in row bands, camisp 84.8 -> 10.6 ms binned, 372 -> 47 ms full (identical output; the live preview was already parallel).
- O4 [DONE 2026-10-29] lskdd respawn cost: dropped.
