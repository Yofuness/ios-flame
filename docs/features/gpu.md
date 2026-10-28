# GPU
**State:** ✅ Adreno 618 at 565 MHz; full recovery from injected and real hangs (~26 ms). **Updated:** 2026-10-28.

## How to use
- Default clock 565 MHz (`sweetgpufreq=5`); 430 MHz = `sweetgpufreq=4`; stock levels 650/800/825 = `sweetgpufreq=6..8` (xnu 0148).
- Recovery: `sweetgpurecover=2` (full), `sweetgpukill=1` (SIGKILL backboardd after recovery), both on by default in sunfish-boot.sh. `sweetsmmureset=0` turns off the SMMU/GBIF reset (0178).
- Hang injector `sweethangtest=N` (0167): bad IB on the Nth client submit; bit 31 = Nth present submit; bit 30 = stall IB (0177); 0xA0000200 = SMMU stall-on-fault present hang at submit 512 (0180).
- App soak: `tools/sunfish-appsoak.sh TAG SECONDS`.

## Facts
- Adreno 618, chip id 0x06010800; register map, firmware and GMU/HFI differences vs flame's A640 are in [reference/adreno618-notes.md](../reference/adreno618-notes.md).
- Metal goes through SweetMetal2 (metal2vulkan + Turnip): see [metal](metal.md).
- xnu 0174: Linux-style Fabia PLL0 restore after the DCVS vote (GX collapse wipes GPU_CC_PLL0; GMU DCVS zeroes PLL0 L).
- xnu 0181: early CB_RESUME at the start of recovery; 0182: SMMU stall-on-fault (CFCFG) by default.
- Real-hang recovery proven: bc1 (kernel70, 2026-10-28), 2 real hangs recovered in 26.5/26.4 ms.
- 565 vs 430 MHz: 430 is not safer (sk430 had 1 hang in 1 h); 565 kept.

## Open
- The Reduce Transparency hang cause is unproven (did not reproduce on kernel52 + nbproto bridge).
- Silent resets during the 2026-10-25 soaks (565 MHz at 468 s, 430 MHz at ~179 s) were never explained; none since 0175/0176.

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

### 2026-10-28 11:49-13:51 sk71: 2 h soak, no reset, but an unrecoverable SMMU-stalled GPU hang at 256.8 s
- kernel70, bundle 7053013, booted as sk70r (no census). 122 min, 414 app steps (sk71a 60 + sk71b 62 min), no open failures, 0 panics / UIWDT / reboots; PhotosFileProvider 1 crash (254.97 s).
- 256.8 s: fence 2361 done 2345, smmu fsr 0; rbbm status3 bit24 (SMMU stalled), cb fsr raw 400 -> recover2 failed, GBIF-halt path: soft reset + CP init -> -1 twice, "GPU not recovered", backboardd killed once, "recover2: FAILED after 3994512 us". No SWEET GPU lines after 261 s.
- Yet the UI kept working for 2 h (webcam frames 11:49-1:51 show changing apps; SWEET FB 5s lines to 7475 s, ~30 swaps/5 s): presumably CPU compositing after the GPU died (not checked).
- The same signature as bl1 (kernel70, 227 s, also unrecovered). The recovered real hangs (bc1, bl2, ab-off) had no SMMU stall. Pool data stops at 261 s (160 / peak 416 / 24 MB): the "pool flat" check could not be made.
- Verdict for plan priority 1: reboots PASS, unrecovered hangs FAIL, pool flat not measurable.

### 2026-10-28 12:00 silent resets: analysis (read-only)
- Nothing records a reset reason: the shim writes 0x77665500 to IMEM 0x146aa65c at every boot (shim/shim.c:27, :80), so every reset lands in fastboot and the IMEM word says nothing.
- Ruled out for sk70: the kernel's own reboot paths (flamereboot, boot watchdog 0189, UI watchdog rung 2) all log a FLAME WDT / SWEET BOOTWDT line first, and none is in sk70-rescue.txt. The UFS rst3 reset was a real panic, fixed in 0175.
- What fits: a hang that stops every printer, including the 5 s APSS watchdog pet thread (SweetPlatformExpert.cpp:49-104; the shim arms a 20-30 s bite), or an instant TZ/XPU/NoC or PMIC reset. Ranked: GPU/SMMU bus stall ~45% (kernel70 had 3 real GPU hangs in ~45 min, one with the SMMU stalled and not recovered; sk68 on kernel68 had 0 in 2 h); XPU/TZ from Wi-Fi ~20% (exec wifip2pd 45 ms before the end); non-GPU hard hang ~15%; PMIC UVLO ~10%.
- Next: log the PMIC PON/POFF/fault reason registers (SID 0 periph 0x08) at boot (read-only SPMI), so the boot after the next reset says what it was; then a kernel68-vs-kernel70 soak with the same bundle.

### 2026-10-28 11:40-11:55 sk70: silent reset to fastboot at 337.8 s (2 h soak aborted)
- kernel70 `sweetbocensus=600`, bundle 7053013 (bridge 0105), 4455 mV. Fresh-boot bench sk70p: 22.7 / 22.1 swaps/s, 16.1 / 15.6 panel updates/s (below the 21-24 of earlier kernels/bundles; not yet explained).
- App soak cycle 2: the phone fell into fastboot at uptime ~337.8 s. RAM-log rescue (sk70r boot; header total 495807, n 258048, logs/sk70-rescue.txt) ends mid-swipe at `[ 337.776] SWEET CMD: swipe 540,1800 -> 540,1000 in 30 steps`: no panic, watchdog, GPU hang, SError, jetsam or thermal line. GPU pool 160 MB (backboardd), GPU busy 13%. Same pattern as the unexplained 10-25 soak resets (468 s, ~179 s; storage suspect: the UFS I/O error path), the first since 0175/0176. sk68 (2 h, kernel68) had none.
- Retrying the 2 h soak on the sk70r boot.

### 2026-10-28 08:30 bl2: real GPU hang recovered
- kernel70, uptime 615.9 s: fence 11512 done 11420, smmu fsr 0; recovered in 26326 us; backboardd pid 58 killed, pid 740 came up and the soak went on. (bl1's failed recovery had the SMMU stalled; this one did not.)

### 2026-10-28 07:49 bl1: real GPU hang, recovery failed (new signature)
- kernel70 + turnip 0106, 2nd app-soak cycle, uptime 227.4 s: fence 2892 done 2858, smmu fsr 0 far 0; rbbm status c30d35, status3 ce00000 (bit24 smmu stalled), cb fsr raw 400, cp_status1 8113471e. recover2 went through cp stop / gbif halt / sw reset / gx off / gx on, then "GPU not recovered" at 231.4 s; the kernel killed backboardd anyway. Swaps came back (99 in the 5 s to 246 s), then the screen idled. Unlike bc1's hangs (rbbm 800005, no SMMU stall), this one has the SMMU stalled. 3 real hangs in ~45 min of kernel70 soaks vs 0 in sk52b-sk68: kernel70 (0196) or turnip 0106 still a suspect.

### 2026-10-28 02:05-03:15 overnight run, part 3: the GPU pool leak
- See [gpu-memory](gpu-memory.md): bc1 had 2 real GPU hangs (rbbm 800005, ring pending, no SMMU fault), both recovered on the first path in 26.5/26.4 ms. Real-hang recovery is now proven.

### 2026-10-28 02:05-03:15 overnight run, part 3: the GPU pool leak
- See [gpu-memory](gpu-memory.md) for this entry; it also touches this feature.

### 2026-10-27 ~07:55-09:00 kernel58 (xnu 0182: SMMU stall-on-fault by default)
- 0182: GPU context bank SCTLR gets CFCFG (0x80) at setup and again after the recovery's SMMU/GBIF reset; a6_clear_fault terminates a stalled fault (CB_RESUME) before clearing FSR. Off with `sweetsmmustall=0`. CI run 40499063303 -> work/kernel58.
- es1 (565 MHz, `sweethangtest=0xA0000200`): injected at 130.3 s; "early smmu resume: fsr 40000402"; full path, cp init + zap -> 0; **recover2 done after 26440 us**; reset leaves sctlr e7 (stall bit on). backboardd respawned at 132.4 s, lock screen on webcam, then **21.5 swaps / 20.5 panel updates per s at +60-120 s with swipes**: the 6-8/s after er4-er7 was the idle lock screen, not a slow GPU. (Boot-time smmu sctlr line not captured: the live log came up 14 s late.)
- sk58 (430 MHz, appsoak 3600 s): 204/204 opens, 0 resets, 0 GPU hangs, 0 SMMU fault lines, 0 "clearing smmu", 0 UIWDT. Pool 97 -> 342 MB, peak 436. Battery 4436 -> 4432 mV. Mean 2.4 swaps/s over the hour (lower than sk57's 9.0; not checked whether that is the averaging window or real).
- Noise: 108x CRASH assetsd, 36x CRASH TipsSpotlightInd, 256x EXC_CORPSE_NOTIFY, 154x CATransformLayer allowsGroupOpacity.
- Phone left on final58 (kernel58, default args incl. watchdog), home screen, 4456 mV.

### 2026-10-27 06:29-07:31 sk57: 1 h app soak, kernel57 at 430 MHz (sweetgpufreq=4)
- 61.6 min, 35 cycles x 6 apps: 210/210 opens, 0 resets, **0 GPU hangs** (sk430 on kernel52 had 1 in the same mix). So the real-hang test of 0181 is still open.
- Pool 161 MB at 89 s -> 345 MB at end, peak 444. Battery 4455 -> 4428 mV. Mean 9.0 swaps/s over the run (includes lock/idle time).
- Noise: 411x "Failed to send exception EXC_CORPSE_NOTIFY ... pid 1148" (from 821 s), 273x apple80211 CURRENT_NETWORK error 6.
- Phone left on kernel57 at 565 MHz (sweetpresentwait=1 flamereboot=0), home screen.

### 2026-10-27 kernel57 (xnu 0181) er4-er7: early CB_RESUME recovery repeats
- Same args as er3 (`sweethangtest=0xA0000200`). Injected at 110.2 / 152.6 / 139.9 / 153.5 s; SMMU fault ~2.0-2.15 s after each injection; fsr 40000402 every time.
- er4 / er5 / er6: first me_init idle (rptr 9 wptr 9), first path, **26.06 / 26.10 / 26.24 ms**. er7: backboardd respawned at 155.6 s and screen was back on the webcam, but the me_init / "recover2: done" lines are missing from the live log (lost lines), so its path is unproven.
- Together with er3: 5/5 recoveries, 4 of them timed at ~26 ms.
- Open: swaps 25-30 s after recovery were 6.1 / 6.7 / 8.2 / 6.1 per s against ~19 before (rec10 on kernel50 was back to 19 at full speed). Could be the lock screen after the respawn rather than a slow GPU; check in the next soak (+2-3 min window).
- er7 boot also logged "SWEET USB: busERR addr 0; SMMU cb fsr 400" every ~5.6 s from 43 to 105 s (not seen on er4-er6).
- Phone left on kernel57, no injection, 100% brightness, 4456 mV. Next: sk57, 1 h app soak at 430 MHz (running).

### 2026-10-27 experiments.md lines
- 2026-10-27 er4-er7: kernel57 SMMU-stall injector (0xA0000200), 4/4 injected; er4-er6 recovered on the first path in 26.06/26.10/26.24 ms (fsr 40000402), er7 respawned + screen back but recovery lines lost from the log. With er3: 5/5 recovered. Swaps after recovery 6-8/s (pre-hang ~19).
- 2026-10-27 sk57: kernel57 at 430 MHz, 1 h app soak (61.6 min, 210/210 opens): 0 hangs, 0 resets, pool 161 -> 345 MB (peak 444); no real hang to test 0181 against.
- 2026-10-27 es1: kernel58 (0182 SMMU stall-on-fault by default) injected SMMU-stall hang: early resume fsr 40000402, recovered first path in 26.4 ms, sctlr e7 after reset (stall bit kept), 21.5 swaps/s at +60-120 s with swipes (so er4-er7's 6-8/s was the idle lock screen).
- 2026-10-27 sk58: kernel58 at 430 MHz, 1 h app soak: 204/204 opens, 0 resets, 0 hangs, 0 SMMU faults, 0 UIWDT; pool 97 -> 342 MB (peak 436). Stall mode causes no extra hangs; still no real hang met.

### 2026-10-26 ~23:15 kernel57 (xnu 0181) er3: early CB_RESUME fixes the stall-on-fault recovery
- `sweethangtest=0xA0000200` (present submit 512): injected at 83.7 s, fault at 85.8 s.
- "recover2: early smmu resume: fsr 40000402"; after GBIF halt rbbm status3 4f20101; first me_init idle (rptr 9 wptr 9 status 1, cp_status1 1000000), zap rc 0; **recover2 done after 26308 us** (kernel56 with the late resume: 2.6 s on the 3rd path; reset off: FAILED). backboardd killed and respawned; 'SWEET GPU: 5s' continues; lock screen at 5.6-7.8 swaps/s after (pre-hang home ~19).
- One run only (er1/er2 at index 1536 never injected). Phone left on night5 (kernel53, 5%), 4435 mV.
- Open: does the real hang (sk430, terminate mode, fsr 0) also clear with 0181? Next real-hang soak on kernel57 at 430 MHz will tell; consider enabling CFCFG (stall mode) in normal operation so real faults look like this injector.

### 2026-10-26 ~22:50-23:06 kernel57 (xnu 0181: CB_RESUME at the start of recovery) — inconclusive
- er1, er2 (`sweethangtest=0xA0000600`, boot-args tail confirmed): no "hangtest: injecting" line in 4 min of swipes each, so recovery was never exercised. 0181 only touches sweet_a6xx.c recovery code, not the arm path, so the cause is likely the run (present submit 1536 not reached: swaps dropped to 0.2/s around 426 s; possibly not unlocked), not the kernel. Not verified.
- Next: rerun er with a lower present index (e.g. 0xA0000200) and confirm the home screen first.
- Phone left on night4 (kernel53, 5%), 4435 mV.

### 2026-10-26 ~22:35-22:43 kernel56 (xnu 0180) SMMU stall-on-fault injector
- `sweethangtest=0xA0000600`: present blit from unmapped VA with CB SCTLR CFCFG (stall on fault). Hang: smmu fsr 40000002, rbbm status1 10301 status2 10100 status3 1f00101 (bit 24 SMMU stalled), cb fsr raw 40000402.
- **ss2, reset off (sweetsmmureset=0): reproduces the real-hang failure exactly**: me_init `rptr 0 wptr 9 status c00015 cp_status1 81104006 TIMEOUT` on every path, "GBIF halt + soft reset + CP init -> -1", recovery FAILED, UI on CPU copies (same as sk430).
- **ss1, reset on: recovers, but slowly**: "smmu stalled fault terminated (CB_RESUME)" seen; first me_init still TIMEOUT (c20015), GBIF-halt path TIMEOUT (c20001), then soft reset + CP init idle -> "recover2: done after 2618248 us". GPU 5 s lines continue.
- Reading: a stalled SMMU transaction is enough to make CP init fail; terminating it (CB_RESUME) is what lets the CP come back. The resume comes too late in the full path (after GBIF halt + GX cycle), so the first attempts still fail. The real sk430 hang printed fsr 0 in terminate mode, so whether it was an SMMU stall is not proven, but the failure signature is identical.
- Phone on night3 (kernel53, 5%), 4455 mV.

### 2026-10-26 ~22:00 Control: kernel54 (xnu 0178) sweetsmmureset=0, stall hangs, 565 MHz
- ctl1/ctl2 present stall (0xC0000600), ctl3 client stall (0x40000BB8): 3/3 injected, "smmu/gbif reset: skipped", me_init idle (status 1, cp_status1 1000000), recover2 done 25.96-26.41 ms. 'SWEET GPU: 5s' lines continue.
- So the stall injector recovers WITH and WITHOUT the SMMU/GBIF reset: it does not reproduce the real-hang failure, and 0177's reset is neither proven nor disproven. Kept on by default (harmless: +0 ms).
- Difference to the real hang (sk430): the real one had ring work pending (rptr 570, wptr 949), ib1 rem 7e and rbbm cf9495 (many blocks busy). The stall leaves the ring drained and rbbm 800005/c00005.
- Also built: kernel55 = xnu 0179 UI watchdog (`sweetuiwdt=30`: touch within 10 s + no swap for 30 s -> kill backboardd; still frozen 30 s later -> stop APSS pets -> fastboot; backboardd also killed once after a failed GPU recovery). CI ok (run 40435125065), not booted yet.
- Brightness: at 3/5/8/12% the webcam sees nothing; icons show from 16%. Phone left on a night boot (kernel53, no injection) at 5%; 4456 mV.

### 2026-10-26 ~21:00-21:50 kernel53 (xnu 0177) stall-hang recovery, 565 MHz
- 0177: `sweethangtest` bit 30 = stall IB (CP_WAIT_REG_MEM on a never-written scratch dword, no SMMU fault); SMMU CB reset (SCTLR.M off, FSR clear, TLBIALL+SYNC, restore) + GBIF halt release before every recovery gpu_cp_init; `recover2: pre-me_init:` diagnostics. Hex boot-args parse fine.
- 5 boots (st1-st5; 3 present 0xC0000600, 2 client 0x40000BB8): 5/5 injected, 5/5 "recover2: done" in 26.0-26.4 ms, me_init idle, display back ~6.7 s after the hang; home screen on webcam after unlock; 16.6-17.8 swaps/s and 12.5-12.7 ms/submit on the home screen after (st4, st5).
- Hang state: fsr 0 (no fault, as intended) but rptr == wptr, ib rem 0, rbbm 800005 (one c00014): NOT the real-hang state (sk430: rptr 570 wptr 949, ib1 rem 7e, rbbm cf9495). Pre-reset CB state every time: sctlr 67, fsr 400, tlbstatus 0, GBIF halt 0 ack 1.
- Open: whether the reset is what helps. Control (sweetsmmureset=0, xnu 0178 / kernel54) next.

### 2026-10-26 Why recovery fails after a real hang (investigator, logs + code only)
- Real (sk430 2296 s) vs injected (kernel50, uptime ~190): identical through sw reset, GX off/on, pll0 restore (both locked). Differences: real hang had no SMMU fault (fsr 0), pending ring work (rptr 570 wptr 949, ib1 rem 7e), rbbm cf9495, cp_status1 8113xxxx, two extra gpucc diffs (+1534 0->1, +1554 1->0); injected = SMMU fault fsr 2, ring empty, rbbm 800005, gets "clearing smmu cb1 fault" + TLB flush.
- After GX on both show rbbm 1 / cp_status1 1000000 (idle); real me_init then times out with rptr 0 (CP never fetched the ring): status c00015, cp_status1 81104006. Same signature as pre-0167 soft-reset failures, so the stuck state survives a GX power cycle.
- Leading theory (~40%): stuck state in the CX domain (GPU SMMU context bank / TBU / GBIF) after an in-flight mid-IB hang; the injected path only recovers because the fault handler clears the SMMU. Our "full" recovery is a GX-only cycle with the GMU running; Linux does a full GMU/CX collapse + re-boot.
- Fix candidates: (a) always reset SMMU CB1 (SCTLR.M off, clear FSR, TLBIALL+SYNC, on) and GBIF before gpu_cp_init; (b) escalate to CX collapse + a6_power_up if me_init still fails; (c) log CB1 SCTLR/FSR/TLBSTATUS, GBIF halt/outstanding, CP_RB_BASE/SQE base, gpucc +1534/+1554 before me_init.
- The bad-IB injector (SMMU fault) cannot reproduce this. Need a hang inside valid memory: CP_WAIT_REG_MEM on a never-written dword, or an endless shader.

### 2026-10-26 sk430: 1 h app soak at 430 MHz (kernel52, sweetgpufreq=4), step 2b
- 19:53:05 to 20:54:48, 35 cycles, 210 opens, 0 openfail, 0 resets, 0 panics. Core 429-430 MHz.
- **1 real GPU hang, NOT recovered** at uptime 2296.098 (present: fence 46266, done 46247, smmu fsr 0; rb rptr 570 wptr 949, ib1 rem 7e, rbbm cf9495).
  - recover2 ran every stage OK (cp stop, GBIF halt, sw reset, gx off/on, DCVS, pll0 restore "locked"), then `ring me_init ... status c00015 cp_status1 81104006 TIMEOUT`; GBIF-halt fallback timed out the same way; FAILED after 3.94 s.
  - The UI kept going without the GPU: "GPU present not used: GPU blit failed", swaps resumed at 2303 s with CPU panel copies (~2.4 ms), 22-27 swaps/s, no more `SWEET GPU: 5s` lines. The soak finished all opens on that path.
- Pool: 198 MB at 121 s, 317 / 325 / 312 at 10-min marks, peak 399, 348 at the hang.
- Battery 4078 -> 4390 mV, +870 mA (charging).
- **565 vs 430:** sk52b (565) 0 hangs in 1 h; sk430 1 unrecovered hang in 1 h. 430 is not safer -> keep 565 MHz (sweetgpufreq=5) as default. Pre-hang swap rates are not comparable (app-open mix).
- New open item: recovery after a real (not injected) hang fails at CP me_init; injected hangs (rec10) recovered. Next: compare the CP/GMU state on that path with the injected one.

### 2026-10-26 ~19:20-19:35 nbproto bridge + Reduce Transparency repro (kernel52, sk52b boot)
- See [metal](metal.md) for this entry; it also touches this feature.

### 2026-10-26 sk52b: 1 h app soak on kernel52 (clean)
- Boot: kernel52, `sweetpresentwait=1 flamereboot=0` + defaults (recover=2, kill=1), 3978 mV at start.
- `sunfish-appsoak.sh sk52b 3600`: 18:18:17 to 19:19:55, 35 cycles x 6 apps = 210 opens, 0 openfail.
- 0 GPU hang/fault, 0 recoveries, 0 panics, 0 resets. First full hour without a reset since 0175/0176.
- GPU pool: 198 MB at start, 319 after 10 min, then flat at 373-424 MB (saw-tooth), 410 at end.
- Average 11.0 swaps/s over 601 stats lines (app-open mix, not a swipe bench).
- Battery on TB4: +965 mA charging early on (chg=1).

### 2026-10-26 experiments.md lines
- 2026-10-26 sk52b: kernel52 1 h appsoak, 210 opens, 0 resets/hangs/panics; pool 198 -> flat ~380-420 MB; 11.0 swaps/s avg.
- 2026-10-26 sk430: kernel52 430 MHz 1 h appsoak: 0 resets, 1 real GPU hang at 2296 s, recovery FAILED (me_init TIMEOUT), UI continued on CPU copies; 565 kept as default.
- 2026-10-26 recovery-fail analysis: real hang = no SMMU fault, ring pending; me_init rptr 0 after clean GX cycle; suspect CX-side SMMU/GBIF state; injector can't reproduce (needs wait-forever IB).
- 2026-10-26 st53: kernel53 stall injector + SMMU/GBIF reset, 565 MHz: 5/5 stall hangs recovered in 26 ms; stall state milder than the real hang (ring drained, rbbm 800005).
- 2026-10-26 ctl54: sweetsmmureset=0 control, 3/3 stall hangs still recover in 26 ms -> stall injector too mild; SMMU/GBIF reset unproven. kernel55 (0179 UI watchdog) built, untested.
- 2026-10-26 ss1/ss2: kernel56 SMMU stall injector reproduces the real me_init c00015 failure with reset off; with CB_RESUME reset, recovery succeeds on the 3rd path after 2.6 s.
- 2026-10-26 er1/er2: kernel57 early SMMU resume: injection never fired in 4 min (present 1536 not reached?), untested.
- 2026-10-26 er3: kernel57 (0181 early CB_RESUME) SMMU-stall hang recovered on the first try in 26.3 ms (kernel56: 2.6 s; reset off: FAILED). 1 run.

Older entries (before 2026-10-26): [archive/features/gpu.md](../archive/features/gpu.md).
