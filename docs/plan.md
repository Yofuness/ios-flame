# Plan (2026-10-29)

Rules: CLAUDE.md ("How to work"). Old plan: `docs/plans/2026-10-28-priorities-broken-list.md`. One line per item;
details and history in the feature Logs (`docs/features/<name>.md`).

## Priority (a couple at a time, easy first, dev speed on top; ▶ = being worked; ~half a day max each)
Only items Claude can move now. Items waiting on the user are in "Waiting on you" below; parked ones in docs/backlog.md.
1. ▶ **Camera** (B17): Camera.app rear preview + saved photos work (cam35, cam36); both cameras, auto-exposure, autofocus and sharp photos work (cam37, cam40, af5-af7); Live Photo off by default (it filled mediaserverd); next: switch time, front colour, preview CPU. (camera.md)
2. **iOS sound path**: later, feed /dev/sunfish-audio to the ADSP (needs the speaker). (audio.md)

## Waiting on you (details in user-tasks.md; Claude moves on meanwhile)
Compass comparison (U21), Camera.app (U12), proximity by hand (U17), battery discharge (U3), headphone plug test (U15), haptics feel (U16),
Files check (U13), touch while plugged in (U14).
Parked: Compass accuracy (rough compass; dm12 installed, recorder on, re-check U21 when convenient), smoothness (O7: ~23 swaps/s, serial CPU+GPU frame; redfin's fixes gave ~+0.5), speaker (B16), auto-brightness (B42), headphone jack iOS side (audio.md jack10b).
Finished items: [completed.md](completed.md) (not auto-loaded).

## Questions for you (Claude took the recommended path meanwhile)
- (none open)

## Your tasks at the phone
In [user-tasks.md](user-tasks.md) (not auto-loaded): 10 open. Claude doesn't wait on them; it moves to the next item.

## Dev speed (on top of the priority list when found; ~1 h max each)
- (none open)

## Backlog
The Broken, Later and Optimize lists are in [backlog.md](backlog.md) (not auto-loaded).

## Verification
Each item: numbers in the feature Log, this plan and the README status table in the same commit, a journal line,
a webcam check for anything visible, battery ≥ 3600 mV before each boot.
