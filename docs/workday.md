
## [13:50] Wi-Fi join — Phase 1 (diff vs Linux ath10k): 3 fixes, same crash
- Fixed vs ath10k: pdev chain mask 0x3 from rf chains (INIT says 0x7), pmf/dynamic-bw/idle-ps/ani pdev params,
  resource config back to ath10k wcn3990 values (33 peers, skid 16, bcn offload 2, host_capab MGMT_BUNDLE_TX_COMPL),
  vdev NSS 2 after vdev create, WMI queue 8 → 32 (the extra commands had dropped vdev create).
- All accepted by fw (scan still works: 13–14 networks), but vdev start still asserts:
  `EX:wlan_process:0x1:WLAN RT:0x1084:PC=0xb01ab7b0`, modem ERR-FATAL. Tests 4 and 6.
- Bisect is effectively done: every command up to and including vdev wmm is fine; vdev start is the trigger.
- Side issues: in-place `wland` restart reset the phone once (test 5); test-6 boot lost USB/SSH for 10 min,
  watchdog returned it to fastboot. Joins now via reboot; recovery boot uses `sweetwifi=0`.
- wlan.conf deleted after every test, verified absent (last check 13:50 on k46).
- Next: Phase 3 — firmware debug log (WMI_DBGLOG_CFG + diag pipe) to see what vdev start does before the assert.

## [17:36] STOP — hard hang (demo boot k50, flamereboot=0)
- k50 (demo: sweettouch=2, sweetwifi=0, flamereboot=0) booted 17:10, USB link came up again at 17:15, the live
  log stops at 82 s uptime, phone answers neither ARP nor SSH, no fastboot. flamereboot=0 means no timed return.
- Needs a human: hold power (+vol down) to reach fastboot.
- Test 8 (scan only 2437 MHz) never ran: the phone was already unreachable when it started. The tests 6 and 8
  "hangs" earlier may be the same thing: phone alive per its own log but no USB networking.
- Wi-Fi Phase 3 so far: dbglog over WMI gives nothing (WCN3990 logs via diag). Firmware disassembled
  (llvm-mc hexagon). Crash function 0xb01ab640 = WMI event buffer-pool allocator; pool struct (0x934 bytes,
  malloc'd at init, global gp+0x6b0) has its header array pointers (+0x10/+0x14/+0x18) overwritten →
  firmware heap overflow from a neighbouring block, somewhere between init and vdev start.
- wlan.conf: not present at last check (k49, 17:0x); test 8 rerun never wrote it (phone unreachable).

## [18:56] STOP — iOS USB (USB 2) dead on every boot since ~17:15; fastboot (USB 3) fine
- PC: `usb 5-2-port2: Cannot enable. Maybe the USB cable is bad?` on every iOS boot (k51–k56).
- Not software: fails with Wi-Fi off, after `fastboot reboot-bootloader`, and with this morning's kernel14
  (GPU off) that ran all day. Port power-cycle and dock USB2 hub reset changed nothing.
- Needs a human: unplug/replug the cable (or another port/cable), full power-off of the phone.
- Found on the way, fixed: GPU pool range 2 began at 0xe1000000 = wland's rmtfs buffer (hyp-assigned to
  the modem). xnu 0150 starts it at 0xe1400000 (kernel26).
- No wlan.conf written since k49 (verified absent then).

## [20:00] Offline work while the phone is unreachable
- wland (CI 40041467448): ath10k station steps we skipped, behind /var/local/wlan-steps.conf (hex, default 0xf):
  bit0 STA_KEEPALIVE off (0x1c005, tags 0x7f/0x80), bit1 sta ps params (0x9002 tag 0x6d: rx wake WAKE,
  tx wake ALWAYS, pspoll NO_MAX), bit2 vdev RTS threshold 0xffffffff, bit3 vdev start in HT20 mode (5/4, was 11G/11A).
- tools/sunfish-wifitest.sh: guarded join test (WIFI_PSK via env only, lock, aborts if unreachable,
  always deletes + verifies wlan.conf, recovery boot with sweetwifi=0, log to logs/wland-NAME.log).
- Checked vs mainline (no difference): board file (byte-identical google_sunfish BDF), sunfish DT (no quirks),
  QMI cfg/host_cap/BDF/cal report, CE tables, TFTP serving, firmware-5.bin features.
- USB: sunfish HS PHY is QUSB2 v2 (mainline sm7150.dtsi), the driver ran flame's femto init on it.
  xnu 0151 (kernel27): GCC_QUSB2PHY_PRIM_BCR pulse + Linux qusb2 v2 table (sweetqusb=0 = old path).
  k57 (kernel27, 19:52): USB 2 still does not enumerate → needs the physical replug / cold power-off.

## [20:55] Wi-Fi join — **ROOT CAUSE FOUND, JOIN WORKS**
- Cause: we gave the WLAN firmware a 1 MB MSA region over QMI; sunfish's DT wlan_msa_mem is
  0x98d00000 + **2 MB**. The firmware heap's upper half was never assigned to it → its WMI event buffer pool
  (allocator 0xb01ab640) got corrupted → assert at the first heavy allocation (vdev start).
- Found by bisecting with wlan-steps probes: crash with no scan (k64), no HTT setup (k65), and no
  regdomain/pdev/chan list (k66) → only QMI/INIT/vdev left → compared MSA vs mainline sunfish DTS.
- Fix: wlfw.c MSA_SIZE 0x200000 (CI 40046754986). k67 probe: `vdev start resp: status 0`.
- k68 join test: auth 0, assoc 0 aid 1, 4-way handshake done, `*** joined 'Johann' ***`, rx frames to IP.
- Open: wland's own wlan.conf join (bridge mode) gets no IP on en0 — iOS (configd) only does DHCP for
  networks wifid joined. Next: join from Settings → Wi-Fi.
- wlan.conf deleted + verified after every test.

## [21:10] Wi-Fi — **ON THE INTERNET**
- Join from Settings → Wi-Fi works (wifid → /dev/wlan → wland): associated, state 4, link up.
- No IP at first: iOS's preferences.plist had no network set (written before en0 existed), so configd never
  made a Wi-Fi service and IPConfiguration never ran DHCP. tools/sunfish-netprefs.py adds the Wi-Fi (en0,
  DHCP) service + set and restarts configd (backup: logs/preferences.plist.bak-*).
- Result: en0 192.168.2.30/24, router + DNS 192.168.2.1, primary interface en0;
  nettest: DNS apple.com OK, TCP OK, HTTPS 200.
- Safari pages were black: WebKit's GPU process got no Metal device (only SpringBoard on /var/local/mtl2-clients)
  and has no CPU fallback. Added com.apple.WebKit.GPU + MobileSafari (sunfish-metal2-install.sh too).
  fast.com renders: 7.7 Mbps down, 8.9 Mbps up, 35 ms.

## [22:35] GPU stability — what actually freezes/blacks the screen
- **Black screens during app stress were Photos/Notes/Weather stuck at launch** (Photos waits on assetsd /
  PhotosFileProvider, which crash every launch). A hung frontmost app also blocks openapp switches.
  Not a GPU problem; test with Safari/Settings/Calculator.
- **The "zoom + freeze" and the later respring = GPU pool exhaustion in backboardd.** Pool 940/944 MB →
  backboardd's next Turnip command-stream BO refused → Turnip writes through cs->cur == NULL
  (libvulkan_freedreno+0x57b68, called from the bridge's RenderCommandEncoder::end_encoding) → backboardd
  SIGSEGV → the new backboardd gets OutOfMemory → no acceleration / black.
- **backboardd leaks pool memory on app switches**: 0154/0155 census — small (<256K) BOs only grow
  (≈340 per switch, ~7 MB), plus 10 MB render targets churn (alloc+bzero each time). 5 Safari page loads
  ≈ +280 small; 6 switches between the same two apps ≈ +2000 (linear: a leak, not a cache).
  Frontend telemetry: commits == completions (no command-buffer leak), 100 pipelines (stable).
  Critical memory pressure (tools: memwarn) frees only ~33 MB.
- Other fixes today: 0152 (hang = no CP progress for 2 s; GBIF-halt reset retry; recovery from an
  SMMU fault now works), 0153 (full ring waits in kernel; v2 error log), 0154/0155 census.
- A real GPU hang also happened once at 565 MHz (k70); none at 430 MHz (sweetgpufreq=4) so far.
- Next: find the small-BO owner (bridge runtime vs Turnip) — per-BO alloc call-site tagging; cap the
  render-target churn (reuse instead of alloc+bzero); then smoothness (CP_WAIT_FOR_IDLE, async waits).

## [23:05] Smoothness measurements + memory work (stopped at 16% battery)
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

## [00:40] Night run: where backboardd's GPU time goes
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

## [08:40] CA vsync: backboardd was never on IOMFB vsync
- QuartzCore (research/dsc, `ipsw dyld disass --image QuartzCore`): `IOMFBServer::set_vsync_enabled` subscribes to
  IOMFB vsync (sel 72, type 5) only when `CADeviceUseVBL()`; else it runs its own CADisplayTimer.
  `CADeviceUseVBL` = getenv("CA_FORCE_TIMER") != 0 || MobileGestalt ChipID in {0x7000, 0x7001, 0x8000-0x8004, 0x8010}.
  Our ChipID 0x8015 is not, so CA never saw our vsync. (Setting it via /var/local/mtl2-env is too late: dispatch_once.)
- Fix: launchd.plist (`/System/Library/xpc/launchd.plist`), backboardd EnvironmentVariables CA_FORCE_TIMER=1
  (original: /var/local/launchd.plist.pre-vbl). backboardd now logs "notifications type 5 on/off".
- Bench (kernel36, thread off): 21.6 swaps/s = 20.8 panel updates/s (no dropped swaps; was 17-19 panel, 19-27 swaps).
  With sweetfb_thread=1: 14.7 panel/s. xnu 0161 present-on-swap (kernel37, sweetfb_onswap=1): 14.1 panel/s. Both off.
- Present inline on the vsync tick still takes 10-17 ms wall (blit waits behind backboardd's next job): next lever.

## [09:50] Step 1A moot, step 2 (turnip 0105) failed
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

## [10:20] Step 3: BO census over app switches (kernel37, k3)
- Census with TU_DEBUG=bos on backboardd: idle 630 BOs/157 MB; after 6 app switches 1380/248 MB peak, settled 1318/204 MB; after 12 more switches 1335 then 1353 BOs (207, 208 MB). Drift ~1.5 BOs and ~0.2 MB per switch; GPU pool ~250 MB (peak 317).
- The earlier "340 BOs per switch" was cache warm-up (command-stream BOs), not a steady leak. Step 3 downgraded in plan.md.
- The phone auto-rebooted to fastboot once at the 1800 s flamereboot timer (expected, not a hang); rebooted as k3 at 3814 mV.

## [10:30] Step 1B (second ringbuffer): not feasible, no code
- sweet_a6xx.c sets up RB0 only and has no preemption support (no preempt records, CP_CONTEXT_SWITCH_*, pseudo-regs). A second ring would never be fetched without a software context switch; real preemption is a multi-day bring-up. Parked; alternative ideas (panel-size surface, CPU scaler, blit timing) are in plan.md step 1.

## [10:25] Step 1B skipped, step 4 charger dump (kernel38 = kernel37 + 0162, read-only)
- CI first skipped 0162 ("already exists in working directory"): `git add -A` in work/src/xnu had committed the whole untracked tree into the patch. Re-committed with only SweetBattery.cpp. In work/src/xnu always `git add` single files.
- Dump (docs/sunfish/charger-regs.md): status 3 (fast charge), charging enabled, FCC 2.0 A, USBIN limit 1.5 A, battery -70 mA at 3762 mV with screen on. TYPEC/MISC blocks not readable. Drain is the source's power budget, not a charger setup bug. Details in plan.md step 4.

## [10:45] Charger: Thunderbolt 4 port fixes the drain
- Moved the cable to the laptop's free TB4 port (240 W USB4 cable) while the phone was running: screen stayed on (32%, charging bolt) but the USB link died (RX errors 55604, 0 packets). Forced fastboot, rebooted as k38c with the cable left in place.
- Same charger registers (status 3, ICL 0x1e = 1.5 A, FCC 2.0 A). Battery: +968 mA idle at 3986 mV, +948 mA during the 60 s bench (tb4b: 24.1 swaps/s, 23.0 panel updates/s; tb4: 23.4 / 22.1). Before: about 0 to -250 mA. The source was the limit.

## [11:30] Step 0 (video spike), partial; new plan written
- Plan archived to docs/sunfish/plans/2026-10-24-present-path.md; new docs/sunfish/plan.md (video spike, per-frame timeline xnu 0163, backboardd GPU time, ordering).
- New tool phone/tools/vtdecode (AVAssetReader + VideoToolbox probe; userland CI). On kernel38 + current bundle:
  - H.264 and HEVC 1080x1920: VTIsHardwareDecodeSupported = 0; VTDecompressionSession with RequireHardwareAcceleratedVideoDecoder -> OSStatus -12906 (no hardware decoder, as expected).
  - VTDecompressionSession without that requirement -> OSStatus 0, UsingHardwareAcceleratedVideoDecoder = no: a software decoder exists for both codecs.
  - Decode fps NOT measured: AVAssetReader fails "Cannot Access URL ... sandbox extension was not issued" (file in /var/local/tmp or /var/mobile/Media/vt, run as root over ssh; an entitlements plist did not help).
- Safari to http://192.168.7.1:8000 (local test page) is refused: "not connected to the internet" (USB ethernet is not an iOS network). A Safari test needs Wi-Fi: run tools/sunfish-install-wifi.sh yourself (credentials stay out of git and out of chat), or measure decode in-process by feeding VT sessions from a parsed mp4 (not done).
- Webcam note: phone sits rotated 90 deg (top toward the right of the image); flame-cmd taps are in portrait panel coordinates; flame-shot returned a garbled image today.
- Phone is on the 1800 s reboot cycle (k38d booted 11:00).

## [11:30] Step 1 done: per-frame timeline (xnu 0163 on kernel39, boot args sweetpresentwait=1 sweettrace=1)
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

## [11:50] Present-mode comparison with the timeline (kernel39 / kernel40 = + xnu 0164 sweetpresentslots)
- Same Settings swipe bench, two runs per config, sweettrace=1:
| config | swaps/s | panel updates/s | ticks/s | present call p95 | put off |
|---|---|---|---|---|---|
| presentwait=1 (default) | 20.3 | 21.7-22.3 | 52 | 43 ms | 0 |
| presentwait=0 (nb1, nb2) | 24.5 / 24.4 | 14.4 / 13.7 | 59 | 1.4 ms | 514 / 523 |
| presentwait=0 slots=4 (s4a, s4b) | 22.4 / 23.0 | 15.0 / 14.8 | 59 | 1.4 ms | 336 / 407 |
- Non-blocking fixes the tick (59/s, present never blocks) and lifts swaps, but half the presents are put off because the blit waits behind heavy backboardd jobs (queue wait p95 114 ms); 4 slots recover only ~1 update/s. Default left at presentwait=1, slots=2.
- Next lever (plan step 2): the ~29 ms from backboardd's last GPU end to SwapEnd and the 40 ms GPU frames; neither is the present path.

## [12:30] Step 2: Turnip GPU trace under swipes (gt1, MESA_GPU_TRACES=print; kernel already running, 4184 mV charging)
- Bench with tracing: 19.3 swaps/s, 18.0 panel updates/s. Timeline: backboardd GPU/submit p50 9.0 ms, p95 39 ms; GPU end -> swap p50 26.7 ms.
- 1569 render passes: <5 ms 1212, 5-10 173, 10-20 18, 20-40 166, >40 0. All long passes are full-screen 1125x2436 GMEM (44 bins 288x224).
- 5 longest passes: 33-36 ms with only 21-92 draws; draw_ib_gmem is >95% of it, gmem load/store ~1 ms each, binning 0.1-0.3 ms. So a few expensive draws (fill/shader: likely blur or backdrop), not draw count or size.
- copy_buffer_to_image: 10018 copies, 3.5 s total (0.35 ms each); 44-50 copies (~10 ms) ride in the same submits as long passes.
- Follow-up (PC only): the trace has one draw_ib_gmem span per bin, no per-draw data. Slow passes are uniformly slow: nearly every one of the 44 bins costs ~0.7-1.3 ms (fast passes ~0.08 ms per bin), even with only 21 draws. So the cost is one or more full-screen expensive draws (blur/backdrop or large texture sampling), spread over the whole screen. Per-draw attribution needs per-draw timestamps in Turnip's trace.

## [12:10] turnip 0105 per-draw trace (metal2 run 40109072496, commit f09fdb5)
- pd1 bench (TU_SWEET_DRAW_TRACE=1 + MESA_GPU_TRACES=print): 17.6 swaps/s, 16.4 panel updates/s (tracing overhead). 4300 mV.
- Phone dropped to fastboot right after the bench, at kernel uptime ~1890 s: matches the 1800 s auto-reboot cycle, not a hang (fastboot answered, 4384 mV). Trace was not yet copied; rebooting with flamereboot=0 to fetch it.
- pd2 (per-draw trace, phone rebooted with flamereboot=0; bench 3.8 swaps/s with this tracing, so absolute times are inflated, shares are what count). 154 passes, 17,786 draw records.
- Slowest passes (~40 ms wall, ~26.5 ms of draw time, 45 bins): every expensive draw is a 6-vertex quad (full-screen layer), instances=1, blend0=0 (opaque).
  - Pass 126 top draws: idx16 fs 780adfdf 6.45 ms (16%); idx3 fs 666966cb 4.3 ms; idx0/1/2 all fs f8357eea, 3.2-3.7 ms each: the same opaque full-screen shader three times in a row, each later one covering the earlier.
  - Over all passes >20 ms: fs f8357eea 94.9 ms (1305 records), 780adfdf 58.2 ms (0.144 ms per bin-draw, 2x the rest), 93ffe710 26.4, 666966cb 17.1.
  - ~14 ms per slow pass is outside the draws (per-bin overhead/timestamp waits).
- Reading: overdraw of stacked opaque full-screen layers plus one heavier shader (780adfdf, blur candidate). Next: disassemble those fragment shaders to see what they sample, and check why CA stacks three opaque full-screen quads.
- [12:45] Stopped for the day (user unplugging). State: phone booted as pd1b (kernel40, sweetpresentwait=1 flamereboot=0, so no auto-reboot), mtl2-env empty (tracing off), turnip 0105 bundle installed (inert without TU_SWEET_DRAW_TRACE). Traces in ../logs/pd2-trace.log, gt1-trace.log; parsers in the session scratchpad (lost on reboot of the PC). Next: shader disassembly of fs f8357eea / 780adfdf.

## [15:00] New location; phone booted as "demo"
- Booted kernel40 (sweetpresentwait=1 flamereboot=0) at 4005 mV. UI fine on the webcam (home screen, 33% charging), kernel log shows 18-34 swaps/s at boot.
- USB network not reachable after boot (link flaps "link up" every second, ssh: No route to host, no fastboot). Phone work blocked until someone can reboot it by hand.
- Webcams here: Iriun = /dev/video0 (iPhone over USB), laptop camera = /dev/video1.
- Wi-Fi does not join at this location (user report); left for later.

## [15:40] Bridge drawlog (metal2 run 40123962779, commit 0472fb8, SWEET_DRAWLOG)
- dl1 bench with drawlog on: 23.4 swaps/s, 22.6 panel updates/s. After reset, dl1check gave 9.6 / 9.2 (not explained yet; re-check on a fresh boot).
- 20 big passes logged (../logs/dl1-drawlog.txt). The slow 17-draw pass: draws 0-2 = PBGRAXm_A2Xhf, opaque solid fill, three identical full-width quads; draw 3 = PBGRAXm_TimgA2Xhfn_Isrc, opaque wallpaper quad at x -185..1310 that covers the whole target. So draws 0-2 are dead (~10 of 26 traced ms). Draws 15/16 (fs 780adfdf) use programmable blend (Bdso/Bvcm, read the destination), so they can't be culled and don't cover anything.
- Every CA draw uses one uber shader (fixed_vert_lph_spc / fixed_frag_lph_cph) specialised per pipeline; a solid fill costs ~0.08 ms per 288x224 bin (~0.8 Gpx/s), so the shaders are heavy in general. Possible later lever.
- Next: SWEET_CULL in the frontend (defer the encoder's calls, skip draws under a whitelisted opaque full-target quad).

## [17:00-18:00] SWEET_CULL (bridge dead-draw culling), metal2 runs up to 40134633213 + debug
- Rounds: (1) only single-colour-target passes: CA's big pass has two (c0 + c1 1125x2436, c1 load DontCare) so nothing deferred; (2) required covering draws to overwrite all targets: CA pipelines write only c0 (opaque 1, att 3); (3) per-target rule (earlier draw dead if its targets are all overwritten); (4) scissor: CA redraws damage regions (e.g. sc 0,192,1120,2208), so covering is now per clip rectangle, and a draw is only culled if no surviving draw in between overlaps it.
- Final build works: replay dump shows the three PBGRAXm_A2Xhf fills (d6/d8/d10) culled under the wallpaper quad; no GPU faults; home screen correct on the webcam.
- But it does not help: 108 draws culled in 2700 passes (the fill-under-wallpaper pattern is ~1 pass in 25); swipe bench 19.7 / 19.7 swaps/s (19.0 panel updates/s) vs 18.6-22.8 without; backboardd GPU per submit 13.2-13.4 ms with culling vs 13.1-15.3 without; longest submit still ~40.7 ms. The per-draw trace (pd2) overstated these fills (per-bin timestamps add overhead).
- Left SWEET_CULL=1 in /var/local/mtl2-env for now (harmless, off by default in code). Debug lines (first-pass, not-covering reasons, replay dump) still in the build.
- Ops note: each CI bundle download is 278 MB; /tmp hit its quota after ~12 (files are read-only: chmod -R u+w before rm).
- [18:00] TU_DEBUG=sysmem under swipes (sys1/sys2): 15.9 / 13.3 ms per backboardd submit, longest 40-45 ms, 18.9 swaps/s: same as GMEM, so tile overhead is not the cause.
- Reading: a full-damage redraw stacks ~10+ full-screen layers (~40 Mpx) and CA's uber shader shades at ~1 Gpx/s, which is ~40 ms. The lever is shader cost. IR3_SHADER_DEBUG=disasm + MESA_SHADER_CACHE_DISABLE only dumped Turnip's internal blit shaders (CA pipelines come from the m2v cache; disasm goes to stderr). Next: SWEET_PIPESTATS (VK_KHR_pipeline_executable_properties per CA pipeline label), being written.

## [18:40] Bridge 0103 SWEET_PIPESTATS (VK_KHR_pipeline_executable_properties) works
- First build logged nothing (cause not found by reading); the version that writes straight to mtl2.log works: device enabled, properties Ok(3), statistics Ok(28) per pipeline.
- 118 fragment pipelines after a restart + swipes: FS instruction count min 0, p50 57, p90 211, max 314 (the solid fill is 5 instructions; downsample_8_frag_lph blur 211-282, 16-19 registers, 10-12 waves).
- Half-registers_used=0 in every pipeline, although CA's shaders are the "_lph" (low-precision half) variants: the bridge's translation (metal2vulkan) or Turnip turns all half math into 32-bit. A6xx runs half at 2x ALU rate with half the registers.
- Reading: the solid fill is not shader-bound (5 instr) -> fill/ROP rate for those; the textured/blur layers are ALU-bound at full precision. Next: find where half precision is lost (AIR -> SPIR-V: Float16 or RelaxedPrecision; ir3 lowers mediump only for RelaxedPrecision) and keep it.
- Labels are missing from the PIPESTAT lines (state N shows only function names).
- [19:10] Bridge 0104 METAL2VULKAN_HALF_RELAXED (half values as RelaxedPrecision, skip per-op half rounding; metal2 run 40139944623). Env reaches backboardd, but no effect: 59 FS pipelines, Half-registers_used=0, instruction counts unchanged (p50 57, p90 212). Benches: on 18.9 / 19.7 swaps/s (13.0 / 13.5 ms per submit), off 20.3 / 19.7 (13.3 / 13.2 ms). No faults, screen fine. Being debugged (diagnostics added to 0104). Note: toybox has no killall; restart backboardd with psx + kill as sunfish-m2env-bench.sh does.
- [19:18] Demo boot for the user: kernel40, sweetpresentwait=1 flamereboot=0, mtl2-env cleared (all bridge diagnostics off). 0104 diagnostic test was stopped before it ran; resume it next session (scratchpad half-diag.sh logic: build is metal2 latest, set SWEET_PIPESTATS=1 + METAL2VULKAN_HALF_RELAXED=1, grep 'half-relaxed').

## [20:00] Half precision: the "Half-registers_used=0" premise was wrong
- 0104 diagnostics: half_relaxed()=true; backboardd translated=50 round_skipped=389 marked=993 decorated=990; SPIR-V dump (METAL2VULKAN_SPV_DUMP) shows 6-66 RelaxedPrecision decorations per fragment module. Turnip 85fda3b sets mediump_16bit_alu.
- ir3 disassembly (IR3_SHADER_DEBUG=disasm, MESA_SHADER_CACHE_DISABLE, caches bypassed): with 0104 on, 880 f2fmp / 503 f2f16 / 810 half-register operands; with it off, 498 f2f16 / 4088 half-register operands (bigger log). So ir3 already runs much of CA's half math in 16 bit, and the PIPESTAT "Half-registers_used" statistic reads 0 on a6xx (merged register file), which misled the plan.
- 0104's real effect is the skipped per-op rounding (FS p50 57 -> 43-68 depending on the pipeline mix); benches showed no gain (on 18.9 / 19.7 swaps/s, 13.0 / 13.5 ms per submit; off 20.3 / 19.7, 13.3 / 13.2 ms). Leave 0104 off (default).
- Phone reset: mtl2-env empty, no mtl2-tu-debug. backboardd caches moved aside on the phone: /var/local/m2v-cache/backboardd.bak (original) and backboardd.half (half on); the live one was rebuilt.

## [20:30] Step 3: GPU end -> swap gap (29 ms)
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

## [21:30] xnu 0166 CPU sampler works (kernel42, clean CI run 40150300651)
- Earlier boots tested nothing: the patch re-added osfmk/kern/thread.c as a new file (untracked in work/src/xnu), dxb skipped it ("already exists"), and my check grepped "SWEET CPU:", which SweetCPU.cpp also prints. Fixed: helper in osfmk/arm64/cpu.c, sampler driven from sweet_gpu.c's 5 s stats tick, check for "sampler started". New files need -f clean=true.
- Fresh boot, sweetpresentwait=1 sweettrace=1 sweetcpusample=1, swipe benches cs7/cs8: 18.9 / 19.3 swaps/s (18.0 / 18.6 panel).
- backboardd tid 576 (the IOMFB vsync/render thread): 0.66-1.85 s CPU per 5 s window during swipes, ~10-15 ms per frame at 19-25 fps; 55-75% of it already on the A76s. Next threads: tid 934 0.17-0.43 s, tid 544 / 931 ~0.1 s. SpringBoard tid 256 (main) 0.22-0.48 s per 5 s.
- Per-CPU busy% during swipes: c0 7-18, c1-c5 0-12, c6/c7 8-24. The CPUs are mostly idle; the GPU is ~30% busy.
- Reading: neither CPU nor GPU is saturated and the render thread needs only ~10-15 ms per frame, so the ~29 ms GPU end -> swap gap and the ~20 fps are waiting/pacing, not work. A big-core scheduler would save at most a few ms per frame. Open: the completion path (bridge handler thread + dispatch hop to CA) is the one untested hop.
- Caution: later benches (ctc1/ctc2/notel1/notel2, 7-10 swaps/s) ended on the widgets page (blurred widget materials): the swipe position drifted, so those runs and the mtl2 telemetry ctc numbers are not comparable. Reboot before benching.

## [21:55] Completion-path stamps (metal2 SWEET_CTL + timeline --ctl), fresh boot kernel42, sweetpresentwait=1 sweettrace=1
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

## [22:30] GPU busy vs wait, Reduce Transparency, DDR vote (kernel42, sweetpresentwait=1 sweettrace=1)
- **Busy vs waiting (existing timeline, ctlA/ctlB/cs7/cs8):** no other span overlaps any backboardd CP span (0 of ~620 per bench), so the 13-40 ms is that submit's own work, not interleaving. Long submits are tightly clustered: ~11.5 ms (10-20 bucket) and 38.6 ms p50 / 40.4 p95 (>=20 bucket); they queue 61-74 ms (p50) before CP start. GPU busy 25-37%; only 39-90 of ~620 spans start back-to-back. The trace cannot tell busy from stalled inside a span; the near-constant 38.6 ms is suspicious (fixed cost or a wait quantum). Needs RBBM busy perfcounter or a CP timestamp per render pass.
- **Bus vote:** `tools/sunfish-boot.sh` passes `sweetddr=0` by default (set 10-23 when the DDR vote cost panel updates on the old present path), so the GPU trail had no "bus vote" lines. With `sweetddr=1`: "bus vote MC0 top -> 0", "bus vote SH0 top -> 0".
- **Baseline (rtbase1/2):** 21.0 / 21.4 swaps/s, 20.1 / 20.3 panel; backboardd ~16.5 / ~17.0 ms/submit, longest 41.9 / 40.7 ms; timeline <=17 ms 19.8 / 23.8%, <=34 ms 62.5 / 64.3%; bb p50/p95 5.8-5.9 / 39.9 ms; GPU end -> swap p50 27.8 / 29.3 ms.
- **Reduce Transparency** (`EnhancedBackgroundContrastEnabled=1` in com.apple.Accessibility, backboardd restarted): dock turned opaque grey, but backboardd logged "vertexFunction is not a MTLFunction" / "fragmentFunction is not a MTLFunction" (CA's RT shaders do not build through the bridge), then ~3 s later "present: GPU hang or fault (fence 4375, done 4367)" and recovery failed ("ring me_init ... TIMEOUT", "GBIF halt + soft reset + CP init -> -1"). rton1/rton2 (24.6 / 23.6 panel) are invalid (GPU dead, no GPU stats). Pref removed (cfprefsd had cached it; killed cfprefsd, deleted key), fresh boot rt-clean healthy: 19.1 swaps/s, 18.1 panel, ~15-17 ms/submit. Blur cost NOT measured this way.
- **sweetddr=1 (ddr1a/ddr1b):** 17.9 / 17.9 swaps/s, 17.3 / 16.9 panel; backboardd 13.2 / 13.0 ms/submit (more submits, mean drops) but p50/p95 unchanged 5.7 / 40.0 ms, longest 40.8 / 40.9 ms; <=17 ms 21.1 / 22.3%, <=34 ms 62.2 / 63.1%; blit 2.37 ms; GPU end -> swap 29.6 / 29.7 ms. The 40 ms submits do not move with DDR bandwidth -> not bandwidth-bound; panel rate ~3/s lower. Keep `sweetddr=0`.
- Battery 4432-4456 mV throughout. Phone left booted on ddr1 (sweetddr=1); next boot is back to default.

## [2026-10-25 11:30] Wi-Fi on "Epik Wifi" (parents' OpenWrt) — joins and works
- Networks (read-only SSH, routers untouched): R1-Dlink ch 1 / ch 100 (DFS), R3-One ch 6 / ch 149; both `psk-mixed`
  (hostapd wpa=3, CCMP only, AKM WPA-PSK + FT-PSK, 802.11r/k/v on, PMF off).
- Boot epik0 = kernel42 `sweetpresentwait=1`, 4392 mV; then `tools/sunfish-wifitest.sh 40151712848 epik1`
  (WIFI_SSID='Epik Wifi', psk via env), 4459 mV.
- wland picked R1 2.4 GHz ch 1, -64 dBm: auth 0, assoc 0 (aid 4), msg 1 -> msg 2, keys installed (GTK idx 1), joined.
  hostapd: AP-STA-CONNECTED auth_alg=open, EAPOL-4WAY-HS-COMPLETED. Stats after 90 s: tx 2635, 0 failed.
- nettest: DNS apple.com 45 ms, TCP 51 ms, HTTPS 200 in 374 ms. wlan.conf deleted + verified absent.
- So the radio/WPA stack handles wpa=3 + FT-PSK beacons. Not tested: the join from Settings → Wi-Fi (wifid path),
  5 GHz BSSes, roaming between R1 and R3.
- 11:44 epik2 (kernel42 `sweetpresentwait=1 sweettouch=2 flamereboot=0`, 4456 mV): join from Settings → Wi-Fi,
  password typed with flame-cmd taps (webcam-guided). wifid ASSOCIATE: any access point, auth 0x1/0x10, 32-byte
  key kind 6 (PMK); bridge "join 'Epik Wifi' (PMK) -> 0"; R1 2.4 GHz ch 1 -64 dBm; 4-way OK; join over in 0.47 s.
  Settings shows the checkmark and the status-bar Wi-Fi icon. nettest: DNS 122 ms, TCP 211 ms, HTTPS 200 (401 ms).
- The user's earlier endless spinner did not reproduce at -64 dBm. Open: weak-signal joins (user was in a weaker
  spot), 5 GHz BSSes (ch 100 DFS, ch 149), roaming R1 <-> R3. Note: epik1's `flamereboot=600` rebooted the phone
  to fastboot after 10 min.

## 2026-10-25 soak0 — baseline stability soak (plan step 0), aborted at ~16 min
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

## 2026-10-25 soak1 — short app soak with stall capture (plan step 0, run 2)
- Same boot (soak0r, kernel42). Loop: Preferences → Safari → Notes → Calendar, 10 s each with 3 swipes, a webcam snap after every app. Auto-stop on 2 consecutive `SWEET FB 5s` samples < 1 fps, or on a failed open.
- 12:39-13:01 (uptime 788-2086 s): **no stall.** All 48 opens succeeded (no Code=5), and the home screen is fine on the webcam at the end.
- Swaps/s per minute: 8.7-17.1, mostly 10-13. That is lower than the ~20 of the swipe bench, because of the app launches. 12 isolated samples were < 1 fps, never two in a row.
- 104 `not a MTLFunction` lines and 2 `EXC_CORPSE` lines, without a stall. So those errors alone don't freeze the UI.
- Not reproduced: soak0's stall came about 4 min after a fresh boot, with 6 apps, including Clock and Photos. Suspects now: the first-boot launch burst, or the Clock/Photos apps (PhotosFileProvider crashes on every launch).
- The workload script is saved as `tools/sunfish-appsoak.sh`.

## 2026-10-25 soak2 — fresh boot, 6 apps (plan step 0, run 3)
- Fresh boot soak2 at 13:02: kernel42 `sweetpresentwait=1 sweettouch=2 flamereboot=0`, 4456 mV.
- Loop: Preferences, Safari, Notes, Calendar, Clock, Photos; ran 13:12-13:34 (20 min). The loop started ~10 min after boot, not right away.
- **No stall.** The home screen was fine at the end. Swaps/s per minute: 7.4-11.7.
- Counts after the marker: 0 `not a MTLFunction` (soak1 had 104), 0 `EXC_CORPSE`, 0 Code=5.
- The Photos step snap is black apart from the home indicator. Photos opens to a black/empty view; check whether it renders at all.
- The `=== soak2 stall` marker at 13:04:51 is false: the agent's check was inverted, and the phone was healthy at ~10 swaps/s.
- soak0's stall (~4 min after boot, Code=5 on every open) did not reproduce in 2 runs (40 min). It is rare, or tied to something in soak0's first minutes (the Safari surface burst, 160 → 293 MB).

## 2026-10-25 GPU hang recovery — xnu 0167, kernel43 (plan step 1)
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

## 2026-10-25 Plan step 2: hang triggers
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
- **[17:02] rst3: a raw disk read resets the phone, reproducibly.**
  - Boot: kernel50, `sweetpresentwait=1 flamereboot=0`. Booted fine at 4454 mV; snaps OK.
  - The phone has no `dd` (/var/local/bin/dd → toybox without dd).
  - I ran `toybox xxd -s $((0xc80ffb-1153)*4096) -l 64 /dev/rufsroot0`, i.e. a read 64 bytes long at the rescue-block offset computed from NOTES.md's flame userdata size.
  - The phone dropped to fastboot instantly. The live log stops at 159.0 s, with no panic line.
  - That offset is probably past the end of sunfish's userdata (the size is unknown on sunfish). **An out-of-range or raw read on rufsroot0 kills the kernel at once and silently.**
  - The soak resets look the same (instant stop, no panic). A UFS I/O fault while apps load is now a suspect.
  - Next: find sunfish's real userdata size (shim log "ufs: userdata first= blocks=", or the GPT from backup/), and look at how the XNU UFS driver handles an out-of-range LBA or a UFS error (sense data, timeouts).
- **UFS investigation (no phone use):**
  - **Partition size:** sunfish userdata is 0x1B65FFB000 bytes (`fastboot getvar partition-size:userdata`) = 0x1B65FFB 4 KB blocks. So the rst3 read offset was inside the partition. The rescue area starts at block 28728186 of /dev/rufsroot0 (1153 = 1025 XNU log ring + 64 scratch + 64 rescue, shim/ufs.h).
  - **Not out of range:** sweet_ufs_strategy bounds-checks (EINVAL beyond part_bytes). Timeouts return ETIMEDOUT and SCSI errors EIO; there is no panic on either path.
  - **Likely cause of the rst3 reset (~65%):** for raw (physio, B_PHYS) I/O, sweet_ufs_strategy memcpy's from the bounce buffer straight into the user virtual address (sweet_ufs.c:448-449, write at 459). With PAN on, that is a data abort → panic, which never reaches the UDP log. The watchdog then bites into fastboot.
    - Fix: copyout/copyin for B_PHYS.
    - Also: an LBA range check in ufs_read_blocks, log the partition geometry, and more state on timeouts.
  - **The soak resets have no UFS lines before them** (no timeouts or errors), so they are probably a different cause. Still open.
- **[17:55] rsc1 (kernel51, xnu 0175): raw reads no longer reset the phone.**
  - The boot-args line was logged at "2815 s": uptime did not reset across the last reset, but it is a fresh boot. The partition-size line does not reach the live log.
  - `toybox xxd -p -s OFF -l N /dev/rufsroot0` at offset 0 and at the rescue block (28728186) works, and the phone stays up. The first rescue block starts with `SWEETRSC`, total 0x4e093 bytes.
  - **New bug:** xxd -p reads the raw device 30 bytes at a time, and every sub-block read returns the start of its 4 KB block. So the rescue dump is garbage: the header repeats every 30 bytes.
  - The decoded fragments show a panic report around 159.8 s, the rst3 xxd reset (lr 0xfffffe0007863940), so rst3 was a panic. The panic string is not readable.
  - /dev/ufsroot0 (the block device) is busy because it is mounted, and xxd's -c is limited to 256.
  - Next: make the raw read path honour the offset within a block (or reject unaligned raw I/O with EINVAL), or add a phone tool that preads whole 4 KB blocks.
- **[18:10] Rescue log read with kernel51 (no new boot, so it was not overwritten).**
  - The shim rescues the previous kernel's RAM log on every boot. Read the rescue before rebooting, or it is lost.
  - Method: `toybox cmp -l /dev/rufsroot0 /dev/zero OFF 0 | toybox head -n 262144`. cmp seeks and reads large aligned chunks and prints each non-zero byte with its position; rebuild on the PC (../logs/rsc-good.txt).
  - **rst3 was a panic, as suspected.** `panic(cpu 0 ...): Kernel data abort at pc 0xfffffe0007bce8b8, lr 0xfffffe000751b710`, esr 0x9600004f (permission fault), far 0x100767918 = x0 = the user buffer of `toybox xxd`. It happened at 159.79 s, 10 ms after `exec toybox`.
  - That is the PAN fault on memcpy into a user address, fixed by 0175. The rescue-area layout and the decode both work.
  - The soak resets' rescue data was overwritten by later boots, so they are still unexplained. Next soak: on a reset, read the rescue with this method right after the next boot (kernel52 also fixes unaligned raw reads, xnu 0176, built and not booted yet).
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

## 2026-10-26 sk52b: 1 h app soak on kernel52 (clean)
- Boot: kernel52, `sweetpresentwait=1 flamereboot=0` + defaults (recover=2, kill=1), 3978 mV at start.
- `sunfish-appsoak.sh sk52b 3600`: 18:18:17 to 19:19:55, 35 cycles x 6 apps = 210 opens, 0 openfail.
- 0 GPU hang/fault, 0 recoveries, 0 panics, 0 resets. First full hour without a reset since 0175/0176.
- GPU pool: 198 MB at start, 319 after 10 min, then flat at 373-424 MB (saw-tooth), 410 at end.
- Average 11.0 swaps/s over 601 stats lines (app-open mix, not a swipe bench).
- Battery on TB4: +965 mA charging early on (chg=1).

## 2026-10-26 ~19:20-19:35 nbproto bridge + Reduce Transparency repro (kernel52, sk52b boot)
- Installed work/metal2-nbproto bundle (class_addProtocol NBFunction/NBLibrary): "is not a MTLFunction" lines after backboardd restart: 0 (was thousands per boot).
- RT run 1 (rtj): `EnhancedBackgroundContrastEnabled=1`, cfprefsd killed, backboardd restarted, 90 s on the lock screen only: no hang. pipelines.log 8664 -> 8790; drawlog 40 passes.
- RT run 2 (rtj2): unlocked, 12 home swipe pairs, dock pull-up, one app open, ~164 s: 0 hang/recover lines. Swap rate fell from 21.8 to 5.8/s during swipes, avg present copy 22-38 ms (RT is slow, not hung). pipelines.log 8935 -> 9165; drawlog 20 passes. Webcam: dock and search pill drawn dark opaque (RT looks active, not certain).
- Undo both times: key removed (plist 494 B), SWEET_DRAWLOG removed, 21-26 swaps/s, home healthy.
- Reading: the 2026-10-24 RT hang (kernel42, old bridge) did not reproduce on kernel52 + nbproto bridge. Cause unproven (old bridge, the pre-0174 clock state, or chance). Logs: ../logs/rtj*-{pipelines,drawlog,live}.

## 2026-10-26 sk430: 1 h app soak at 430 MHz (kernel52, sweetgpufreq=4), step 2b
- 19:53:05 to 20:54:48, 35 cycles, 210 opens, 0 openfail, 0 resets, 0 panics. Core 429-430 MHz.
- **1 real GPU hang, NOT recovered** at uptime 2296.098 (present: fence 46266, done 46247, smmu fsr 0; rb rptr 570 wptr 949, ib1 rem 7e, rbbm cf9495).
  - recover2 ran every stage OK (cp stop, GBIF halt, sw reset, gx off/on, DCVS, pll0 restore "locked"), then `ring me_init ... status c00015 cp_status1 81104006 TIMEOUT`; GBIF-halt fallback timed out the same way; FAILED after 3.94 s.
  - The UI kept going without the GPU: "GPU present not used: GPU blit failed", swaps resumed at 2303 s with CPU panel copies (~2.4 ms), 22-27 swaps/s, no more `SWEET GPU: 5s` lines. The soak finished all opens on that path.
- Pool: 198 MB at 121 s, 317 / 325 / 312 at 10-min marks, peak 399, 348 at the hang.
- Battery 4078 -> 4390 mV, +870 mA (charging).
- **565 vs 430:** sk52b (565) 0 hangs in 1 h; sk430 1 unrecovered hang in 1 h. 430 is not safer -> keep 565 MHz (sweetgpufreq=5) as default. Pre-hang swap rates are not comparable (app-open mix).
- New open item: recovery after a real (not injected) hang fails at CP me_init; injected hangs (rec10) recovered. Next: compare the CP/GMU state on that path with the injected one.

## 2026-10-26 Why recovery fails after a real hang (investigator, logs + code only)
- Real (sk430 2296 s) vs injected (kernel50, uptime ~190): identical through sw reset, GX off/on, pll0 restore (both locked). Differences: real hang had no SMMU fault (fsr 0), pending ring work (rptr 570 wptr 949, ib1 rem 7e), rbbm cf9495, cp_status1 8113xxxx, two extra gpucc diffs (+1534 0->1, +1554 1->0); injected = SMMU fault fsr 2, ring empty, rbbm 800005, gets "clearing smmu cb1 fault" + TLB flush.
- After GX on both show rbbm 1 / cp_status1 1000000 (idle); real me_init then times out with rptr 0 (CP never fetched the ring): status c00015, cp_status1 81104006. Same signature as pre-0167 soft-reset failures, so the stuck state survives a GX power cycle.
- Leading theory (~40%): stuck state in the CX domain (GPU SMMU context bank / TBU / GBIF) after an in-flight mid-IB hang; the injected path only recovers because the fault handler clears the SMMU. Our "full" recovery is a GX-only cycle with the GMU running; Linux does a full GMU/CX collapse + re-boot.
- Fix candidates: (a) always reset SMMU CB1 (SCTLR.M off, clear FSR, TLBIALL+SYNC, on) and GBIF before gpu_cp_init; (b) escalate to CX collapse + a6_power_up if me_init still fails; (c) log CB1 SCTLR/FSR/TLBSTATUS, GBIF halt/outstanding, CP_RB_BASE/SQE base, gpucc +1534/+1554 before me_init.
- The bad-IB injector (SMMU fault) cannot reproduce this. Need a hang inside valid memory: CP_WAIT_REG_MEM on a never-written dword, or an endless shader.

## 2026-10-26 ~21:00-21:50 kernel53 (xnu 0177) stall-hang recovery, 565 MHz
- 0177: `sweethangtest` bit 30 = stall IB (CP_WAIT_REG_MEM on a never-written scratch dword, no SMMU fault); SMMU CB reset (SCTLR.M off, FSR clear, TLBIALL+SYNC, restore) + GBIF halt release before every recovery gpu_cp_init; `recover2: pre-me_init:` diagnostics. Hex boot-args parse fine.
- 5 boots (st1-st5; 3 present 0xC0000600, 2 client 0x40000BB8): 5/5 injected, 5/5 "recover2: done" in 26.0-26.4 ms, me_init idle, display back ~6.7 s after the hang; home screen on webcam after unlock; 16.6-17.8 swaps/s and 12.5-12.7 ms/submit on the home screen after (st4, st5).
- Hang state: fsr 0 (no fault, as intended) but rptr == wptr, ib rem 0, rbbm 800005 (one c00014): NOT the real-hang state (sk430: rptr 570 wptr 949, ib1 rem 7e, rbbm cf9495). Pre-reset CB state every time: sctlr 67, fsr 400, tlbstatus 0, GBIF halt 0 ack 1.
- Open: whether the reset is what helps. Control (sweetsmmureset=0, xnu 0178 / kernel54) next.

## 2026-10-26 ~22:00 Control: kernel54 (xnu 0178) sweetsmmureset=0, stall hangs, 565 MHz
- ctl1/ctl2 present stall (0xC0000600), ctl3 client stall (0x40000BB8): 3/3 injected, "smmu/gbif reset: skipped", me_init idle (status 1, cp_status1 1000000), recover2 done 25.96-26.41 ms. 'SWEET GPU: 5s' lines continue.
- So the stall injector recovers WITH and WITHOUT the SMMU/GBIF reset: it does not reproduce the real-hang failure, and 0177's reset is neither proven nor disproven. Kept on by default (harmless: +0 ms).
- Difference to the real hang (sk430): the real one had ring work pending (rptr 570, wptr 949), ib1 rem 7e and rbbm cf9495 (many blocks busy). The stall leaves the ring drained and rbbm 800005/c00005.
- Also built: kernel55 = xnu 0179 UI watchdog (`sweetuiwdt=30`: touch within 10 s + no swap for 30 s -> kill backboardd; still frozen 30 s later -> stop APSS pets -> fastboot; backboardd also killed once after a failed GPU recovery). CI ok (run 40435125065), not booted yet.
- Brightness: at 3/5/8/12% the webcam sees nothing; icons show from 16%. Phone left on a night boot (kernel53, no injection) at 5%; 4456 mV.

## 2026-10-26 ~22:14-22:24 kernel55 UI watchdog (xnu 0179) test (uw1)
- "SWEET UIWDT: armed, 30 s" at 27.7 s. No trigger in 90 s idle or during 6 swipe pairs.
- Rung A works: backboardd SIGSTOPped at 241.1 s (last swap) with taps every 5 s -> kill at 271.1 s (30.0 s), new pid, "swaps resumed" at 285.9 s. A second kill at 320.9 s fired after a real 30 s freeze of the respawned backboardd (taps arriving), recovered again.
- Rung B not reached: the respawned backboardd swapped before it could be stopped, which resets the rungs (by design: a respawn that draws counts as recovered). Rung B (stop pets -> fastboot) untested; to test, stop backboardd's respawn before its first swap, or add a debug arg.
- Phone left on night2 (kernel53, no injection, 5%), 4456 mV.

## 2026-10-26 ~22:35-22:43 kernel56 (xnu 0180) SMMU stall-on-fault injector
- `sweethangtest=0xA0000600`: present blit from unmapped VA with CB SCTLR CFCFG (stall on fault). Hang: smmu fsr 40000002, rbbm status1 10301 status2 10100 status3 1f00101 (bit 24 SMMU stalled), cb fsr raw 40000402.
- **ss2, reset off (sweetsmmureset=0): reproduces the real-hang failure exactly**: me_init `rptr 0 wptr 9 status c00015 cp_status1 81104006 TIMEOUT` on every path, "GBIF halt + soft reset + CP init -> -1", recovery FAILED, UI on CPU copies (same as sk430).
- **ss1, reset on: recovers, but slowly**: "smmu stalled fault terminated (CB_RESUME)" seen; first me_init still TIMEOUT (c20015), GBIF-halt path TIMEOUT (c20001), then soft reset + CP init idle -> "recover2: done after 2618248 us". GPU 5 s lines continue.
- Reading: a stalled SMMU transaction is enough to make CP init fail; terminating it (CB_RESUME) is what lets the CP come back. The resume comes too late in the full path (after GBIF halt + GX cycle), so the first attempts still fail. The real sk430 hang printed fsr 0 in terminate mode, so whether it was an SMMU stall is not proven, but the failure signature is identical.
- Phone on night3 (kernel53, 5%), 4455 mV.

## 2026-10-26 ~22:50-23:06 kernel57 (xnu 0181: CB_RESUME at the start of recovery) — inconclusive
- er1, er2 (`sweethangtest=0xA0000600`, boot-args tail confirmed): no "hangtest: injecting" line in 4 min of swipes each, so recovery was never exercised. 0181 only touches sweet_a6xx.c recovery code, not the arm path, so the cause is likely the run (present submit 1536 not reached: swaps dropped to 0.2/s around 426 s; possibly not unlocked), not the kernel. Not verified.
- Next: rerun er with a lower present index (e.g. 0xA0000200) and confirm the home screen first.
- Phone left on night4 (kernel53, 5%), 4435 mV.

## 2026-10-26 ~23:15 kernel57 (xnu 0181) er3: early CB_RESUME fixes the stall-on-fault recovery
- `sweethangtest=0xA0000200` (present submit 512): injected at 83.7 s, fault at 85.8 s.
- "recover2: early smmu resume: fsr 40000402"; after GBIF halt rbbm status3 4f20101; first me_init idle (rptr 9 wptr 9 status 1, cp_status1 1000000), zap rc 0; **recover2 done after 26308 us** (kernel56 with the late resume: 2.6 s on the 3rd path; reset off: FAILED). backboardd killed and respawned; 'SWEET GPU: 5s' continues; lock screen at 5.6-7.8 swaps/s after (pre-hang home ~19).
- One run only (er1/er2 at index 1536 never injected). Phone left on night5 (kernel53, 5%), 4435 mV.
- Open: does the real hang (sk430, terminate mode, fsr 0) also clear with 0181? Next real-hang soak on kernel57 at 430 MHz will tell; consider enabling CFCFG (stall mode) in normal operation so real faults look like this injector.

## 2026-10-27 kernel57 (xnu 0181) er4-er7: early CB_RESUME recovery repeats
- Same args as er3 (`sweethangtest=0xA0000200`). Injected at 110.2 / 152.6 / 139.9 / 153.5 s; SMMU fault ~2.0-2.15 s after each injection; fsr 40000402 every time.
- er4 / er5 / er6: first me_init idle (rptr 9 wptr 9), first path, **26.06 / 26.10 / 26.24 ms**. er7: backboardd respawned at 155.6 s and screen was back on the webcam, but the me_init / "recover2: done" lines are missing from the live log (lost lines), so its path is unproven.
- Together with er3: 5/5 recoveries, 4 of them timed at ~26 ms.
- Open: swaps 25-30 s after recovery were 6.1 / 6.7 / 8.2 / 6.1 per s against ~19 before (rec10 on kernel50 was back to 19 at full speed). Could be the lock screen after the respawn rather than a slow GPU; check in the next soak (+2-3 min window).
- er7 boot also logged "SWEET USB: busERR addr 0; SMMU cb fsr 400" every ~5.6 s from 43 to 105 s (not seen on er4-er6).
- Phone left on kernel57, no injection, 100% brightness, 4456 mV. Next: sk57, 1 h app soak at 430 MHz (running).

## 2026-10-27 06:29-07:31 sk57: 1 h app soak, kernel57 at 430 MHz (sweetgpufreq=4)
- 61.6 min, 35 cycles x 6 apps: 210/210 opens, 0 resets, **0 GPU hangs** (sk430 on kernel52 had 1 in the same mix). So the real-hang test of 0181 is still open.
- Pool 161 MB at 89 s -> 345 MB at end, peak 444. Battery 4455 -> 4428 mV. Mean 9.0 swaps/s over the run (includes lock/idle time).
- Noise: 411x "Failed to send exception EXC_CORPSE_NOTIFY ... pid 1148" (from 821 s), 273x apple80211 CURRENT_NETWORK error 6.
- Phone left on kernel57 at 565 MHz (sweetpresentwait=1 flamereboot=0), home screen.

## 2026-10-27 ~07:40-07:50 uw2: UI watchdog rung B (kernel57) + new boot default
- Freeze: a phone-side loop `psx -k backboardd 17` (SIGSTOP) every 0.1 s, started detached (`(sh /var/local/freeze.sh &)`; there is no nohup/pgrep/proc on the phone), so each respawned backboardd stops before its first swap. Taps every 5 s from the PC.
- Last swap ~216 s. **Rung 1** "killing backboardd" at 246.2 s; **rung 2** "still frozen: stopping watchdog pets" at 276.2 s (+30.0 s). The kernel log ran on to 325.6 s (~49 s pet-stop to bite), then the phone was in fastboot at 07:44:40. Works as designed.
- Two earlier attempts by the agent did not run the test (no pgrep; then an accidental SIGCONT + SIGTERM). psx takes a numeric signal: 17 STOP, 19 CONT, default 15 TERM.
- `tools/sunfish-boot.sh` default changed: `flamereboot=1800` -> `flamereboot=0 sweetuiwdt=30` (kernels before 55 ignore sweetuiwdt, so on those there is no auto-reboot at all now).
- Booted uw2b (kernel57, sweetpresentwait=1, new defaults): "UIWDT: armed, 30 s", home screen on the webcam, 4456 mV in fastboot.

## 2026-10-27 ~07:55-09:00 kernel58 (xnu 0182: SMMU stall-on-fault by default)
- 0182: GPU context bank SCTLR gets CFCFG (0x80) at setup and again after the recovery's SMMU/GBIF reset; a6_clear_fault terminates a stalled fault (CB_RESUME) before clearing FSR. Off with `sweetsmmustall=0`. CI run 40499063303 -> work/kernel58.
- es1 (565 MHz, `sweethangtest=0xA0000200`): injected at 130.3 s; "early smmu resume: fsr 40000402"; full path, cp init + zap -> 0; **recover2 done after 26440 us**; reset leaves sctlr e7 (stall bit on). backboardd respawned at 132.4 s, lock screen on webcam, then **21.5 swaps / 20.5 panel updates per s at +60-120 s with swipes**: the 6-8/s after er4-er7 was the idle lock screen, not a slow GPU. (Boot-time smmu sctlr line not captured: the live log came up 14 s late.)
- sk58 (430 MHz, appsoak 3600 s): 204/204 opens, 0 resets, 0 GPU hangs, 0 SMMU fault lines, 0 "clearing smmu", 0 UIWDT. Pool 97 -> 342 MB, peak 436. Battery 4436 -> 4432 mV. Mean 2.4 swaps/s over the hour (lower than sk57's 9.0; not checked whether that is the averaging window or real).
- Noise: 108x CRASH assetsd, 36x CRASH TipsSpotlightInd, 256x EXC_CORPSE_NOTIFY, 154x CATransformLayer allowsGroupOpacity.
- Phone left on final58 (kernel58, default args incl. watchdog), home screen, 4456 mV.

## 2026-10-27 ~09:15-09:55 kernel59 (xnu 0183: GPU pool pressure) pp1
- 0183: crossing `sweetpoolwarn`% of the 940 MB pool (default 80) logs "pool: WARNING" and bumps a pressure counter; each v2 client drops its 0156 reuse cache on its next alloc/free and keeps none until use is 10 points under. CI run 40509335807 -> work/kernel59.
- pp1 (`sweetpoolwarn=40` = 376 MB to force it, 565 MHz, appsoak 1900 s): 5 WARNING lines at 1033 / 1561 / 1666 / 1982 / 2000 s (376-382 MB), each followed by "back under 30%" (272-278 MB) 1-4 s later. Drops: 5, all backboardd, 25/17/25/25/17 MB (109 MB). 108/108 opens, 0 hangs, 0 resets, 0 UIWDT, 0 "no bytes free". Pool 192 -> 314 MB, peak 382. ~10.8 swaps/s mean. Battery 4436 -> 4434 mV.
- Phone left on final59 (kernel59, default args, warning at 80%), home screen.

## 2026-10-27 ~10:05-10:45 plan step 6: crash-looping daemons (dd1)
- Dropped launchd jobs com.apple.spotlight.IndexAgent, com.apple.corespotlightservice, com.apple.assetsd, com.apple.assetsd.nebulad, com.apple.driverkitd with tools/sunfish-launchd-drop.sh. Previous plist: /var/local/launchd.plist.pre-step6 (phone), work/userland/launchd.plist.pre-step6 (PC); original still /System/Library/xpc/launchd.plist.pre-drop.
- dd1 (kernel59, 30 min appsoak): 102/102 opens, 0 resets/hangs, 10.8 swaps/s, pool end 317 MB (peak 385), battery 4434 mV flat.
- CRASH lines dd1 vs pp1 (same 30 min mix): assetsd 0 vs 88; TipsSpotlightInd 19/19, PassKitSpotlight 19/19, ContactsCoreSpot 10/9, VoiceMemos 10/9, MobileSMSSpotlig 9/9, podcasts 9/9, PhotosFileProvider 1/1, fileproviderd 1/1.
- Photos opens to "What's New in Photos" + a notification prompt (it rendered black before). Home screen normal. Spotlight UI and lock screen not checked (the agent's swipes missed).
- Open: the Spotlight index extensions are launched by something other than the two dropped jobs (candidates: searchd, spotlightknowledged, duetexpertd).

## 2026-10-27 ~10:50-11:10 step 6 dd2: searchd launches the Spotlight extensions
- In dd1's log every *SpotlightIndexExtension exec follows `exec searchd` by ~0.4 s (searchd started 21 times in 30 min). Dropped com.apple.searchd.
- dd2 (kernel59): lock screen and Spotlight screen normal on the webcam (scratchpad dd2-lock.jpg, dd2-spotlight.jpg). 15 min appsoak: 54/54 opens; 1 CRASH line (PhotosFileProvider), 0 extension launches. The VoiceMemos/podcasts/SMS crashes are gone too.
- New file docs/sunfish/broken.md: disabled launchd jobs (9 so far), broken services, other broken things.

## 2026-10-27 ~11:20-11:58 plan step 5: USB link survives a port reset (kernel60, xnu 0184)
- Cause (investigator): sweet_usb.c handled a bus reset by clearing the address only; ep0 state, `configured`, `tx_busy` and the started transfers stayed stale, so after a host reset the endpoints were never reconfigured. VBUS is forced valid, so no disconnect event. The "busERR / SMMU cb fsr 400" line is a harmless diagnostic print (translation off, FSR format bits).
- 0184: on reset/disconnect, ENDTRANSFER (ForceRM) on all started endpoints, CLEARSTALL, DALEPENA=3, address 0, clear configured/tx queue/tx_busy, link down event, re-arm ep0 SETUP; ignore stale data-endpoint events until configured; DEVTEN link-state change logged. Off with `sweetusbreset=0`. CI run 40528851023 (third attempt: the first two never got a macOS runner).
- ur1 (kernel60, defaults): link up 12 s after boot. 5 port cycles from the PC (`echo 1 > /sys/bus/usb/devices/3-0:1.0/usb3-port1/disable`, 5 s, `echo 0`): link back after 2 s in 5/5, SSH ok. No "0184" lines reached the live log (lines during the reset are lost; not checked further).
- ur0 control (kernel60, `sweetusbreset=0`): 0/2; the PC re-enumerated the phone (new device 51, cdc_ether registered, 192.168.7.1 assigned) but no ping. The phone is now unreachable (no Wi-Fi either) and needs a physical reboot (hold power + vol-down to fastboot).
- Open: physical unplug/replug (VBUS loss) untested; the link "flap" after boot not reproduced today.

### k60d physical replug (2026-10-27 18:46)
- kernel60 defaults, battery 4457 mV. Cable out 18:46:26-18:46:42, host saw 2 bounces (dev 53, 54), final enumeration 18:46:45; ping 0% loss, SSH ok.

## 2026-10-27 ~19:00-20:07 plan steps 1-4 checks (ck1-ck3, kernel60 defaults)
- Battery 4456 mV in fastboot before each boot.
- Step 4 USB: ck1/ck2/ck3, 5 min each, SSH probe every 10 s: 0 failures, one `=== link up` per boot, boot to SSH 29 s (ck2) / 43 s (ck3). Done.
- Step 1 Wi-Fi: not joined at boot+8 min. nettest: DNS for apple.com fails, https error -1009 "No network route"; no Wi-Fi icon (`logs/ck1-wifi.jpg`). Live log: 1072 wifi lines in 8 min; wifid gets 11 (SCAN_RESULT) answered 341x, gets 103 (CURRENT_NETWORK) error 6 331x, gets 1 (SSID) error 6; configd deleted en0 addresses 192.168.1.238 and 192.0.0.2 once each. The phone has no ifconfig/netstat/ping/ps in toybox. status.md said "✅ Safari works": that was with wland + wlan.conf, not iOS's own join. Corrected.
- Step 2 battery: 17 samples 19:10-19:40: 4431-4434 mV, 0 mA, chg=0, target/shown 100%; on screen 100% at +15 and +30 min. Earlier today: +120-148 mA with chg=1 at 100%. Nothing learned about discharge.
- Step 3 AegirPoster: 0 crashes, 0 CRASH lines of any kind in ~50 min idle. Bundle `/System/Library/CoreServices/AegirProxyApp.app/PlugIns/AegirPoster.appex`. Recount during an app soak.
- Process: the first phone-runner left background loggers, ended its turn waiting on them and never ran ck2/ck3; a fresh foreground-only agent finished. Phone left up on ck3.

## 2026-10-27 ~21:50- overnight run (user away until 05:00; plan.md "Overnight run")
- Storage: root is HFS+ 16 GiB, 94% full; userdata has ~93.6 GiB free after it (rescue/console logs in the last 1153 blocks). xnu 0185 (kernel61) adds /dev/ufsroot1 + rufsroot1 = blocks 4194304 .. nblocks-1153-256 (24,533,626 blocks); offset/end checks pass (block 0 and 1000 identical to the partition blocks, last block reads, one past = 0 bytes).
- newfs_hfs failed with ENOTTY: 0187 answers DKIOCGETMAXBYTECOUNT*/MAXSEGMENTBYTECOUNT* (kernel62). Then "Initialized /dev/rufsroot1 as a 94 GB case-sensitive HFS Plus volume with a 8192k journal" (1.2 s).
- Escape hatch 0186 `sweetnofstab=1` (kernel62): /etc/fstab hidden from LOOKUP; boot normal. `rm` fails (it stats first); `toybox unlink /private/etc/fstab` works -> that is the repair command.
- Copy test (g62e): 3.1 GiB /var copied in 84 s; 1977/2000 sampled files identical, same after umount/remount; differences = aps.db* (live writes) and 20 emoji-poster snapshots created after the copy; sockets can't be copied.
- fsck "Volume header needs minor repair" + mount_hfs "GetMasterBlock: Error 22": (1) d_psize returned the size in sectors where specfs wants the sector size -> every read(2)/write(2) on a block node failed EINVAL since 0028 (0191, kernel65); (2) alternate VHB still unreadable: strategy failed transfers that cross the device end instead of clipping like IOMedia (0192, kernel66).
- Boot watchdog 0189 `sweetbootwdt=N`: the counter starts when the pet thread starts (~26 s kernel time). N=20 never fired (UI drew first); N=5 (kernel66): "SWEET BOOTWDT: no swap 5 s after boot, resetting" at 31.3 s, phone in fastboot ~35 s later. Live log only starts ~27 s after boot (early kernel lines like "SWEET UFS" never appear).
- Also built: 0188 Wi-Fi diag logging, 0190 torch via whitelisted SPMI write (sweettorch=1), kernels 63-66 (CI 40617690407, 40618033335, 40618342567, 40621294989).
- Stage 2 attempts: k66v (toybox copy) died at 35 s after installcoordinationd EACCES x3 (copy lost ACLs + com.apple.containermanager.* xattrs). New phone tool vcopy (copyfile; its fcopyfile data path got EFAULT on 5691 sqlite files -> own read/write data copy; it nests SRC's basename into an existing DST; files got ACLs inherited from their dir -> -m pass clears them). Final copy: 0 metadata differences in 47924 entries.
- k67v died at 63.6 s: RAM-log rescue (64 blocks at nblocks-1153, read via `toybox xxd -s 117670649856 -l 262144 -p /dev/rufsroot0`) had the panic: "userspace panic: unexpected exec of non-platform binary: /var/local/bin/wland". sweet_amfi.c (0035) marks binaries platform only on MNT_ROOTFS -> 0194 adds f_mntfromname == /dev/ufsroot1.
- k68v (kernel68, CI 40626725130): /var on ufsroot1 (98134504 KB, 3.7 GB used), home screen on the webcam, SSH up > 3 min, crashes: FitnessWidget x3 only. DONE.
- Wi-Fi diag (k66e, 0188): scan elements intact (room 1024, have 325, returned 325); no ASSOCIATE ever; set 51 COUNTRY_CODE and set 383 (4104-byte XML plist) refused; no com.apple.wifi.known-networks.plist on the phone. 0193 (kernel67+) answers both.
- Lessons: the escape boot hides fstab from cat/stat too, so stage files elsewhere and copy them in on a normal boot; the live log starts ~27-60 s after boot, the RAM-log rescue has the rest.

## 2026-10-28 00:00-02:10 overnight run, part 2
- sk68 2 h exit soak (kernel68, /var on ufsroot1, 15 chunks, 120.7 min): 408/408 app opens, 0 resets, 0 GPU hangs, 0 SMMU faults, 0 UIWDT, CRASH only PhotosFileProvider x4 (AegirPoster 0). Pool 192 -> 709 peak -> 631 MB end: "pool flat" not met. Battery 4431 -> 4429 mV, 0 mA, 100% (full on the charger; discharge still untested).
- Pool analysis: all growth is backboardd live BOs; <256 KB BOs 16 -> 180 MB in the first 15 min then flat, 1-8 MB BOs +50 MB/h; apps free theirs; reuse cache 0-48 MB.
- Panel brightness (pb1, kernel68): tools/sunfish-panel-bright.sh sends DCS 51 HH LL through the DSI0 command DMA (packet at 0x9d6ff000, MDP autorefresh paused around it). TRIG_CTRL was already SW (4). 128/32/400/128: all done, ack_err 0, no faults; webcam: 32 near-black, 400 brighter. Works; resets on reboot.
- Torch: k68t (kernel68 + sweettorch=1) panicked at 36.2 s: "Use sysctl_register_oid_early to register permanent nodes" (RAM-log rescue). xnu 0195 (kernel69, CI 40640759295) uses SYSCTL_OID_MANUAL. k69t boots; new tool phone/tools/sctl (CI 40641144876). sctl kern.sweet_torch=50 / =100: all writes land, readback ok, 5 s auto-off; the webcam sees only the front, the user saw the LED turn on and off (confirmed by eye).
- Clock: kernel UTC was right; the phone's timezone was US/Pacific (3 h behind). /var/db/timezone/localtime -> zoneinfo/America/Toronto; lock screen 1:58 Wed Oct 28 = PC time.
- Wi-Fi (sk68 counts): 0193 answers set 51 (1/1) and set 383 (1277/1277); get 103 asked 7178x, answered 0; get 423 refused; 0 ASSOCIATE. iOS has no known network: needs one join in Settings by the user.
- Phone left on k69t (kernel69, sweettorch=1), dim 10.

## 2026-10-28 02:05-03:15 overnight run, part 3: the GPU pool leak
- xnu 0196 (kernel70, CI 40641592758) `sweetbocensus=N`: per-client census of live 1-8 MB BOs older than 60 s.
- bc1 (35.6 min app soak, census every 300 s): backboardd's own 1024 KB BOs 14 -> 38 -> 48 -> 60 (oldest = process age), the only growing group; 8 MB BOs flat 6-8. 2 real GPU hangs (rbbm 800005, ring pending, no SMMU fault, same signature as the failed sk430 one) recovered first path in 26.5/26.4 ms; backboardd respawned. Real-hang recovery is now proven.
- bos1 (TU_DEBUG=bos, ~10 min): Turnip's live BOs are mostly command streams (cmd cs 1491, draw cs 1992, draw sub cs 2013, tile store cs 996), 6732 BOs 484 MB; no 1024 KB BO in its table while the kernel counts 46 -> 61. The 1 MB BOs live in the kernel after Turnip let go of them (suspect: the NativeAdreno backend's "retaining native allocation" paths; not in the logs we have).
- /var/local/mtl2.log is 321 MB (never rotated).
- Phone left on bc1 (kernel70, sweetbocensus=300), dim 10, mtl2-env empty as before.
