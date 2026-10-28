# Smoothness: archived log (entries before 2026-10-25)
Current file: [features/smoothness.md](../../features/smoothness.md). Same format, newest first.

## Log

### 2026-10-24 22:30 GPU busy vs wait, Reduce Transparency, DDR vote (kernel42, sweetpresentwait=1 sweettrace=1)
- **Busy vs waiting (existing timeline, ctlA/ctlB/cs7/cs8):** no other span overlaps any backboardd CP span (0 of ~620 per bench), so the 13-40 ms is that submit's own work, not interleaving. Long submits are tightly clustered: ~11.5 ms (10-20 bucket) and 38.6 ms p50 / 40.4 p95 (>=20 bucket); they queue 61-74 ms (p50) before CP start. GPU busy 25-37%; only 39-90 of ~620 spans start back-to-back. The trace cannot tell busy from stalled inside a span; the near-constant 38.6 ms is suspicious (fixed cost or a wait quantum). Needs RBBM busy perfcounter or a CP timestamp per render pass.
- **Bus vote:** `tools/sunfish-boot.sh` passes `sweetddr=0` by default (set 10-23 when the DDR vote cost panel updates on the old present path), so the GPU trail had no "bus vote" lines. With `sweetddr=1`: "bus vote MC0 top -> 0", "bus vote SH0 top -> 0".
- **Baseline (rtbase1/2):** 21.0 / 21.4 swaps/s, 20.1 / 20.3 panel; backboardd ~16.5 / ~17.0 ms/submit, longest 41.9 / 40.7 ms; timeline <=17 ms 19.8 / 23.8%, <=34 ms 62.5 / 64.3%; bb p50/p95 5.8-5.9 / 39.9 ms; GPU end -> swap p50 27.8 / 29.3 ms.
- **Reduce Transparency** (`EnhancedBackgroundContrastEnabled=1` in com.apple.Accessibility, backboardd restarted): dock turned opaque grey, but backboardd logged "vertexFunction is not a MTLFunction" / "fragmentFunction is not a MTLFunction" (CA's RT shaders do not build through the bridge), then ~3 s later "present: GPU hang or fault (fence 4375, done 4367)" and recovery failed ("ring me_init ... TIMEOUT", "GBIF halt + soft reset + CP init -> -1"). rton1/rton2 (24.6 / 23.6 panel) are invalid (GPU dead, no GPU stats). Pref removed (cfprefsd had cached it; killed cfprefsd, deleted key), fresh boot rt-clean healthy: 19.1 swaps/s, 18.1 panel, ~15-17 ms/submit. Blur cost NOT measured this way.
- **sweetddr=1 (ddr1a/ddr1b):** 17.9 / 17.9 swaps/s, 17.3 / 16.9 panel; backboardd 13.2 / 13.0 ms/submit (more submits, mean drops) but p50/p95 unchanged 5.7 / 40.0 ms, longest 40.8 / 40.9 ms; <=17 ms 21.1 / 22.3%, <=34 ms 62.2 / 63.1%; blit 2.37 ms; GPU end -> swap 29.6 / 29.7 ms. The 40 ms submits do not move with DDR bandwidth -> not bandwidth-bound; panel rate ~3/s lower. Keep `sweetddr=0`.
- Battery 4432-4456 mV throughout. Phone left booted on ddr1 (sweetddr=1); next boot is back to default.

### 2026-10-24 21:55 Completion-path stamps (metal2 SWEET_CTL + timeline --ctl), fresh boot kernel42, sweetpresentwait=1 sweettrace=1
- ctlA / ctlB: 19.0 / 19.5 swaps/s (18.1 / 18.7 panel). 1607 CTL records for backboardd (pid 58).
| hop (per swap) | p50 | p95 | max |
|---|---|---|---|
| GPU end -> bridge completed() | 0.18-0.19 ms | 1.6-2.6 ms | 6.8-8.8 ms |
| completed() -> -finish | 0.00 | 0.01 | 0.02 |
| flushSurfaces | 0.65 | 1.55-1.68 | 2.4 |
| -finish -> CA completion queue block | 0.09 | 0.24-0.27 | 0.41 |
| CA completed handlers | 0.00 | 0.02 | 0.09 |
| handlers done -> swap | 28.2-28.5 ms | 93.9 ms | 96-100 ms |
| commit -> GPU start | 0.59 ms | 102-105 ms | 150-162 ms |
- The bridge and kernel completion path costs <1 ms. All of the 29 ms is after CA knows the frame is done: CA waits for its next usable vsync tick. With render on a tick (~6 ms CPU) plus 13-40 ms of GPU, the previous frame finishes after the following tick, so CA renders every 2nd-3rd tick (~20/s). Lever: per-frame GPU time under ~10 ms (step 2), or a deeper CA pipeline.
- mtl2-env cleared afterwards.

### 2026-10-24 21:30 xnu 0166 CPU sampler works (kernel42, clean CI run 40150300651)
- Earlier boots tested nothing: the patch re-added osfmk/kern/thread.c as a new file (untracked in work/src/xnu), dxb skipped it ("already exists"), and my check grepped "SWEET CPU:", which SweetCPU.cpp also prints. Fixed: helper in osfmk/arm64/cpu.c, sampler driven from sweet_gpu.c's 5 s stats tick, check for "sampler started". New files need -f clean=true.
- Fresh boot, sweetpresentwait=1 sweettrace=1 sweetcpusample=1, swipe benches cs7/cs8: 18.9 / 19.3 swaps/s (18.0 / 18.6 panel).
- backboardd tid 576 (the IOMFB vsync/render thread): 0.66-1.85 s CPU per 5 s window during swipes, ~10-15 ms per frame at 19-25 fps; 55-75% of it already on the A76s. Next threads: tid 934 0.17-0.43 s, tid 544 / 931 ~0.1 s. SpringBoard tid 256 (main) 0.22-0.48 s per 5 s.
- Per-CPU busy% during swipes: c0 7-18, c1-c5 0-12, c6/c7 8-24. The CPUs are mostly idle; the GPU is ~30% busy.
- Reading: neither CPU nor GPU is saturated and the render thread needs only ~10-15 ms per frame, so the ~29 ms GPU end -> swap gap and the ~20 fps are waiting/pacing, not work. A big-core scheduler would save at most a few ms per frame. Open: the completion path (bridge handler thread + dispatch hop to CA) is the one untested hop.
- Caution: later benches (ctc1/ctc2/notel1/notel2, 7-10 swaps/s) ended on the widgets page (blurred widget materials): the swipe position drifted, so those runs and the mtl2 telemetry ctc numbers are not comparable. Reboot before benching.

### 2026-10-24 20:30 Step 3: GPU end -> swap gap (29 ms)
- Path traced (investigator): kernel fence wait is a 20 us busy loop (Turnip waits in 1 ms slices), metal2vulkan's per-queue waiter uses vkWaitSemaphores; because our bridge always registers a completed handler, finishing goes through the runtime's handler thread plus a dispatch_async to CA's completion queue. IOMFB SwapEnd itself doesn't block (SwapWait sel 6 is a no-op).
- From the tl1 trace (no new code): swaps land 4-8 ms after a vsync notification (p10 3.6, p50 6.5, p90 17.6 ms; 768 swaps). So CA swaps on the tick after the GPU finishes, and at ~20 swaps/s it starts a new frame only every ~3rd tick: looks like one frame in flight.
- Our IOMFB only ever sends notification type 5 (vsync); no swap-complete / buffer-release notifications. backboardd calls sel 72 (Enable/DisableNotifications) very often. Next: find in QuartzCore what gates the next frame (research/re/QuartzCore.s, investigator running).
- [21:40] QuartzCore RE (investigator): CA subscribes only to vsync (type 5) for rendering; no swap-complete/buffer events are used; 2 swap slots freed via SwapWait (ours returns 0 at once, not a gate). Hypothesis: vsync_callback skips ticks earlier than a "vbl threshold" = end of last render + one period, capping ~30/s.
- xnu 0165 (kernel41, CI 40144904620): sweetvsyncahead=US reports each tick's time US later. Fresh-boot matrix, sweetpresentwait=1 sweettrace=1, 2 boots each:
| sweetvsyncahead | swaps/s (bench) | panel/s | swap-to-swap p50 / p95 | <=17 ms | GPU end -> swap p50 |
|---|---|---|---|---|---|
| 0 | 20.5 / 18.7 | 19.5 / 18.3 | 33.2 / 100.5 ms | 23 / 19% | 29.3 / 29.1 ms |
| 8000 | 19.4 / 19.6 | 18.5 / 18.7 | 33.2 / 100-114 ms | 23 / 24% | 29.2 / 27.6 ms |
| 16667 | 18.8 / 21.1 | 18.2 / 20.2 | 33.3 / 100.6 ms | 24 / 22% | 29.3 / 29.0 ms |
- No effect: the threshold hypothesis is wrong (or that branch isn't taken). Default stays 0. Next: measure CA's own per-frame CPU (backboardd render thread pstack under swipes; bridge commit stamps vs swaps) to see what fills the 29 ms.
- [22:30] CPU profile under swipes (pstack, 120 samples each): backboardd's CA::Render::Server thread idle in 100% of samples (mach_msg); rendering happens on the IOMFBServer vsync thread (vsync_callback -> render_for_time), running in 6.7% of samples. SpringBoard main thread idle 99%. But psx shows backboardd 69.6% CPU and SpringBoard 37.7% during the bench: pstack only catches threads at syscalls, so it hides running time. Reading: CA's per-frame CPU (render_for_time on one thread) is the likely filler of the 29 ms.
- The kernel scheduler is not big/little aware (no cluster types; 6x A55 1.8 GHz + 2x A76 2.2 GHz scheduled as equals), so the render thread often runs on an A55.
- Next candidates: (a) measure render_for_time CPU time per frame and which CPU it runs on (kernel per-thread stats or a sampling profiler that doesn't stop the process); (b) make the scheduler prefer the A76 cluster for UI/render threads (XNU AMP: cluster types from the device tree + the Edge scheduler, or a simpler bias).
- [21:00] xnu 0166 (CPU sampler): three silent boots. Cause: the xnu CI build is incremental; a new file added to bsd/conf/files is not compiled (the kernel had no "SWEET CPU:" strings). New source files need gh workflow run xnu.yml -f clean=true. Sampler now driven from sweet_gpu.c's 5 s stats tick.

### 2026-10-24 17:00-18:00 SWEET_CULL (bridge dead-draw culling), metal2 runs up to 40134633213 + debug
- See [metal](metal.md) for this entry; it also touches this feature.

### 2026-10-24 15:40 Bridge drawlog (metal2 run 40123962779, commit 0472fb8, SWEET_DRAWLOG)
- See [metal](metal.md) for this entry; it also touches this feature.

### 2026-10-24 12:30 Step 2: Turnip GPU trace under swipes (gt1, MESA_GPU_TRACES=print; kernel already running, 4184 mV charging)
- Bench with tracing: 19.3 swaps/s, 18.0 panel updates/s. Timeline: backboardd GPU/submit p50 9.0 ms, p95 39 ms; GPU end -> swap p50 26.7 ms.
- 1569 render passes: <5 ms 1212, 5-10 173, 10-20 18, 20-40 166, >40 0. All long passes are full-screen 1125x2436 GMEM (44 bins 288x224).
- 5 longest passes: 33-36 ms with only 21-92 draws; draw_ib_gmem is >95% of it, gmem load/store ~1 ms each, binning 0.1-0.3 ms. So a few expensive draws (fill/shader: likely blur or backdrop), not draw count or size.
- copy_buffer_to_image: 10018 copies, 3.5 s total (0.35 ms each); 44-50 copies (~10 ms) ride in the same submits as long passes.
- Follow-up (PC only): the trace has one draw_ib_gmem span per bin, no per-draw data. Slow passes are uniformly slow: nearly every one of the 44 bins costs ~0.7-1.3 ms (fast passes ~0.08 ms per bin), even with only 21 draws. So the cost is one or more full-screen expensive draws (blur/backdrop or large texture sampling), spread over the whole screen. Per-draw attribution needs per-draw timestamps in Turnip's trace.

### 2026-10-24 11:50 Present-mode comparison with the timeline (kernel39 / kernel40 = + xnu 0164 sweetpresentslots)
- See [display](display.md) for this entry; it also touches this feature.

### 2026-10-24 11:30 Step 1 done: per-frame timeline (xnu 0163 on kernel39, boot args sweetpresentwait=1 sweettrace=1)
- Wi-Fi install tool was run by the user (credentials stay out of git). Charger on the TB4 port, 4125 mV at boot.
- Bench tl1/tl2 (Settings swipes, 30 s): 23.4 / 22.5 swaps/s, 22.3 / 21.7 panel updates/s (bench script) with tracing on. Timeline (both runs agree; first version of the tool used 24 MHz and printed times 20% too short, fixed: mach time and CP counter are 19.2 MHz, 718M ticks in a 37 s bench):
| metric | p50 | p95 | max |
|---|---|---|---|
| swap-to-swap | 33.2 ms | 100 ms | 1210-1376 ms |
| backboardd GPU per submit | 5.7-5.9 ms | 40 ms | 41-44 ms |
| present blit GPU | 2.4 ms | 2.6 ms | 2.9 ms |
| backboardd GPU end -> swap | 29 ms | 95 ms | 96-100 ms |
| swap -> present call (tick wait) | 10.7 ms | 29-35 ms | 79-106 ms |
| present call duration | 0.65-0.68 ms | 43 ms | 83-122 ms |
| present submit -> blit GPU start | 0 | 114 ms | 162 ms |
- Rates over the swap span: 20.2-20.3 swaps/s, 19.4-19.5 presents/s, vsync ticks 51.6-52.0/s (nominal 60: after a blocked tick the timer fires late and in bursts).
- Frame intervals: 21-22% <= 17 ms, 64-65% <= 34 ms, ~21% > 50 ms.
- Reading: the GPU is not the main gap. 29 ms from backboardd's last GPU end to the swap (CA/CPU side), the bimodal 40 ms GPU frames, and a vsync tick that only delivers ~52/s dominate; the present wait (p95 43 ms) hits ~5% of ticks.

### 2026-10-24 11:30 Step 0 (video spike), partial; new plan written
- Plan archived to docs/sunfish/plans/2026-10-24-present-path.md; new docs/sunfish/plan.md (video spike, per-frame timeline xnu 0163, backboardd GPU time, ordering).
- New tool phone/tools/vtdecode (AVAssetReader + VideoToolbox probe; userland CI). On kernel38 + current bundle:
  - H.264 and HEVC 1080x1920: VTIsHardwareDecodeSupported = 0; VTDecompressionSession with RequireHardwareAcceleratedVideoDecoder -> OSStatus -12906 (no hardware decoder, as expected).
  - VTDecompressionSession without that requirement -> OSStatus 0, UsingHardwareAcceleratedVideoDecoder = no: a software decoder exists for both codecs.
  - Decode fps NOT measured: AVAssetReader fails "Cannot Access URL ... sandbox extension was not issued" (file in /var/local/tmp or /var/mobile/Media/vt, run as root over ssh; an entitlements plist did not help).
- Safari to http://192.168.7.1:8000 (local test page) is refused: "not connected to the internet" (USB ethernet is not an iOS network). A Safari test needs Wi-Fi: run tools/sunfish-install-wifi.sh yourself (credentials stay out of git and out of chat), or measure decode in-process by feeding VT sessions from a parsed mp4 (not done).
- Webcam note: phone sits rotated 90 deg (top toward the right of the image); flame-cmd taps are in portrait panel coordinates; flame-shot returned a garbled image today.
- Phone is on the 1800 s reboot cycle (k38d booted 11:00).

### 2026-10-24 08:40 CA vsync: backboardd was never on IOMFB vsync
- QuartzCore (research/dsc, `ipsw dyld disass --image QuartzCore`): `IOMFBServer::set_vsync_enabled` subscribes to
  IOMFB vsync (sel 72, type 5) only when `CADeviceUseVBL()`; else it runs its own CADisplayTimer.
  `CADeviceUseVBL` = getenv("CA_FORCE_TIMER") != 0 || MobileGestalt ChipID in {0x7000, 0x7001, 0x8000-0x8004, 0x8010}.
  Our ChipID 0x8015 is not, so CA never saw our vsync. (Setting it via /var/local/mtl2-env is too late: dispatch_once.)
- Fix: launchd.plist (`/System/Library/xpc/launchd.plist`), backboardd EnvironmentVariables CA_FORCE_TIMER=1
  (original: /var/local/launchd.plist.pre-vbl). backboardd now logs "notifications type 5 on/off".
- Bench (kernel36, thread off): 21.6 swaps/s = 20.8 panel updates/s (no dropped swaps; was 17-19 panel, 19-27 swaps).
  With sweetfb_thread=1: 14.7 panel/s. xnu 0161 present-on-swap (kernel37, sweetfb_onswap=1): 14.1 panel/s. Both off.
- Present inline on the vsync tick still takes 10-17 ms wall (blit waits behind backboardd's next job): next lever.

### 2026-10-24 00:40 Night run: where backboardd's GPU time goes
- Brightness: tools/sunfish-dim.sh 10 (LM_0 blend alpha); user confirmed "a lot lower", text readable.
- Experiments without rebuilds: tools/sunfish-m2env-bench.sh TAG [KEY=VALUE...] (writes
  /var/local/mtl2-env, TUDBG=... -> /var/local/mtl2-tu-debug, restarts backboardd, benches).
  Pre-create /var/local/turnip-<proc>.log mode 666 (clients run as mobile).
- No effect on fps or backboardd GPU ms: TU_AUTOTUNE_ALGO prefer_sysmem / prefer_gmem, TU_DEBUG=sysmem / gmem,
  turnip 0102 TU_SWEET_PRIM_ORDER=0 (no per-overlap flush for fetch draws). Not fill rate.
- Bridge telemetry (/var/local/mtl2-telemetry): 1 render pass per frame, 14-46 draws, ctc 7.5-16 ms.
- MESA_GPU_TRACES=print (per-op GPU timestamps) per backboardd frame (~11.5 ms):
  copy_buffer_to_image ~13x = 6.6 ms, copy_image_to_buffer 1.4 ms, the render pass itself 3.5 ms.
- Cause: Turnip reports requiresDedicatedAllocation for every external image, so metal2vulkan never
  aliases an IOSurface (host-pointer import) and keeps a private "shadow" image, copied in/out each frame.
  Also CA's 16/32-byte row pitches are refused for explicit DRM-linear layouts (turnip 0103 logs them).
- Fixes: xnu 0159 (kernel35) rounds single-plane IOSurface rows to 64 bytes (sweetsurfalign=0 off) — alone
  no change; turnip 0104: host-allocation images not dedicated (testing).
- Results (kernel35 + turnip 0104 + bridge 0102): the copy-out is gone and most copy-ins are gone.
  dr23 frame GPU 9.4 -> 6.4 ms, dr14 7.4 -> 4.4 ms. Remaining: ~3 ms/frame of small A8/RGBA8 buffer
  textures with 32-byte pitches (Turnip needs 64 for explicit linear).
- fps barely moved (17-19 panel updates/s): the GPU is only 35-60% busy, and backboardd is idle ~45%
  of the time (pstack samples + phone/tools/a2s; shared-cache symbols are redacted, but most CPU is
  in QuartzCore, little in the bridge). The limit is now pacing/latency, not GPU work.
- Matrix on kernel35 (tools/sunfish-cfgbench.sh): sweetfb_thread=0 vs 1 is the same (~17 panel/s).
- backboardd never calls IOMFB sel 72 (vsync notification enable) on this boot. Check how CA gets
  vsync now (0121 relied on it). This is the next lead for pacing.
- Idle 10 Hz full redraws = the Clock icon's second hand (normal).
- Next: (1) trace CA's vsync source / swap pacing; (2) Turnip: allow 16/32-byte linear pitch for
  sampled-only images (removes the last copies); (3) charger config (the phone loses about 0.5 A while benching).

### 2026-10-24 experiments.md lines
| date | patch / switch | hypothesis | predicted ms saved | measured fps and GPU ms | verdict | commit |
|---|---|---|---|---|---|---|
| 2026-10-24 | `TU_AUTOTUNE_ALGO`, `TU_DEBUG=gmem/sysmem` | render mode / tiling cost | few | no change | dead end | - |
| 2026-10-24 | backboardd `CA_FORCE_TIMER=1` (launchd.plist) | CA ignores our vsync (chip-id gate) | pacing | swaps == panel updates, 20-23/s | kept | 7058eca |
| 2026-10-24 | xnu 0163 timeline (`sweettrace=1`) | measurement | 0 | no overhead (22.3 / 21.7 fps) | kept (tool) | 3e72521 |
| 2026-10-24 | sysmem under swipes | tile overhead makes the 40 ms frames | 10+ | same 13-16 ms/submit, longest 40-45 ms | dead end | e9fd4fa |
| 2026-10-24 | xnu 0165 `sweetvsyncahead=8/16.7` | beat CA's vbl threshold | pacing | no effect | dead end | 244f7e7 |
| 2026-10-24 | xnu 0166 `sweetcpusample` | CA is CPU-bound | - | render thread 10-15 ms CPU/frame, 55-75% on A76; CPUs mostly idle | dead end (waiting, not CPU) | fe0675c |
| 2026-10-24 | SWEET_CTL completion stamps | completion path is slow | - | GPU end -> handlers < 1 ms | dead end | 98a324c |
| 2026-10-24 | timeline analysis: CP span overlap/busy (ctlA/B, cs7/8) | long submits include waiting or others' work | - | 0 overlaps; long spans fixed at 38.6 ms p50 / 40.4 p95; GPU 25-37% busy; trace can't split busy vs stall | inconclusive (needs RBBM busy counter / per-pass CP timestamps) | - |
| 2026-10-24 | `sweetddr=1` DDR/NoC top vote (off by default in sunfish-boot.sh) | 40 ms submits are bandwidth-bound | 5+ | 17.3/16.9 fps vs 20.1/20.3; bb p50/p95 5.7/40.0 ms unchanged (mean 13.1 vs ~16.7, more short submits) | dead end (keep sweetddr=0) | - |

### 2026-10-23 23:05 Smoothness measurements + memory work (stopped at 16% battery)
Bench (tools/sunfish-fpsbench.sh, BENCH_TIGHT=1, Settings swipes, 30 s):
| build | GPU MHz | swaps/s | panel updates/s |
|---|---|---|---|
| 0157 no WFI | 430 | 34.6 | 15.5 |
| 0157 + sweetwfi=1 | 430 | 34.3 | 15.4 |
| 0157 | 565 | 29.5 | 17.4 |
| 0157 | 650 | 28.5 | 17.8 |
| 0158 non-blocking present (+ TU census on) | 565 | 22.7 | 12.5 |
- Bottleneck: the present path, not GPU clock or WFI. present() ~23 ms wall (blit queued behind
  backboardd jobs of 10-42 ms on the single ring) for ~2.4 ms of GPU work; GPU only ~50% busy.
- 0158 (present returns busy instead of waiting) did not help (put off 701 of 600 frames). Not kept as
  default; next idea: present on a second ringbuffer / priority, or scan out without the blit.
- 0156 per-client pool reuse works (73 hits in 6 app switches, no 10 MB bzero each).
- Turnip TU_DEBUG=bos census (phone/turnip/patches/0101, /var/local/tubos): at the first print backboardd
  held ~128 MB but Turnip listed 5 BOs (1 MB); later prints were buffered (fflush added, re-run pending).
- Phone left on k91 (kernel33, 565 MHz, flamereboot=0) to charge, battery 3706 mV.

### 2026-10-23 13:15 Display/GPU tuning — **best: 23.4 panel updates/s** (Metal2, 565 MHz, defaults updated)
Tight bench (continuous swipes), kernel with 0147+0148+0149; panel updates/s (what the screen shows):
| config | swaps/s | panel/s |
|---|---|---|
| CPU render + GPU composite (k37) | 17.0 | 17.2 |
| Metal2 565 MHz, no big-core pref (k36) | 18.6 | 18.2 |
| Metal2 565 + 0147 big cores (k38) | 32 | 17.6 |
| + 0149 present thread, 650 MHz (k40) | 22.3 | 13.9 |
| + 0149 thread, DDR vote (k41) | 22.3 | 11.9 |
| **0149 thread off, 565 MHz, no DDR vote (k42)** | 25.6 | **23.4** |
| thread off + DDR vote (k43) | 31.5 | 17.7 |
- 825 MHz (0148 level 8): GMU acks, but CP me_init times out (GX not running; likely needs ACD/MX vote) → don't use.
  650 MHz works but is not faster: render jobs stay ~15 ms → not clock-bound.
- 0149 (present thread) hurts → off by default (`sweetfb_thread=0`, kept for later). DDR vote: more swaps, fewer panel
  updates (more GPU contention for the present).
- sunfish-boot.sh defaults now: `sweetmtl=2 sweetgpufreq=5 sweetgpudelay=0 sweetfb_thread=0` (Metal2 needs the
  bundle installed: tools/sunfish-metal2-install.sh).
- CI incremental build: build step **109 s** (full build 406 s), run wall time 4.7 min incl. queue.
- (Recovered from `git show 1808cf9^:docs/sunfish/workday.md`; that commit deleted it.)

### 2026-10-23 12:35 0147 big-core preference — **backboardd ~2x faster; panel path is now the cap**
- 0147: threads with priority ≥ foreground take an idle A76 (CPUs 6-7) first (ADT has one SMP cluster).
- Tight bench (k38, Metal2, 565 MHz): backboardd **30–34 swaps/s** (was ~18), panel **16.5–18.6 updates/s**.
- Cap: each panel copy waits ~25–34 ms because the present blit queues behind backboardd's render jobs
  (12–19 ms each, GPU 40–67% busy) on the single ring, and the copy is synchronous.
- Next: 0148 adds the A618's stock 650/800/825 MHz levels (shorter render jobs); later: present on a
  higher-priority path or asynchronous panel copy.
- CI: incremental xnu builds (cache of source/objects/fakeroot; `clean=true` input for a full build).
- (Recovered from `git show 1808cf9^:docs/sunfish/workday.md`; that commit deleted it.)

### 2026-10-23 12:10 FPS reality check — benchmark metric corrected
- The bench counted `swaps`; the screen shows `panel copies`. With Metal at 267 MHz many swaps never reached the
  panel (k35: 22.8 swaps/s but 13.2 panel updates/s). Bench now reports both; `BENCH_TIGHT=1` = continuous swipes.
- Panel updates/s, continuous swipes (k36/k37, GPU 565 MHz acked via DCVS):
  Metal2 + Turnip **18.2** · CPU render + GPU composite **16.7–17.6**. Old bench (with pauses): all paths ~16–17.
- Metal halves backboardd CPU (49 s → 22 s per bench). Neither CPU (~0.55 core) nor GPU (11–29% busy) is
  saturated → the ~18 fps ceiling is frame pacing/latency, not throughput. Next: where backboardd waits per frame.
- (Recovered from `git show 1808cf9^:docs/sunfish/workday.md`; that commit deleted it.)

### 2026-10-23 08:50 Fallback: CPU render path — **pass: 4.3 → 17.2 fps** (4×)
- New `port/tools/sunfish-fpsbench.sh TAG SECS`: unlock + scripted home-page swipes, averages the
  `SWEET FB: 5s` fps lines. Same workload for every measurement.
| Change | Swipe fps |
|---|---|
| baseline `sweetfb_hz=30` | 4.3 |
| `sweetfb_hz=60` | 4.9 |
| drop `com.apple.voicemail.vmd` (Visual Voicemail, spun ~1 core without cellular) | **16.7** |
| drop `com.apple.ReportCrash` (was busy with TTS-synth crash reports) | **17.2** |
- Jobs removed with new `port/tools/sunfish-launchd-drop.sh LABEL…` (backup on phone:
  /System/Library/xpc/launchd.plist.pre-drop). `sweetfb_hz=60` is now the helper default.
- Remaining CPU in the bench: backboardd (~1.1 cores) = the actual renderer.
- (Recovered from `git show 1808cf9^:docs/sunfish/workday.md`; that commit deleted it.)

### 2026-10-23 experiments.md lines
| date | patch / switch | hypothesis | predicted ms saved | measured fps and GPU ms | verdict | commit |
|---|---|---|---|---|---|---|
| 2026-10-23 | `sweetfb_hz=60` | 30 Hz vsync caps the UI | - | 4.3 -> 4.9 fps | kept | df05f86 |
| 2026-10-23 | GPU clock 430 / 565 / 650 MHz | GPU-bound | several | 15.5 / 17.4 / 17.8 fps; GPU ms unchanged | dead end | - |
| 2026-10-23 | xnu 0157 no wait-for-idle between submits (`sweetwfi=1` restores) | WFI serializes submits | few | 15.5 vs 15.4 fps at 430 MHz | kept, no gain | 095806b |

### 2026-10-23 10:00 status.md update
- UI 17 fps while swiping (was 4.3): dropped launchd jobs vmd, ReportCrash, axassetsd; sweetfb_hz=60.
