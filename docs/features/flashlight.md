# Flashlight
**State:** 🟡 LED works from a tool (kernel69, `sweettorch=1`); no Control Center button yet. **Updated:** 2026-10-28.

## How to use
- Boot with `sweettorch=1` (kernel69+, xnu 0190+0195), then `/var/local/bin/sctl kern.sweet_torch=50` (mA, cap 100, 5 s auto-off).

## Facts
- PM6150L flash block SID 5 base 0xd300 (recon/live.dts:5840-6012, qpnp-flash-led-v2, 2 LEDs, torch max 500 mA, 12.5 mA steps). TYPE 0x18 SUBTYPE 0x4.
- Registers, checked against upstream leds-qcom-flash.c (mvflash_3ch, type 0x18 not 0x20): TIMER 0x40+ch (bit7 en; 0 = off for torch), ITARGET 0x43+ch [6:0], MODULE_EN 0x46 bit7, IRESOLUTION 0x47 (2 bits/ch, 3 = 5 mA torch steps), CHAN_STROBE 0x49+ch (0x01 = SW, level, active high; NOT 0x80), CHAN_EN 0x4c bit ch.
- Torch LED1 50 mA: 0x47 bits1:0=3, 0x43=9, 0x40=0, 0x46=0x80, 0x49=0x01, 0x4c=0x01; off: 0x4c=0, 0x49=0, 0x46=0.
- Kernel write path: sweet_spmi_write via chnls 0x0c600000 + 0x80*apid, owner check, whitelist SID 5 periph 0xd3 offsets {40,43,46,47,49,4c}. A late SYSCTL_PROC is a permanent node (k68t panic); 0195 uses SYSCTL_OID_MANUAL.
- Charger registers stay off limits.

## Open
- Control Center hookup (iOS drives the torch through the camera stack); maybe drop the 5 s auto-off for a real flashlight.

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

### 2026-10-28 00:00-02:10 overnight run, part 2 (torch part)
- Torch: k68t (kernel68 + sweettorch=1) panicked at 36.2 s: "Use sysctl_register_oid_early to register permanent nodes" (RAM-log rescue). xnu 0195 (kernel69, CI 40640759295) uses SYSCTL_OID_MANUAL. k69t boots; new tool phone/tools/sctl (CI 40641144876). sctl kern.sweet_torch=50 / =100: all writes land, readback ok, 5 s auto-off; the webcam sees only the front, the user saw the LED turn on and off (confirmed by eye).

### 2026-10-28 experiments.md lines
- 2026-10-28 k68t: kernel68 + sweettorch=1: panic at 36.2 s "Use sysctl_register_oid_early to register permanent nodes" (RAM-log rescue, 104850 B); the late SYSCTL_PROC registration of kern.sweet_torch is a permanent node. The register read before it worked: TYPE 0x18 SUBTYPE 0x4, 0x40-0x4c = 93 93 93 00 00 00 00 00 00 01 01 01 00. Fix: xnu 0195 SYSCTL_OID_MANUAL (kernel69).
- 2026-10-28 k69t (kernel69 = 0195, sweettorch=1): boots fine; new phone tool sctl (no sysctl binary on the phone). `sctl kern.sweet_torch=50` and `=100`: all six writes land (readback 0x46=0x80 0x4c=0x1), 5 s auto-off works, SSH up; webcam (front view) shows no change, so whether the rear LED lit is UNCONFIRMED (needs a look at the back of the phone). Battery current useless for this (on charger, 0 mA). Lock screen 1:58 Wed Oct 28 = PC EDT: timezone fix confirmed.
- 2026-10-28 k69t torch: the user saw the rear LED turn on and off during the 50/100 mA runs. Flashlight WORKS (LED only).

### 2026-10-28 broken.md "Fixed" row
| Area | Fixed | How |
|---|---|---|
| Flashlight (LED only) | 2026-10-28 | xnu 0190+0195, sweettorch=1, `sctl kern.sweet_torch=50`; user saw it light; no Control Center button yet |

No entries before 2026-10-25.
