# Sunfish experiments log (append-only)

One line per experiment, dead ends included. Never delete or rewrite lines; add a new line to correct one.
fps = panel updates/s from `tools/sunfish-fpsbench.sh` (Settings swipes) unless noted; GPU ms = backboardd GPU ms per frame/submit.
Stop rule: a gain under 1 ms GPU (or inside the ±3 updates/s bench noise) is a dead end.

| date | patch / switch | hypothesis | predicted ms saved | measured fps and GPU ms | verdict | commit |
|---|---|---|---|---|---|---|
| 2026-10-23 | `sweetfb_hz=60` | 30 Hz vsync caps the UI | - | 4.3 -> 4.9 fps | kept | df05f86 |
| 2026-10-23 | drop launchd job vmd (Visual Voicemail) | vmd spins a core without cellular | CPU | 4.9 -> 16.7 fps | kept | df05f86 |
| 2026-10-23 | drop launchd job ReportCrash | busy with TTS synthesizer crashes | CPU | 16.7 -> 17.2 fps | kept (within noise) | df05f86 |
| 2026-10-23 | xnu 0145 GMEM 512 KB + UBWC hbb 14, Turnip A618 device info | correct A618 config for Turnip | bring-up | first Metal UI, 22.8 fps (old metric); no isolated A/B | kept (required) | ddb9e31 |
| 2026-10-23 | xnu 0146 CP ROQ, VBIF QoS, UCHE_CLIENT_PF as kgsl | match kgsl init | bring-up | no isolated A/B | kept | e59cf83 |
| 2026-10-23 | xnu 0147 UI threads prefer A76 (`sweetbig=0` off) | render thread lands on A55s | CPU | no bench recorded; 0166 later saw 55-75% of render CPU on A76s | inconclusive | 9b6dfee |
| 2026-10-23 | xnu 0148 stock clock levels 650/800/825 (`sweetgpufreq=6..8`) | more GPU clock | some | see GPU clock line | kept (levels), no gain | c3ff231 |
| 2026-10-23 | GPU clock 430 / 565 / 650 MHz | GPU-bound | several | 15.5 / 17.4 / 17.8 fps; GPU ms unchanged | dead end | - |
| 2026-10-23 | xnu 0156 per-client pool reuse | stop 10 MB bzero per app switch | memory | 73 hits in 6 switches; no fps change | kept (memory, not fps) | eeca377 |
| 2026-10-23 | xnu 0157 no wait-for-idle between submits (`sweetwfi=1` restores) | WFI serializes submits | few | 15.5 vs 15.4 fps at 430 MHz | kept, no gain | 095806b |
| 2026-10-23 | xnu 0158 non-blocking present | present wait blocks vsync | ~10 | 12.5 fps (vs 17.4); put off 701/600 frames | dead end (off) | c149da1 |
| 2026-10-23 | turnip 0102 `TU_SWEET_PRIM_ORDER=0` | raster-order prim flush is the 0.33 ms/draw | ~5 | no change in fps or GPU ms | dead end | 8d02b68 |
| 2026-10-24 | `TU_AUTOTUNE_ALGO`, `TU_DEBUG=gmem/sysmem` | render mode / tiling cost | few | no change | dead end | - |
| 2026-10-24 | xnu 0159 + turnip 0104 + bridge 0102 (IOSurface aliasing, no shadow copies) | copies are ~8 of 11.5 ms (trace) | ~8 | dr23 frame 9.4 -> 6.4 ms, dr14 7.4 -> 4.4 ms (3 ms); fps 17-19 (barely moved) | kept | 4a2077c e7ec319 0e768b6 |
| 2026-10-24 | `sweetfb_thread=1` present thread | inline present delays vsync | ~10 | ~17 then ~14.7 fps | dead end (off) | - |
| 2026-10-24 | backboardd `CA_FORCE_TIMER=1` (launchd.plist) | CA ignores our vsync (chip-id gate) | pacing | swaps == panel updates, 20-23/s | kept | 7058eca |
| 2026-10-24 | xnu 0161 present on swap (`sweetfb_onswap=1`) | present off the vsync tick | ~10 | ~14 fps | dead end (off) | cad3009 |
| 2026-10-24 | xnu 0163 timeline (`sweettrace=1`) | measurement | 0 | no overhead (22.3 / 21.7 fps) | kept (tool) | 3e72521 |
| 2026-10-24 | `sweetpresentwait=0` | blocking present costs swaps | - | 24.5 swaps/s but 13.7-14.4 fps | dead end | 53068a9 |
| 2026-10-24 | xnu 0164 `sweetpresentslots=4` | more presents in flight | - | 14.8-15.0 fps, still 336-407 put off | dead end | a122fae |
| 2026-10-24 | turnip 0105 relaxed 16-byte pitch (first 0105) | remove last 3 ms of A8 copies | 3 | SMMU fault, DeviceLost | failed (phone/turnip/failed) | 245a190 |
| 2026-10-24 | SWEET_CULL dead-draw culling (bridge) | 3 opaque fills under wallpaper waste fill | 3-10 | 13.2-13.4 vs 13.1-15.3 ms/submit; longest still ~40 ms | dead end (works, no gain) | 4803137 |
| 2026-10-24 | sysmem under swipes | tile overhead makes the 40 ms frames | 10+ | same 13-16 ms/submit, longest 40-45 ms | dead end | e9fd4fa |
| 2026-10-24 | bridge 0104 `METAL2VULKAN_HALF_RELAXED` | half math lost in translation | few | no gain; ir3 already used 16-bit; the stat reads 0 on a6xx | dead end | 07f83f2 |
| 2026-10-24 | xnu 0165 `sweetvsyncahead=8/16.7` | beat CA's vbl threshold | pacing | no effect | dead end | 244f7e7 |
| 2026-10-24 | xnu 0166 `sweetcpusample` | CA is CPU-bound | - | render thread 10-15 ms CPU/frame, 55-75% on A76; CPUs mostly idle | dead end (waiting, not CPU) | fe0675c |
| 2026-10-24 | SWEET_CTL completion stamps | completion path is slow | - | GPU end -> handlers < 1 ms | dead end | 98a324c |
| 2026-10-24 | timeline analysis: CP span overlap/busy (ctlA/B, cs7/8) | long submits include waiting or others' work | - | 0 overlaps; long spans fixed at 38.6 ms p50 / 40.4 p95; GPU 25-37% busy; trace can't split busy vs stall | inconclusive (needs RBBM busy counter / per-pass CP timestamps) | - |
| 2026-10-24 | Reduce Transparency (`EnhancedBackgroundContrastEnabled=1`) | blur passes are the 40 ms frames | 3-10 | CA RT shaders fail in bridge ("vertexFunction is not a MTLFunction"), GPU hang, recovery fails; not measurable | failed (bridge bug; pref removed) | - |
| 2026-10-24 | `sweetddr=1` DDR/NoC top vote (off by default in sunfish-boot.sh) | 40 ms submits are bandwidth-bound | 5+ | 17.3/16.9 fps vs 20.1/20.3; bb p50/p95 5.7/40.0 ms unchanged (mean 13.1 vs ~16.7, more short submits) | dead end (keep sweetddr=0) | - |
- 2026-10-25 epik1 (kernel42, userland 40151712848): wland wlan.conf join to "Epik Wifi" (OpenWrt psk-mixed, FT-PSK, 11r/k/v) on 2.4 GHz ch 1: joined, 4-way OK, nettest HTTPS 200. Settings/wifid join and 5 GHz untested.
- 2026-10-25 epik2: Settings → Wi-Fi join to "Epik Wifi" (wifid PMK path) works at -64 dBm, HTTPS 200; the user's spinner did not reproduce. Weak-signal and 5 GHz untested.
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
- 2026-10-25 'not a MTLFunction' log lines = missing MTLFunction protocol on NBFunction (Metal NSLogs and continues); bridge now declares MTLFunction/MTLLibrary. RT hang = CP stall in a valid pipeline's draw, culprit unknown.
- 2026-10-25 soak565 (kernel50, 1 h app soak): silent reset to fastboot at ~468 s (cycle 4), no panic/GPU lines, GPU pool 410-467 MB. Aborted; 430 MHz run not done.
- 2026-10-25 rst1 (kernel50): second silent reset at 178.7 s during an SSH dd of the userdata tail; empty output; cause unknown.
- 2026-10-25 soak430 (kernel50, sweetgpufreq=4, driven by a leftover driver): silent reset at ~179 s during app cycle 4 → resets happen at 430 MHz too.
- 2026-10-25 rst3 (kernel50): raw read of /dev/rufsroot0 at offset (0xc80ffb-1153)*4 KB (flame's userdata size) → instant silent reset to fastboot, no panic. UFS I/O error path kills the kernel; suspect for the soak resets too.
- 2026-10-25 UFS read-path review: sunfish userdata 0x1B65FFB blocks (rst3 offset was in range); raw-device reads memcpy into user VA (PAN abort) = likely rst3 cause; soak resets show no UFS errors, cause still open.
- 2026-10-25 rsc1 (kernel51, xnu 0175): raw rufsroot0 reads no longer crash; SWEETRSC found at block 28728186, but sub-block raw reads return the block start (xxd reads 30 B) → dump garbled. rst3 confirmed as a panic (~159.8 s).
- 2026-10-25 rescue log (read via toybox cmp -l vs /dev/zero): rst3 = Kernel data abort, esr 0x9600004f, far = toybox xxd user buffer → PAN memcpy bug (fixed 0175). Soak-reset rescues were overwritten.
- 2026-10-25 sk52 (kernel52, 20 min app soak, stopped for teardown): no reset, no GPU hang; 12.9 swaps/s; pool peak 470 MB; Spotlight importer extensions crash-loop (232 CRASH lines).
- 2026-10-26 sk52b: kernel52 1 h appsoak, 210 opens, 0 resets/hangs/panics; pool 198 -> flat ~380-420 MB; 11.0 swaps/s avg.
- 2026-10-26 rtj/rtj2: nbproto bridge installed (0 MTLFunction lines); Reduce Transparency on kernel52 ~4 min incl. home swipes: 0 GPU hangs; RT drops to ~6 swaps/s.
- 2026-10-26 sk430: kernel52 430 MHz 1 h appsoak: 0 resets, 1 real GPU hang at 2296 s, recovery FAILED (me_init TIMEOUT), UI continued on CPU copies; 565 kept as default.
- 2026-10-26 recovery-fail analysis: real hang = no SMMU fault, ring pending; me_init rptr 0 after clean GX cycle; suspect CX-side SMMU/GBIF state; injector can't reproduce (needs wait-forever IB).
- 2026-10-26 st53: kernel53 stall injector + SMMU/GBIF reset, 565 MHz: 5/5 stall hangs recovered in 26 ms; stall state milder than the real hang (ring drained, rbbm 800005).
- 2026-10-26 ctl54: sweetsmmureset=0 control, 3/3 stall hangs still recover in 26 ms -> stall injector too mild; SMMU/GBIF reset unproven. kernel55 (0179 UI watchdog) built, untested.
- 2026-10-26 uw1: kernel55 UI watchdog: no false triggers (idle 90 s, swipes); rung A kills a frozen backboardd 30.0 s after the last swap, UI back; rung B untested.
- 2026-10-26 ss1/ss2: kernel56 SMMU stall injector reproduces the real me_init c00015 failure with reset off; with CB_RESUME reset, recovery succeeds on the 3rd path after 2.6 s.
- 2026-10-26 er1/er2: kernel57 early SMMU resume: injection never fired in 4 min (present 1536 not reached?), untested.
- 2026-10-26 er3: kernel57 (0181 early CB_RESUME) SMMU-stall hang recovered on the first try in 26.3 ms (kernel56: 2.6 s; reset off: FAILED). 1 run.
- 2026-10-27 er4-er7: kernel57 SMMU-stall injector (0xA0000200), 4/4 injected; er4-er6 recovered on the first path in 26.06/26.10/26.24 ms (fsr 40000402), er7 respawned + screen back but recovery lines lost from the log. With er3: 5/5 recovered. Swaps after recovery 6-8/s (pre-hang ~19).
