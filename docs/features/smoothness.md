# Smoothness
**State:** 🟡 ~21 screen updates/s ceiling while swiping; present blit waits behind backboardd. **Updated:** 2026-10-27.

## How to use
- Bench: `tools/sunfish-fpsbench.sh TAG SECONDS` (BENCH_TIGHT=1); config matrix `tools/sunfish-cfgbench.sh`; timeline `sweettrace=1` + `tools/sunfish-timeline.py`. Details in [methods.md](../methods.md).
- backboardd needs `CA_FORCE_TIMER=1` in `/System/Library/xpc/launchd.plist` (backup `/var/local/launchd.plist.pre-vbl`).
- Experiments rule (experiments.md): fps = panel updates/s from `tools/sunfish-fpsbench.sh` (Settings swipes) unless noted; GPU ms = backboardd GPU ms per frame/submit. Stop rule: a gain under 1 ms GPU (or inside the ±3 updates/s bench noise) is a dead end.

## Facts
### Frame path
1. **The app or SpringBoard draws its layers.** These are IOSurfaces from SweetSurface; Metal goes through the
   SweetMetal2 bundle (metal2vulkan + Turnip).
2. **backboardd (CoreAnimation's render server) composites one render pass per frame**, 14–46 draws at
   1125x2436, into a 1125x2436 IOSurface.
3. **It swaps through IOMFB SwapEnd (sel 5), which records only the frame**
   (`iokit/Kernel/arm/SweetFramebuffer.cpp`).
4. **The vsync tick (a 60 Hz software timer) presents it**: a GPU blit that scales the frame into the ABL panel
   buffer, scanned out by the MDP.
5. **The vsync tick also notifies clients (type 5).**

#### 1. IOSurface shadow copies (fixed)
- **Symptom:**
  - backboardd's GPU time is linear in its draw count: ~3 ms + 0.33 ms per draw.
  - Neither the GPU clock nor the render mode changes it: GMEM, sysmem and `TU_SWEET_PRIM_ORDER=0` give the same
    numbers.
- **How it was found:** `MESA_GPU_TRACES=print` showed `copy_buffer_to_image` taking ~6.6 ms and
  `copy_image_to_buffer` ~1.4 ms per frame, against a 3.5 ms render pass.
- **Causes:**
  - Turnip reported `requiresDedicatedAllocation` for every external image, so the bridge never aliased an
    IOSurface. Fixed by `phone/turnip/patches/0104`.
  - Turnip rounds linear image rows up to a multiple of 4, so textures whose height isn't a multiple of 4
    were refused. Fixed by `phone/metal2/bridge/patches/0102` (read-only textures may alias past their last row).
  - Core Animation asks for 16/32-byte row pitches, while Turnip needs 64 for explicit DRM-linear layouts.
    xnu 0159 rounds single-plane IOSurface rows to 64 (`sweetsurfalign`).
- **Result:** a dr23 frame went from 9.4 to 6.4 ms of GPU time.
- **Left over:** about 3 ms/frame of small A8/RGBA8 buffer textures with 32-byte pitch. These are not
  IOSurfaces, and Turnip refuses them (turnip 0103 logs each refusal).

#### 2. CA never used our vsync (fixed)
- **The gate:** `CA::WindowServer::IOMFBServer::set_vsync_enabled` subscribes to IOMFB vsync (sel 72, type 5) only
  when `CADeviceUseVBL()` is true. Otherwise it runs its own CADisplayTimer.
- **The check:** `CADeviceUseVBL` = `getenv("CA_FORCE_TIMER")` is non-zero, or MobileGestalt ChipID is in
  {0x7000, 0x7001, 0x8000–0x8004, 0x8010}. Our synthetic chip-id is 0x8015, which is not in the list.
- **Fix:** backboardd `EnvironmentVariables` gets `CA_FORCE_TIMER=1` in `/System/Library/xpc/launchd.plist`
  (the original is backed up at `/var/local/launchd.plist.pre-vbl`). Setting it from `/var/local/mtl2-env` is
  too late, because CA reads it in a `dispatch_once` before the Metal plugin loads.
- **Result:** 20–23 swaps/s with swaps equal to panel updates. Before: 17–19 panel updates/s against 19–27 swaps.

#### 3. The present is the next limit (open)
- The present blit takes 10–17 ms of wall time but only ~2.4 ms of GPU work. It waits on the single ring
  behind backboardd's next job, and because it runs inline on the vsync tick, the vsync notification goes out
  late too.
- **Tried and worse; keep these off:**
  - present thread (`sweetfb_thread=1`): ~14.7 panel updates/s
  - present-on-swap (xnu 0161, `sweetfb_onswap=1`): ~14 panel updates/s
  - non-blocking present (0158): put frames off
  - DDR vote while presenting
- **Ideas:**
  - notify before presenting
  - a second, higher-priority ringbuffer for the present
  - scanout without the blit (risky: MDP registers)

#### 4. Other facts
- **CPU and GPU are both partly idle:** the GPU is 35–60% busy and backboardd is idle ~45% of the time
  (pstack samples). The limit is pacing, not throughput.
- **Most backboardd CPU time is in QuartzCore;** the bridge and Turnip are small.
- **Idle redraws:** 10 full redraws/s when idle come from the Clock icon's second hand. This is normal.
- **Clock and render mode don't matter:** no gain from GPU 430/565/650 MHz, `TU_AUTOTUNE_ALGO`, `TU_DEBUG=gmem/sysmem`,
  or 0157 (no wait-for-idle).
- **Battery drains on the charger:** about -0.5 A while benching with the screen on. The kernel never configures
  the charger.

### Patches from this work
| Patch | What it does |
|---|---|
| xnu 0159 | 64-byte IOSurface rows |
| xnu 0160 | Log IOMFB async calls |
| xnu 0161 | Present on swap (off by default) |
| turnip 0102 | Prim-order switch |
| turnip 0103 | Log refused explicit layouts |
| turnip 0104 | Host-pointer images don't need a dedicated allocation |
| bridge 0102 | Read-only textures may alias past their last row |

Kernels:
- **kernel35:** up to 0159
- **kernel36:** up to 0160
- **kernel37:** up to 0161. **Best: kernel37 with `sweetpresentwait=1`.**

## Open
- The present is the next limit (see Facts, finding 3).
- Perf: return to the ~21 updates/s ceiling with the soak as the regression gate (plan "Later").
- About 3 ms/frame of small A8/RGBA8 buffer textures with 32-byte pitch remain (see metal).

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

### 2026-10-29 15:00 zero-copy step A: VIG0 source switch works (zc1, kernel98 = 97 + xnu 0221)
- 0221: `kern.sweet_mdp_src` (quad RW): write = VIG0 SRC0_ADDR (validated inside the splash carveout) + CTL0 flush 0x20001 (VIG0 | CTL bit 17, as ios-s9). sctl now writes hex / 64-bit values.
- zc1: src 0x9c000000 -> 0x9c06a400 (+100 rows): picture moved toward the top, garbage strip at the bottom (webcam zc1-b); back to 0x9c000000: identical to the baseline (zc1-c). Log: `VIG0 src 0x9c06a400 (flush 0x20001)`, `after 20 ms ... flush 0` (latched within 20 ms). No reset; ssh 17 s; 4438 mV.
- Next: the design for removing the blit: either backboardd renders 1080x2340 directly into carveout slots (canvas change) or 1125x2436 slots + QSEED3LITE scaling on VIG0; a real SwapWait.

### 2026-10-29 14:40 MDP dump read (md2, kernel97 = 95 + revised 0220; sctl buffer 256 B -> 16 KB)
- 0220 revised: nothing mapped at registration; the first read maps 11 single 4 KB MDSS pages (never 0x0ae44000, which SweetFramebuffer notes hangs the bus). kernel97: 90 s untouched, no hang; reads work. (kernel96's hang: whole-range mapping covering 0x44000 is the best guess, not proven.) `SWEET USB: busERR ... SMMU cb fsr 400` repeats every ~5.7 s on kernel95 too (12 per boot): unrelated.
- Layout ABL leaves: only VIG0 active: src_size = out_size 0x09240438 (1080x2340, no scaling), addr0 0x9c000000, ystride0 0x1100 (4352 = 1088 px x 4 B), fmt 0x000237ff, op 0x80000000; qseed words 00002004 00000000 0013b736 ... (scaler not used). CTL0 layer0 0x01000005 (VIG0 -> LM0), top 0x00020000, flush 0. LM0 out 0x09240438. INTF1 autorefresh 0x80000001. VIG1/DMA0-2 off.
- So zero-copy = VIG0 SRC0_ADDR + CTL0 flush. Baby step A next: a write sysctl, point VIG0 at 0x9c000000 + 100 rows (inside the splash carveout), webcam, restore.

### 2026-10-29 14:15 RESET: kernel96 (xnu 0220 kern.sweet_mdpdump) hangs ~60 s after boot (md1)
- kernel96 = kernel95 + 0220 (read-only MDP/SMMU dump sysctl, maps MDSS 0x0ae00000+0x84208 and apps SMMU 0x15000000+1 MB at registration, 10 s after SweetBattery start). "SWEET MDP: kern.sweet_mdpdump ready" at 30.0 s; the dump was never read. Webcam: black with white vertical streaks; log ends at 59.9 s with `SWEET USB: busERR addr 0; SMMU cb fsr 400 far 000144d675929c97 fsynr0 220c2d fsynr1 74`; phone in fastboot (4455 -> 4438 mV). ssh up 21 s before that.
- Cause unknown (the sysctl was not read; mapping alone should not touch hardware). Smaller step: map lazily on the first read, MDSS only (SMMU part dropped). kernel95 stays the default.

### 2026-10-29 13:45 zero-copy plan (research/notes/zero-copy-plan.md)
- SM7150 MDSS (0x0ae00000) SSPP/CTL/LM offsets match SDM845 (ios-s9 0096 carries over); scaler is QSEED3LITE (VIG+0xa00), not QSEED3. MDP stream 0x800/0x440 on the apps SMMU; splash carveout 0x9c000000+0x1700000. Our surfaces are page-granular (not contiguous).
- Steps: 0 read-only `kern.sweet_mdpdump` sysctl (pipes, CTL, LM, INTF, PP, SMMU S2CR for 0x800); 1 contiguous slots if the stream is in bypass; 2 VIG0 src = a copy of the framebuffer (webcam); 3 1125x2436 slot + scaler; 4 flip = SRC0_ADDR + CTL flush; 5 real SwapWait. Estimate 21 -> 40-60 swaps/s (guess).

### 2026-10-29 11:25 ios-s9 (Galaxy S9 port, MIT) display work vs ours (research/notes/ios-s9-display.md)
- Already ours: panel autorefresh on every TE (0x80000001), CPU frequencies, UI threads on big cores.
- Not ours: zero-copy scanout (their 0096/0097): an MDP VIG pipe reads backboardd's surface directly with the QSEED3 scaler, CTL flush latches at frame start, SwapWait (sel 6) returns once the swap is on screen. Ours: a GPU blit into the bootloader framebuffer (0x9c000000) per vsync, ~2.4 ms GPU but 10-17 ms wall queued behind backboardd on the single ring (= O2); our SwapWait is a no-op (SweetFramebuffer.cpp:1303).
- Plan: read the MDP pipe layout ABL leaves (read-only), then point a pipe at backboardd's 1125x2436 surface and scale/crop, latch via CTL, real SwapWait. 1-2 days; MDP writes risk a reset only. Estimate (unmeasured): 35-55 panel updates/s. Second: @2x canvas A/B (small gain, our cost is per draw).

### 2026-10-28 21:55 present mode 2 (xnu 0213, retry a put-off present from a 1 ms timer within 10 ms) is worse
- kernel94, fresh boots, `BENCH_TIGHT=1 sunfish-fpsbench.sh TAG 30`: sweetpresentwait=1: pw1a 23.0 swaps/s, 21.9 panel/s; pw1b 22.5 / 21.1. sweetpresentwait=2: pw2a 22.7 / 16.0; pw2b 20.7 / 14.7. Mode 1 (blocking present) stays the default; mode 2 drops ~6 panel updates/s (retries land late or miss the next tick).

### 2026-10-28 15:30 presentwait A/B on kernel83 (3 fresh boots each)
- off: kpw0a/b/c 22.3/16.1, 22.3/14.9, 20.2/14.5 (mean 15.2 panel/s); on: kpw1a/b/c 23.6/21.9, 22.4/21.6, 21.5/20.3 (mean 21.3). Confirms O1.

### 2026-10-28 17:45 the drop explained: sweetpresentwait=1 was not a default
- kernel83 fresh boots: kpw0c (no wait) 657 swaps / 459 panel copies per 30 s (70%), avg copy 1.2-1.9 ms, 383 of 600 ticks put off (sweet_gpu.c:1010-1020: the present's IB slot from two presents back is still queued behind backboardd on the single ring; the put-off frame is retried only on the next tick, and the next swap often replaces it). kpw1c (wait) 538 / 508 (94%), copy 5.3-15.6 ms wall vs ~2.5 ms GPU, 0 put off: ~20.3 vs ~15.3 panel updates/s. backboardd jobs 13-18 ms GPU each, longest 41 ms, GPU 33-54% busy: FIFO latency, not throughput. sweetpresentwait=1 is now a default.

### 2026-10-28 15:45 /var on root vs ufsroot1: the same; sweetpresentwait is not a default
- kernel68: /var on the root (sweetnofstab=1) knfa/b/c 21.8/14.6, 21.0/14.4, 20.7/14.4; /var on ufsroot1 kfsa/b 22.7/16.4, 21.6/15.0. Not the cause.
- So far ruled out: the Linux bundle build, kernel68 vs 81, dim 10 vs 50, /var location. But tools/sunfish-boot.sh's default args do NOT include `sweetpresentwait=1`, which the best measured config used ("kernel37 + sweetpresentwait=1"; the old status.md boot line too). Today's benches all ran without it. A/B on kernel83 next.

### 2026-10-28 15:20 kernel68 vs kernel81, fresh boots: the same
- kab68a/b/c 21.8/14.6, 22.3/14.3, 22.2/15.7; kab81a/b 21.7/14.8, 22.0/14.3 (swaps/s, panel/s). No kernel patch caused the drop. kernel68 is also when /var moved to the ufsroot1 HFS volume (fstab): next, kernel68 with sweetnofstab=1 (/var back on the root).

### 2026-10-28 15:20 dim 10% vs 50% on one boot (k81r): no difference
- d10a 22.4 / 14.6, d50a 19.3 / 13.9, d10b 22.2 / 14.0, d50b 20.8 / 13.9 (swaps/s / panel updates/s). The LM_0 alpha is not the cause. Next: kernel68 vs kernel81 fresh boots.

### 2026-10-28 14:50 CI vs local bundle, fresh boots on kernel79: no difference
| tag | bundle | swaps/s | panel/s |
|---|---|---|---|
| abci1 / abci2 / abci3 | CI (work/metal2-0106) | 20.7 / 19.8 / 22.4 | 14.6 / 12.4 / 14.4 |
| ablo1 / ablo2 / ablo3 | local (Linux build) | 19.9 / 21.5 / 21.9 | 13.8 / 15.5 / 15.6 |
- Means: CI 13.8, local 15.0 panel updates/s. The Linux build is not the cause. Both are well below the 21-24 of 10-24..10-27. Next suspect: those benches ran at full brightness; since the overnight run the dim tool keeps LM_0's constant alpha at 10% (a blended mixer stage). A/B dim 10 vs 50 on one boot next; then kernel68 vs kernel79.

### 2026-10-28 12:00 fresh-boot panel updates 15.6-16.2/s (sk70p) vs 21-24 before
- Build comparison (read-only): the Linux build (tools/build-metal2-linux.sh) and CI use the same flags: meson buildtype release, optimization 3, b_ndebug true; cargo --release; frontend -O2 -g; strip -x. Only the SDK differs (16.5 vs 17.5). Turnip 0106/0107 and xnu 0196 cost ~0 when off.
- The pool cap (bridge 0105) is not it either: on one boot cap 0 gave 17.1, cap 8 18.5, cap 32 18.85.
- Next (after the soak): a fresh boot with CI's bundle work/metal2-0106 vs the local bundle, two benches each; then kernel68 vs kernel70.

### 2026-10-27 experiments.md lines
- (see [gpu](gpu.md), experiments line for 2026-10-27) 2026-10-27 es1: kernel58 (0182 SMMU stall-on-fault by default) injected SMMU-stall hang: early resume fsr 40000402, recovered first path in 26.4 ms, sctlr e7 after reset (stall bit kept), 21.5 swaps/s at +60-120 s with swipes (so er4-er7's 6-8/s was the idle lock screen).

### 2026-10-25 experiments.md lines
- (see [gpu](gpu.md), experiments line for 2026-10-25) 2026-10-25 xnu 0174 (kernel50): Linux-style Fabia PLL0 restore after the DCVS vote locks; core 564 MHz after recovery (was 9-56). GMU DCVS is what zeroes PLL0 L. Post benches 9 swaps/s on a widget panel (not comparable); backboardd 20 vs 13 ms/submit.

Older entries (before 2026-10-25): [archive/features/smoothness.md](../archive/features/smoothness.md).
