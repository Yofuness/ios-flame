# Plan (2026-10-30)

Rules: CLAUDE.md ("How to work"). Old plan: `docs/plans/2026-10-28-priorities-broken-list.md`. One line per item;
details and history in the feature Logs (`docs/features/<name>.md`).

## Priority (a couple at a time, easy first, dev speed on top; ▶ = being worked; ~half a day max each)
Only items Claude can move now. Items waiting on the user are in "Waiting on you" below; parked ones in docs/backlog.md.
1. ▶ **60 Hz / GPU speed** (the user's top ask; O7): 50-58 swaps/s in Settings at full canvas (kernel135 sweetswapbig=6, bridge dead fill mode 1, LTO, allocator): CPU ~14 ms, GPU ~13 ms, p50 down to 16.8 ms in the best boot. @2x and 650 MHz (O16) stay spare. Next: ▶ (a) fresh PC profile; cut the bridge's top: redfin 0130-0137, unchanged-draw fast path; (b) surviving fill as a clear (mode 2) only once it's clean; (c) slower mode after a backboardd restart (2nd submit +6 ms); (d) GPU hangs with failed recovery. Stop rule in smoothness.md. (smoothness.md)
2. **Speaker** (B16): WORKS: iOS sound through the bottom speaker (sp26, speaker.on = -20, boot arg sweetampreset=1), capped ~37 dB under stock until the user listens (U21). Left: louder after that, first-sound delay, earpiece, mic. (audio.md)
3. **Garbled images** (B44): lock screen, Control Center, Settings, Home fixed (xnu 0251, kernel132). Compass was a capture artefact. Left: Tips banner black blocks, Photos thumbnails (may be old files). Next: look at those two (apps.md).
4. **Battery + camera CPU** (O14): -9% in 10 min; preview takes ~5 cores (not profiled); needs a trustworthy battery % (U3).
5. **Camera polish** (B48): shutter ignored in the first session after a boot, switch ~7 s + ~10 s, front colour cast, focus on open ~25 s. GPU device lost (gpu.md): read "DEVLOST first" if it returns.

## Waiting on you (details in user-tasks.md; Claude moves on meanwhile)
proximity by hand (U17), battery discharge (U3), headphone plug test (U15), 
Files check (U13), touch while plugged in (U14).
Parked for later (the user, 2026-10-30): Compass accuracy (20-30 deg off, jumps; dm12, recorder on), camera far focus (lens range; backlog Later), smoothness (O7: ~23 swaps/s, serial CPU+GPU frame; redfin's fixes gave ~+0.5), speaker (B16), auto-brightness (B42), headphone jack iOS side (audio.md jack10b).
Finished items: [completed.md](completed.md) (not auto-loaded).

## Questions for you (Claude took the recommended path meanwhile)
- Did this phone's speakers work under Android before the port? Assumed yes; if not, the speaker hunt may be a hardware fault.

## Your tasks at the phone
In [user-tasks.md](user-tasks.md) (not auto-loaded): 10 open. Claude doesn't wait on them; it moves to the next item.

## Dev speed (on top of the priority list when found; ~1 h max each)
- (none open)

## Backlog
The Broken, Later and Optimize lists are in [backlog.md](backlog.md) (not auto-loaded).

## Verification
Each item: numbers in the feature Log, this plan and the README status table in the same commit, a journal line,
a webcam check for anything visible, battery ≥ 3600 mV before each boot.
