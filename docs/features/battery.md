# Battery
**State:** 🟡 real reading works; IR-compensated % (0128) shows no jumps at idle, discharge unchecked. **Updated:** 2026-10-28.

## How to use
- `SWEET BATTERY: mV mA ... shown N%` lines in the live log; negative mA means draining. In fastboot: `fastboot getvar battery-voltage`.

## Facts
- status.md: "Battery % — real reading works; was jumpy (voltage-only estimate). Patch 0128: IR-compensated, averaged, 1%/30 s, direction from measured current, no 100% flash. Booted as `k10`; **needs checking**."

## Open
- Discharge behaviour still unchecked (plan step 2); check during the next app soak off the charger.

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

### 2026-10-28 00:00-02:10 overnight run, part 2 (battery numbers from sk68)
- sk68 2 h exit soak (kernel68, /var on ufsroot1, 15 chunks, 120.7 min): 408/408 app opens, 0 resets, 0 GPU hangs, 0 SMMU faults, 0 UIWDT, CRASH only PhotosFileProvider x4 (AegirPoster 0). Pool 192 -> 709 peak -> 631 MB end: "pool flat" not met. Battery 4431 -> 4429 mV, 0 mA, 100% (full on the charger; discharge still untested).

### 2026-10-28 experiments.md lines
- (see [gpu-memory](gpu-memory.md), experiments line for 2026-10-28) 2026-10-28 sk68: kernel68 (/var on ufsroot1), 2 h app soak (15 chunks sk68a-n, 120.7 min, 6 apps): 408/408 opens, 0 resets, 0 hangs, 0 SMMU faults, 0 UIWDT, 0 pool warnings; CRASH only PhotosFileProvider x4 (AegirPoster 0); pool 192 -> 709 peak -> 631 MB end, drifting up (exit criterion "pool flat" NOT met); battery 4431 -> 4429 mV, 0 mA, 100% on screen (on charger, full: discharge still unchecked); swaps 12.8/s, panel 5.7/s mean (idle-diluted); df /private/var 3.66 GB used of 98 GB.

### 2026-10-27 ~19:00-20:07 plan steps 1-4 checks (ck1-ck3, kernel60 defaults) (step 2 battery part)
- Battery 4456 mV in fastboot before each boot.
- Step 2 battery: 17 samples 19:10-19:40: 4431-4434 mV, 0 mA, chg=0, target/shown 100%; on screen 100% at +15 and +30 min. Earlier today: +120-148 mA with chg=1 at 100%. Nothing learned about discharge.

### 2026-10-27 experiments.md lines
- (see [wifi](wifi.md), experiments line for 2026-10-27) 2026-10-27 ck1 (kernel60, defaults, 30 min idle on charger): Wi-Fi not joined (nettest DNS fail, -1009 no route; wifid CURRENT_NETWORK error 6 x331/8 min, SCAN_RESULT answered x341); battery 4431-4434 mV, 0 mA, 100% shown and on screen, no jumps (discharge untested); AegirPoster 0 crashes, 0 CRASH lines total.

Older entries (before 2026-10-25): [archive/features/battery.md](../archive/features/battery.md).
