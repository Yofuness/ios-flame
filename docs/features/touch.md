# Touch
**State:** ✅ ST FTM5 over SE7 I²C 0x49 (patch 0125, `sweettouch=2`). **Updated:** 2026-10-28.

## How to use
- Boot arg `sweettouch=2`. Remote input: `python3 -I tools/flame-cmd.py swipe|tap`.

## Facts
- ST FTM5 over SE7 I²C 0x49 (patch 0125) (status.md).

## Open
- None open.

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

No entries before 2026-10-25.### 2026-10-30 03:45 touch1 (kernel112 = 111 + xnu 0236, CAMERA=1): idle status line for B36
- 0236 (bsd/dev/sweet_touch.c, the FTM5 polling loop of stfts_run(); no change to touch behaviour): every 30 s without a touch event, "SWEET TOUCH: idle 30 s: irq N (+d), reads N (+d), errors N, last status 0x.. se 0x.., gpio irq level L" (irq = poll passes with the IRQ pin GPIO 9 low; reads = successful FIFO reads; errors = failed reads). `sweettouchlog=0` off.
- touch1 (4439 mV, no reset): idle baseline: reads +277..279 per 30 s (~9/s), irq stays 1 (+0), errors 0, last status 0x00, se 0xc000081, gpio irq level 1. Steady over 6 windows. Bluetooth up, camd idle.
- B36 (touch silent ~4.5 min while plugged in, kernel107 mo1): in a repeat, these lines show whether the chip stopped asserting IRQ, reads started failing, or reads stopped. User repro = U14.


