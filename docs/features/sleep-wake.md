# Sleep and wake
**State:** 🟡 power-button sleep/wake stable (11/11); the panel is not switched off. **Updated:** 2026-10-23.

## How to use
- `flame-cmd.py key 0 150` presses power remotely (xnu 0137).

## Facts
- Every press → `SWEET HID: key 0x30` → `SWEET FB: display off/on`; "display off" only stops presenting frames.

## Open
- Does the OLED actually go dark on "display off"? Real panel-off needs a DSI DCS command (see brightness).

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

### 2026-10-28 14:50 k81 (xnu 0205): panel sleep 10/10
- 10 cycles (`flame-cmd.py 'key 0'`, 3 s off / 3 s on): every cycle req off -> `blank alpha (saved 1a0000)` -> dcs 28, 10 (done 1, ack 0, t 0-1 ms) -> req on -> dcs 11, 29 -> `unblank alpha 1a0000`. Off frames black (mean 6-7), wake frames the picture at the same 10% brightness (23-25 vs 23 before). One dcs 29 line missing in cycle 1 (screen fine: a lost log line). 0 failed lines. Next: make sweetpanelsleep=1 a default boot arg after a longer test.

### 2026-10-28 15:00 why sleep-in lagged: the CPU fade is slow, not a stalled tick
- vsyncFired runs the fade inline (SweetFramebuffer.cpp:429): each step is a volatile read-modify-write of all ~2.5M words of the 1080x2340 surface through an uncached kernel mapping, ~0.35-0.7 s per step, 2.4-3.9 s for all 7. No tick (and no vsync notification to backboardd) fires meanwhile: the 5 s stats line slips 2.24-3.86 s per off; DCS 28 came 2.38-2.75 s after display off (k78, k79). "fade 5 at req on" = the fade was still running when the 3 s off ended.
- Fix (next, 0205): black the screen with the MDP LM_0 blend alpha (as tools/sunfish-dim.sh does: 0x0ae45000 +0x24/+0x3c/+0x54/+0x6c, CTL_0 FLUSH 0x0ae02018 bit 6) instead of the CPU fade; restore the saved alpha on wake.

### 2026-10-28 14:40 k79 (xnu 0203): wakes 10/10, sleep-in 4/10 (the fade stalls at step 5)
- 10 cycles (`flame-cmd.py 'key 0'`, 3 s off / 3 s on): every off frame black, every wake frame the picture, no failed lines, 0 GPU hangs. Log: 12 req off (all asleep 0, fade 0), 12 req on: 5 at (1,7) with 11/29, 7 at (0,5) with no DCS. So in 6 cycles the tick's fade stopped at step 5 (picture at 1/32: black on the webcam) and the sleep-in condition (fade > 6) never fired. One wake logged 11 but no 29.
- Fix in progress: the panel thread finishes the fade itself 150 ms after `req off`.
- `fastboot getvar all` after a clean reboot saved as logs/getvar-clean.txt (only slot-retry-count matched reboot/reason/pon).

### 2026-10-28 14:15 k78: kernel panel sleep (xnu 0202, sweetpanelsleep=1), 10 power-button cycles over `flame-cmd.py 'key 0'`
- Off frames black 10/10; wake frames picture 8/10 (cycles 7 and 8 stayed black, both came back on their own later). Only 3 DCS sequences ran for 10 cycles (28/10/11/29 x3, all done 1 ack 0, t 0-1 ms, 0 failed lines); 9 display off / 11 display on lines. So the panel thread misses requests (the fade-step condition or the wake-up), and black wakes match pairs without DCS lines. A GPU hang (unrecoverable) came 1.0 s after one `display off` (gpu.md).

### 2026-10-28 15:00 k76: panel off/on from the shell works (tools/sunfish-panel-sleep.sh)
- 3 cycles: dcs 28, 10, 11, 29 all `done yes ack_err 0x00000000`; autorefresh restored to 0x80000001; webcam black at +3 s, picture back by +5-8 s, 3/3; SSH up after. Headers 0x800500NN (short write, LONG bit clear). Next: the kernel hook on IOMFB sel 12.

### 2026-10-28 12:30 panel-off design (read-only)
- Today: sleep = IOMFB sel 12 RequestPowerChange(0/1) (SweetFramebuffer.cpp:926-937). fPowerOff makes the vsync tick fade the frame to black over 6 ticks and skip the present (:386-409); the tick and the type-5 notifications keep running; no DCS is sent, so the panel stays on in normal mode.
- SOFEF01 sequences (recon/live.dts:12228-12296): off = 05 28 (20 ms), 05 10, then a Samsung F0 5A5A / B0 05 / F4 01 / F0 A5A5 block (120 ms). On = 05 11 (10 ms), 15 35 00 (TE on), F0-unlocked EB/B7/F2 setup, 2A 0000 0437, 2B 0000 0923, 15 53 20, B0 09 / E8 ..., 05 29. lp1/nolp use 53 22 / 53 20 (AOD): avoid.
- Design (~120 lines, SweetFramebuffer.cpp, boot arg sweetpanelsleep=1): kernel `sweet_dsi_dcs_short(cmd)` (packet 0x80050000|cmd via the 0x9d6ff000 buffer, DMA_BASE +0x48, DMA_LEN +0x4c, TRIG_DMA +0x90, poll INTR_CTRL +0x110, ACK_ERR +0x68); on off: after the fade, save + clear autorefresh 0x0ae6bab4, 28, 20 ms, 10, 120 ms; on wake: 11, 120 ms, 29, restore autorefresh, redraw. Off the user-client thread (IOSleep). On any DMA failure restore autorefresh.
- First test without a kernel: 28/10 then 11/29 from the shell tool (a short-write variant of sunfish-panel-bright.sh) with the webcam.

Older entries (before 2026-10-25): [archive/features/sleep-wake.md](../archive/features/sleep-wake.md).
