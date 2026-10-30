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

### 2026-10-30 08:15 ub1: kernel114 (xnu 0238, UI threads on the A76s) RESETS ~28 s after boot
- Boot `CAMERA=1 ... kernel114 sweetse9=2 sweetcampmic=2` (4455 mV): SSH up at 17 s; "SWEET UI: tagged thread 599 com.apple.coreanimation.render-server (0238 class 2, kern.sweet_ui_bigcore 1)" [25.639], "tagged thread 616 com.apple.coreanimation.display.primary (class 1)" [25.737], "SWEET UIWDT: armed, 30 s" [25.815]; last line "[27.901] exec duetexpertd"; no panic text; the phone came back in fastboot. "bigcore first use" never printed.
- Leading suspect: 0238 prints that first-use line with printf() from inside choose_processor() (pset locked, interrupts off): a printf there can deadlock the scheduler. Second suspect: returning a running big core for a real-time CA thread outside the normal RT path.
- Smaller step: kernel115 = 0238 without any printf under the lock (counters + a sysctl instead), and a boot-arg A/B starting with sweetuibig=0 (tag only, no placement change).

### 2026-10-30 08:00 df1: ios-redfin's SWEET_DEADFILL (b0491b0) finds almost nothing to drop here; no fps gain
- tools/sunfish-m2env-bench.sh, 3 rounds each, kernel113: swaps/s base 21.8 / 22.9 / 21.9 (mean 22.2), SWEET_DEADFILL=1 22.1 / 23.6 / 22.4 (22.7), =2 21.7 / 24.2 / 22.2 (22.7); GPU busy % samples overlap (noise). Settings with mode 2 drawn correctly (webcam). No crash, no reset.
- Telemetry (needs /var/local/mtl2-telemetry, every 10 s into /var/local/mtl2.log): "DF mode2 dropped 12 cleared 16 held 720" after ~720 frames (idle + 2 swipes), i.e. CoreAnimation here rarely paints full-target fills that are later overwritten; redfin's 13.7 -> 7.9 ms win does not carry over. Idle commit-to-complete mean 6.1 ms.
- Our cost is elsewhere (research/notes/fps-limit.md: blur/backdrop passes 35-58 ms in swipes, render thread 10-15 ms CPU often on an A55, one frame in flight). Next from the redfin line: xnu 1041 UI threads on the A76s (port in progress). DEADFILL stays in the build, default off; /var/local/mtl2-env left empty.

### 2026-10-30 02:55 auto flip (kernel104): garbled once, clean once; zero-copy parked
- zc6 (sweetfb_zerocopy=2): "auto flip on" at 32.9 s, 357 flips, 0 skipped, but every frame garbled (vertical stripes on the wallpaper, apps unreadable). No reset.
- zc7 (same boot args): auto flip at 34.4 s; lock screen clean (only the B29 lock buttons striped). Registers equal the working manual setup: QS OP_MODE 0x20034401, steps 0x215555/0x215015, SSPP SRC_SIZE 0x09840465, OUT 0x09240438, ystride 0x1200, REQ_PIXELS 0x09840465, SRC0 0x9cb00000; re-entering mode 2 only changed SRC0. zc6's garbage not explained (apps were open there; zc7 only looked at the lock screen).
- Parked (the user's rule: usable before fast): zero-copy gives no fps gain (A/B), manual flip works (zc4), auto flip is flaky. Default stays off. To resume: reproduce zc6 with apps open on kernel104 and dump the regs while garbled; then a heavy-app A/B where the freed GPU time could matter. The fps limit itself is O7.

### 2026-10-30 02:35 Where the ~21 fps comes from (read-only; research/notes/fps-limit.md)
- Facts (5 s "SWEET GPU" lines in the zc5 swipe windows): backboardd GPU per submit 25-30 ms average, longest 55-66 ms, GPU busy only 35-49 %, in both modes. Present blit adds 2.5 ms per present. Our code has no ~20 Hz pacing (60 Hz tick SweetFramebuffer.cpp:104/405-442; SwapWait returns at once outside flip mode).
- Inference: CA renders on a tick (10-15 ms CPU), submits, the GPU takes 25-30 ms (blur/backdrop passes up to 66 ms), one frame in flight, swap on the next tick -> 2-3 ticks per frame = ~21/s. CPU and GPU never overlap.
- Cheapest next checks: flip mode + sweettrace=1 timeline; a no-blur screen bench; pin backboardd's render thread to the A76 cores. The big lever (2 frames in flight) needs QuartzCore RE.

### 2026-10-30 02:20 A/B: flip mode saves the GPU copy but not frames (zc5a-d; BENCH_TIGHT=1 fpsbench 30 s)
| run | mode | fps lines | avg fps | other |
|---|---|---|---|---|
| zc5a | flip (k103) | 8 | 20.2 | ~21 flips/s, 0 panel copies, skipped 2 |
| zc5b | blit (k98) | 8 | 20.6 | avg copy ~7200 us (600-13000) |
| zc5c | flip (k103) | 7 | 21.5 | ~25 flips/s, skipped 1 |
| zc5d | blit (k98) | 7 | 22.5 | avg copy ~9200 us (2500-14200) |
- Observation: same ~21 fps in both modes; flip mode removes 7-9 ms of GPU copy per frame. So the ~21 fps limit is upstream (backboardd/CA render or GPU submission), not the panel copy. No reset; still frames sharp in both modes.
- Guess (untested): the GPU time freed may matter under app load (render + copy share the GPU); a heavy-app bench would show it.

### 2026-10-30 02:00 xnu 0225 auto flip mode built (kernel104 = 103 + 0225; untested)
- `sweetfb_zerocopy=2`: when both slots are owned, flip mode turns on by itself (retry from swap() once per second if the MDP isn't mapped yet); frames are dropped (not blitted over slot0) until then; a freed slot surface leaves flip mode and the next pair re-enters. `=1` unchanged.

### 2026-10-30 01:40 E3c FLIP MODE WORKS (zc3, zc4; kernel103 = 102 + 0224)
- zc3: with sweetfb_zerocopy=1 and mode 0, opening Calculator froze the screen on stripes (backboardd renders into slot0 = the old fb; "5s: 72 swaps, avg copy 16041 us"); `sctl kern.sweet_zc_scan=0` (forced redraw) recovered it. Expected from the slot0 overlap: mode 0 must not be used once slots are owned.
- zc4: slots at 33.9/34.1 s (slot1 id 2, slot0 id 3); `kern.sweet_zc_scan=2` right after -> lock screen, home, Calculator, Settings all normal; 5 taps -> "22,222" live; skipped 0, waits_to 0 throughout. Rates: idle 0-0.2 flips/s, taps ~22/s, Settings scroll load ~27 flips/s (641 in 24 s). One mid-scroll frame showed tearing/ghosting (e2; webcam blur vs slot mixing unknown). `=0` afterwards: clean (redraw worked). No reset, 4439 mV.
- Next: A/B with tools/sunfish-fpsbench.sh (kernel98 vs kernel103 mode 2, same load); enter flip mode automatically when both slots are owned (no mode-0 window); check the tearing (SwapWait/flush latch timing).

### 2026-10-30 01:15 xnu 0224 zero-copy E3c built (kernel103 = 102 + 0224; untested)
- Two slots (slot1 0x9cb00000 first, slot0 0x9c000000 over the old fb) for backboardd's two 1125x2436 surfaces. `kern.sweet_zc_scan=2` = flip mode (EBUSY unless both slots owned): swap() decodes the SwapEnd record (mask rec+0x104, id rec+0x54+4*layer), writes VIG0 SRC0 + CTL0 flush and skips present; frames without a slot are dropped (logged). SwapWait polls the flush bits (1 ms, cap 33 ms). `=0` reverts and forces a redraw. `kern.sweet_zc_stats` string.
- Open risks: with presents stopped, swap-complete/vsync pacing may throttle CA; leaving flip mode may show a wrong frame (slot0 = fb).

### 2026-10-30 00:55 E3b WORKS: VIG0 scans backboardd's own surface via the scaler (zc2, kernel102)
- `sweetfb_zerocopy=1`: "sweet zc: slot1 surface id 2 1125x2436 bpr 4608 size 0xab8000" at 32.8 s (backboardd's 1st display surface; the 2nd stays in normal memory).
- `sctl kern.sweet_zc_scan=1` -> "zc scan 1 src 0x9cb00000 bpr 4608": Calculator unchanged, sharp, full screen, not frozen; 10 s later same; a `T` tap changed "5" to "2" live while scanning. `=0` -> "src 0x9c000000 bpr 4352", normal. No reset, no underflow (full 1125x2436 downscale fetch is fine). 4455 mV.
- Caveat: only one of backboardd's two buffers is in the slot, so while scanning, every other frame may be stale; not visible by eye on the webcam.
- Next E3c: both display surfaces in slots (slot0 at carveout offset 0 over the old fb), swap() flips SRC0_ADDR to the presented surface, present() skipped while zero-copy is on; then SwapWait + fpsbench.

### 2026-10-30 00:40 zc1: no slot surface (kernel101); slot limit raised (kernel102)
- zc1 (kernel101, sweetfb_zerocopy=1): boot normal, no reset. backboardd creates two 1125x2436 BGRA surfaces at 32.5/32.7 s with bpr 4608, planes 0, size 0xab8000 (> the 0xA90000 slot limit), so no "slot1 surface" line; sweet_zc_scan stayed 0, step 4 not run. SpringBoard also makes one 1125x2436 at 43.9 s.
- 0223 revised: SWEET_ZC_SLOT_LEN 0xB00000 (slot 0x9cb00000..0x9d600000, inside the 0x1700000 carveout). kernel102 built (53 s).

### 2026-10-30 00:10 xnu 0223 zero-copy E3a/E3b built (kernel101 = 98 + 0222 + 0223; untested)
- E3a (boot-arg `sweetfb_zerocopy=1`): backboardd's first 1125x2436 BGRA surface (allocSize ≤ 0xA90000) is backed by the carveout slot PA 0x9cb00000 (withPhysicalAddress, write-combined, bzero_phys); logs "sweet zc: create ..." for every create with width ≥ 1000 (cap 20) and "sweet zc: slot1 surface id ...".
- E3b: `kern.sweet_zc_scan` 1 = VIG0 scans the slot with the full 1125x2436 -> 1080x2340 downscale (steps 0x215555/0x215015), 0 = ABL values. EPERM without the boot-arg, EBUSY without a slot surface. Registered late (~10 s), skipped with sweetmdpdump=0.
- Risk: the full-size fetch can underflow the MDP (reset). Design: research/notes/zero-copy-e3.md.

### 2026-10-29 23:00 E1 WORKS: QSEED3LITE scales VIG0 (e1a, e1b; kernel98, pmem from userland)
- Recipe from the Pixel 4/4a msm-4.14 SDE source: research/notes/qseed3lite.md; scripts research/notes/e1-on.sh / e1-off.sh (pmem writes, one CTL0 flush 0x20001).
- e1a read-only dump (ABL state): QS+0 = 0x2004 is the scaler VERSION (not "unused"); QS OP_MODE 0, steps 0x200000, preload 0x03040304; SSPP SRC_SIZE/OUT_SIZE 0x09240438, SRC0 0x9c000000, ystride 0x1100, fmt 0x237ff; REQ_PIXELS (+0x108/118/128) 0x09240438; CTL0 flush 0.
- e1b: crop 1037x2246 at (21,47), steps 0x1eb9d6/0x1eb6eb, OP_MODE written 0xa0034401 (reads 0x20034401: bit 31 not kept) -> image zoomed ~4%, edges cut, no garbage/stripes, stable 13 s; e1-off -> baseline. No reset, no MDP/underrun log lines.
- So a 1125x2436 surface can be scanned and scaled to the panel by hardware. Next: E3 (display surfaces in 2 carveout slots below 4 GB, swap() flips SRC0_ADDR, SwapWait), first the full-size 1125x2436 downscale check (more fetch per line: underflow risk).

### 2026-10-29 16:00 E2: no contiguous 9.7 MB below 4 GB (e2b, kernel100 = 98 + revised 0222)
- The 15:35 panic was `ml_io_map` of 10 MB (io_map submap is 8 MB, KMA_NOFAIL), not the contiguous allocation. Revised 0222: early non-panicking kmem_alloc_contig, copy via ml_phys_read.
- e2b: `e2 alloc below 4 GB failed (3)`, `e2 alloc any PA ok, PA 0x1101dc000`; write 2 -> ERANGE (VIG0 SRC0_ADDR is 32-bit), VIG0 untouched; no hang. So display buffers must be below 4 GB: either reserved at boot (shim/ADT reserved-memory like the splash carveout) or carved from the splash carveout (24.1 MB: 2 x 1125x2436 frames). E2's DRAM-outside-carveout question stays open (only answerable with a buffer < 4 GB).

### 2026-10-29 15:35 RESET: E2 allocation panics (e2a, kernel99 = 98 + xnu 0222)
- `sctl kern.sweet_mdp_e2=1` -> `panic(...): kmem_alloc(..., 10190848, 0x100021): failed with 3 @vm_kern.c:178` (kmem_alloc_contig of 9.7 MB below 4 GB with KMA_PERMANENT failed; the call panics instead of returning). No e2 log lines; phone to fastboot; rescued on kernel98 (e2r, ssh 18 s, 4439 mV). sctl 64-bit retry (11522a2) worked up to the write.
- No verdict on E2. Next: allocate the contiguous buffer early (at registration, when memory is not fragmented), with a non-panicking call, or reserve it at boot; fail with ENOMEM.

### 2026-10-29 15:15 zero-copy design (research/notes/zero-copy-design.md)
- Chosen: backboardd keeps 1125x2436; its display IOSurfaces allocated physically contiguous (SweetSurface.cpp:759, fallback normal); swap() (SweetFramebuffer.cpp:868) points VIG0 at the swapped surface (SRC_SIZE 1125x2436, OUT 1080x2340, QSEED3LITE scaling) + CTL0 flush; real SwapWait polls the flush bits (1 ms, 33 ms cap). Rejected: 1080x2340 canvas (360x780 pt: red rdar status bar, would need an iPhone 13 mini identity), carveout slots (2 x 11.1 MB fill the 24.1 MB carveout). Rounded-corner mattes lost at first.
- Experiments: E2 (MDP reads contiguous memory outside the carveout? SMMU stream 0x800) first, E1 (QSEED3LITE scale test), E3 (flip + SwapWait behind sweetfb_zerocopy, fpsbench).

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
