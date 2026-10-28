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

### 2026-10-28 16:55 k88a (kernel88, sweetbl=2): black screen at boot; fix 0214 (kernel90)
- backlight-control matching now finds AppleARMBacklight. At 32.927 s CoreBrightness set brightness-nits = 0 and DisplayBrightnessSliderPosition = 0; the kernel sent DCS 51 = 4 (panel minimum, near-black) at 32.958 s and nothing raised it. Recovered with tools/sunfish-panel-bright.sh 128.
- xnu 0214 (kernel90): nits 0 is not sent to the panel; DCS = 32 + nits * 479 / 500 (32..511). Test k90a pending.

### 2026-10-28 16:20 CoreBrightness read (research/re/CoreBrightness.s)
- Find: `-[CBDisplayContaineriOS findBacklight]` matches only {IOPropertyMatch: {backlight-control: true}}; built-in only if IOObjectConformsTo(svc, "AppleARMBacklight") (else handleDisplayArrival treats it as external, looks up kUSBContainerID). DisplayOpen also opens IOMobileFramebuffer and reads MG "OLEDDisplay".
- Set: `_SetBLDriverNitsValue` -> "brightness-nits" = CFNumber SInt32 nits*65536 via IORegistryEntrySetCFProperty (= setProperties on our side), then `_SetIOMFBBrightness` -> "IOMFBBrightnessLevel" (SInt32 16.16) on the IOMFB service; also "brightness-nits-cap", "BLNitsCap", "AmbientBrightness", "TrustedLux". No user-client selector. Writes are skipped in low-power mode.
- Next (xnu 0209): rename the service class to AppleARMBacklight; sweetbl=2 maps nits (cap 500) to DCS 0x51 4..511 from the kernel panel thread; AppleCLCD accepts the IOMFB brightness keys.

### 2026-10-28 12:40 Control Center slider: design (read-only)
- Nothing reaches the kernel today: backboardd and SpringBoard look for an IOService with `backlight-control` = true and find none (live log, e.g. `[ 270.802] IOK SpringBoard: matching1 ... backlight-control ... -> none`, also at 28.2 / 35.5 / 98.7 / 991-997 s). SweetFramebuffer.cpp:874-945 answers sels 3, 4, 5, 7, 8, 12, 18, 28, 72 only; no setProperties override.
- Shared-cache strings (sub-cache .13, likely CoreBrightness): AppleARMBacklight, backlight-control, brightness-nits, brightness-nits-cap, IODisplayParameters, IOMFBBrightnessLevel, rawBrightness. Likely (~60%): CoreBrightness sets `brightness-nits` / IODisplayParameters via IORegistryEntrySetCFProperty -> setProperties(). Not disassembled yet.
- The DCS 0x51 send exists only in tools/sunfish-panel-bright.sh (pmem); no kernel code maps 0x0ae94000 / 0x0ae6bab4.
- Design (~200 lines, new SweetBacklight.cpp): publish an AppleARMBacklight-like service with backlight-control and IODisplayParameters; setProperties() logs every key and, with `sweetbl=1`, maps nits 0-500 to DCS 4..511 (only opcode 0x51, never 0x53, never 0x0ae44000), at most one send per 50 ms, on a work thread under the fb lock.
- Test: boot A log-only (the matching line must find the service; record the keys); boot B with sweetbl=1 at 10/30/50%, webcam + battery mA + one swipe bench.

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
