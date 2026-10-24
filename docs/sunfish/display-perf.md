# Sunfish display and GPU performance: findings and methods

What is known about the frame path on the Pixel 4a (sunfish), how it was found, and what is left. The newest dated
log is in `docs/workday.md`. The step-by-step next plan is in `docs/sunfish/plan.md`.

## Frame path
1. **The app or SpringBoard draws its layers.** These are IOSurfaces from SweetSurface; Metal goes through the
   SweetMetal2 bundle (metal2vulkan + Turnip).
2. **backboardd (CoreAnimation's render server) composites one render pass per frame**, 14–46 draws at
   1125x2436, into a 1125x2436 IOSurface.
3. **It swaps through IOMFB SwapEnd (sel 5), which records only the frame**
   (`iokit/Kernel/arm/SweetFramebuffer.cpp`).
4. **The vsync tick (a 60 Hz software timer) presents it**: a GPU blit that scales the frame into the ABL panel
   buffer, scanned out by the MDP.
5. **The vsync tick also notifies clients (type 5).**

## Findings
### 1. IOSurface shadow copies (fixed)
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

### 2. CA never used our vsync (fixed)
- **The gate:** `CA::WindowServer::IOMFBServer::set_vsync_enabled` subscribes to IOMFB vsync (sel 72, type 5) only
  when `CADeviceUseVBL()` is true. Otherwise it runs its own CADisplayTimer.
- **The check:** `CADeviceUseVBL` = `getenv("CA_FORCE_TIMER")` is non-zero, or MobileGestalt ChipID is in
  {0x7000, 0x7001, 0x8000–0x8004, 0x8010}. Our synthetic chip-id is 0x8015, which is not in the list.
- **Fix:** backboardd `EnvironmentVariables` gets `CA_FORCE_TIMER=1` in `/System/Library/xpc/launchd.plist`
  (the original is backed up at `/var/local/launchd.plist.pre-vbl`). Setting it from `/var/local/mtl2-env` is
  too late, because CA reads it in a `dispatch_once` before the Metal plugin loads.
- **Result:** 20–23 swaps/s with swaps equal to panel updates. Before: 17–19 panel updates/s against 19–27 swaps.

### 3. The present is the next limit (open)
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

### 4. Other facts
- **CPU and GPU are both partly idle:** the GPU is 35–60% busy and backboardd is idle ~45% of the time
  (pstack samples). The limit is pacing, not throughput.
- **Most backboardd CPU time is in QuartzCore;** the bridge and Turnip are small.
- **Idle redraws:** 10 full redraws/s when idle come from the Clock icon's second hand. This is normal.
- **Clock and render mode don't matter:** no gain from GPU 430/565/650 MHz, `TU_AUTOTUNE_ALGO`, `TU_DEBUG=gmem/sysmem`,
  or 0157 (no wait-for-idle).
- **Battery drains on the charger:** about -0.5 A while benching with the screen on. The kernel never configures
  the charger.

## Methods and tools (reuse these)
### Where the data comes from
- **Live kernel log:**
  - `tools/sunfish-livelog.sh ../logs/usb-live.log` runs on the PC in the background and appends every kernel log
    line from the phone. It also brings up the USB network link (phone 192.168.7.2).
  - If SSH says "No route to host", this listener is probably not running.
  - Every boot or bench script writes a `=== NAME ...` marker line into it. Slice the log between markers with
    `grep -n '=== NAME'`, or `sed -n '/=== bench X start/,/=== bench X end/p'`.
- **Kernel stats lines, every 5 s:**
  - `SWEET FB: 5s: N swaps (fps), M panel copies, avg copy U us`: what CA sent versus what reached the panel,
    and the present's wall time.
  - `SWEET GPU: 5s: N submits, GPU busy X ms (P%), longest, mean; <proc> n/ms ...`: GPU time per process.
    For backboardd, ms divided by n is the GPU time per frame.
  - `SWEET GPU: present: N frames, avg cache clean, GPU blit, put off` (every 300 frames).
  - `SWEET GPU: pool: N MB in use` and the census lines track GPU memory.
  - `SWEET BATTERY: mV mA ... shown N%` gives the battery level; negative mA means draining.
- **Benchmark:** `tools/sunfish-fpsbench.sh TAG SECONDS` (BENCH_TIGHT=1)
  - It swipes between home pages with `flame-cmd swipe`.
  - It then averages the `SWEET FB: 5s` lines between its markers into "swaps/s, panel updates/s".
  - `sunfish-cfgbench.sh` and `sunfish-m2env-bench.sh` both call it.
- **Soak logger:** `tools/sunfish-soak.sh ../logs/soak.log` logs up / unreachable / REBOOTED, battery and new
  crash lines every 60 s. Use it to catch reboots and hangs while away.
- **Battery in fastboot:** `fastboot getvar battery-voltage`.
- **Screen (webcam):**
  - `ffmpeg -f v4l2 -i /dev/video0 -frames:v 1 snap.jpg`. The Iriun device number can change; check with
    `v4l2-ctl --list-devices`.
  - Check every visual change on the webcam.
- **GPU trace totals:**
  - Copy the `MESA_GPU_TRACES` log to the PC, then run `tools/gputrace-sum.py` on it.
  - It prints GPU time per operation, e.g. `copy_buffer_to_image` against `render_pass`.
- **Bridge telemetry:**
  - `/var/local/mtl2.log` `K` lines give per commit: `rp` passes, `dr` draws, `kpx` target size, and
    `ctc` = commit-to-callback (about the GPU time).
  - Commit timestamps show the frame pacing.
- **CPU sampling:**
  - On the phone: `pstack PID >> file` 80× every 0.25 s during a bench.
  - On the PC: symbolize the addresses with `a2s`, then count leaf and inclusive frames.
  - Threads with pc 0 or in libsystem_kernel are idle.

### Metal client experiments without a rebuild
- **`tools/sunfish-m2env-bench.sh TAG KEY=VAL...`** writes `/var/local/mtl2-env`, restarts backboardd and benches.
  `TUDBG=opts` sets TU_DEBUG.
- **Clients run as mobile:** pre-create log files with mode 666, e.g. `/var/local/turnip-backboardd.log` or
  `/var/local/m2v-msg.log`.
- **GPU time per operation:** `MESA_GPU_TRACES=print MESA_GPU_TRACEFILE=/var/local/turnip-backboardd.log`.
  Sum `start_X`/`end_X` pairs.
- **Bridge degradations:** `SWEET_M2V_MESSAGES=/var/local/m2v-msg.log`. These are shadowed textures and similar,
  and each kind is logged only once.
- **Frontend telemetry:** `touch /var/local/mtl2-telemetry` makes `/var/local/mtl2.log` show per-commit
  passes, draws and commit-to-callback time every 10 s.

### Boot matrix
- **`tools/sunfish-cfgbench.sh NAME KERNEL args`** reboots, boots the kernel, and runs two 20 s swipe benches.
  Set `DIM=N` to dim the screen.
- **Bench noise is about ±3 updates/s.** Always run twice, and compare configs from fresh boots only.

### CPU profiling
- Run `pstack PID` in a loop on the phone, then symbolize shared-cache addresses with `/var/local/bin/a2s`
  (`phone/tools/a2s.c`).
- Shared-cache local symbols are redacted on the phone. Use the PC copy for real names.

### Reverse engineering iOS frameworks
- **`research/dsc/`** is the full shared cache pulled from the phone. It is gitignored because it contains Apple's
  files; keep it out of git.
- **Disassemble one image:**
  `systemd-run --user --scope -p MemoryMax=8G ipsw dyld disass research/dsc/dyld_shared_cache_arm64 --image QuartzCore > research/re/QuartzCore.s`
  Then grep for `_stub` calls and the symbols of the `__ZN…` functions that contain them.
- **Never run `ipsw dyld xref --all`:** it ran the 16 GB PC out of memory.
- **Copying a big file from the phone:** boot with `flamereboot=0`, and give `ssh` `</dev/null` inside
  `while read` loops (otherwise it reads the rest of the loop's input).

### Brightness
- **`tools/sunfish-dim.sh N`** sets the LM_0 blend alpha. It resets on reboot; leave it at 100 unless asked.
- **Reading 0x0ae44000 hangs the bus.** The MDP registers are at the catalog offset + 0x1000.

## Patches from this work

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
