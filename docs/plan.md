# Plan (2026-10-29)

Rules: CLAUDE.md ("How to work"). Old plan: `docs/plans/2026-10-28-priorities-broken-list.md`. One line per item;
details and history in the feature Logs (`docs/features/<name>.md`).

## Priority (a couple at a time, easy first, dev speed on top; ▶ = being worked; ~half a day max each)
Only items Claude can move now. Items waiting on the user are in "Waiting on you" below; parked ones in docs/backlog.md.
1. ▶ **App extensions** (B8): Files "On My iPhone" OPENS (kernel121, xnu 0240 v5: Sandbox op 4 answers the container from the exec's HOME). Next: PhotosFileProvider, survey widgets/share/keyboard extensions. (apps.md)
2. ▶ **GPU smoothness** (the user's ask; ours ~23 swaps/s vs redfin's 55-64): DEADFILL and UI threads on the A76s
   gave ~+0.5; CA blur keys nothing. Next: CA_EMPTY_SHADERS bound via backboardd's job env, then blur-pass cuts
   (research/notes/blur-cost.md). (smoothness.md)
3. **Sensors** (B12): auto-rotate works; open: proximity into iOS, iOS-native motion services. (sensors.md)
4. **iOS sound path**: later, feed /dev/sunfish-audio to the ADSP (needs the speaker). (audio.md)

## Waiting on you (details in user-tasks.md; Claude moves on meanwhile)
Camera.app (U12), AirPods audio (U11), battery discharge (U3), headphone plug test (U15), haptics feel (U16),
Files check (U13), touch while plugged in (U14).
Parked: speaker (B16), auto-brightness (B42), headphone jack iOS side (audio.md jack10b).
Finished items: [completed.md](completed.md) (not auto-loaded).

## Questions for you (Claude took the recommended path meanwhile)
- (none open)

## Your tasks at the phone
In [user-tasks.md](user-tasks.md) (not auto-loaded): 10 open. Claude doesn't wait on them; it moves to the next item.

## Dev speed (on top of the priority list when found; ~1 h max each)
- D7 tools/flame-shot gives garbage frames on kernel112/CAMERA=1 boots (u13); the webcam works meanwhile. Fix: port iOSPlus10's negroni-shot (B41).
- D6 [PARTLY 2026-10-30] boot cmdline over 591 bytes (B38): -15 B, fast failure; a third extra arg still needs work/rtc-offset moved aside; kernel-side fix later.

## Backlog
The Broken, Later and Optimize lists are in [backlog.md](backlog.md) (not auto-loaded).

## Verification
Each item: numbers in the feature Log, this plan and the README status table in the same commit, a journal line,
a webcam check for anything visible, battery ≥ 3600 mV before each boot.
