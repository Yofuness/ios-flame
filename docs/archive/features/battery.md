# Battery: archived log (entries before 2026-10-25)
Current file: [features/battery.md](../../features/battery.md). Same format, newest first.

## Log

### 2026-10-23 06:40 Step 0 preflight — pass (with a battery warning)
- **Battery patch 0128 (k10) works:** shown % tracks the estimate and steps 1% at a time in the
  direction of the measured current: 35% → 34% → 32% at −169…−289 mA. No 100% flash since boot.
- **Battery warning:** the phone *drains* on this USB port while iOS runs (~−200 mA, ≈1% per 5 min).
  At 32% (06:39) it reaches the 10% gate around 08:30–09:00. Mitigation: keep the phone in fastboot
  between test boots (iOS not running) and keep boots short.
- **`flamereboot=0`:** read `iokit/Kernel/arm/SweetPlatformExpert.cpp`: `reboot_s <= 0` pets the
  watchdog forever → no timed return to fastboot (watchdog stays armed, so a real hang still resets).
  Not used for test boots.
- **Baseline:** idle `SWEET FB: 5s: 50 swaps (10.0 fps), avg copy ~2.7 ms`; boot animation ~24 fps.
  Screenshot logs/shots/baseline-0640.png.
- **Soak:** `port/tools/sunfish-soak.sh` → logs/soak.log every 60 s.
- (Recovered from `git show 1808cf9^:docs/sunfish/workday.md`; that commit deleted it.)
