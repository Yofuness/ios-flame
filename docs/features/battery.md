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

### 2026-10-30 11:20 bat1: kernel106 coulomb counting runs; unproven on the charger (xnu 0227, sweetbattery=2)
- 0227 (from ios-redfin 0966, MIT): sweetbattery=2 = count charge from QG LAST_ADC_I every 5 s, seed from QG SDAM 0xb646/0xb647 (inferred offsets) or the voltage estimate, rest pull to the voltage estimate; capacity 3080 mAh (DT) in both modes (was 5020). =1 = old voltage mode.
- bat1 (on USB, full, 4454 mV): "pct 100 (count 3080 mAh, vEst 100%, I 0 mA, mode counting)" at 82/142/202 s; status bar 100. The first-poll lines (SDAM b640 dump, QG subtype, seeded) are not in the live log: they ran before it starts (~30 s); the RAM-log rescue would have them. No reset; Bluetooth up.
- Open: SDAM offsets unconfirmed; a discharge run (off the charger, U3) is the real test. Default stays sweetbattery=1 until then.

### 2026-10-28 14:30 PON baseline: the same after a clean reboot (lead withdrawn)
- After a deliberate `flame-cmd.py reboot` (PSCI SYSTEM_RESET), k78b reads the identical line: `08:03 ... 11:01 13:01 | reason hard_reset|smpl| warm 0 poff/fault 0/0`. PON_REASON1 is sticky from the original cold power-on (warm resets don't rewrite it; WARM_RESET_REASON 0x0a stays 0 even after PSCI reset, so maybe a different offset on this GEN2 block), so the "smpl" after k77s says nothing about the silent reset. Next idea: `fastboot getvar all` right after a silent reset vs after a clean reboot (ABL's own reboot reason), and the GEN2 PON offsets from a real source.

### 2026-10-28 14:20 k78: PON reason after the k77s silent reset: hard_reset|smpl
- `sctl kern.sweet_pon` (xnu 0197/0201): `type 01/04 rev 01 01 11 00 | 08:03 09:00 0a:00 0b:00 0c:00 0d:00 0e:00 0f:00 10:00 11:01 12:00 13:01 | reason hard_reset|smpl| warm 0 poff/fault 0/0`. PON_REASON1 = 0x03: bit0 hard reset, bit1 SMPL (sudden momentary power loss: the PMIC saw VBAT/VPH dip and restarted). If confirmed (baseline after a clean reboot pending), the silent resets are power brownouts, which ties them to the never-configured charger (the battery carries every load spike).
- The "(repeat)" line from 0201 is not in usb-live.log (it uses IOLog; the console capture seems to miss it); the sysctl works. First battery line at 370.7 s.

### 2026-10-28 15:00 k76: the xnu 0197 PON line never reached the PC
- The code is in kernel74/76 (strings show it) and sweetbattery=1 is set, but SweetBattery::start prints it before the USB live log is up (this boot's log starts at ~41 s; the first SWEET BATTERY line at 324 s). Fix: repeat the PON line with the first periodic battery print (or expose a sysctl).

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
