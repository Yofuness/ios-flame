# Plan (2026-10-29)

Rules: CLAUDE.md ("How to work"). Old plan: `docs/plans/2026-10-28-priorities-broken-list.md`. One line per item;
details and history in the feature Logs (`docs/features/<name>.md`).

## Priority (a couple at a time, easy first, dev speed on top; ▶ = being worked; ~half a day max each)
Source of most items: ios-redfin (MIT, Pixel 5, research/notes/redfin-survey.md) and iOSPlus10 (MIT).
0. ▶ **GPU smoothness from ios-redfin** (the user's ask; research/notes/redfin-survey-2026-10-30.md): theirs 55-64 fps / 5.4 ms GPU, ours ~21 fps / 25-30 ms. DEADFILL ported, no gain here (df1: 22.2 -> 22.7 swaps/s, few fills to drop); our cost is blur passes + an A55 render thread. Next: xnu 1041 UI threads on the A76s (porting), then the blur passes.
1. **Speaker** (B16): PARKED 2026-10-30 after ~15 runs (all silent; amp, DSP, routing, memory proven); next: prove
   SD0 activity (tight pin sampler / AFE loopback). (audio.md)
2. **Bluetooth follow-ups** (B15): Xbox controller + AirPods pair and connect (auto RFR flow control), 3 Mbaud
   (firmware 3.7 s). Boot bring-up fixed (bt12: 4/4). Open: AirPods audio = U11. (bluetooth.md)
3. ▶ **iOS sound path**: SunfishAudio.driver (null sink, from redfin) + VirtualAudio installed, mediaserverd stable (au1).
   Next: AirPods playback test (user, daytime); later feed /dev/sunfish-audio to the ADSP. (audio.md)
4. **Sensors -> auto-rotate** (B12): WORKS (adspboot --sns-rot writes R N; sweetmotion=0x37; the user confirmed).
   Open: proximity/light, iOS-native motion services. (sensors.md)
5. **Battery %**: kernel106 counting mode built and runs (bat1); needs a discharge run (U3, boot with sweetbattery=2) and the SDAM
   offsets from a RAM-log rescue. (battery.md)
6. ▶ **Camera** (B17): both cameras stream (rear 60 fps, front 30 fps), camd + camera_device in SunfishAudio.driver, iOS lists
   Back + Front (kernel112 CAMERA=1 = current boot), preview textures + colour tuning installed. Next: Camera.app (U12). (camera.md)
7. **Files "On My iPhone"** (B37): LocalStorage linked to its data container (lsreg -r), pkd/fileproviderd accept it; check U13. (apps.md)
8. **Headphone jack detect** (B30, the user's ask): codec answers over SoundWire (jack5); MBHC setup clean (jack6); next S3 = real plug test (U15). iOS side PARKED: VirtualAudio only creates "Null_Device"/"Actuator" (jack10b; patch option in audio.md). (audio.md)
9. ▶ **Haptics** (B39): iOS HAPTICS WORK (hp3: Settings switch taps -> hapticd -> motor, 4/4). Next: launchd job + install script (MG cache step), HAPTICS=1 in the boot recipe; the user feels it (U16). (haptics.md)
Parked 2026-10-30: auto-brightness (B42; lux path works, no way for iOS's ramp to reach the panel yet).
Later from the survey: haptics (B39: Actuator audio device + ADT /product/haptics + DRV2624 on I2C; hakios-xnu, MIT). CPU governor: not needed for speed (clocks already max, cpu.md).

Done 2026-10-30: VIBRATION MOTOR BUZZES (hp2), camera opens fixed (2ed9d22), daily upstream check. Done 2026-10-30 (night): BOTH CAMERAS STREAM + iOS LISTS THEM (cam1-cam33), Bluetooth boot bring-up fixed (bt12), oslog survives restarts (B31), B36 = touch silent while plugged in (kernel112 status line). Done 2026-10-30: AUTO-ROTATE (accelerometer -> R; sensors via the ADSP), BLUETOOTH IN iOS (Settings on, devices listed; kernel105 + btbridge, iOSPlus10/redfin code), Bluetooth LE scan from btup (bt3, 26 devices), Bluetooth chip answers (bt1), H/K home+lock commands, B34 closed, reproducibility audit (docs/setup.md, docs/device-changes.md, tools/sunfish-setup-phone.sh), zero-copy flip mode works (kernel103 manual, no fps gain; parked, O2/O7). Done 2026-10-29: rotation incl. landscape touch (`R 3`/`R 1`; auto-rotate waits for sensors B12), Notes keyboard typing, fake taps/swipes for tests (`T`/`W`, methods.md), Location Services on (`L 1`), rotation follows app switches (rot12), rotation via the front scene's settings (Calculator landscape), r/hackios survey (research/notes/hackios-survey.md: SM7150 audio/BT/camera references), kernel95 = 0 crashes per boot (AegirPoster, App Store fixed), Photos opens (assetsd re-enabled), Music opens (kernel95, xnu 0219 sandbox extensions), Notes and Wallet open fine (survey black was session state), Maps opens (B24: listed + 3 bridge selector commits), B1 closed (mtl2.log truncates at 8 MB by design; 0.8 MB now), B19 lskdd/fairplayd.H2/wifip2pd dropped (xpcproxy 633 -> 314 per 10 min), app survey (11 of 18 apps fine; B24-B27 in backlog), D2 SSH earlier (dropbear Interactive priority: boot to SSH 16-18 s, was 24-41), VMware bridge pinned (boot to SSH 24-29 s, 0 lost sessions), SMMU streams for the DSP (U7),
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
- U12 Camera app (when you're up): ready now (kernel111, camera on, camd idle, preview textures installed): open Camera (Utilities folder), say what you see (preview? colours?), switch to the front camera, take one photo each, check Photos. Keep Live Photo off. ~3 min. If backboardd misbehaves: Claude runs `tools/sunfish-metal2-install.sh --undo`.
- U13 Files app: open Files > Browse: is "On My iPhone" listed now? (B37 fix, 2026-10-30.) ~1 min.
- U14 Touch while plugged in (B36): on the charger, leave the phone idle 3 min, then tap a few times: does it react? Also: in yesterday's Calculator freeze, were you touching the screen? ~4 min.
- U15 Headphone jack: READY (jack6 passed): Claude starts a 60-s detect run; plug wired headphones in at ~15 s, out at ~35 s. ~2 min.
- U16 Haptics: toggle a switch in Settings (Sounds & Haptics): do you feel a tap? Is it too strong or weak? ~1 min (after Claude starts hapticd).
- U11 AirPods music: with the AirPods connected, play Pirates.mp3 (Filza: /var/mobile/Documents, or Music after import). ~2 min.
- U8 [DONE 2026-10-30: works] home swipe with a finger: from inside any app, swipe up from the bottom edge: does it go home? ~1 min. (B33)
- U5 [DONE 2026-10-30] speaker beep: silent in bypass and boost (audio.md).

## Dev speed (on top of the priority list when found; ~1 h max each)
- D7 tools/flame-shot gives garbage frames on kernel112/CAMERA=1 boots (u13); the webcam works meanwhile. Fix: port iOSPlus10's negroni-shot (B41).
- D6 [PARTLY 2026-10-30] boot cmdline over 591 bytes (B38): -15 B, fast failure; a third extra arg still needs work/rtc-offset moved aside; kernel-side fix later.
- D5 [DONE 2026-10-29] fake taps/swipes (`T`/`W` in /var/local/sweet-orient): tests no longer need a finger.
- D2 [DONE 2026-10-29] SSH earlier at boot: dropbear Interactive, SSH up 16-18 s.
- D4 [DONE 2026-10-29] live log per boot (`../logs/boots/current.log`); the 474 MB log gzipped in `../logs/archive/`.

## Backlog
The Broken, Later and Optimize lists are in [backlog.md](backlog.md) (not auto-loaded).

## Verification
Each item: numbers in the feature Log, this plan and the README status table in the same commit, a journal line,
a webcam check for anything visible, battery ≥ 3600 mV before each boot.
