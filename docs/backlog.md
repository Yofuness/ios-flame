# Backlog (2026-10-29)

Not loaded automatically: read it when waiting on a test or build, after finishing a priority item (to pick the
next easy item), and when adding something newly broken. Priority items live in [plan.md](plan.md); details and
history in the feature Logs.

## Broken (cheapest first; [phone] = needs the phone)
- B1 mtl2.log never rotated: fix built, check on the next install. [phone]
- B8 PhotosFileProvider crashes (~2-4 per 2 h, uncaught ObjC exception in PlugInKit); low impact.
- B10 Reduce Transparency slow in app opens: RT on/off benches + one GPU trace. [phone]
- B11 UI watchdog misses a kernel hang with swaps still running.
- B12 Sensors: all behind the ADSP's SSC; wait for audio. Fallback: a sysctl that injects rotation events.
- B13 Battery % while discharging unchecked (U3).
- B14 Charging drain under load = the USB source's budget; optional read-only TYPEC/APSD check (U4).
- B15 Bluetooth: WCN3990 over a QUP UART + QCA firmware + a Broadcom shim for bluetoothd; 1-3 weeks.
- B16 Audio: see priority 1; then APR + CS35L41 for a beep (U5).
- B17 Camera frames (M2): camcc PLL for 24 MHz MCLK, CSIPHY/CSID/IFE, camera SMMU; weeks.
- B19 Respawn loops: see priority 4.
- B20 Screen rotation: see priority 2.
- B23 Any HLOS buffer the DSP touches resets the SoC: see priority 1.
- B18 Cellular: not planned. iCloud: not expected to work.

## Later (needs repeated 1 h+ tests)
- B21 GPU busy-hang recovery + pool growth (no real hang seen since xnu 0207).
- B4 Silent resets in long app soaks (PON reason is sticky, no lead).

## Optimize (last)
- O2 panel rate ~21 updates/s: only a second, preemptible GPU ring is left (big).
- O3 Reduce Transparency speed (with B10).
- O4 lskdd respawn cost (with B19).
- O5 display-off CPU fade when sweetpanelsleep is off.
- O6 failed GPU hang recovery takes 3.9-12.8 s: fail faster.
