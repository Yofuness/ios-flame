# Plan of 2026-10-28: priorities, then the broken list

Old plan: `docs/plans/2026-10-27-usability-quick-wins.md`. Rules: RAM-only boots, stop below 3600 mV, one change per boot, push only to `main`, a webcam check after every visual change, a hard hang stops phone work. Conservative: risky hardware writes (PMIC charging, speaker amp, camera rails) go behind boot args, with safe limits from the DT, and the user is asked first where the plan says so. Builds are local (methods.md); CI is the fallback.

**How this list is worked (the user's rules, 2026-10-28):** work the **Priority** list top-down. While a test runs and no priority item can move, take the next **Broken** item that needs no phone (cheapest first). Keep iterating until everything is done. Push-notify the user when an item is done or when they are needed. Anything newly found broken goes on the Broken list. Audio and camera are a dream: only after everything cheaper.

## Priority (at most 5; the one being worked is marked ▶)
1. ▶ [ ] **2 h exit soak, retry (sk71).** kernel70 + bundle 7053013 (bridge 0105). Pass: 0 reboots, 0 unrecovered GPU hangs, pool flat. sk70 reset silently at 337.8 s (no panic in the rescued RAM log).
2. [ ] **Reset reason line (kernel74 = xnu 0197).** One boot: `SWEET PON:` must print (not "read failed"). Decode the subtype (GEN1/GEN2), check PON_REASON1 says hard reset / warm reset for the last reset. ~5 min.
3. [ ] **Panel-rate drop.** Fresh boot each: CI bundle (work/metal2-0106) vs the local bundle, two `BENCH_TIGHT=1 sunfish-fpsbench.sh` runs each. Fresh-boot sk70p gave 15.6-16.2 panel updates/s, before 21-24; the build flags are identical (features/smoothness.md). If both are ~16, try kernel68 vs kernel70 with the same bundle. ~30 min.
4. [ ] **Wi-Fi status in iOS.** Answer wifid's get 103 (CURRENT_NETWORK), SSID, BSSID, channel, RSSI from wland's joined state, plus the link event, so the status bar shows Wi-Fi and Settings lists the network. ~half a day. Analysis (features/wifi.md 2026-10-28 12:45): the gets are already answered once joined; the missing piece is the link-up event on wland's self-join (~5 LOC) + fixed answers for 14/8/17/57. First a webcam check: the icon may already show.
5. [ ] **Panel off when the phone sleeps.** DCS 28 (display off) + 10 (sleep in) on the IOMFB display-off power state, 11 + 29 on wake, autorefresh paused around it. Saves battery and burn-in. Test: webcam black, battery mA drops, wake 10/10. ~half a day. Design ready (features/sleep-wake.md 2026-10-28 12:30): first a shell test of 28/10 + 11/29, then ~120 lines in SweetFramebuffer.cpp behind sweetpanelsleep=1.

## Broken list (cheapest first; [phone] = needs the phone)
B1. [built, not yet on the phone] **`/var/local/mtl2.log` never rotated** (reached 321 MB). SweetMetal2.mm sm2_open_log(): clients run as mobile and cannot rename in /var/local, so the file (mode 666) is truncated in place once past 8 MB, with a `log truncated at N bytes` line. Built locally (14 s rebuild). Check on the next bundle install.
B2. [ ] **macOS VM auto-login fails** (sysadminctl error 22), so a VM reboot stops at the login prompt. Write /etc/kcpassword + loginwindow autoLoginUser, or fix the sysadminctl call. ~30 min, VM only.
B3. [ ] **Block driver direct I/O EFAULT**: copyfile's fcopyfile data path fails on /dev/ufsroot* files (vcopy has its own loop). Fix sweet_ufs.c's strategy for user-space buffers. ~2-3 h + [phone] test.
B4. [ ] **Silent resets in app soaks** (sk70 337.8 s, two on 10-25). Diagnostic xnu 0197 (PON reason) is priority 2; then **Silent resets vs kernel.** Only if sk71 resets too: 1 h soak on kernel68 (sk68 was clean for 2 h) vs kernel74, same bundle, PON line after each reset. ~2.5 h. [phone]
B5. [ ] **Brightness slider in Control Center** [phone]. **Brightness slider in Control Center.** iOS's brightness call reaches SweetFramebuffer -> DCS 0x51 from the kernel (today only the PC tool sends it). Cap at 50% during tests. Test: slider moves on the webcam, the panel follows. ~half a day. Design ready (features/brightness.md 2026-10-28 12:40): iOS finds no `backlight-control` service today; publish one (SweetBacklight.cpp, setProperties), log-only boot first, then sweetbl=1. Before writing: disassemble CoreBrightness to confirm the key.
B6. [ ] **Wi-Fi join from Settings** [phone + user types the password once]: after priority 4, check that com.apple.wifi.known-networks.plist appears and survives a reboot.
B7. [ ] **Flashlight button in Control Center** [phone]. **Flashlight button in Control Center.** iOS drives the torch through the camera stack; find the smallest hook (AVCaptureDevice torch -> a kernel call to kern.sweet_torch). Investigate after 5-7.
B8. [ ] **PhotosFileProvider crashes** (sig 6, ~2-4 per 2 h soak) [phone]: find the reason in the crash line; drop the job if it is not needed (tools/sunfish-launchd-drop.sh).
B9. [ ] **Panel-rate ceiling** (~21 updates/s at best; present blit waits behind backboardd; features/smoothness.md). After priority 3. Days, uncertain.
B10. [ ] **Reduce Transparency slow** (~6 swaps/s; CA's RT shaders fail through the bridge: "vertexFunction is not a MTLFunction"). Investigate the bridge's function lookup. ~1 day.
B11. [ ] **UI watchdog only catches a frozen UI**, not a kernel hang with swaps running. Extend it once B4 says what the resets are.
B12. [ ] **Sensors** (accelerometer, gyro, proximity, ambient light): survey first (DT, the SSC/sensors DSP vs direct I²C). Unknown size.
B13. [ ] **Battery % while discharging** unchecked (set aside by the user; needs the phone off the charger 30 min).
B14. [ ] **Charging** (~0.5 A drain under load on the charger): PMIC charger writes; ask the user before any write (docs/reference/charger-regs.md).
B15. [ ] **Bluetooth**: survey first (WCN3990 BT over the same SoC path as Wi-Fi?). Weeks.
B16. [ ] **Audio** (dream): **Audio M0: boot the ADSP** (features/audio.md): reuse wland's PAS loader with pas-id 1 + adsp.mdt; the glink "lpass" edge must come up. Then M1 a speaker beep (APR + CS35L41, -20 dB, ≤3 s). M0 tool adspboot is built, not run (features/audio.md).
B17. [ ] **Camera** (dream): **Camera M1: read the rear sensor's chip ID** (features/camera.md): first read CAMCC/GDSC registers read-only and find the camx sensor bins in the vendor image; then slg51000 + camcc + CCI drivers. (features/camera.md)
B18. [ ] Cellular: not planned.

## Done today (2026-10-28)
Wi-Fi join at boot, GPU memory leak (bridge 0105), local builds (userland, metal2, kernel in a macOS VM with hibernate), per-feature docs, work on main.

## Verification
Each item: numbers into the feature Log + this plan in the same commit, a journal line, a webcam check for anything visible. Battery ≥3600 mV before each boot.
