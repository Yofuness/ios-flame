# Brightness
**State:** 🟡 panel brightness via DCS 0x51 works from a tool; no Control Center slider yet. **Updated:** 2026-10-28.

## How to use
- Panel: `tools/sunfish-panel-bright.sh VALUE` (4-511): DCS 51 HH LL via the DSI0 command DMA. Resets on reboot.
- Blend fallback: `tools/sunfish-dim.sh N` sets the LM_0 blend alpha. It resets on reboot; leave it at 100 unless asked.
- Night rule (plan, overnight run): `tools/sunfish-dim.sh 10` right after every boot when the phone is in the bedroom; brightness tests never above 50%, back to 10% after.

## Facts
- Panel Samsung SOFEF01 (OLED, `bl_ctrl_dcs`). Brightness = DCS long write 51 HH LL, 10 bit, 0x04-0x3ff (recon/live.dts:12228-12358); on-sequence already has 53 20 (never write other 0x53 values: HBM/LP).
- DSI0 at 0x0ae94000 is a 6G v2.3 host (B+0x0 = 0x20030000 = VERSION): Linux register offsets are shifted by +4 (CTRL at B+0x4 = 0x1f5: enable, cmd mode, 4 lanes).
- Recipe (Linux offsets +4): pause autorefresh, write packet C0390003 / 00LLHH51 into DMA-able RAM (splash tail 0x9d6ff000), DMA_BASE, DMA_LEN=8, TRIG_DMA=1, poll CMD_DMA_DONE in INTR_CTRL, restore autorefresh. TRIG_CTRL was already SW (4).
- Webcam: at 3/5/8/12% blend the webcam sees nothing; icons show from 16%; 10% is the night setting (Iriun exposure locked for 10%).

## Open
- Control Center slider hookup (plan "Later").

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

### 2026-10-28 00:00-02:10 overnight run, part 2 (brightness part)
- Panel brightness (pb1, kernel68): tools/sunfish-panel-bright.sh sends DCS 51 HH LL through the DSI0 command DMA (packet at 0x9d6ff000, MDP autorefresh paused around it). TRIG_CTRL was already SW (4). 128/32/400/128: all done, ack_err 0, no faults; webcam: 32 near-black, 400 brighter. Works; resets on reboot.

### 2026-10-28 experiments.md lines
- 2026-10-28 pb1: kernel68, tools/sunfish-panel-bright.sh (DCS 0x51 via DSI0 command DMA, packet at 0x9d6ff000, autorefresh paused): TRIG_CTRL 0x4 (SW), values 128/32/400/128 all `done yes ack_err 0`, SSH up, 0 smmu/fault/panic/DSI lines; webcam: 32 near-black, 128 darker than boot default, 400 brighter. Panel brightness WORKS (on top of dim 10).

### 2026-10-28 broken.md "Fixed" row
| Area | Fixed | How |
|---|---|---|
| Panel brightness | 2026-10-28 | tools/sunfish-panel-bright.sh VALUE (DCS 0x51); not yet the Control Center slider |

### 2026-10-26 ~22:00 Control: kernel54 (xnu 0178) sweetsmmureset=0, stall hangs, 565 MHz
- See [gpu](gpu.md) for this entry; it also touches this feature.

Older entries (before 2026-10-25): [archive/features/brightness.md](../archive/features/brightness.md).
