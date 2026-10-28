# Metal: archived log (entries before 2026-10-25)
Current file: [features/metal.md](../../features/metal.md). Same format, newest first.

## Log

### 2026-10-24 22:30 GPU busy vs wait, Reduce Transparency, DDR vote (kernel42, sweetpresentwait=1 sweettrace=1)
- See [smoothness](smoothness.md) for this entry; it also touches this feature.

### 2026-10-24 20:00 Half precision: the "Half-registers_used=0" premise was wrong
- 0104 diagnostics: half_relaxed()=true; backboardd translated=50 round_skipped=389 marked=993 decorated=990; SPIR-V dump (METAL2VULKAN_SPV_DUMP) shows 6-66 RelaxedPrecision decorations per fragment module. Turnip 85fda3b sets mediump_16bit_alu.
- ir3 disassembly (IR3_SHADER_DEBUG=disasm, MESA_SHADER_CACHE_DISABLE, caches bypassed): with 0104 on, 880 f2fmp / 503 f2f16 / 810 half-register operands; with it off, 498 f2f16 / 4088 half-register operands (bigger log). So ir3 already runs much of CA's half math in 16 bit, and the PIPESTAT "Half-registers_used" statistic reads 0 on a6xx (merged register file), which misled the plan.
- 0104's real effect is the skipped per-op rounding (FS p50 57 -> 43-68 depending on the pipeline mix); benches showed no gain (on 18.9 / 19.7 swaps/s, 13.0 / 13.5 ms per submit; off 20.3 / 19.7, 13.3 / 13.2 ms). Leave 0104 off (default).
- Phone reset: mtl2-env empty, no mtl2-tu-debug. backboardd caches moved aside on the phone: /var/local/m2v-cache/backboardd.bak (original) and backboardd.half (half on); the live one was rebuilt.

### 2026-10-24 18:40 Bridge 0103 SWEET_PIPESTATS (VK_KHR_pipeline_executable_properties) works
- First build logged nothing (cause not found by reading); the version that writes straight to mtl2.log works: device enabled, properties Ok(3), statistics Ok(28) per pipeline.
- 118 fragment pipelines after a restart + swipes: FS instruction count min 0, p50 57, p90 211, max 314 (the solid fill is 5 instructions; downsample_8_frag_lph blur 211-282, 16-19 registers, 10-12 waves).
- Half-registers_used=0 in every pipeline, although CA's shaders are the "_lph" (low-precision half) variants: the bridge's translation (metal2vulkan) or Turnip turns all half math into 32-bit. A6xx runs half at 2x ALU rate with half the registers.
- Reading: the solid fill is not shader-bound (5 instr) -> fill/ROP rate for those; the textured/blur layers are ALU-bound at full precision. Next: find where half precision is lost (AIR -> SPIR-V: Float16 or RelaxedPrecision; ir3 lowers mediump only for RelaxedPrecision) and keep it.
- Labels are missing from the PIPESTAT lines (state N shows only function names).
- [19:10] Bridge 0104 METAL2VULKAN_HALF_RELAXED (half values as RelaxedPrecision, skip per-op half rounding; metal2 run 40139944623). Env reaches backboardd, but no effect: 59 FS pipelines, Half-registers_used=0, instruction counts unchanged (p50 57, p90 212). Benches: on 18.9 / 19.7 swaps/s (13.0 / 13.5 ms per submit), off 20.3 / 19.7 (13.3 / 13.2 ms). No faults, screen fine. Being debugged (diagnostics added to 0104). Note: toybox has no killall; restart backboardd with psx + kill as sunfish-m2env-bench.sh does.
- [19:18] Demo boot for the user: kernel40, sweetpresentwait=1 flamereboot=0, mtl2-env cleared (all bridge diagnostics off). 0104 diagnostic test was stopped before it ran; resume it next session (scratchpad half-diag.sh logic: build is metal2 latest, set SWEET_PIPESTATS=1 + METAL2VULKAN_HALF_RELAXED=1, grep 'half-relaxed').

### 2026-10-24 17:00-18:00 SWEET_CULL (bridge dead-draw culling), metal2 runs up to 40134633213 + debug
- Rounds: (1) only single-colour-target passes: CA's big pass has two (c0 + c1 1125x2436, c1 load DontCare) so nothing deferred; (2) required covering draws to overwrite all targets: CA pipelines write only c0 (opaque 1, att 3); (3) per-target rule (earlier draw dead if its targets are all overwritten); (4) scissor: CA redraws damage regions (e.g. sc 0,192,1120,2208), so covering is now per clip rectangle, and a draw is only culled if no surviving draw in between overlaps it.
- Final build works: replay dump shows the three PBGRAXm_A2Xhf fills (d6/d8/d10) culled under the wallpaper quad; no GPU faults; home screen correct on the webcam.
- But it does not help: 108 draws culled in 2700 passes (the fill-under-wallpaper pattern is ~1 pass in 25); swipe bench 19.7 / 19.7 swaps/s (19.0 panel updates/s) vs 18.6-22.8 without; backboardd GPU per submit 13.2-13.4 ms with culling vs 13.1-15.3 without; longest submit still ~40.7 ms. The per-draw trace (pd2) overstated these fills (per-bin timestamps add overhead).
- Left SWEET_CULL=1 in /var/local/mtl2-env for now (harmless, off by default in code). Debug lines (first-pass, not-covering reasons, replay dump) still in the build.
- Ops note: each CI bundle download is 278 MB; /tmp hit its quota after ~12 (files are read-only: chmod -R u+w before rm).
- [18:00] TU_DEBUG=sysmem under swipes (sys1/sys2): 15.9 / 13.3 ms per backboardd submit, longest 40-45 ms, 18.9 swaps/s: same as GMEM, so tile overhead is not the cause.
- Reading: a full-damage redraw stacks ~10+ full-screen layers (~40 Mpx) and CA's uber shader shades at ~1 Gpx/s, which is ~40 ms. The lever is shader cost. IR3_SHADER_DEBUG=disasm + MESA_SHADER_CACHE_DISABLE only dumped Turnip's internal blit shaders (CA pipelines come from the m2v cache; disasm goes to stderr). Next: SWEET_PIPESTATS (VK_KHR_pipeline_executable_properties per CA pipeline label), being written.

### 2026-10-24 15:40 Bridge drawlog (metal2 run 40123962779, commit 0472fb8, SWEET_DRAWLOG)
- dl1 bench with drawlog on: 23.4 swaps/s, 22.6 panel updates/s. After reset, dl1check gave 9.6 / 9.2 (not explained yet; re-check on a fresh boot).
- 20 big passes logged (../logs/dl1-drawlog.txt). The slow 17-draw pass: draws 0-2 = PBGRAXm_A2Xhf, opaque solid fill, three identical full-width quads; draw 3 = PBGRAXm_TimgA2Xhfn_Isrc, opaque wallpaper quad at x -185..1310 that covers the whole target. So draws 0-2 are dead (~10 of 26 traced ms). Draws 15/16 (fs 780adfdf) use programmable blend (Bdso/Bvcm, read the destination), so they can't be culled and don't cover anything.
- Every CA draw uses one uber shader (fixed_vert_lph_spc / fixed_frag_lph_cph) specialised per pipeline; a solid fill costs ~0.08 ms per 288x224 bin (~0.8 Gpx/s), so the shaders are heavy in general. Possible later lever.
- Next: SWEET_CULL in the frontend (defer the encoder's calls, skip draws under a whitelisted opaque full-target quad).

### 2026-10-24 12:30 Step 2: Turnip GPU trace under swipes (gt1, MESA_GPU_TRACES=print; kernel already running, 4184 mV charging)
- See [smoothness](smoothness.md) for this entry; it also touches this feature.

### 2026-10-24 12:10 turnip 0105 per-draw trace (metal2 run 40109072496, commit f09fdb5)
- pd1 bench (TU_SWEET_DRAW_TRACE=1 + MESA_GPU_TRACES=print): 17.6 swaps/s, 16.4 panel updates/s (tracing overhead). 4300 mV.
- Phone dropped to fastboot right after the bench, at kernel uptime ~1890 s: matches the 1800 s auto-reboot cycle, not a hang (fastboot answered, 4384 mV). Trace was not yet copied; rebooting with flamereboot=0 to fetch it.
- pd2 (per-draw trace, phone rebooted with flamereboot=0; bench 3.8 swaps/s with this tracing, so absolute times are inflated, shares are what count). 154 passes, 17,786 draw records.
- Slowest passes (~40 ms wall, ~26.5 ms of draw time, 45 bins): every expensive draw is a 6-vertex quad (full-screen layer), instances=1, blend0=0 (opaque).
  - Pass 126 top draws: idx16 fs 780adfdf 6.45 ms (16%); idx3 fs 666966cb 4.3 ms; idx0/1/2 all fs f8357eea, 3.2-3.7 ms each: the same opaque full-screen shader three times in a row, each later one covering the earlier.
  - Over all passes >20 ms: fs f8357eea 94.9 ms (1305 records), 780adfdf 58.2 ms (0.144 ms per bin-draw, 2x the rest), 93ffe710 26.4, 666966cb 17.1.
  - ~14 ms per slow pass is outside the draws (per-bin overhead/timestamp waits).
- Reading: overdraw of stacked opaque full-screen layers plus one heavier shader (780adfdf, blur candidate). Next: disassemble those fragment shaders to see what they sample, and check why CA stacks three opaque full-screen quads.
- [12:45] Stopped for the day (user unplugging). State: phone booted as pd1b (kernel40, sweetpresentwait=1 flamereboot=0, so no auto-reboot), mtl2-env empty (tracing off), turnip 0105 bundle installed (inert without TU_SWEET_DRAW_TRACE). Traces in ../logs/pd2-trace.log, gt1-trace.log; parsers in the session scratchpad (lost on reboot of the PC). Next: shader disassembly of fs f8357eea / 780adfdf.

### 2026-10-24 09:50 Step 1A moot, step 2 (turnip 0105) failed
- Plan step 1 option A: `vsyncFired` already sends the client notifications before `present()` and re-arms the timer first, so no 0162. The tick still slips because present blocks the timer thread (present thread tested worse earlier).
- Step 2, turnip 0105 (16-byte pitch for sampled-only explicit-layout images), kernel37 + `sweetpresentwait=1`, BENCH_TIGHT 30 s:
| config | swaps/s | panel updates/s |
|---|---|---|
| baseline (0104 bundle) | 24.7 / 24.5 | 23.6 / 23.1 |
| 0105, TRANSFER_DST excluded (no effect, still refused 34x) | 24.4 / 24.4 | 23.1 / 23.1 |
| 0105, TRANSFER_DST allowed | 25.0 / 25.9 | 23.9 / 24.8 (INVALID: device lost) |
| bundle rebuilt without 0105 | 21.8 / 22.5 | 21.1 / 21.2 |
- With 0105 allowed: refusals dropped to 0, then within ~2 s of the backboardd restart the GPU hit an SMMU translation fault (fsr 2, far 0x100e634240, fence 19680 vs 19673), reset, and every Metal call returned DeviceLost; screen black. Reinstalling the bundle without 0105 recovered without a reboot.
- Patch parked in `phone/turnip/failed/`. Baseline drifts ~2/s between sessions (bench noise ±3).

### 2026-10-24 00:40 Night run: where backboardd's GPU time goes
- See [smoothness](smoothness.md) for this entry; it also touches this feature.

### 2026-10-24 experiments.md lines
| date | patch / switch | hypothesis | predicted ms saved | measured fps and GPU ms | verdict | commit |
|---|---|---|---|---|---|---|
| 2026-10-24 | xnu 0159 + turnip 0104 + bridge 0102 (IOSurface aliasing, no shadow copies) | copies are ~8 of 11.5 ms (trace) | ~8 | dr23 frame 9.4 -> 6.4 ms, dr14 7.4 -> 4.4 ms (3 ms); fps 17-19 (barely moved) | kept | 4a2077c e7ec319 0e768b6 |
| 2026-10-24 | turnip 0105 relaxed 16-byte pitch (first 0105) | remove last 3 ms of A8 copies | 3 | SMMU fault, DeviceLost | failed (phone/turnip/failed) | 245a190 |
| 2026-10-24 | SWEET_CULL dead-draw culling (bridge) | 3 opaque fills under wallpaper waste fill | 3-10 | 13.2-13.4 vs 13.1-15.3 ms/submit; longest still ~40 ms | dead end (works, no gain) | 4803137 |
| 2026-10-24 | bridge 0104 `METAL2VULKAN_HALF_RELAXED` | half math lost in translation | few | no gain; ir3 already used 16-bit; the stat reads 0 on a6xx | dead end | 07f83f2 |
| 2026-10-24 | Reduce Transparency (`EnhancedBackgroundContrastEnabled=1`) | blur passes are the 40 ms frames | 3-10 | CA RT shaders fail in bridge ("vertexFunction is not a MTLFunction"), GPU hang, recovery fails; not measurable | failed (bridge bug; pref removed) | - |

### 2026-10-23 21:10 Wi-Fi — **ON THE INTERNET**
- See [wifi](wifi.md) for this entry; it also touches this feature.

### 2026-10-23 11:55 Metal on the GPU — **PASS: backboardd renders through Metal2 + Turnip on the A618**
- New CI workflow `metal2.yml` (macos-14): Mesa 85fda3b3 + UniXNU mesa patches (pinned e95b37d9) + our turnip
  patch → Turnip; metal2vulkan 720ef1ce + bridge patches → SweetMetal2.bundle; also vktest/v2test.
  Install: `tools/sunfish-metal2-install.sh DIR`. Boot: `sweetmtl=2 sweetgpudelay=0`. Cache dir mode 1777.
- 0145: Turnip device info for the A618 (chip 0x06010800 → "Adreno 618", GMEM 512 KB, hbb 14) + hw_init GMEM/UBWC.
- 0146: hw_init as kgsl for the A618: **CP ROQ thresholds 0x010000c0** (A640 values hung the CP in Turnip's IBs →
  Vulkan DeviceLost → black screen), VBIF QoS reg 0x10, UCHE_CLIENT_PF 1.
- Result (k35): home screen correct (logs/shots/mtl-k35.png), no GPU hangs, swipe **22.8 fps** (peaks 27.4;
  CPU path 17.2, GPU-composite-only 16.4). backboardd CPU 49 s → 22 s per bench. GPU 33–53% busy at 267 MHz.
- (Recovered from `git show 1808cf9^:docs/sunfish/workday.md`; that commit deleted it.)

### 2026-10-23 10:20 GPU — **PASS: Adreno 618 composites the UI** (0140 + 0141)
- See [gpu](gpu.md) for this entry; it also touches this feature.

### 2026-10-23 experiments.md lines
| date | patch / switch | hypothesis | predicted ms saved | measured fps and GPU ms | verdict | commit |
|---|---|---|---|---|---|---|
| 2026-10-23 | turnip 0102 `TU_SWEET_PRIM_ORDER=0` | raster-order prim flush is the 0.33 ms/draw | ~5 | no change in fps or GPU ms | dead end | 8d02b68 |
