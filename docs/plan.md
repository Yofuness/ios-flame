# Next steps: stability, then features (plan of 2026-10-28)

Old plan: `docs/plans/2026-10-27-usability-quick-wins.md`. Rules still apply: RAM-only boots, stop below 3600 mV, one change per boot, push only to `main`, a webcam check after every visual change, a hard hang stops phone work. Builds are local now (methods.md: `tools/build-userland-linux.sh`, `tools/build-metal2-linux.sh`, `tools/vm-build-xnu.sh NN --stop`); CI is the fallback. The user set the battery work aside for now.

## A. Tests (right after the sk71 soak)
1. [ ] **2 h exit soak, retry (sk71).** kernel70 + bundle 7053013 (bridge 0105). Pass: 0 reboots, 0 unrecovered GPU hangs, pool flat. sk70 reset silently at 337.8 s (no panic in the rescued RAM log).
2. [ ] **Reset reason line (kernel74 = xnu 0197).** One boot: `SWEET PON:` must print (not "read failed"). Decode the subtype (GEN1/GEN2), check PON_REASON1 says hard reset / warm reset for the last reset. ~5 min.
3. [ ] **Panel-rate drop.** Fresh boot each: CI bundle (work/metal2-0106) vs the local bundle, two `BENCH_TIGHT=1 sunfish-fpsbench.sh` runs each. Fresh-boot sk70p gave 15.6-16.2 panel updates/s, before 21-24; the build flags are identical (features/smoothness.md). If both are ~16, try kernel68 vs kernel70 with the same bundle. ~30 min.
4. [ ] **Silent resets vs kernel.** Only if sk71 resets too: 1 h soak on kernel68 (sk68 was clean for 2 h) vs kernel74, same bundle, PON line after each reset. ~2.5 h.

## B. Features (prep running now, read-only; each is its own build + test)
5. [ ] **Panel off when the phone sleeps.** DCS 28 (display off) + 10 (sleep in) on the IOMFB display-off power state, 11 + 29 on wake, autorefresh paused around it. Saves battery and burn-in. Test: webcam black, battery mA drops, wake 10/10. ~half a day. Design ready (features/sleep-wake.md 2026-10-28 12:30): first a shell test of 28/10 + 11/29, then ~120 lines in SweetFramebuffer.cpp behind sweetpanelsleep=1.
6. [ ] **Brightness slider in Control Center.** iOS's brightness call reaches SweetFramebuffer -> DCS 0x51 from the kernel (today only the PC tool sends it). Cap at 50% during tests. Test: slider moves on the webcam, the panel follows. ~half a day. Design ready (features/brightness.md 2026-10-28 12:40): iOS finds no `backlight-control` service today; publish one (SweetBacklight.cpp, setProperties), log-only boot first, then sweetbl=1. Before writing: disassemble CoreBrightness to confirm the key.
7. [ ] **Wi-Fi status in iOS.** Answer wifid's get 103 (CURRENT_NETWORK), SSID, BSSID, channel, RSSI from wland's joined state, plus the link event, so the status bar shows Wi-Fi and Settings lists the network. ~half a day. Analysis (features/wifi.md 2026-10-28 12:45): the gets are already answered once joined; the missing piece is the link-up event on wland's self-join (~5 LOC) + fixed answers for 14/8/17/57. First a webcam check: the icon may already show.
8. [ ] **Flashlight button in Control Center.** iOS drives the torch through the camera stack; find the smallest hook (AVCaptureDevice torch -> a kernel call to kern.sweet_torch). Investigate after 5-7.

## B2. Big features (weeks; started 2026-10-28)
12. [ ] **Audio M0: boot the ADSP** (features/audio.md): reuse wland's PAS loader with pas-id 1 + adsp.mdt; the glink "lpass" edge must come up. Then M1 a speaker beep (APR + CS35L41, -20 dB, ≤3 s).
13. [ ] **Camera M1: read the rear sensor's chip ID** (features/camera.md): first read CAMCC/GDSC registers read-only and find the camx sensor bins in the vendor image; then slg51000 + camcc + CCI drivers.

## C. Small fixes (no phone, any time)
9. [ ] `/var/local/mtl2.log` rotation in the frontend (it reached 321 MB).
10. [ ] macOS VM auto-login (sysadminctl error 22) so a reboot doesn't stop at the login prompt.
11. [ ] Block driver direct I/O EFAULT (copyfile's fcopyfile path on /dev/ufsroot*).

## Later
- Charging (PMIC writes: ask the user first; notes in `docs/reference/charger-regs.md`). Battery % discharge check.
- Sound and mic (weeks). Bluetooth, camera, sensors (hard). Cellular: skip.
- Perf: back to the ~21-24 updates/s ceiling, the soak as the regression gate.

## Verification
Each step: numbers into the feature Log + this plan in the same commit, a journal line, a webcam check for anything visible. Battery ≥3600 mV before each boot.
