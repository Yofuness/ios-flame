
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
