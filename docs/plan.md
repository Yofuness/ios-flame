# Plan (2026-10-30)

Rules: CLAUDE.md ("How to work"). Old plan: `docs/plans/2026-10-28-priorities-broken-list.md`. One line per item;
details and history in the feature Logs (`docs/features/<name>.md`).

## Priority (a couple at a time, easy first, dev speed on top; ▶ = being worked; ~half a day max each)
Only items Claude can move now. Items waiting on the user are in "Waiting on you" below; parked ones in docs/backlog.md.
1. ▶ **60 Hz / GPU speed** (the user's top ask, 2026-10-30; O7): the path can do 60 (camera preview: 300 swaps per 5 s); with the render thread bound to an A76 (xnu 0250, kernel131, `sweetswapbig=6`): full canvas 34 swaps/s and even (p95 31 ms; GPU 26.5 ms per frame caps it), @2x 51 (off: lock screen breaks). Done: bridge LTO cut CPU per swap 24 -> 18 ms; Reduce Transparency no gain. Everyday boot now: kernel131 sweetswapbig=6 (32 wakes clean, 1 unexplained hang earlier). 650 MHz +11 %, needs a soak (O16). Next: ▶ (a) GPU cost at full canvas: copies, passes, Turnip knobs; (b) @2x lock screen, and ask the user about softer text; (c) more bridge cuts: redfin 0130-0137, unchanged-draw fast path; (d) two frames in flight, days. Stop rule in smoothness.md. (smoothness.md)
2. **Speaker** (the user's ask; B16): data lines were swapped: with `--swap-lanes` the tone reaches both amps and echoes back (sp20, silent). Next: ▶ audible test at -20 dB after 05:00; then swap by default, feed /dev/sunfish-audio to the ADSP. Time-boxed retries; hard item. Quiet hours apply. (audio.md)
3. **Garbled images** (B44/B29): glyphs sheared ~16 texels per row, offset lead dead (apps.md); size-matched dump found no fill tonight; next: dump all glyph-size textures (M2V_SHADOW_DUMP=all), reshape on the PC to place the fault.
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
