# Plan (2026-10-29)

Rules: CLAUDE.md ("How to work"). Old plan: `docs/plans/2026-10-28-priorities-broken-list.md`. One line per item;
details and history in the feature Logs (`docs/features/<name>.md`).

## Priority (a couple at a time, easy first, dev speed on top; ▶ = being worked; ~half a day max each)
Source of most items: ios-redfin (MIT, Pixel 5, research/notes/redfin-survey.md) and iOSPlus10 (MIT).
1. **Speaker** (B16): PARKED 2026-10-30 after ~15 runs (all silent; amp, DSP, routing, memory proven); next: prove
   SD0 activity (tight pin sampler / AFE loopback). (audio.md)
2. **Bluetooth follow-ups** (B15): Xbox controller + AirPods pair and connect (auto RFR flow control), 3 Mbaud
   (firmware 3.7 s). Open: btbridge first bring-up sometimes fails at boot; AirPods audio = U11. (bluetooth.md)
3. ▶ **iOS sound path**: SunfishAudio.driver (null sink, from redfin) + VirtualAudio installed, mediaserverd stable (au1).
   Next: AirPods playback test (user, daytime); later feed /dev/sunfish-audio to the ADSP. (audio.md)
4. **Sensors -> auto-rotate** (B12): WORKS (adspboot --sns-rot writes R N; sweetmotion=0x37; the user confirmed).
   Open: proximity/light, iOS-native motion services. (sensors.md)
5. **Battery %**: kernel106 counting mode built and runs (bat1); needs a discharge run (U3, boot with sweetbattery=2) and the SDAM
   offsets from a RAM-log rescue. (battery.md)
6. ▶ **Camera** (B17): plan research/notes/camera-port.md (6 stages); a2 + b DONE (MCLK 24 MHz, IMX363 streams ~62 fps;
   boot sweetse9=2 sweetcampmic=2, `camid --addr 0x1a --stream`). d + e1 DONE (raw photos; streaming 60.1 fps), e2 exposure/gain set (daytime check). Next: e3 phone ISP, f Camera.app. (camera.md)
Later from the survey: haptics (PM6150 vibrator + their SweetHaptics approach), CPU governor (0963), camera (same
IMX363/IMX355 sensors; 3-5 days).

Done 2026-10-30: AUTO-ROTATE (accelerometer -> R; sensors via the ADSP), BLUETOOTH IN iOS (Settings on, devices listed; kernel105 + btbridge, iOSPlus10/redfin code), Bluetooth LE scan from btup (bt3, 26 devices), Bluetooth chip answers (bt1), H/K home+lock commands, B34 closed, reproducibility audit (docs/setup.md, docs/device-changes.md, tools/sunfish-setup-phone.sh), zero-copy flip mode works (kernel103 manual, no fps gain; parked, O2/O7). Done 2026-10-29: rotation incl. landscape touch (`R 3`/`R 1`; auto-rotate waits for sensors B12), Notes keyboard typing, fake taps/swipes for tests (`T`/`W`, methods.md), Location Services on (`L 1`), rotation follows app switches (rot12), rotation via the front scene's settings (Calculator landscape), r/hackios survey (research/notes/hackios-survey.md: SM7150 audio/BT/camera references), kernel95 = 0 crashes per boot (AegirPoster, App Store fixed), Photos opens (assetsd re-enabled), Music opens (kernel95, xnu 0219 sandbox extensions), Notes and Wallet open fine (survey black was session state), Maps opens (B24: listed + 3 bridge selector commits), B1 closed (mtl2.log truncates at 8 MB by design; 0.8 MB now), B19 lskdd/fairplayd.H2/wifip2pd dropped (xpcproxy 633 -> 314 per 10 min), app survey (11 of 18 apps fine; B24-B27 in backlog), D2 SSH earlier (dropbear Interactive priority: boot to SSH 16-18 s, was 24-41), VMware bridge pinned (boot to SSH 24-29 s, 0 lost sessions), SMMU streams for the DSP (U7),
Wi-Fi auto-join accepted (U2), FastRPC listener written. Done 2026-10-28: camera IDs, brightness slider, flashlight
button, SE9 I2C + amps, boot-arg defaults.


## Questions for you (Claude took the recommended path meanwhile)
- (none open)

## Your tasks at the phone (when you're available)
- U1 flashlight button by hand: tap on/off, long-press for a lower level. ~2 min.
- U3 battery % while discharging: phone off the charger 30 min, on kernel106 with sweetbattery=2 (Claude boots it). (B13)
- U4 one boot on the TB4 port + 240 W cable, to read the charger status on both sources (optional). (B14)
- U9 [DONE 2026-10-30: Xbox controller pairs; AirPods crash bluetoothd] Bluetooth pairing: put headphones or a keyboard in pairing mode near the phone, tap it in Settings > Bluetooth. ~3 min.
- U10 auto-rotate: when Claude says it's ready, turn the phone on its side and back (Calculator open). ~1 min.
- U11 AirPods music: with the AirPods connected, play Pirates.mp3 (Filza: /var/mobile/Documents, or Music after import). ~2 min.
- U8 [DONE 2026-10-30: works] home swipe with a finger: from inside any app, swipe up from the bottom edge: does it go home? ~1 min. (B33)
- U5 [DONE 2026-10-30] speaker beep: silent in bypass and boost (audio.md).

## Dev speed (on top of the priority list when found; ~1 h max each)
- D5 [DONE 2026-10-29] fake taps/swipes (`T`/`W` in /var/local/sweet-orient): tests no longer need a finger.
- D2 [DONE 2026-10-29] SSH earlier at boot: dropbear Interactive, SSH up 16-18 s.
- D4 [DONE 2026-10-29] live log per boot (`../logs/boots/current.log`); the 474 MB log gzipped in `../logs/archive/`.

## Backlog
The Broken, Later and Optimize lists are in [backlog.md](backlog.md) (not auto-loaded).

## Verification
Each item: numbers in the feature Log, this plan and the README status table in the same commit, a journal line,
a webcam check for anything visible, battery ≥ 3600 mV before each boot.
