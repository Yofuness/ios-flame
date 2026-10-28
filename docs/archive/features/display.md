# Display: archived log (entries before 2026-10-25)
Current file: [features/display.md](../../features/display.md). Same format, newest first.

## Log

### 2026-10-24 11:50 Present-mode comparison with the timeline (kernel39 / kernel40 = + xnu 0164 sweetpresentslots)
- Same Settings swipe bench, two runs per config, sweettrace=1:
| config | swaps/s | panel updates/s | ticks/s | present call p95 | put off |
|---|---|---|---|---|---|
| presentwait=1 (default) | 20.3 | 21.7-22.3 | 52 | 43 ms | 0 |
| presentwait=0 (nb1, nb2) | 24.5 / 24.4 | 14.4 / 13.7 | 59 | 1.4 ms | 514 / 523 |
| presentwait=0 slots=4 (s4a, s4b) | 22.4 / 23.0 | 15.0 / 14.8 | 59 | 1.4 ms | 336 / 407 |
- Non-blocking fixes the tick (59/s, present never blocks) and lifts swaps, but half the presents are put off because the blit waits behind heavy backboardd jobs (queue wait p95 114 ms); 4 slots recover only ~1 update/s. Default left at presentwait=1, slots=2.
- Next lever (plan step 2): the ~29 ms from backboardd's last GPU end to SwapEnd and the 40 ms GPU frames; neither is the present path.

### 2026-10-24 10:30 Step 1B (second ringbuffer): not feasible, no code
- sweet_a6xx.c sets up RB0 only and has no preemption support (no preempt records, CP_CONTEXT_SWITCH_*, pseudo-regs). A second ring would never be fetched without a software context switch; real preemption is a multi-day bring-up. Parked; alternative ideas (panel-size surface, CPU scaler, blit timing) are in plan.md step 1.

### 2026-10-24 09:50 Step 1A moot, step 2 (turnip 0105) failed
- See [metal](metal.md) for this entry; it also touches this feature.

### 2026-10-24 08:40 CA vsync: backboardd was never on IOMFB vsync
- See [smoothness](smoothness.md) for this entry; it also touches this feature.

### 2026-10-24 00:40 Night run: where backboardd's GPU time goes
- See [smoothness](smoothness.md) for this entry; it also touches this feature.

### 2026-10-24 experiments.md lines
| date | patch / switch | hypothesis | predicted ms saved | measured fps and GPU ms | verdict | commit |
|---|---|---|---|---|---|---|
| 2026-10-24 | `sweetfb_thread=1` present thread | inline present delays vsync | ~10 | ~17 then ~14.7 fps | dead end (off) | - |
| 2026-10-24 | xnu 0161 present on swap (`sweetfb_onswap=1`) | present off the vsync tick | ~10 | ~14 fps | dead end (off) | cad3009 |
| 2026-10-24 | `sweetpresentwait=0` | blocking present costs swaps | - | 24.5 swaps/s but 13.7-14.4 fps | dead end | 53068a9 |
| 2026-10-24 | xnu 0164 `sweetpresentslots=4` | more presents in flight | - | 14.8-15.0 fps, still 336-407 put off | dead end | a122fae |

### 2026-10-23 23:05 Smoothness measurements + memory work (stopped at 16% battery)
- See [smoothness](smoothness.md) for this entry; it also touches this feature.

### 2026-10-23 experiments.md lines
| date | patch / switch | hypothesis | predicted ms saved | measured fps and GPU ms | verdict | commit |
|---|---|---|---|---|---|---|
| 2026-10-23 | xnu 0158 non-blocking present | present wait blocks vsync | ~10 | 12.5 fps (vs 17.4); put off 701/600 frames | dead end (off) | c149da1 |
