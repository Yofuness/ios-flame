# iOS on Pixel 4a (sunfish) — status

Updated 2026-10-28 06:45 (rows below are current; the dated "Update" sections at the end are history). Port of ios-flame / ios-sweet. Code: `port/` (branch `sunfish`,
github.com/Yofuness/ios-flame). Night log: [overnight.md](overnight.md).

## Working
| Area | State | Notes |
|---|---|---|
| Boot | ✅ | RAM-only `fastboot boot`; iOS 16.7.16 (iPhone X userland) root on userdata |
| Storage | ✅ | /private/var on /dev/ufsroot1 (94 GB HFSX) via /private/etc/fstab, kernel68+; escape: boot `sweetnofstab=1` |
| CPUs | ✅ | 8 cores; A55 1804 MHz, A76 2208 MHz (read from OSM registers) |
| Display | ✅ | 1080x2340, ABL splash buffer 0x9c000000; GPU-composited (Metal → Turnip), vsync-paced ~21 updates/s; brightness: panel `tools/sunfish-panel-bright.sh 4-511` (DCS 0x51, 2026-10-28) or blend `tools/sunfish-dim.sh N` |
| GPU | ✅ | Adreno 618 at 565 MHz, Metal via SweetMetal2 (metal2vulkan + Turnip); see docs/sunfish/display-perf.md |
| Touch | ✅ | ST FTM5 over SE7 I²C 0x49 (patch 0125, `sweettouch=2`) |
| Buttons | ✅ | power, vol-down (PON), vol-up (PM6150L GPIO2, patch 0126) |
| USB | ✅ | Ethernet 192.168.7.2, live kernel log (UDP 5140, full backlog: 0127) |
| SSH | ✅ | dropbear + dash + toybox; `port/tools/sunfish-ssh` |
| Real time | ✅ | PM6150 RTC + `sweetrtcoff` (`port/work/rtc-offset`); timezone America/Toronto since 2026-10-28 |
| Stock apps | ✅ | 32 staged apps installed in /Applications |
| Wi-Fi | ✅ | wland joins the home network at boot from /var/local/wlan.conf (kept on the phone since 2026-10-28): DHCP, DNS/TCP/HTTP 200 OK (wj1, kernel69). The iOS join from Settings (wifid) still fails: CURRENT_NETWORK error 6 |

## Partial / in progress
- **Smoothness** — next steps are in docs/sunfish/plan.md (present path, the last texture copies).
- **GPU memory leak** — backboardd leaks small BOs on app switches; a full pool caused the freezes.
- **Charging** — the battery drains ~0.5 A under load even on the charger (the charger isn't configured).
- **Battery %** — real reading works; was jumpy (voltage-only estimate). Patch 0128: IR-compensated,
  averaged, 1%/30 s, direction from measured current, no 100% flash. Booted as `k10`; **needs checking**.
- **Wi-Fi join** — fails. WLAN firmware crashes at vdev start (first TX/channel setup):
  `PD FAILURE "wlan_process": EX:wlan_process:0x1:WLAN RT:0x1084/0x1089:PC=0xb01ab7b0`, then modem
  ERR-FATAL. Ruled out: BDF (EVT1.0 vs prod: same crash), channel TLV encoding, missing TFTP files.
  Suspects: RF/PA rail voltages (L10C/L11C 3304, L9A 640 mV), extra AOP/QMP resource, firmware
  HL.3.2.2 vs flame's HL.3.2.0.
- **`flamereboot=0`** — used for k10 to disable the auto-reboot; meaning unverified.

- **Flashlight** — kernel69 + `sweettorch=1`: `/var/local/bin/sctl kern.sweet_torch=50` (5 s auto-off); works (the user saw the LED, 2026-10-28). No Control Center button yet.

## Not started
Audio, Bluetooth, cellular, camera, sensors.

## Key fixes found during the port
- ABL leaves UFS powered down → shim sends START STOP UNIT to W-LUN 0xd0.
- RAM ends 0x1fcc00000 → XNU high range shrunk 64 MB; log stashes moved below.
- IMEM restart reason at 0x146aa65c; panel 1080x2340 (was 2280 in kernel constants).
- Vol-up polled the Pixel 4's PM8150 GPIO6 → sunfish PM6150L GPIO2 (SID 4).
- Modem XPU violation: rmtfs buffer above 4 GB → fixed at 0xE1001000.
- `fastboot flash userdata` is slow on big FILL chunks, not hung → trailing FILL → SKIP.

## How to boot (from fastboot)
```
cd port && tools/sunfish-boot.sh NAME work/kernel37/kernel.development.sweet sweetpresentwait=1
```
Tools: `flame-cmd.py reboot|swipe|tap`, `flame-files.py get|put`, `flame-shot OUT.png`,
`sunfish-install-ssh.sh`, `sunfish-install-wifi.sh`, `sunfish-livelog.sh` (log → logs/usb-live.log).

## Recovery
Backups: `backup/` (+ Lily). Restore Android: `restore/factory/…s2…zip` → `flash-all.sh`.
Never flash xbl/abl/tz/etc.; never relock.

## Update 2026-10-23 10:00
- UI 17 fps while swiping (was 4.3): dropped launchd jobs vmd, ReportCrash, axassetsd; sweetfb_hz=60.
- GPU (A618): GMU boots; HFI blocked. The GMU bus-faults on its first SMMU read (fw pc 0x19a8). Suspect: the GPU
  SMMU clock gcc_gpu_memnoc_gfx stays halted → SMR writes dropped. Next steps are in gpu-notes.md.
- Wi-Fi: scan OK, join blocked (WLAN fw assert at vdev start, PC 0xb01ab7b0).
- Sleep/wake via power button: stable (11/11).
