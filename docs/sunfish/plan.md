# Next steps: GPU stability and usability (plan of 2026-10-25)

Old plan: `docs/sunfish/plans/2026-10-25-60hz-smoothness.md`. Rules from it still apply (RAM-only boots, stop below 3600 mV, one change per boot, push only to `sunfish`).

## Context
Speed work (plan.md step 2/3) has stalled: culling, sysmem, half precision, DDR votes all gave nothing; ~21 updates/s is the ceiling for now. Meanwhile the phone still dies in ways that block both daily use and future perf work:
- GPU hangs are unrecoverable once recovery fails (`ring me_init TIMEOUT`, `GBIF halt + soft reset + CP init -> -1`) → black screen, reboot needed. Triggers seen: Reduce Transparency (bridge can't build CA's RT shaders), turnip 0105 SMMU fault, one hang at 565 MHz (none at 430).
- GPU pool exhaustion once caused freeze/respring (940/944 MB); leak since shown to be small (~1.5 BOs, 0.2 MB per app switch) but never soaked.
- USB link loss after boot / replug blocks all phone work ("link flaps", no fastboot).
- Battery drains under load (charger not configured on old port); 1800 s auto-reboot cycle.
- Crashing apps (PhotosFileProvider etc.) block app switching.
A stable phone gives reproducible benches (perf later) and lets the user actually use it.

## Steps (each: [ ] until done, with numbers)

0. **Baseline soak + crash catalogue (no code).** [run 1 aborted 2026-10-25: UI stalled at ~4 min (swaps 0.2/s, app opens fail Code=5), SB idle not deadlocked, GPU healthy; suspects CA RT-shader failures in the bridge / crashed daemon. Run 2 (soak1, 20 min, 4 apps, warm boot): no stall, 48/48 opens. Run 3 (soak2, fresh boot, 6 apps, 20 min): no stall; Photos renders black. Stall is rare: make the 1 h soak with auto-capture the default and move on to step 1.] phone-runner: fresh boot kernel42 `sweetpresentwait=1 flamereboot=0`, run `tools/sunfish-soak.sh` 1 h with a scripted mix (swipes, open/close 6 stock apps via `flame-cmd`, Safari scroll). Record: reboots, GPU hang/fault lines, pool MB over time, crash reports, battery mV/mA. Output: a table "failure → count → first log line". This ranks everything below.
1. **GPU hang recovery that works.** investigator reads the reset path in `sweet_gpu.c` / `sweet_a6xx.c` (0152 GBIF-halt retry) against Linux msm `a6xx_gmu`/`adreno_recover`; implementer adds a full recovery (GMU power-collapse + reboot of GMU/CP, re-init ring), then signals DeviceLost fences so backboardd restarts cleanly instead of black screen. Test: reproduce a hang on purpose (Reduce Transparency or a `sweetgpuhang=1` debug injector) → screen back within ~5 s, 5/5.
2. **Stop known hang triggers.** (a) bridge: fail RT pipelines gracefully (return an error object, not an invalid MTLFunction) so CA falls back; (b) decide default GPU clock: 430 vs 565 MHz soak (step 0 mix, 1 h each) — pick the one with zero hangs if perf diff < bench noise (±3/s).
3. **GPU memory under control.** BO census in the soak (TU_DEBUG=bos + kernel pool lines); hard cap/LRU in the per-client pool reuse (0156) and a kernel warning at 80% pool; chase the drift only if pool > 400 MB after 1 h.
4. **Watchdog instead of blind auto-reboot.** Replace the 1800 s `flamereboot` timer with a userspace/kernel watchdog: reboot only if backboardd makes no swap for 30 s or the kernel log stalls. Usable demo boots no longer die every 30 min and true hangs still recover.
5. **USB link robustness.** investigator: why the link flaps after boot / dies on replug (USB gadget re-enumeration in the shim). Fix so the link renegotiates; test: 5 replugs, link back each time.
6. **Usability quick wins.** Disable/stub crash-looping daemons and apps (PhotosFileProvider etc.) like vmd/ReportCrash earlier; charger config (later, own step — PMIC writes are risky, ask user first).

Exit criteria: 2 h soak with app switches, 0 reboots, 0 unrecovered GPU hangs, pool flat, battery not draining on TB4. Then return to perf with the soak as the regression gate.

## Files
- `work/src/xnu`: `iokit/Kernel/arm/SweetFramebuffer.cpp`, `sweet_gpu.c`, `sweet_a6xx.c` → new `xnu/patches/NNNN-sunfish-*.patch`
- `phone/metal2/bridge/patches/` (RT shader fallback), `phone/turnip/patches/`
- `tools/sunfish-soak.sh` (extend with app-switch mix + pool/hang columns)
- docs: `docs/sunfish/plan.md`, `status.md`, `experiments.md`, `workday.md`

## Verification
Each step: phone-runner run of the soak or targeted repro; numbers into experiments.md + plan.md in the same commit. Webcam check after every recovery test. Battery ≥3600 mV before each boot.
