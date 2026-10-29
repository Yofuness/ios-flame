# Plan (2026-10-29)

Rules: CLAUDE.md ("How to work"). Old plan: `docs/plans/2026-10-28-priorities-broken-list.md`. One line per item;
details and history in the feature Logs (`docs/features/<name>.md`).

## Priority (a couple at a time, easy first; ▶ = being worked)
1. ▶ **Audio: share memory with the DSP via the hypervisor** (B16/B23): the DSP resets the SoC on any HLOS buffer
   (rpc2, rpc3). Read-only research of the SCM mem-assign call first, then baby steps: a no-op assign on one page,
   then a shared page the CPU can still use, then the FastRPC attach. `adspboot --rpc` needs `--unsafe`. (audio.md)
2. **Screen rotation** (B20): SpringBoard and Safari rotate (method E), the screen stays portrait; next: the
   compositing side (backboardd display transform / our framebuffer). (display.md)
3. **B1 check:** the mtl2.log truncation on the next Metal bundle install (easy).
4. **B19 respawn loops:** lskdd every ~1 s, bluetoothd / fairplayd.H2 / wifip2pd every ~10 s: exit reasons via
   `oslog` / `launchctl print`, then drop what nothing needs (easy-ish).

Done 2026-10-29: VMware bridge pinned (boot to SSH 24-29 s, 0 lost sessions), SMMU streams for the DSP (U7),
Wi-Fi auto-join accepted (U2), FastRPC listener written. Done 2026-10-28: camera IDs, brightness slider, flashlight
button, SE9 I2C + amps, boot-arg defaults.

## Questions for you (Claude took the recommended path meanwhile)
- (none open)

## Your tasks at the phone (when you're available)
- U1 flashlight button by hand: tap on/off, long-press for a lower level. ~2 min.
- U3 battery % while discharging: phone off the charger 30 min. (B13)
- U4 one boot on the TB4 port + 240 W cable, to read the charger status on both sources (optional). (B14)
- U5 speaker beep: listen for a 2 s tone once audio works.

## Dev speed (priority over other optimizations; ~1 h max each)
- D2 boot time: dropbear only starts ~33 s into boot (start it earlier: a root write); boot to SSH is ~25 s now.
- D4 [DONE 2026-10-29] live log per boot (`../logs/boots/current.log`); the 474 MB log gzipped in `../logs/archive/`.

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

## Verification
Each item: numbers in the feature Log, this plan and the README status table in the same commit, a journal line,
a webcam check for anything visible, battery ≥ 3600 mV before each boot.
