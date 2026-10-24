
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
