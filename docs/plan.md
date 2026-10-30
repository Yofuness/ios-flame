# Plan (2026-10-30)

Rules: CLAUDE.md ("How to work"). Old plan: `docs/plans/2026-10-28-priorities-broken-list.md`. One line per item;
details and history in the feature Logs (`docs/features/<name>.md`).

## Priority (a couple at a time, easy first, dev speed on top; ▶ = being worked; ~half a day max each)
Only items Claude can move now. Items waiting on the user are in "Waiting on you" below; parked ones in docs/backlog.md.
1. ▶ **60 Hz / GPU speed** (the user's top ask, 2026-10-30; O7): the path can do 60 (camera preview: 300 swaps per 5 s); swipes get ~21: CA CPU 10-15 ms + GPU 25-30 ms (blur up to 66), serial, one frame in flight. Ladder, stop when good: (a) per-screen fps table + no-blur bench (Reduce Transparency, B10), 1-2 h; (b) lower render resolution, upscale in the present blit, 2-4 h; (c) blur passes cheaper (half-res/skip backdrop in the bridge), 0.5-1 day; (d) two frames in flight (QuartzCore RE), days; (e) redfin's list O9/O11. Stop rule in smoothness.md. (smoothness.md)
2. **Speaker** (the user's ask; B16): amps, DSP, AFE run, silent; first prove samples reach the amp (SD0 pin sampler / AFE loopback; research/notes/speaker-next.md), check the Pixel 5 port's progress; then feed /dev/sunfish-audio to the ADSP. Time-boxed retries; hard item. Quiet hours apply. (audio.md)
3. **Garbled images** (B44/B29): probably not a speed issue (geometry bug on the shadow path); next: offset % texel for shadowed RGBA8/BGRA8, one screen per run; measure shadow cost per frame while at (a).
4. **Battery + camera CPU** (O14): -9% in 10 min; preview takes ~5 cores (not profiled); needs a trustworthy battery % (U3).
5. **Camera polish** (B48): shutter ignored in the first session after a boot, switch ~7 s + ~10 s, front colour cast, focus on open ~25 s. GPU device lost (gpu.md): read "DEVLOST first" if it returns.

## Waiting on you (details in user-tasks.md; Claude moves on meanwhile)
proximity by hand (U17), battery discharge (U3), headphone plug test (U15), 
Files check (U13), touch while plugged in (U14).
Parked for later (the user, 2026-10-30): Compass accuracy (20-30 deg off, jumps; dm12, recorder on), camera far focus (lens range; backlog Later), smoothness (O7: ~23 swaps/s, serial CPU+GPU frame; redfin's fixes gave ~+0.5), speaker (B16), auto-brightness (B42), headphone jack iOS side (audio.md jack10b).
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
