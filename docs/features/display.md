# Display
**State:** ✅ 1080x2340, ABL splash buffer 0x9c000000; GPU-composited (Metal → Turnip), vsync-paced. **Updated:** 2026-10-24.

## How to use
- Best present settings: kernel37+ with `sweetpresentwait=1`.

## Facts
- 1080x2340, ABL splash buffer 0x9c000000; GPU-composited (Metal → Turnip), vsync-paced ~21 updates/s (status.md).
- Panel Samsung SOFEF01, DSI command mode (not video), refreshed by MDP INTF1 autorefresh on TE (0x0ae6bab4 = 0x80000001) (plan step 7).
- Reading 0x0ae44000 hangs the bus. The MDP registers are at the catalog offset + 0x1000 (display-perf.md).
- Frame path, present path and the IOMFB/vsync findings: see [smoothness](smoothness.md) "Facts".
- Present switches (all off by default, all worse): `sweetfb_thread=1`, `sweetfb_onswap=1` (0161), 0158 non-blocking present, `sweetpresentwait=0`, `sweetpresentslots=4` (0164).
- sweet_a6xx.c sets up RB0 only and has no preemption support, so a second ringbuffer for the present is not feasible without a software context switch (2026-10-24).
- Sleep: "display off" only stops presenting frames (see sleep-wake).

## Open
- The present blit waits behind backboardd on the single ring (see smoothness).
- Scanout without the blit (risky: MDP registers).

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

Older entries (before 2026-10-25): [archive/features/display.md](../archive/features/display.md).
