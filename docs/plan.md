# Plan (2026-10-29)

Rules: CLAUDE.md ("How to work"). Old plan: `docs/plans/2026-10-28-priorities-broken-list.md`. One line per item;
details and history in the feature Logs (`docs/features/<name>.md`).

## Priority (a couple at a time, easy first, dev speed on top; ▶ = being worked; ~half a day max each)
Source of most items: ios-redfin (MIT, Pixel 5, research/notes/redfin-survey.md) and iOSPlus10 (MIT).
0. ▶ **GPU smoothness from ios-redfin** (the user's ask; research/notes/redfin-survey-2026-10-30.md): theirs 55-64 fps / 5.4 ms GPU, ours ~21 fps / 25-30 ms. DEADFILL ported, no gain here (df1: 22.2 -> 22.7 swaps/s, few fills to drop); our cost is blur passes + an A55 render thread. xnu 0238 (UI threads on the A76s, redfin 1041) works but +0.5 swaps/s (ub2). Next: the blur/backdrop passes.
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
   Back + Front (current boot: kernel116 HAPTICS=1 CAMERA=1 sweetse9=2 sweetcampmic=2), preview textures + colour tuning installed. Next: Camera.app (U12). (camera.md)
7. ▶ **Files "On My iPhone"** (B37): LISTED now (fpfix.dylib in fileproviderd via a sh exec wrapper: FP library on our HFSX /var); opening it shows "Content Unavailable": next the enumeration error. (apps.md)
8. **Headphone jack detect** (B30, the user's ask): codec answers over SoundWire (jack5); MBHC setup clean (jack6); next S3 = real plug test (U15). iOS side PARKED: VirtualAudio only creates "Null_Device"/"Actuator" (jack10b; patch option in audio.md). (audio.md)
Parked 2026-10-30: auto-brightness (B42; lux path works, no way for iOS's ramp to reach the panel yet).
Finished items: [completed.md](completed.md) (not auto-loaded).
Later from the survey: CPU governor: not needed for speed (clocks already max, cpu.md).

## Questions for you (Claude took the recommended path meanwhile)
- (none open)

## Your tasks at the phone (when you're available)
- U1 flashlight button by hand: tap on/off, long-press for a lower level. ~2 min.
- U3 battery % while discharging: phone off the charger 30 min, on kernel106 with sweetbattery=2 (Claude boots it). (B13)
- U4 one boot on the TB4 port + 240 W cable, to read the charger status on both sources (optional). (B14)
- U10 auto-rotate: when Claude says it's ready, turn the phone on its side and back (Calculator open). ~1 min.
- U12 Camera app (when you're up): ready now (kernel111, camera on, camd idle, preview textures installed): open Camera (Utilities folder), say what you see (preview? colours?), switch to the front camera, take one photo each, check Photos. Keep Live Photo off. ~3 min. If backboardd misbehaves: Claude runs `tools/sunfish-metal2-install.sh --undo`.
- U13 Files app: open Files > Browse: is "On My iPhone" listed now? (B37 fix, 2026-10-30.) ~1 min.
- U14 Touch while plugged in (B36): on the charger, leave the phone idle 3 min, then tap a few times: does it react? Also: in yesterday's Calculator freeze, were you touching the screen? ~4 min.
- U15 Headphone jack: READY (jack6 passed): Claude starts a 60-s detect run; plug wired headphones in at ~15 s, out at ~35 s. ~2 min.
- U16 Haptics: toggle a switch in Settings (Sounds & Haptics): do you feel a tap? Is it too strong or weak? ~1 min (after Claude starts hapticd).
- U11 AirPods music: with the AirPods connected, play Pirates.mp3 (Filza: /var/mobile/Documents, or Music after import). ~2 min.

## Dev speed (on top of the priority list when found; ~1 h max each)
- D7 tools/flame-shot gives garbage frames on kernel112/CAMERA=1 boots (u13); the webcam works meanwhile. Fix: port iOSPlus10's negroni-shot (B41).
- D6 [PARTLY 2026-10-30] boot cmdline over 591 bytes (B38): -15 B, fast failure; a third extra arg still needs work/rtc-offset moved aside; kernel-side fix later.

## Backlog
The Broken, Later and Optimize lists are in [backlog.md](backlog.md) (not auto-loaded).

## Verification
Each item: numbers in the feature Log, this plan and the README status table in the same commit, a journal line,
a webcam check for anything visible, battery ≥ 3600 mV before each boot.
