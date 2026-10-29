# Plan (2026-10-29)

Rules: CLAUDE.md ("How to work"). Old plan: `docs/plans/2026-10-28-priorities-broken-list.md`. One line per item;
details and history in the feature Logs (`docs/features/<name>.md`).

## Priority (a couple at a time, easy first, dev speed on top; ▶ = being worked; ~half a day max each)
Source of most items: ios-redfin (MIT, Pixel 5, research/notes/redfin-survey.md) and iOSPlus10 (MIT).
1. ▶ **Speaker** (B16): redfin's amp recipe: clear AMP_OUT_MUTE after GLOBAL_EN, SP_RATE_CTRL/FS_MON clocks, then
   internal boost (stock DT values) if still silent. One sp run each; laptop mic + U5. (audio.md)
2. **Bluetooth follow-ups** (B15): pairing test (needs U9), UART drops (3 Mbaud / interrupt RX), faster boot
   firmware load; A2DP later (redfin 1016). (bluetooth.md)
3. **iOS sound path**: redfin's kernel APR stream + RedwoodAudio.driver HAL plug-in, once the speaker plays. (audio.md)
4. ▶ **Sensors -> auto-rotate** (B12): all 6 sensors answer; iOS rotates natively from the orientation HID service
   (kernel107, faked events). Next: per-sensor QRTR ports so real samples flow, then a tilt test (U10). (sensors.md)
5. **Battery %**: kernel106 counting mode built and runs (bat1); needs a discharge run (U3, boot with sweetbattery=2) and the SDAM
   offsets from a RAM-log rescue. (battery.md)
Later from the survey: haptics (PM6150 vibrator + their SweetHaptics approach), CPU governor (0963), camera (same
IMX363/IMX355 sensors; 3-5 days).

Done 2026-10-30: BLUETOOTH IN iOS (Settings on, devices listed; kernel105 + btbridge, iOSPlus10/redfin code), Bluetooth LE scan from btup (bt3, 26 devices), Bluetooth chip answers (bt1), H/K home+lock commands, B34 closed, reproducibility audit (docs/setup.md, docs/device-changes.md, tools/sunfish-setup-phone.sh), zero-copy flip mode works (kernel103 manual, no fps gain; parked, O2/O7). Done 2026-10-29: rotation incl. landscape touch (`R 3`/`R 1`; auto-rotate waits for sensors B12), Notes keyboard typing, fake taps/swipes for tests (`T`/`W`, methods.md), Location Services on (`L 1`), rotation follows app switches (rot12), rotation via the front scene's settings (Calculator landscape), r/hackios survey (research/notes/hackios-survey.md: SM7150 audio/BT/camera references), kernel95 = 0 crashes per boot (AegirPoster, App Store fixed), Photos opens (assetsd re-enabled), Music opens (kernel95, xnu 0219 sandbox extensions), Notes and Wallet open fine (survey black was session state), Maps opens (B24: listed + 3 bridge selector commits), B1 closed (mtl2.log truncates at 8 MB by design; 0.8 MB now), B19 lskdd/fairplayd.H2/wifip2pd dropped (xpcproxy 633 -> 314 per 10 min), app survey (11 of 18 apps fine; B24-B27 in backlog), D2 SSH earlier (dropbear Interactive priority: boot to SSH 16-18 s, was 24-41), VMware bridge pinned (boot to SSH 24-29 s, 0 lost sessions), SMMU streams for the DSP (U7),
Wi-Fi auto-join accepted (U2), FastRPC listener written. Done 2026-10-28: camera IDs, brightness slider, flashlight
button, SE9 I2C + amps, boot-arg defaults.


## Questions for you (Claude took the recommended path meanwhile)
- (none open)

## Your tasks at the phone (when you're available)
- U1 flashlight button by hand: tap on/off, long-press for a lower level. ~2 min.
- U3 battery % while discharging: phone off the charger 30 min, on kernel106 with sweetbattery=2 (Claude boots it). (B13)
- U4 one boot on the TB4 port + 240 W cable, to read the charger status on both sources (optional). (B14)
- U9 Bluetooth pairing: put headphones or a keyboard in pairing mode near the phone, tap it in Settings > Bluetooth. ~3 min.
- U10 auto-rotate: when Claude says it's ready, turn the phone on its side and back (Calculator open). ~1 min.
- U8 home swipe with a finger: from inside any app, swipe up from the bottom edge: does it go home? ~1 min. (B33)
- U5 speaker beep: listen next to the phone for a 3 s 1 kHz tone (amp powers up cleanly; the PC mic hears nothing). Ready now.

## Dev speed (on top of the priority list when found; ~1 h max each)
- D5 [DONE 2026-10-29] fake taps/swipes (`T`/`W` in /var/local/sweet-orient): tests no longer need a finger.
- D2 [DONE 2026-10-29] SSH earlier at boot: dropbear Interactive, SSH up 16-18 s.
- D4 [DONE 2026-10-29] live log per boot (`../logs/boots/current.log`); the 474 MB log gzipped in `../logs/archive/`.

## Backlog
The Broken, Later and Optimize lists are in [backlog.md](backlog.md) (not auto-loaded).

## Verification
Each item: numbers in the feature Log, this plan and the README status table in the same commit, a journal line,
a webcam check for anything visible, battery ≥ 3600 mV before each boot.
