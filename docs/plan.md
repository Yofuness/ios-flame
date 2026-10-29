# Plan (2026-10-29)

Rules: CLAUDE.md ("How to work"). Old plan: `docs/plans/2026-10-28-priorities-broken-list.md`. One line per item;
details and history in the feature Logs (`docs/features/<name>.md`).

## Priority (a couple at a time, easy first, dev speed on top; ▶ = being worked; ~half a day max each)
1. **Audio via the SM7150 reference** (B16/B23): DSP addresses carry the SMMU stream (IOVA | 3<<32), pd-mapper adsp
   domains, root PD attach first (xorbidden/hakios-sm7150 facts; no hyp assign needed). Done: FastRPC, ADSP stable, APR, AFE, PCM stream, CS35L41
   powers up (PLL lock, PUP_DONE, OTP trims). Unheard by the PC mic: waits for U5 (the user listens) or a loopback capture. (audio.md)
2. ▶ **Screen rotation** (B20): `R 3` / `R 1` rotates the front app (Calculator landscape, scientific layout; rot10).
   Next: Safari draws only the left ~45% in landscape; touch mapping (`R N t`); sensors later. (display.md)
3. **Zero-copy scanout** (O2; the user liked ios-s9's smoothness): an MDP pipe scans backboardd's surface directly
   instead of our GPU blit (ios-s9 xnu 0096/0097, MIT); first a read-only MDP layout dump. (smoothness.md)

Done 2026-10-29: rotation via the front scene's settings (Calculator landscape), r/hackios survey (research/notes/hackios-survey.md: SM7150 audio/BT/camera references), kernel95 = 0 crashes per boot (AegirPoster, App Store fixed), Photos opens (assetsd re-enabled), Music opens (kernel95, xnu 0219 sandbox extensions), Notes and Wallet open fine (survey black was session state), Maps opens (B24: listed + 3 bridge selector commits), B1 closed (mtl2.log truncates at 8 MB by design; 0.8 MB now), B19 lskdd/fairplayd.H2/wifip2pd dropped (xpcproxy 633 -> 314 per 10 min), app survey (11 of 18 apps fine; B24-B27 in backlog), D2 SSH earlier (dropbear Interactive priority: boot to SSH 16-18 s, was 24-41), VMware bridge pinned (boot to SSH 24-29 s, 0 lost sessions), SMMU streams for the DSP (U7),
Wi-Fi auto-join accepted (U2), FastRPC listener written. Done 2026-10-28: camera IDs, brightness slider, flashlight
button, SE9 I2C + amps, boot-arg defaults.

## Questions for you (Claude took the recommended path meanwhile)
- (none open)

## Your tasks at the phone (when you're available)
- U1 flashlight button by hand: tap on/off, long-press for a lower level. ~2 min.
- U3 battery % while discharging: phone off the charger 30 min. (B13)
- U4 one boot on the TB4 port + 240 W cable, to read the charger status on both sources (optional). (B14)
- U5 speaker beep: listen next to the phone for a 3 s 1 kHz tone (amp powers up cleanly; the PC mic hears nothing). Ready now.

## Dev speed (on top of the priority list when found; ~1 h max each)
- D2 [DONE 2026-10-29] SSH earlier at boot: dropbear Interactive, SSH up 16-18 s.
- D4 [DONE 2026-10-29] live log per boot (`../logs/boots/current.log`); the 474 MB log gzipped in `../logs/archive/`.

## Backlog
The Broken, Later and Optimize lists are in [backlog.md](backlog.md) (not auto-loaded).

## Verification
Each item: numbers in the feature Log, this plan and the README status table in the same commit, a journal line,
a webcam check for anything visible, battery ≥ 3600 mV before each boot.
