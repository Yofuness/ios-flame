# GPU: archived log (entries before 2026-10-26)
Current file: [features/gpu.md](../../features/gpu.md). Same format, newest first.

## Log

### 2026-10-25 Plan step 2: hang triggers
- **The "vertexFunction/fragmentFunction is not a MTLFunction" lines are noise, not the Reduce Transparency failure.** Found by investigator, from a Metal.framework disassembly.
  - They come from Metal's `-[MTLRenderPipelineDescriptorInternal setVertexFunction:]`, which checks conformsToProtocol(MTLFunction), NSLogs, then stores the function anyway.
  - NBFunction never declared the protocol, so the line appears on every pipeline creation (~31k lines in the log, healthy boots included).
  - Fix in the bridge: class_addProtocol for NBFunction/MTLFunction and NBLibrary/MTLLibrary.
- **The RT hang is a real CP stall mid-IB with no SMMU fault** (`rbbm status c30d15`), in a submit built from valid pipelines. Which RT pipeline or draw causes it is unknown.
  - Next: an RT boot with the pipeline journal (pipelines.log) and SWEET_DRAWLOG, to name the last pipeline before the hang. Then refuse it in newRenderPipelineStateWithDescriptor, or fix it.
  - Recovery now covers this case (0167-0174).
- **[16:41] soak565 run A aborted: the phone silently reset to fastboot.**
  - Boot: kernel50, default args + `sweetpresentwait=1 flamereboot=0`.
  - The reset came at ~468 s uptime, during app-soak cycle 4. The last kernel lines are routine (AKS, lskdd respawn, a Wi-Fi scan), with no panic, GPU hang or recovery line.
  - The GPU pool was high: 410-416 MB in use, peak 467 MB. It was 132 MB at 55 s.
  - Fastboot answers, at 4435 mV.
  - The soak driver kept sending app opens for ~10 min after the reset, because it had no reachability check. I stopped it, and run B (430 MHz) never ran.
  - Open: what reset the phone (watchdog? a USB-side reset?). Look for a PMIC/IMEM restart reason on the next boot.
- **[16:52] rst1: second silent reset to fastboot, at ~178.7 s uptime.**
  - Boot: kernel50, `sweetpresentwait=1 flamereboot=0`.
  - It happened while the agent was running an SSH `dd` of the last 1153 blocks of the userdata disk, to read the shim rescue log and the XNU ring. The output file is empty.
  - The last kernel lines are routine vsync on/off. There is no panic line, and the log stops instantly.
  - Unknown whether the raw disk read caused it or it is the same reset as soak565. Next time, read the rescue block without touching anything else, and snap the webcam before each step.
- **[17:00] Correction: a leftover driver kept using the phone.**
  - When soak565 was stopped at 16:41, the soak agent's `driver.sh` (scratchpad) was still alive. It rebooted the phone into run B and booted soak430 (kernel50, `sweetgpufreq=4`) at 16:41:54, then drove apps until 16:57. It was killed at 17:00.
  - So the "rst1" reset at 178.7 s was soak430 (430 MHz) resetting silently during app cycle 4, not the userdata `dd`.
  - **Two silent resets during app soaks: 565 MHz at 468 s and 430 MHz at ~179 s.** Neither left a panic line. Lowering the clock does not prevent them.
  - The rst2 boot (16:57) collided with the driver. Afterwards the phone showed the ABL unlocked-warning screen with the USB link down.
- Lesson: stopping an agent does not stop the background scripts it started. Check `ps` for scratchpad scripts after every TaskStop.
- **[18:26] sk52 soak (kernel52, default args incl. GPU recovery, `sweetpresentwait=1 flamereboot=0`), stopped after ~20 min for teardown.**
  - Ran 18:06:39-18:26: 12 cycles of 6 apps, **no reset**, no GPU hang or recovery.
  - Avg 12.9 swaps/s over 186 samples (app launching).
  - GPU pool 382 MB at the end, peak 470 MB.
  - 232 CRASH lines, all Spotlight importer extensions crash-looping (TipsSpotlightIndex and PassKitSpotlight ×56; MobileSMS, Contacts, VoiceMemos, Podcasts ×28). That is a usability quick-win candidate (plan step 6).
  - The earlier resets came at 468 s and ~179 s. This run passed ~1200 s without one. Not conclusive: kernel52 changes only UFS raw reads.
- Not done: the full 1 h soak and a reset capture. Next session:
  - Boot kernel52.
  - Run `tools/sunfish-appsoak.sh TAG 3600`.
  - On a reset, boot once and read the rescue first (`toybox cmp -l /dev/rufsroot0 /dev/zero $((28728186*4096)) 0 | toybox head -n 262144`).

### 2026-10-25 GPU hang recovery — xnu 0167, kernel43 (plan step 1)
- 0167 adds four things:
  - `sweetgpurecover=2`: full recovery (CP stop, GBIF halt, soft reset, GX off/on through the GMU, CP init + zap).
  - `sweethangtest=N`: injects a bad IB on the Nth client submit; with bit 31 set, on the Nth present submit.
  - `sweetgpukill=1`: SIGKILL to backboardd after a recovery.
  - Present re-arm after any recovery.

  The defaults are unchanged. The first CI build failed on missing declarations; the fix was folded into 0167. CI run 40218123380.
- rec0 (no new flags): boots, 19.6 swaps/s, 18.6 panel updates/s.
- rec1 (client-submit hang at 273.7 s):
  - Injected SMMU fault fsr 2, far fff0000000.
  - All 7 recover2 stages returned 0, in 28.6 ms in total.
  - The present was re-armed. backboardd (pid 57) was killed and respawned.
  - Webcam: black at +2 s, spinner at +5 s, lock screen animating from +8 s.
- rec2 (present-submit hang at 191.6 s; this case failed 6/6 before 0167): recovered in 22.4 ms, backboardd respawned, lock screen back at about +8 s.
- **First GPU hang recoveries that bring the screen back.**
- **Problem after recovery:** the GPU runs very slowly.
  - backboardd submits take 100-980 ms each (mean 93-190 ms against ~13 ms before), with the GPU 27-99% busy.
  - Frame rate is 1.2-7.4 swaps/s, and the 20 s benches recorded no FB samples.
  - Suspect: the GPU clock or DCVS level is not restored after GX on (maybe the lowest level), or register state such as HWCG/UCHE was lost with the GX collapse.
- **xnu 0168 (kernel44, rec3): OOB gpu_set re-sent after GX on — no effect on the speed.**
  - Recovery was acked (dcvs and OOB gpu_set) and took 23.4 ms. backboardd was respawned.
  - Before the hang: 19.4 swaps/s, 13.1 ms per submit.
  - After: 0 swaps/s in both benches, backboardd ~910-990 ms per submit, GPU 100% busy. The screen stayed on the lock screen and the unlock swipe did not take.
  - So the slow-down is not the missing OOB vote. Next: xnu 0169 measures the real GPU core MHz (RBBM perf counter) and dumps clock/GX state at boot and after recovery.
- An SMMU fault repeats on the USB path before the hang (cb fsr 400, every ~5.6 s from 46.8 s, `busERR addr 0`). This is new and unexplained.
- **xnu 0169 (kernel45, rec4): the clock probe finds the cause.**
  - The GPU core runs at 564 MHz from boot to the hang (379.8 s). After recovery it reads 175 MHz, then 14 MHz from 393 s on.
  - backboardd submits go from ~24 ms to 90-680 ms, with the GPU 99-100% busy.
  - The recovery diag shows dcvs ret 0, perf_level 5, dcvs perf 30000005: the GMU acked the same level without reprogramming the GX clock, which the GX collapse had reset.
  - Before the hang: 17.7 swaps/s. The "clk diag (boot)" line never printed.
- Fix attempt xnu 0170: in a6_gx_on, vote another DCVS level first, then the real one (CI run 40223666383).
- **xnu 0170 (kernel46, rec5): the DCVS re-vote changes the post-recovery clock, but it does not fix it.**
  - The core reads 222 MHz, then stays at 56 MHz (it was 14 MHz without the re-vote).
  - Benches: before the hang 18.8 swaps/s and 18.2 panel updates/s; after, 12.6 / 12.0 and 12.3 / 12.0.
  - The live log has no fault, recover2 or kill lines for this hang, only `exec backboardd` at 295.4 s. Lines were probably lost while the kernel was busy.
  - The "clk diag (boot)" line was missing in rec4 too. That may be the same lost-lines problem.
  - Theory: the GX collapse also resets GPUCC clock state that ABL programmed (PLL/RCG), and the GMU does not redo it.
  - Next: xnu 0171 snapshots GPUCC (0-0x1800) at boot and logs every register that differs after a recovery.
- **xnu 0171 (kernel47, rec6): GPUCC diff after the injected present hang (recovery 23.9 ms, backboardd killed).**
  - 8 registers differ from boot:
    - +0000 PLL0 MODE: d1000005 → 01000005, so lock is lost.
    - +0004 L_VAL: 3a → 0.
    - +0038 FRAC: daaa → e881.
    - Five CBCRs (+105c, +1080, +1084, +1088, +1094): 80000000 → 0.
  - The core clock is 564 MHz before the hang and 56 MHz after.
  - **Cause: the GX collapse wipes GPU_CC_PLL0 (Fabia).** ABL programmed it, and neither the GMU nor our code redoes it.
  - Bench before the hang: 20.0 swaps/s, 19.2 panel updates/s.
- The live log starts ~30 s into boot, so the early boot lines ("gpucc snap (boot)", "clk diag (boot)") are never seen. The snapshot is still taken.
- Fix attempt xnu 0172: after GX on, restore PLL0 L_VAL and FRAC from the boot snapshot, then run the Fabia enable (OUTCTRL off, OPMODE standby, values, RESET_N, OPMODE run, wait for lock, OUTCTRL on), then the DCVS votes.
- **xnu 0172 (kernel48, rec7): no effect.**
  - The restore ran before the DCVS votes, found PLL0 intact and returned silently. Afterwards the diff showed L_VAL 0 again.
  - So the GMU's DCVS/OOB step is what leaves PLL0 at L 0.
  - Core clock 564 MHz → 56 MHz.
  - Benches: before 18.4 swaps/s and 17.7 panel updates/s; after, 10.8 / 10.3 and 12.2 / 11.4.
- xnu 0173: restore PLL0 after the votes, and log "intact" too.
- **xnu 0173 (kernel49, rec8): worse.**
  - The PLL0 restore after the votes wrote L 3a and FRAC daaa, but got LOCK TIMEOUT (mode 1000005 → 1000005). The GX rail/SPTPRAC step also timed out this time.
  - Core clock 480 MHz, then 9 MHz. GPU work stops: 0 ms busy, and the screen shows a half-drawn frame.
  - The phone stayed reachable. backboardd was killed twice (pids 58 and 332).
  - Before the hang: 17.0 panel updates/s, 13.7 ms per submit.
  - My Fabia enable sequence is wrong or incomplete (CONFIG_CTL, CAL_L or the PLL_UPDATE latch?). This is the 4th attempt at the post-recovery clock (0168, 0170, 0172, 0173).
- Defaults are unaffected: everything is behind `sweetgpurecover=2`.
- **xnu 0174 (kernel50, rec9): the post-recovery clock is fixed.**
  - PLL0 restore follows Linux's Fabia configure + enable (reset recal, CAL_L 3f, CONFIG/USER/TEST registers from the boot snapshot).
  - Step logs: PLL0 is intact before and after GX on (L 3a). **The GMU's DCVS vote is what zeroes it** (after dcvs: mode 1000005, L 0, frac e881).
  - The restore then locked (mode d1800005). Recovery took 27.8 ms and backboardd was killed.
  - **The core clock reads 564 MHz on every line after the hang,** up to 303 s.
  - backboardd: 13.3 ms per submit before the hang, 20.3-20.5 ms after.
  - Benches: before 19.3 swaps/s and 19.0 panel updates/s; after, 9.0 / 8.6 and 9.2 / 9.0.
  - The post-hang benches ran on a Batteries widget panel, not the home screen, so swaps/s is not comparable. The cause of the remaining ~50% gap on that screen is open.
- **rec10 (kernel50, all benches from the home screen): full speed after recovery, verified.**

  | bench | swaps/s | panel updates/s | backboardd ms/submit | core MHz |
  |---|---|---|---|---|
  | before the hang | 19.7 | 19.2 | 13.23 | 564 |
  | post1 | 19.1 | 18.6 | 13.13 | 564 |
  | post2 | 18.7 | 18.1 | 13.45 | 564 |
  | post3 | 19.4 | 18.7 | 12.53 | 564 |

  - The injected present hang recovered in 26.2 ms. PLL0 relocked and backboardd was killed and respawned.
  - rec9's low numbers came from the screen: the bench swipes alternate 900→150 and 150→900, so a bench can end on the widget (Today) panel. Check for the home screen before each bench.
- sunfish-boot.sh now passes `sweetgpurecover=2 sweetgpukill=1` by default (from kernel50 on; older kernels ignore them).

### 2026-10-25 soak2 — fresh boot, 6 apps (plan step 0, run 3)
- Fresh boot soak2 at 13:02: kernel42 `sweetpresentwait=1 sweettouch=2 flamereboot=0`, 4456 mV.
- Loop: Preferences, Safari, Notes, Calendar, Clock, Photos; ran 13:12-13:34 (20 min). The loop started ~10 min after boot, not right away.
- **No stall.** The home screen was fine at the end. Swaps/s per minute: 7.4-11.7.
- Counts after the marker: 0 `not a MTLFunction` (soak1 had 104), 0 `EXC_CORPSE`, 0 Code=5.
- The Photos step snap is black apart from the home indicator. Photos opens to a black/empty view; check whether it renders at all.
- The `=== soak2 stall` marker at 13:04:51 is false: the agent's check was inverted, and the phone was healthy at ~10 swaps/s.
- soak0's stall (~4 min after boot, Code=5 on every open) did not reproduce in 2 runs (40 min). It is rare, or tied to something in soak0's first minutes (the Safari surface burst, 160 → 293 MB).

### 2026-10-25 soak1 — short app soak with stall capture (plan step 0, run 2)
- Same boot (soak0r, kernel42). Loop: Preferences → Safari → Notes → Calendar, 10 s each with 3 swipes, a webcam snap after every app. Auto-stop on 2 consecutive `SWEET FB 5s` samples < 1 fps, or on a failed open.
- 12:39-13:01 (uptime 788-2086 s): **no stall.** All 48 opens succeeded (no Code=5), and the home screen is fine on the webcam at the end.
- Swaps/s per minute: 8.7-17.1, mostly 10-13. That is lower than the ~20 of the swipe bench, because of the app launches. 12 isolated samples were < 1 fps, never two in a row.
- 104 `not a MTLFunction` lines and 2 `EXC_CORPSE` lines, without a stall. So those errors alone don't freeze the UI.
- Not reproduced: soak0's stall came about 4 min after a fresh boot, with 6 apps, including Clock and Photos. Suspects now: the first-boot launch burst, or the Clock/Photos apps (PhotosFileProvider crashes on every launch).
- The workload script is saved as `tools/sunfish-appsoak.sh`.

### 2026-10-25 soak0 — baseline stability soak (plan step 0), aborted at ~16 min
- Boot soak0: kernel42 `sweetpresentwait=1 sweettouch=2 flamereboot=0`; workload = swipes + 6 stock apps + Safari scroll.
- The user saw a crashed screen. Webcam (`../logs/soak0-crash.jpg`): left half white, right half black, no UI. Kernel alive (log at 1031 s), phone reachable.
- Log at the time: every SpringBoard thread sampled at the same pc 0x1e44314f0 (955 s) → SpringBoard stuck; backboardd idle (1 swap per 5 s, 2 GPU submits, GPU 0% busy); no GPU hang/fault/SMMU lines; `lskdd` respawned 630 times in the soak (crash loop).
- So the first failure is a SpringBoard hang, not the GPU. Lesson: the soak loop must snap the webcam each cycle and stop when swaps/s drops to ~0.
- Rebooted with `flame-cmd reboot` (4456 mV), booted soak0r with the same args; lock screen fine on the webcam.
- **Correction after log analysis (investigator):** the 955 s SpringBoard samples are idle stacks (main thread in CFRunLoop/mach_msg, workers in `__workq_kernreturn`), byte-identical to the healthy samples at 115 s. So SpringBoard was not deadlocked.
- The UI actually stopped at ~222-262 s uptime:
  - Swaps/s: 17-19 (176-188 s) → ~5 with 40 ms GPU submits (194-217 s) → 0.2 from 262.9 s until the reboot at ~1057 s.
  - The last good app open was Notes (~223 s). From Calendar on, every open failed with `FBSOpenApplicationServiceErrorDomain Code=5`, and HID swipes kept arriving with no redraw.
- Signals around the stall:
  - 187× backboardd `vertexFunction/fragmentFunction is not a MTLFunction` (CA's RT/blur shaders, from 80 s on).
  - `EXC_CORPSE_NOTIFY` failed for pids 329, 341, 101 (183 s).
  - `hfs_clonefile: cluster_write failed - 34`.
  - The GPU pool rose 160 → 293 MB on Safari's launch.
  - Unbalanced SBSwitcherViewController transitions.
- Not the cause: no panic, jetsam, watchdog or GPU fault. The lskdd loop (642 execs, every 1.03 s) only starts at 390 s, after the stall. fairplayd.H2, bluetoothd and wifip2pd also respawn every ~10 s.
- Hypothesis (low confidence): SpringBoard's UI state wedged around the switcher/Safari burst, possibly from the failed CA shaders or a crashed daemon.
- Next:
  - Short soak repeating Preferences → Safari → Notes → Calendar.
  - Auto-stop when swaps drop below 1/s, then pstack SpringBoard, take `launchctl list` and pull the CrashReporter logs at that moment.
  - Fix the bridge for CA's RT shaders.

### 2026-10-25 experiments.md lines
- 2026-10-25 soak0 (kernel42, stability baseline): SpringBoard hang after ~16 min (all threads at pc 0x1e44314f0), screen half white/half black, GPU idle and healthy, lskdd crash loop 630x. Aborted; rebooted OK.
- 2026-10-25 soak0 analysis: SB stacks idle (not deadlocked); real stall at ~222-262 s (swaps 0.2/s, app opens fail Code=5) after Notes; coincides with 187x CA 'not a MTLFunction' shader failures + corpse notify failures; lskdd loop starts later (390 s), not the cause.
- 2026-10-25 soak1 (kernel42, 20 min Preferences/Safari/Notes/Calendar loop, auto-stop): no stall, 48/48 opens OK, 8.7-17 swaps/s; 104 MTLFunction errors without a stall. soak0 stall not reproduced.
- 2026-10-25 soak2 (fresh boot, 6 apps incl. Clock/Photos, 20 min): no stall, 7.4-11.7 swaps/s, 0 MTLFunction/CORPSE/Code=5; Photos shows black. soak0 stall not reproduced in 2 runs.
- 2026-10-25 xnu 0167 (kernel43) GPU full recovery: injected client and present hangs both recover (22-29 ms, all stages rc 0), backboardd respawns, lock screen back in ~8 s. But afterwards the GPU is ~10x slower (submits 100-980 ms, 1-7 swaps/s): clock/DCVS level or lost register state after GX power cycle.
- 2026-10-25 xnu 0168 (kernel44): re-sending OOB gpu_set after GX on does not fix the post-recovery slowdown (910-990 ms/submit, 0 swaps/s).
- 2026-10-25 xnu 0169 (kernel45): GPU core clock 564 MHz before the injected hang, 14 MHz after recovery (GMU acks the same-level DCVS without reprogramming) = cause of the post-recovery slowdown.
- 2026-10-25 xnu 0170 (kernel46): DCVS re-vote after GX on → core 56 MHz after recovery (was 14); 12.5 swaps/s after vs 18.8 before. Not fixed; suspect GPUCC PLL/RCG state lost in GX collapse.
- 2026-10-25 xnu 0171 (kernel47): GPUCC diff after recovery: PLL0 L_VAL 3a→0, FRAC changed, unlocked → GX collapse wipes GPU_CC_PLL0 = cause of 14-56 MHz after recovery.
- 2026-10-25 xnu 0172 (kernel48): PLL0 restore placed before DCVS found PLL intact; DCVS itself zeroes L_VAL → still 56 MHz. 0173 restores after the votes.
- 2026-10-25 xnu 0173 (kernel49): PLL0 restore after DCVS → LOCK TIMEOUT, core 9 MHz, GPU stops. Hand-rolled Fabia enable does not work; 4th failed attempt at post-recovery clock.
- 2026-10-25 investigator (Linux sunfish 4.14 clk-alpha-pll.c, gpucc-sdmmagpie.c, kgsl): GPUCC+0 = gpu_cc_pll0 Fabia; GMU firmware owns the GPU clock (kgsl uses gmu dcvs, no PLL code); full fabia configure needs CAL_L + CONFIG/USER/TEST regs and reset recal. xnu 0174 implements that from the boot snapshot.
- 2026-10-25 xnu 0174 (kernel50): Linux-style Fabia PLL0 restore after the DCVS vote locks; core 564 MHz after recovery (was 9-56). GMU DCVS is what zeroes PLL0 L. Post benches 9 swaps/s on a widget panel (not comparable); backboardd 20 vs 13 ms/submit.
- 2026-10-25 rec10 (kernel50, home screen): after injected present-hang recovery 18.7-19.4 swaps/s and 12.5-13.5 ms/submit vs 19.7 / 13.2 before = full speed restored.
- 2026-10-25 soak565 (kernel50, 1 h app soak): silent reset to fastboot at ~468 s (cycle 4), no panic/GPU lines, GPU pool 410-467 MB. Aborted; 430 MHz run not done.
- 2026-10-25 rst1 (kernel50): second silent reset at 178.7 s during an SSH dd of the userdata tail; empty output; cause unknown.
- 2026-10-25 soak430 (kernel50, sweetgpufreq=4, driven by a leftover driver): silent reset at ~179 s during app cycle 4 → resets happen at 430 MHz too.
- 2026-10-25 sk52 (kernel52, 20 min app soak, stopped for teardown): no reset, no GPU hang; 12.9 swaps/s; pool peak 470 MB; Spotlight importer extensions crash-loop (232 CRASH lines).

### 2026-10-23 22:35 GPU stability — what actually freezes/blacks the screen
- See [gpu-memory](gpu-memory.md) for this entry; it also touches this feature.

### 2026-10-23 11:55 Metal on the GPU — **PASS: backboardd renders through Metal2 + Turnip on the A618**
- See [metal](metal.md) for this entry; it also touches this feature.

### 2026-10-23 11:10 GPU — **PASS: GMU HFI fully working** (0143 diag + 0144 fix)
- 0143 DTCM dump after HFI: GMU fault pc 0x2536 = `bkpt` in the fw's HFI queue reader: message size 0 or >0x100.
  Our hfi_send padded every message to 4 dwords with 0xfafafafa (HFI v2 rule; kgsl pads only for v2). The
  legacy v1 fw read the padding as a 250-dword message header.
- 0144: no padding / no read alignment on A618. Result: `hfi(v1): init, perf/bw tables, test -> 0; dcvs 267 MHz
  acked`, `oob gpu_set acked`, **bring-up 297 ms** (was 6 s with timeouts). No SMMU faults.
- GPU present per frame **0.77 ms** (was 27 ms). Swipe bench 16.4 fps average (samples 17–23 while moving);
  now CPU-bound in backboardd, not GPU-bound.
- (Recovered from `git show 1808cf9^:docs/sunfish/workday.md`; that commit deleted it.)

### 2026-10-23 10:50 GPU — 0142: SID 4 mapped, no SMMU faults at all; GMU still faults after HFI INIT
- SMR1 `80000004` took. After the HFI v1 sequence: sGFSR 0, GMU bank FSR clean → the remaining CM3 fault is
  internal, not a memory-access fault through the SMMU. GPU still composites (CP/zap fine); only DCVS (clock
  boost) is missing. 0143 dumps DTCM after the HFI sequence to decode the new fault PC.
- (Recovered from `git show 1808cf9^:docs/sunfish/workday.md`; that commit deleted it.)

### 2026-10-23 10:20 GPU — **PASS: Adreno 618 composites the UI** (0140 + 0141)
- 0140: kgsl SMMU `attach-impl-defs` written; SMMU state logged at setup: impl regs stick, SMR/S2CR read 0.
  The hypervisor emulates the SMMU global space (Google arm-smmu.c comment) and dropped our SMRs.
- 0141: **SMR mask 0** (DT `#iommu-cells = 1` = exact SIDs; flame's mask 0x400 was refused). SMR now `80000005`,
  `hfi: ctrl init -> status 1 ok`, CP `me_init` executes (rptr = wptr, idle), zap ok, **"GPU ready in 5979 ms"**.
- Framebuffer switched to GPU present by itself ("frames now scaled to fit by the GPU, panel stride 4352").
  Screenshot logs/shots/gpu-k25-home.png (read with `flame-shot --stride 4352`).
- Swipe bench: **20.1 fps** (CPU path 17.2). GPU present ~27 ms/frame → GPU clock probably at minimum:
  HFI messages after INIT still CM3-fault (`info 800000`), so DCVS (clock up) times out. Next: that fault.
- (Recovered from `git show 1808cf9^:docs/sunfish/workday.md`; that commit deleted it.)

### 2026-10-23 09:55 GPU — **BLOCKED, parked** (3 diagnostic boots on the HFI fault)
- 0139 (SMMU global-state diag, k22): the phone reset ~2 min after boot, before the diag lines flushed; no data.
  It returned to fastboot by itself (no hard hang).
- Best lead for next time: `gcc gpu_memnoc_gfx … (still halted)`. That is the GPU SMMU's clock per the DT, which fits
  "SMR readback 0" + "GMU bus fault on first SMMU read". Exact next steps are in docs/gpu-notes.md.
- Phone: k23 (k14, no GPU) running with flamereboot=600 → returns to fastboot to charge. Battery 3806 mV at 09:48.
- (Recovered from `git show 1808cf9^:docs/sunfish/workday.md`; that commit deleted it.)

### 2026-10-23 09:45 GPU blocker: CM3 fault decoded — **root cause narrowed** (0138 diag)
- 0138 copies GMU DTCM/ITCM to low RAM after the fault. k20 died before the console flushed (watchdog → fastboot,
  no hard hang); a warm boot of k14 (k21) read the copy back from 0xE2010000.
- Exception frame: **pc 0x19a8, lr 0x227f, r3 0x60001000**. In a618_gmu.bin that is the first read of the HFI
  queue table → bus fault. The GMU can't reach its HFI memory through the SMMU.
- Our SMR readback is 0 on every sunfish boot (flame: 84000005) → GMU stream 5 likely unmatched.
- Details: docs/gpu-notes.md. Next: 0139 logs global SMMU fault + all SMR/S2CR (diagnostic, k22).
- (Recovered from `git show 1808cf9^:docs/sunfish/workday.md`; that commit deleted it.)

### 2026-10-23 09:10 GPU blocker next-step #1 — eliminated (no code change)
- RSCC/PDC/TCS programming matches kgsl's A618 path word-for-word → not the cause. Details in
  docs/gpu-notes.md. GPU stays blocked; next real step = GMU TCM dump + CM3 fault decode (multi-hour).
- Using the free slot to start Step 3 (Wi-Fi join, 1 h cap) early.
- (Recovered from `git show 1808cf9^:docs/sunfish/workday.md`; that commit deleted it.)

### 2026-10-23 08:30 GPU M4b HFI — **BLOCKED** (3-attempt rule); switching to fallback
- 0134 (4-queue table) and 0135 (HFI at 0x60001000): same CM3 fault at HFI_CTRL_INIT.
- 0136 diagnostic: GMU SMMU FSR 0x400 (no fault bits), FAR 0 → internal CM3 fault, not a page fault.
- Blocker + exact next steps written in docs/gpu-notes.md (A618 RPMh/PDC sequences, GMU crash dump).
- Safety: every failed GPU boot wedged after CP init and the watchdog returned the phone to fastboot.
- Next: fallback = CPU render path (start: `sweetfb_hz` 30 → 60).
- (Recovered from `git show 1808cf9^:docs/sunfish/workday.md`; that commit deleted it.)

### 2026-10-23 07:55 GPU 0133 — partial pass (rails), HFI still faulting → 0134
- k15: `legacy: gfx rail (gx idx 3 mx idx 2) acked, sptprac on`, G2H clean after boot, **GX GDSC on**
  (f8222000). The CM3 fault (bit 23) now appears right at `HFI_CTRL_INIT`.
- Then OOB gpu_set timeout and CP `me_init` TIMEOUT (hwfault c0000) → phone wedged, watchdog put it back
  in fastboot (safety net works).
- Cause candidate: our HFI queue table — 3 queues, size in **bytes**; kgsl: 4 queues (dispatch disabled),
  size in **dwords**. 0134 fixes it for A618.
- (Recovered from `git show 1808cf9^:docs/sunfish/workday.md`; that commit deleted it.)

### 2026-10-23 07:45 GPU M4b HFI — fail → 0133
- k14 (0132): `hfi: ctrl init -> status 0 TIMEOUT`; every message unacked; G2H_INFO 0x800000 =
  **bit 23 HFI_IRQ_CM3_FAULT**: the GMU CPU faults right after boot.
- Differences vs kgsl's legacy start found: SFR chip id must be packed (`0x06018000`, I wrote 0x06010800);
  `a6xx_gmu_gfx_rail_on` (OOB 6, GX/MX vote idx) and `a6xx_gmu_sptprac_enable` (0x778000 → poll bit 3)
  run before HFI start on a615-family. 0133 adds all three.
- (Recovered from `git show 1808cf9^:docs/sunfish/workday.md`; that commit deleted it.)

### 2026-10-23 07:35 GPU M4 GMU boot — **pass** (0131)
- k13 trail: `gmu: 1 firmware blocks loaded` → `gmu: init result babeface BOOTED`.
- Then every A640-style HFI message timed out (`hfi status 0`). kgsl for non-A640: `GMU_HFI_CTRL_INIT=1`
  (0x1f986) + poll status, `H2F_MSG_INIT`, v1 perf table, BW table, `H2F_MSG_TEST`, DCVS via
  `GMU_DCVS_*` registers + OOB 7. cmd-db on sunfish: MC0 0x50000, SH0 0x5003c, ACV 0x5000c (= flame's DDR
  BW entries), CN0 0x500a8 (flame's CNOC entries 0x50034/0x5007c/0x5004c don't exist here).
- 0132 implements that sequence.
- (Recovered from `git show 1808cf9^:docs/sunfish/workday.md`; that commit deleted it.)

### 2026-10-23 07:20 GPU M3 power/clocks — pass; M4 GMU firmware — fail → fixed in 0131
- 0130: GPU log RAM trail (0xE2000000) + `sweetgpudelay`; also fixed **RSCC hardcoded at flame's
  0x02c6a000+0x23000** (on SM7150 an unclocked block → that was the k11 console stall).
- k12 trail: `cx gdsc -> on`, gpucc ahb/cxo/cx_gmu on, GMU regs read (`init_result 0, rpmh_state f`),
  SMMU bank programmed. Then `gmu fw: bad block f1f00060` — a618_gmu.bin is a **legacy raw ITCM image**
  (kgsl `_load_legacy_gmu_fw` for a630/a615 family), not flame's block format.
- 0131: raw ITCM load, chip id 0x06010800 in GMU_SFR_ADDR (was 0x06040100), legacy HFI queue ids
  (cmd 0, msg 4, dbg 5), legacy OOB (set BIT16 / ack BIT24). Still to do: HFI v1 tables if needed.
- (Recovered from `git show 1808cf9^:docs/sunfish/workday.md`; that commit deleted it.)

### 2026-10-23 06:50 GPU M2/M3 constants (patch 0129) — fail (log silent), diagnosed partially
- 0129: A618 constants, fw names (a618_gmu, a615_zap), freq table at/below A640 per level, PDC fix.
  Firmware passed via `GPUFW=… sunfish-boot.sh` (mkpayload --gpufw → /chosen/gpufw,*).
- k11 (`sweetgpu=1 sweetgpufreq=2`): phone boots, SSH works, **but the USB live log is silent from boot**.
  Control k11b (same kernel, no sweetgpu): log flows (3165 lines / 75 s). → GPU bring-up wedges the console
  (likely a CPU stalled on an unclocked/unpowered register while holding the console lock).
- Next (0130): GPU log mirrored into RAM at 0xE2000000 (read via /dev/pmem) + `sweetgpudelay` so bring-up
  starts after USB/SSH are up.
- (Recovered from `git show 1808cf9^:docs/sunfish/workday.md`; that commit deleted it.)

### 2026-10-23 06:42 GPU M1 recon — pass
- docs/gpu-notes.md: A618 chip id 0x06010800; GPU 0x5000000, GPUCC 0x5090000, SMMU 0x5040000; GDSC offsets
  same as A640; GCC ddrss_gpu_axi 0x4452c (A640 0x71154); zap region 0x98f95000, a615_zap; PDC TCS3 cmd1
  data 2 (A640 3); HFI path depends on GMU fw HFI major (v1 → v1 DCVS table + TEST msg; quirk hfi-use-reg).
- Phone parked in fastboot between tests: battery **rises** there (3788 → 3822 mV in 10 min).
- (Recovered from `git show 1808cf9^:docs/sunfish/workday.md`; that commit deleted it.)

### 2026-10-23 experiments.md lines
| date | patch / switch | hypothesis | predicted ms saved | measured fps and GPU ms | verdict | commit |
|---|---|---|---|---|---|---|
| 2026-10-23 | xnu 0145 GMEM 512 KB + UBWC hbb 14, Turnip A618 device info | correct A618 config for Turnip | bring-up | first Metal UI, 22.8 fps (old metric); no isolated A/B | kept (required) | ddb9e31 |
| 2026-10-23 | xnu 0146 CP ROQ, VBIF QoS, UCHE_CLIENT_PF as kgsl | match kgsl init | bring-up | no isolated A/B | kept | e59cf83 |
| 2026-10-23 | xnu 0148 stock clock levels 650/800/825 (`sweetgpufreq=6..8`) | more GPU clock | some | see GPU clock line | kept (levels), no gain | c3ff231 |

### 2026-10-23 10:00 status.md update
- GPU (A618): GMU boots; HFI blocked. The GMU bus-faults on its first SMMU read (fw pc 0x19a8). Suspect: the GPU
  SMMU clock gcc_gpu_memnoc_gfx stays halted → SMR writes dropped. Next steps are in gpu-notes.md.
