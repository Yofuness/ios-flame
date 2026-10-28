# GPU memory
**State:** ✅ the backboardd pool leak is fixed (bridge 0105, spare command pools capped at 8); pool flat over a 20 min soak. **Updated:** 2026-10-28.

## How to use
- Census: `sweetbocensus=N` (xnu 0196) per-client census of live 1-8 MB BOs older than 60 s; Turnip side `TU_DEBUG=bos` for backboardd via `tools/sunfish-m2env-bench.sh` (output in /var/local/turnip-backboardd.log).
- Pool warning: `sweetpoolwarn=N` % of the 940 MB pool (default 80, xnu 0183).

## Facts
- Pool 940 MB; per-client cap 256 MB and the 80% warning protect against exhaustion (plan step 8).
- A full pool caused the 2026-10-23 freezes (backboardd SIGSEGV via Turnip cs->cur == NULL).
- xnu 0156 per-client pool reuse (73 hits in 6 switches).
- Growth is all backboardd live BOs: <256 KB BOs (Turnip command streams) 16 -> 180 MB in the first 15 min then flat; own 1024 KB kernel BOs never freed, ~1 per 25-30 s while opening apps, not tracked by Turnip (bc1/bos1, 2026-10-28).
- UniXNU backend (tu_knl_darwin.cc): the "retaining" messages are device-lost paths, not the leak; freed BOs wait in a 256-entry retire ring reaped head-first by fence.

## Open
- turnip 0106 logs the retire ring and 1 MiB BO life (commit f8d0de6): if the ring stays empty while the census grows, look at the bridge's 1 MiB vkAllocateMemory/vkFreeMemory pairs (plan step 8).
- 2 h soak with pool flat (plan step 8).

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

### 2026-10-28 18:25 sk93 (kernel93, defaults + sweetbl=2 sweettorch=1 sweetse9=2 sweetampreset=1): 40 min app soak, pool higher than the 20 min soak
- 100 app opens in 3 pieces (agent 10 min limit), 0 resets, 0 GPU hangs, UIWDT armed only. CRASH: WebKit 5, PhotosFileProvider 4.
- Pool: 39 MB at 62 s, peak 486 MB, 347 MB at 2365 s (the 20 min soak after bridge 0105: 198 / 324 peak / 245 MB). Not flat proof either way at this length; watch in the next 2 h soak (B-list: per-process census if it keeps climbing).

### 2026-10-28 11:05-11:20 A/B: the pool cap costs no speed
- Same boot as bl4 (kernel70), sunfish-m2env-bench.sh, no logging/telemetry. Panel updates/s: cap 0: 12.9 (only 3 samples, a GPU hang in that run) / 17.1; cap 8: 19.0 / 17.9 (default, no env: 18.9 / 18.1; ab-end 17.4); cap 32: 18.8 / 18.9. Means: cap 8 18.5, cap 32 18.85, clean cap-0 run 17.1. Within the ±3 noise, the cap is not slower; bl4's 16.0/16.2 was the SWEET_BOLEAK + telemetry logging.
- 1 GPU hang (ab-off, uptime 1693 s, present path, fence 36567 done 36545, smmu fsr 0; ring me_init TIMEOUT x3, then the GPU came back).
- Keep the cap at 8 (default). The GPU memory leak is fixed: bridge 0105.

### 2026-10-28 10:35-11:01 bl4: bridge 0105 (spare command pool cap 8) stops the growth
- kernel70 `sweetbocensus=300`, 4455 mV, Linux-built bundle 7053013 (bridge 0105, turnip 0107 off by default), SWEET_BOLEAK=1 + mtl2 telemetry on.
- 20 min app soak: 72 opens, 0 failures, 0 GPU hangs/faults/device-lost, 0 reboots.
- Pool: first 198 MB, peak 324, last 245 MB (sk68 2 h: 192 -> 709). Census backboardd: live 1461 / 1591 / 1214 / 1541 BOs, 235 / 205 / 206 / 234 MB at t 333 / 634 / 937 / 1240 s: flat. No 1024 KB class.
- 0106: alloc1m 157, free1m 152 at t 1377 (gap 2-8 all the time): the 1 MiB cmd cs BOs are freed now, through the pool destroy. live_tu_bos 1400-1800, flat.
- Pools: made 485, kept <= 8, capped 477 over the soak (~0.4 pools/s destroyed).
- Bench bl4p: 21.2 / 22.1 swaps/s, 16.0 / 16.2 panel updates/s (with logging + telemetry on). A/B on one boot (cap 0/8/32) running to separate the cap's cost from the logging.

### 2026-10-28 08:45-09:31 bl3: turnip 0107 (trim cs BOs > 256 KiB on reset) stops the 1 MiB leak but costs speed
- kernel70 `sweetbocensus=300`, 4456 mV, Linux-built bundle with 0107. Bench bl3p: 19.7 / 20.3 swaps/s but 13.6 / 13.6 panel updates/s (baseline ~20-22).
- 20 min app soak (72 opens, 0 fail, 0 GPU hangs, 0 reboots): alloc1m 0 throughout, trims 308 -> 3619; the census has no 1024 KB class any more.
- Still growing: live_tu_bos 1275 -> 5737 during the soak; census backboardd live 1276 -> 5781 BOs, 176 -> 356 MB; pool first 39, peak 443, last 381 MB.
- So the trim fixes the 1 MiB class but freeing the big BO on every reset is too expensive. Next: cap the bridge's spare command pools (metal2vulkan-runtime spares.rs keep_command_pool, no cap; reset without RELEASE_RESOURCES) instead, with 0107 off (SWEET_CS_TRIM_KB=0) or at a high threshold; then find the other growth (live BO count).

### 2026-10-28 08:20-08:40 bl2: the leaking 1 MiB BOs are Turnip 'cmd cs' BOs that are never retired
- Linux-built bundle (tools/build-metal2-linux.sh, turnip 0106 + per-pid Turnip logs in /var/local/turnip/), kernel70 `sweetbocensus=300`, 4455 mV; app soak bl2 + bl2b: 53 opens, 52 ok (1 Photos open failure, FBSOpenApplicationServiceErrorDomain 5).
- 0106: every 1 MiB BoAlloc in backboardd is named 'cmd cs'. alloc1m: pid 58 2 (t 122) -> 6 -> 11 -> 19 -> 28 -> 34 (t 576); pid 740 1 (t 30) -> 5 -> 10 -> 18 -> 25 -> 26 (t 442). retired1m 0 and free1m 0 throughout, 0 skip lines, retire ring 0-49/256, never FAILED; live_tu_bos levels off at ~7.5-8.2k. One identical 8-frame backtrace per pid (two variants in pid 740), being symbolized.
- Kernel census: backboardd 1024 KB n 10 (t 332, oldest 209 s) / 11 (t 918, new pid 740, oldest 272 s). That fits Turnip's count once the census' 60 s age filter is taken into account.
- So the 1 MiB BOs are command-stream BOs Turnip still holds (never bo_finish'd), not a backend free-path bug.

### 2026-10-28 07:00-08:00 turnip 0106 leak log, bl1 (no data)
- turnip 0106 (`SWEET_BOLEAK=1`, CI 40676463855 after 2 runs lost to "job not acquired": GitHub macOS arm64 capacity): logs 1 MiB BoAlloc with backtraces, alloc/retire/free counts and the 256-entry retire ring every 10 s, plus skip reasons. Hooks in tu_knl_darwin.cc darwin_bo_init / bo_finish / reap_locked / queue_submit.
- bl1 (kernel70 `sweetbocensus=300`, 4456 mV): 0106 live (4 SWEETBOLEAK lines at ~60 s), then a GPU hang at 227 s with failed recovery (see [gpu](gpu.md)); the respawned backboardd reopened /var/local/turnip-backboardd.log and truncated it (0 SWEETBOLEAK lines left; its own Turnip init got VK_ERROR_OUT_OF_DEVICE_MEMORY at BO init). Soak stopped at cycle 2 (7 opens ok, Safari open failed, FBSOpenApplicationServiceErrorDomain 5). Pool before the hang: 389-417 MB in use, peak 525 MB at 220 s; backboardd 257-297 MB (allocs 6204-6500, frees 1631-1652; live <1M 113-115 / 44 MB).
- Fix: SweetMetal2 logs to /var/local/turnip/<proc>-<pid>.log when that directory exists (mode 777). Next: rerun the soak with it.

### 2026-10-28 02:05-03:15 overnight run, part 3: the GPU pool leak
- xnu 0196 (kernel70, CI 40641592758) `sweetbocensus=N`: per-client census of live 1-8 MB BOs older than 60 s.
- bc1 (35.6 min app soak, census every 300 s): backboardd's own 1024 KB BOs 14 -> 38 -> 48 -> 60 (oldest = process age), the only growing group; 8 MB BOs flat 6-8. 2 real GPU hangs (rbbm 800005, ring pending, no SMMU fault, same signature as the failed sk430 one) recovered first path in 26.5/26.4 ms; backboardd respawned. Real-hang recovery is now proven.
- bos1 (TU_DEBUG=bos, ~10 min): Turnip's live BOs are mostly command streams (cmd cs 1491, draw cs 1992, draw sub cs 2013, tile store cs 996), 6732 BOs 484 MB; no 1024 KB BO in its table while the kernel counts 46 -> 61. The 1 MB BOs live in the kernel after Turnip let go of them (suspect: the NativeAdreno backend's "retaining native allocation" paths; not in the logs we have).
- /var/local/mtl2.log is 321 MB (never rotated).
- Phone left on bc1 (kernel70, sweetbocensus=300), dim 10, mtl2-env empty as before.

### 2026-10-28 00:00-02:10 overnight run, part 2 (soak and pool part)
- sk68 2 h exit soak (kernel68, /var on ufsroot1, 15 chunks, 120.7 min): 408/408 app opens, 0 resets, 0 GPU hangs, 0 SMMU faults, 0 UIWDT, CRASH only PhotosFileProvider x4 (AegirPoster 0). Pool 192 -> 709 peak -> 631 MB end: "pool flat" not met. Battery 4431 -> 4429 mV, 0 mA, 100% (full on the charger; discharge still untested).
- Pool analysis: all growth is backboardd live BOs; <256 KB BOs 16 -> 180 MB in the first 15 min then flat, 1-8 MB BOs +50 MB/h; apps free theirs; reuse cache 0-48 MB.
- Phone left on k69t (kernel69, sweettorch=1), dim 10.

### 2026-10-28 experiments.md lines
- 2026-10-28 sk68: kernel68 (/var on ufsroot1), 2 h app soak (15 chunks sk68a-n, 120.7 min, 6 apps): 408/408 opens, 0 resets, 0 hangs, 0 SMMU faults, 0 UIWDT, 0 pool warnings; CRASH only PhotosFileProvider x4 (AegirPoster 0); pool 192 -> 709 peak -> 631 MB end, drifting up (exit criterion "pool flat" NOT met); battery 4431 -> 4429 mV, 0 mA, 100% on screen (on charger, full: discharge still unchecked); swaps 12.8/s, panel 5.7/s mean (idle-diluted); df /private/var 3.66 GB used of 98 GB.
- 2026-10-28 sk68 pool analysis (investigator, backboardd `holds` lines): growth is all backboardd live BOs, not apps (pool minus backboardd flat 52-70 MB) and not the reuse cache (kept 0-48 MB). <256 KB BOs 743/16 MB -> 8306/180 MB, almost all in the first 15 min, then flat (~22 KB each: page rounding of tiny buffers); 1-8 MB BOs 14 -> 147 MB, steady ~+50 MB/h (slow leak, unknown buffers); 8 MB+ swing 80-190 MB (peaks). Pool +290 MB in 12 min, then ~+45 MB/h -> 80% (752 MB) after ~3-4 h more. Next: per-size census of long-lived 1-8 MB BOs (SweetAccelerator.cpp alloc ~797 / free ~681); small-BO sub-allocator would save ~150 MB.
- 2026-10-28 bc1 (kernel70 = 0196, sweetbocensus=300, 35.6 min app soak): backboardd census: own 1024 KB BOs 14 (332 s) -> 38 (643) -> 48 (969) -> 60 (1270 s), oldest = process age: never freed, ~1 per 25-30 s while opening apps (~+120 MB/h in this mix). 8 MB own BOs 6-8 flat; imports flat. THIS is the leak. Also 2 REAL GPU hangs (1342.97 s, 2081.68 s; no SMMU fault, ring pending: rbbm 800005, cp_status1 8100430e = the sk430 signature): both recovered first path, me_init ok, 26.5 / 26.4 ms, backboardd killed + respawned. First real-hang recoveries since 0181/0182. 2 hangs in 36 min is more than sk52b-sk68 (0 in 1-2 h): check whether 0196 or chance.
- 2026-10-28 bos1 (kernel70, TU_DEBUG=bos for backboardd via sunfish-m2env-bench, ~10 min app soak): census goes to /var/local/turnip-backboardd.log (not /var/local/tubos). Last print: cmd cs 1491 BOs / 180576 KB, draw cs 1992 / 36512 KB, draw sub cs 2013 / 33024 KB, tile store cs 996 / 15936 KB (= the kernel's <256 KB class: command-stream BOs), host import 137 / 70896 KB, vkAllocateMemory 8192 KB x8, 11096 KB x7, descriptor pool 71 / 2928 KB; total 6732 BOs 484 MB (mid-run 973 BOs 181 MB). No 1024 KB group in Turnip's list, while the kernel census shows 1024 KB n 46 -> 61: the 1 MB BOs are not named tu_bos. 0 GPU hangs in this run; env restored (mtl2-env empty, tu-debug removed).
- 2026-10-28 UniXNU backend read (research/re/unixnu e95b37d, patches/mesa/turnip-darwin-kmd.patch -> tu_knl_darwin.cc): Turnip allocates kernel BOs only in darwin_bo_init (BoAlloc, listed in TU_DEBUG=bos) and darwin_bo_init_host (BoImport). darwin_bo_finish removes the BO from the bos table, then queues it in a 256-entry retire ring tagged with last_fence; darwin_reap_locked frees strictly head-first and stops at the first not-yet-signalled fence. The "retaining native allocation" messages are device-lost paths (not the leak). Kernel BoFree always frees (never busy) and v2Release zeroes the slot, so the census' live 1024 KB BOs were never BoFree'd. Open: are they parked in the retire ring behind a head whose fence never passes (but then other sizes would pile up too)? Needs a phone/turnip logging patch (ring count, head size/fence every 10 s via MESA_LOG_FILE).

### 2026-10-27 ~09:15-09:55 kernel59 (xnu 0183: GPU pool pressure) pp1
- 0183: crossing `sweetpoolwarn`% of the 940 MB pool (default 80) logs "pool: WARNING" and bumps a pressure counter; each v2 client drops its 0156 reuse cache on its next alloc/free and keeps none until use is 10 points under. CI run 40509335807 -> work/kernel59.
- pp1 (`sweetpoolwarn=40` = 376 MB to force it, 565 MHz, appsoak 1900 s): 5 WARNING lines at 1033 / 1561 / 1666 / 1982 / 2000 s (376-382 MB), each followed by "back under 30%" (272-278 MB) 1-4 s later. Drops: 5, all backboardd, 25/17/25/25/17 MB (109 MB). 108/108 opens, 0 hangs, 0 resets, 0 UIWDT, 0 "no bytes free". Pool 192 -> 314 MB, peak 382. ~10.8 swaps/s mean. Battery 4436 -> 4434 mV.
- Phone left on final59 (kernel59, default args, warning at 80%), home screen.

### 2026-10-27 ~07:55-09:00 kernel58 (xnu 0182: SMMU stall-on-fault by default)
- See [gpu](gpu.md) for this entry; it also touches this feature.

### 2026-10-27 06:29-07:31 sk57: 1 h app soak, kernel57 at 430 MHz (sweetgpufreq=4)
- See [gpu](gpu.md) for this entry; it also touches this feature.

### 2026-10-27 experiments.md lines
- 2026-10-27 pp1: kernel59 (0183 pool pressure) with sweetpoolwarn=40 (376 MB), 30 min app soak: 5 warnings (376-382 MB), each back under 30% within 1-4 s; backboardd dropped 17-25 MB of reuse cache each time (109 MB total); 108/108 opens, 0 hangs/resets, no allocation refused. Works.

### 2026-10-26 sk52b: 1 h app soak on kernel52 (clean)
- See [gpu](gpu.md) for this entry; it also touches this feature.

Older entries (before 2026-10-25): [archive/features/gpu-memory.md](../archive/features/gpu-memory.md).
