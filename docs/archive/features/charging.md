# Charging: archived log (entries before 2026-10-25)
Current file: [features/charging.md](../../features/charging.md). Same format, newest first.

## Log

### 2026-10-24 10:45 Charger: Thunderbolt 4 port fixes the drain
- Moved the cable to the laptop's free TB4 port (240 W USB4 cable) while the phone was running: screen stayed on (32%, charging bolt) but the USB link died (RX errors 55604, 0 packets). Forced fastboot, rebooted as k38c with the cable left in place.
- Same charger registers (status 3, ICL 0x1e = 1.5 A, FCC 2.0 A). Battery: +968 mA idle at 3986 mV, +948 mA during the 60 s bench (tb4b: 24.1 swaps/s, 23.0 panel updates/s; tb4: 23.4 / 22.1). Before: about 0 to -250 mA. The source was the limit.

### 2026-10-24 10:25 Step 1B skipped, step 4 charger dump (kernel38 = kernel37 + 0162, read-only)
- CI first skipped 0162 ("already exists in working directory"): `git add -A` in work/src/xnu had committed the whole untracked tree into the patch. Re-committed with only SweetBattery.cpp. In work/src/xnu always `git add` single files.
- Dump (docs/sunfish/charger-regs.md): status 3 (fast charge), charging enabled, FCC 2.0 A, USBIN limit 1.5 A, battery -70 mA at 3762 mV with screen on. TYPEC/MISC blocks not readable. Drain is the source's power budget, not a charger setup bug. Details in plan.md step 4.
