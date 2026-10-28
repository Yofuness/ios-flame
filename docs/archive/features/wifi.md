# Wi-Fi: archived log (entries before 2026-10-25)
Current file: [features/wifi.md](../../features/wifi.md). Same format, newest first.

## Log

### 2026-10-24 15:00 New location; phone booted as "demo"
- See [usb-network](usb-network.md) for this entry; it also touches this feature.

### 2026-10-23 21:10 Wi-Fi — **ON THE INTERNET**
- Join from Settings → Wi-Fi works (wifid → /dev/wlan → wland): associated, state 4, link up.
- No IP at first: iOS's preferences.plist had no network set (written before en0 existed), so configd never
  made a Wi-Fi service and IPConfiguration never ran DHCP. tools/sunfish-netprefs.py adds the Wi-Fi (en0,
  DHCP) service + set and restarts configd (backup: logs/preferences.plist.bak-*).
- Result: en0 192.168.2.30/24, router + DNS 192.168.2.1, primary interface en0;
  nettest: DNS apple.com OK, TCP OK, HTTPS 200.
- Safari pages were black: WebKit's GPU process got no Metal device (only SpringBoard on /var/local/mtl2-clients)
  and has no CPU fallback. Added com.apple.WebKit.GPU + MobileSafari (sunfish-metal2-install.sh too).
  fast.com renders: 7.7 Mbps down, 8.9 Mbps up, 35 ms.

### 2026-10-23 20:55 Wi-Fi join — **ROOT CAUSE FOUND, JOIN WORKS**
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

### 2026-10-23 20:00 Offline work while the phone is unreachable
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

### 2026-10-23 17:36 STOP — hard hang (demo boot k50, flamereboot=0)
- See [boot](boot.md) for this entry; it also touches this feature.

### 2026-10-23 13:50 Wi-Fi join — Phase 1 (diff vs Linux ath10k): 3 fixes, same crash
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

### 2026-10-23 10:50 Wi-Fi join, round 2 — **still blocked** (2 more attempts), crash decoded
- Crash PC 0xb01ab7b0 decoded in wlanmdsp.mbn (Hexagon, `llvm-objdump --triple=hexagon`): a **buffer-pool
  allocator** (global object at gp+0x6b0) walking a pool whose descriptor array was never set up → bad
  pointer load (`memuh(r18+#0x1c)`). Not an assert.
- Tried: (1) pdev TX power limits + vdev WMM before vdev start (ath10k does this); (2) WMI INIT resource config
  exactly as ath10k for wcn3990 (14 peers, not 33; skid 6; beacon offload 3; host_capab 0). Same crash both times.
- Also verified identical to ath10k: CE pipe/service/shadow maps, INIT layout, vdev create/start layout, channel TLV.
- Temporary /var/local/wlan.conf deleted after each test (verified absent).
- Next lead: find which pool (callers of 0xb01ab644 and the pool type/size args) → what host step initializes it.
- (Recovered from `git show 1808cf9^:docs/sunfish/workday.md`; that commit deleted it.)

### 2026-10-23 09:40 Step 3 Wi-Fi join — **BLOCKED** (3 attempts), scan + Settings list still OK
Every join: WLAN fw asserts at `vdev start` (first TX/channel setup), `EX:wlan_process … WLAN RT:0x1084/0x1089
PC=0xb01ab7b0`, then modem ERR-FATAL. Same PC in every variant tried today:
1. rails as Google's icnss driver: order cx-mx, 1.8-xo, 1.3-rfa, **ch1 before ch0**, 3.3 V at **3312 mV** (was 3304) — same crash.
2. cx-mx **752 mV** (icnss default; DT says 640) via new `/var/local/wlan-rails.conf` override — same crash.
3. **WMI_PDEV_SET_REGDOMAIN** (tag 0x51, cmd 0x4001; fw eeprom code is 0) + channels 1–11 / 36–48 no
   longer marked passive (they were all passive) — same crash.
Previously ruled out: BDF variants, channel TLV encoding, missing TFTP files.
Left (not tried): firmware HL3.2.2 vs flame's 3.2.0 — try a WLAN.HL.3.2.0 wlanmdsp.mbn (flame's), or
compare our WMI sequence (INIT resource config, PDEV params before vdev start) with ath10k's for wcn3990.
Join tests used the temporary /var/local/wlan.conf, deleted after each test (verified absent).
After a clean reboot: 3 scans, 14 networks, no modem crash, Settings > Wi-Fi lists Gansekoele/Johann
(logs/shots/wifi-0940.png).
- (Recovered from `git show 1808cf9^:docs/sunfish/workday.md`; that commit deleted it.)

### 2026-10-23 10:00 status.md update
- Wi-Fi: scan OK, join blocked (WLAN fw assert at vdev start, PC 0xb01ab7b0).
