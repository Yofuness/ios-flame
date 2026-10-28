# Metal
**State:** ✅ Metal via SweetMetal2 (metal2vulkan + Turnip) on the A618. **Updated:** 2026-10-26.

## How to use
- Install a bundle: `tools/sunfish-metal2-install.sh <dir containing SweetMetal2.bundle> --no-restart` (after `gh run download ID -D DIR`). Pushing `phone/turnip/patches` or `phone/metal2` starts the metal2 CI.
- `tools/sunfish-m2env-bench.sh TAG KEY=VAL...` writes `/var/local/mtl2-env`, restarts backboardd and benches. `TUDBG=opts` sets TU_DEBUG.
- Metal clients are listed in /var/local/mtl2-clients (com.apple.WebKit.GPU + MobileSafari added 2026-10-23).
- Bridge switches: SWEET_DRAWLOG, SWEET_CULL, SWEET_PIPESTATS, SWEET_CTL, METAL2VULKAN_HALF_RELAXED, SWEET_M2V_MESSAGES. Telemetry: `touch /var/local/mtl2-telemetry`.

## Facts
- Patches from the perf work (display-perf.md): turnip 0102 prim-order switch, 0103 log refused explicit layouts, 0104 host-pointer images don't need a dedicated allocation; bridge 0102 read-only textures may alias past their last row. turnip 0105 (16-byte pitch) failed: parked in `phone/turnip/failed/`.
- Every CA draw uses one uber shader (fixed_vert_lph_spc / fixed_frag_lph_cph) specialised per pipeline.
- "vertexFunction/fragmentFunction is not a MTLFunction" lines were noise: NBFunction never declared the protocol; the bridge now declares MTLFunction/MTLLibrary (a1f539e, nbproto).

## Open
- About 3 ms/frame of small A8/RGBA8 buffer textures with 32-byte pitch (Turnip needs 64 for explicit linear).
- Reduce Transparency works but slow (~6 swaps/s) (broken.md, rtj/rtj2).
- /var/local/mtl2.log: see [apps](apps.md) (never rotated).

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

### 2026-10-28 08:00-08:25 Metal bundle built on Linux
- tools/build-metal2-linux.sh (e2057b6): full build 5 min 11 s, no-change rebuild 14 s, on this PC with the theos iPhoneOS16.5 SDK. Matches CI run 40676463855 in size, dylibs, exports and install names (one weak-defined libc++ export fewer; a few extra weak libc++ imports).
- bl2: installed with sunfish-metal2-install.sh, UI drawn (lock and home screen), 0 dyld errors, a 15-min app soak ran on it.
- SweetMetal2: per-process Turnip logs in /var/local/turnip/<proc>-<pid>.log while that directory exists (a respawned client used to truncate the shared log).

### 2026-10-26 ~19:20-19:35 nbproto bridge + Reduce Transparency repro (kernel52, sk52b boot)
- Installed work/metal2-nbproto bundle (class_addProtocol NBFunction/NBLibrary): "is not a MTLFunction" lines after backboardd restart: 0 (was thousands per boot).
- RT run 1 (rtj): `EnhancedBackgroundContrastEnabled=1`, cfprefsd killed, backboardd restarted, 90 s on the lock screen only: no hang. pipelines.log 8664 -> 8790; drawlog 40 passes.
- RT run 2 (rtj2): unlocked, 12 home swipe pairs, dock pull-up, one app open, ~164 s: 0 hang/recover lines. Swap rate fell from 21.8 to 5.8/s during swipes, avg present copy 22-38 ms (RT is slow, not hung). pipelines.log 8935 -> 9165; drawlog 20 passes. Webcam: dock and search pill drawn dark opaque (RT looks active, not certain).
- Undo both times: key removed (plist 494 B), SWEET_DRAWLOG removed, 21-26 swaps/s, home healthy.
- Reading: the 2026-10-24 RT hang (kernel42, old bridge) did not reproduce on kernel52 + nbproto bridge. Cause unproven (old bridge, the pre-0174 clock state, or chance). Logs: ../logs/rtj*-{pipelines,drawlog,live}.

### 2026-10-26 experiments.md lines
- 2026-10-26 rtj/rtj2: nbproto bridge installed (0 MTLFunction lines); Reduce Transparency on kernel52 ~4 min incl. home swipes: 0 GPU hangs; RT drops to ~6 swaps/s.

### 2026-10-25 Plan step 2: hang triggers
- See [gpu](gpu.md): the MTLFunction-noise finding and the RT hang are in that entry.

### 2026-10-25 experiments.md lines
- 2026-10-25 'not a MTLFunction' log lines = missing MTLFunction protocol on NBFunction (Metal NSLogs and continues); bridge now declares MTLFunction/MTLLibrary. RT hang = CP stall in a valid pipeline's draw, culprit unknown.

Older entries (before 2026-10-25): [archive/features/metal.md](../archive/features/metal.md).
