# Next steps: 60 Hz smoothness and stability (plan of 2026-10-24)

Old plan: `docs/sunfish/plans/2026-10-24-present-path.md`. Full reasoning: `~/.claude/plans/pasted-content-id-a57c-plan-distributed-newt.md`.

## Measured facts that drive this plan
- backboardd's own GPU time is 14-20 ms/frame under swipes (not 2.4 ms; that is the blit), GPU 25-37% busy, longest submit ~40 ms. The timeline shows it is bimodal (p50 5.8 ms, p95 40 ms per submit) and that the biggest gap is on the CPU/CA side: GPU end -> swap p50 29 ms; the vsync tick itself delivers only ~52/s. Both rule out 60 Hz.
- `sweet_gpu_present` is async; the 7-13 ms "avg copy" is `sweetpresentwait=1` waiting behind backboardd's jobs. Cache clean is 0.6-0.8 ms.
- vsync notify already precedes the present; blit-on-swap (0161) was worse; a second ring needs preemption (not present).
- Charging is fine on the laptop's TB4 port (+950 mA under load). The USB link does not survive a replug.

## Rules from the old plan still apply
Boots RAM-only via `tools/sunfish-boot.sh`; stop below 3600 mV; no MDP register access beyond `tools/sunfish-dim.sh`; push only to `sunfish`; one change per boot; log in `docs/workday.md`.

## Steps
0. **Video spike (no code):** serve H.264/HEVC mp4 and a long page from the PC over USB; play in Safari; does it decode, at what fps, hardware or CPU? Result decides whether reels are possible. [ ]
1. **Per-frame timeline (xnu 0163, `sweettrace=1`) [DONE 2026-10-24, kernel39]:** kernel records at swap, submit, CP start/end, present begin/end, vsync; `tools/sunfish-timeline.py LOG "bench TAG start" "bench TAG end"` prints the frame-interval histogram (the 60 Hz metric) and the per-stage gaps. Tracing costs nothing visible (22.3 / 21.7 updates/s with it on). mtl2.log/Turnip trace merge not done yet.
   First numbers (Settings swipe bench, 2 runs agree; mach time and the CP counter run at 19.2 MHz, not 24): 20.2-20.3 swaps/s, vsync ticks only ~52/s (not 60: late ticks come in bursts). swap-to-swap p50 33 ms, p95 100 ms; ~22% of intervals <=17 ms, ~64% <=34 ms, ~21% >50 ms. backboardd GPU per submit is bimodal: p50 5.7-5.9 ms, p95 40 ms, max 41-44 ms. The present blit is 2.4 ms of GPU. Gaps: backboardd GPU end -> swap p50 29 ms / p95 95 ms (CPU/CA side, not GPU); swap -> present call p50 10.7 ms; present call blocks p50 0.7 ms but p95 43 ms (the `sweetpresentwait=1` wait behind heavy jobs); present submit -> blit start p95 114 ms.
   **Present-mode comparison on the timeline (kernel39/40, same bench, 2 runs each):**

   | config | swaps/s | panel updates/s | vsync ticks/s | present call p95 | put off |
   |---|---|---|---|---|---|
   | `sweetpresentwait=1` (default) | 20.3 | 21.7-22.3 | 52 | 43 ms | 0 |
   | `sweetpresentwait=0` | 24.5 | 13.7-14.4 | 59 | 1.4 ms | ~520 per 37 s |
   | `sweetpresentwait=0 sweetpresentslots=4` (xnu 0164) | 22.4-23.0 | 14.8-15.0 | 59 | 1.4 ms | 336-407 |
   Non-blocking keeps vsync on time and CA runs faster, but the blit queues behind backboardd's heavy jobs (present submit -> blit start p95 114 ms), so slots stay full and frames are skipped; more slots help only a little. Panel updates are capped by swaps (20-24/s) either way, and swaps are limited by the CA/CPU side (GPU end -> swap p50 29 ms) plus the bimodal 40 ms GPU frames. Default stays `sweetpresentwait=1`, `sweetpresentslots=2`.

2. **Cut backboardd GPU time (16 ms -> <=10 ms):** profile under swipes; 40 ms outliers; sysmem vs gmem under load; cheaper batched shadow copies; panel-size render only if fill-bound. [in progress]
   gt1 trace (2026-10-24): 166/1569 passes take 20-40 ms, all full-screen; draw_ib_gmem >95% even with 21 draws -> a few expensive draws per bin (fill/shader). Plus ~10 ms of copy_buffer_to_image batches in the same submits. Per-bin split: slow passes cost ~0.7-1.3 ms in nearly every bin (fast ~0.08), so full-screen expensive draws. turnip 0105 per-draw trace (pd2): slow passes are stacked opaque full-screen quads (fs f8357eea x3 per pass, 3.5 ms each with tracing) plus fs 780adfdf (2x cost per bin). Drawlog (dl1): draws 0-2 are three identical opaque solid fills under an opaque full-cover wallpaper quad (draw 3), so they are dead. SWEET_CULL (bridge, per clip rectangle; CA redraws damage regions) culls them correctly but gains nothing measurable: 13.2-13.4 vs 13.1-15.3 ms/submit, the pattern is ~1 pass in 25, longest submit still ~40 ms; the per-draw trace overstated the fills. [done, no gain]
   Still open: what makes the ~40 ms submits (~1 ms in every bin even with 21 draws; ~14 ms per slow pass outside the draws). Next: sysmem vs GMEM for the big pass under swipes; the cost of CA's uber-shader (fixed_frag_lph_cph, ~0.8 Gpx/s for a solid fill); the 10 ms of copy_buffer_to_image in the same submits.
3. **Pipeline ordering from the timeline:** non-blocking present, present on GPU idle, tearing fix. Only after step 2. [ ]
4. **Stability soak (1 h):** Safari scrolling + app switches with BO census; chase create/destroy only if the pool passes ~400 MB. [ ]
5. **Unscoped:** shader/pipeline cache persistence, CPU profile of SpringBoard/Safari. [ ]

## Two sessions, one phone
A: steps 1-3 (xnu, SweetFramebuffer.cpp, sweet_gpu.c, sweet_a6xx.c, tools/sunfish-timeline.py). B: steps 0, 4, 5 and Turnip/bridge fixes (`phone/**`). Take turns on the phone; each appends only its own dated section to `docs/workday.md`.
