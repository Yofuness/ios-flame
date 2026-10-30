# On-device changes

Every change made on the phone, and what reproduces it from git (the user's rule, 2026-10-30: CLAUDE.md
"Reproducible on-device changes"). The base is the root image built by `tools/mkiosroot.sh` +
`tools/sweet-root-tweaks.py` (the Oct 22 image, `../restore/ios16-root.img`); [setup.md](setup.md) goes from a
stock Pixel 4a to the current state. New on-device changes get a line in the Log below, in the same commit.

Audit 2026-10-30 (the phone vs that image, by mtime, launchd job diff and plist diff; raw lists in
`research/audit-2026-10-30/`, not in git): the table is the complete list of intentional changes. Everything else that
differs under /private/var is iOS runtime state (preferences, caches, databases written on first use), which a fresh
install recreates by itself.

## Current changes

| # | What | Where on the phone | Reproduced by | State |
|---|---|---|---|---|
| 1 | Root image + bring-up tweaks (Setup skipped, activation, CommCenter removed, crash loops off) | whole root | `mkiosroot.sh` + `sweet-root-tweaks.py` (setup.md step 2) | scripted |
| 2 | /private/var on its own 94 GB volume | /dev/ufsroot1, /private/etc/fstab | setup.md step 5 (newfs_hfs + vcopy + fstab) | written steps |
| 3 | SSH: dropbear + dash + toybox, root key, launchd job org.iosonpixel.dropbear (Interactive) | /var/local/bin, /System/Library/LaunchDaemons, launchd.plist | `tools/sunfish-install-ssh.sh` | scripted |
| 4 | dropbear starts early (no StandardErrorPath) | launchd.plist | `tools/sunfish-dropbear-early.sh` | scripted |
| 5 | Wi-Fi: wland + modem/WLAN firmware + rmtfs, job org.iosonpixel.wland | /var/local/bin, /var/local/fw, launchd.plist | `tools/sunfish-install-wifi.sh` | scripted |
| 6 | Wi-Fi network and password | /var/local/wlan.conf (mode 600) | setup.md step 7 (by hand; never in git) | written steps |
| 7 | Network service for en0 (DHCP) | /var/preferences/SystemConfiguration/preferences.plist | `tools/sunfish-netprefs.py` | scripted |
| 8 | Metal bridge bundle + client list | /System/Library/Extensions/SweetMetal2.bundle, /var/local/mtl2-clients | `tools/sunfish-metal2-install.sh` | scripted |
| 9 | launchd jobs removed: ReportCrash, axassetsd, assetsd.nebulad, corespotlightservice, driverkitd, fairplayd.H2, lskdd, searchd, spotlight.IndexAgent, voicemail.vmd, wifip2pd | launchd.plist | `tools/sunfish-setup-phone.sh` (calls sunfish-launchd-drop.sh) | scripted |
| 10 | backboardd `CA_FORCE_TIMER=1` (vsync timer) | launchd.plist | `tools/sunfish-setup-phone.sh` | scripted |
| 11 | assetsd `StandardErrorPath /var/local/assetsd.err` (debug leftover, harmless) | launchd.plist | not reproduced on purpose | debug only |
| 12 | Phone tools (openapp, sctl, pmem, psx, oslog, prefs, lsreg, netsvc, vcopy, fcopy, pstack, i2cprobe, camid, ...) | /var/local/bin | `tools/sunfish-setup-phone.sh` (build-userland-linux.sh output) | scripted |
| 13 | Audio: adspboot + ADSP firmware | /var/local/bin, /var/local/fw/adsp | `tools/sunfish-install-adsp.sh` | scripted (feature unfinished) |
| 14 | Sensors files for the DSP (sensor configs, fake soc0) | /var/local/fw/sensors, /var/local/fw/soc0, /var/local/sensors-persist | `tools/sunfish-sensors-files.sh` (prints the push commands), `tools/sunfish-soc0-files.sh` | partly scripted (feature unfinished) |
| 15 | CocoaTop app | /Applications/CocoaTop.app | `tools/build-cocoatop-linux.sh` + `tools/sunfish-setup-phone.sh` | scripted |
| 16 | Timezone America/Toronto | /var/db/timezone/localtime symlink | `tools/sunfish-setup-phone.sh` (TZ variable) | scripted |
| 17 | Flashlight tile flag | /var/local/sweet-flashlight | `tools/sunfish-setup-phone.sh` | scripted |
| 18 | Control file for rotation, fake taps, Location Services | /var/local/sweet-orient | `tools/sunfish-setup-phone.sh` | scripted |
| 19 | Auto-Lock = Never (Settings > Display & Brightness, by fake taps) | iOS settings | setup.md step 9 | written steps |
| 20 | Location Services on (`echo "L 1" > /var/local/sweet-orient`) | locationd state | setup.md step 9 | written steps |
| 21 | Brightness slider at its minimum at night | iOS setting | none needed (user preference) | n/a |
| 22 | TCC MediaLibrary row for Music (B25; did not fix Music, kernel95 did) | /var/mobile/Library/TCC/TCC.db, backup /var/local/tcc-pre-b25 | not reproduced (no effect) | dead end |
| 23 | locationd plist `LocationServicesEnabledIn7.0` (B32 loc1; no effect) | /var/root/Library/Preferences/com.apple.locationd.plist, backup .bak-b32 | not reproduced (no effect) | dead end |
| 24 | Bluetooth: btup + BT firmware (crbtfw21.tlv, crnv21.bin from the factory vendor.img) | /var/local/bin/btup, /var/local/fw/bt | `tools/sunfish-install-bt.sh` | scripted (feature unfinished) |
| 25 | Bluetooth for iOS: btbridge + bluetool-stub; launchd: com.apple.BlueTool runs bluetool-stub, bluetoothd env BT_HCI_TRANSPORT=H4BC:POSIX:com.apple.uart.bluetooth,115200 + BT_BAUDRATE=115200 (kernel control of xnu 0226, boot-arg btbridge=1), job org.iosonpixel.btbridge (opens /dev/btbridge; `-p` = old pty); backup launchd.plist.pre-btbridge; at runtime (pty mode only) /dev/cu.bluetooth symlink, /var/local/btbridge.log, /private/var/tmp/btbridge.pid and bluetool-stub.log | /var/local/bin, launchd.plist | `tools/sunfish-install-btbridge.sh` (`--undo` restores the plist) | installed 2026-10-30, feature under work |
| 26 | Auto-rotate: adspboot + /var/local/bin/sensors-start.sh, job org.iosonpixel.sensors (adspboot --sns-motion --sns-rot, no time limit; log /var/local/sensors.log, .prev; off: /var/local/sensors.off); backup launchd.plist.pre-sensors | /var/local/bin, launchd.plist | `tools/sunfish-install-sensors.sh` (`--undo` removes the job) | scripted |
| 27 | Filza File Manager (TIGI Software 4.0.1-4, the user asked; for its music import): Filza.app, /usr/libexec/filza (setuid launcher, FilzaHelper, WebDAV server), /usr/bin/Filza, launchd job com.tigisoftware.filza.helper | /Applications, /usr/libexec/filza, launchd.plist (backup .pre-filza) | `tools/sunfish-install-filza.sh` (downloads the .deb, checks its sha256; `--undo`) | scripted |
| 28 | Audio output device: SunfishAudio.driver (HAL plug-in, null sink until /dev/sunfish-audio exists); VirtualAudio.plugin moved back from /Library/Audio/Plug-Ins/HAL.disabled to /Library/Audio/Plug-Ins/HAL | /System/Library/Audio/Plug-Ins/HAL/SunfishAudio.driver, /Library/Audio/Plug-Ins/HAL | `phone/audio/build.sh` + `tools/sunfish-install-audio.sh` (`--undo`) | feature under work |
| 29 | Camera test tools and table: /var/local/bin/camid, camclk, camrx, camisp (phone tools), /var/local/cam/imx363-mode8.txt (IMX363 init + mode 8 register list, derived from Google's camx module file, not in git) | /var/local/bin, /var/local/cam | `tools/build-userland-linux.sh` + `tools/sunfish-cam-rx.sh --install`; table: `tools/sunfish-camtab.py --out work/cam/imx363-mode8.txt --mode 8` from vendor.img /lib64/camera/com.qti.sensormodule.metric_imx363.bin | feature under work |

Backups and leftovers on the phone (not needed for a fresh install): /System/Library/xpc/launchd.plist.{orig,pre-*},
/private/etc/launchd.plist.pre-early, /var/local/launchd.plist.pre-*, /var/local/mtl2-clients.pre-*, mtl2-env.pre-*,
com.apple.Accessibility.plist.pre-rt (Reduce Transparency test, reverted), /var/local/bin/*.pre-e111, logs and test
output in /var/local.

## Log
(Newest first. One line per on-device change: date, what, where, how, what reproduces it.)

- 2026-10-31 Camera (feature under work): SunfishAudio.driver rebuilt with the camera device (camera_device.c/inject_media.c/camera_client.c, gated by /var/local/camdev.on, off now) via phone/audio/build.sh + tools/sunfish-install-audio.sh; /var/local/bin/camd + camclient (phone tools); /var/local/camdev.log (0666, empty), /var/local/camd.log; /var/run/sunfish-camera.shm/.sock are created by camd at run time.
- 2026-10-30 Camera (feature under work): camid (+ --mclk-asis, --stream) and camclk in /var/local/bin; /var/local/cam/imx363-mode8.txt (row 29). Test runs leave the SLG51000 LDOs on at camid's verified voltages until reboot; all camcc clocks/GDSC off.
- 2026-10-29 Audio output device (feature under work): SunfishAudio.driver in /System/Library/Audio/Plug-Ins/HAL, VirtualAudio.plugin back to /Library/Audio/Plug-Ins/HAL (tools/sunfish-install-audio.sh, `--undo`; not yet installed).
- 2026-10-30 Filza installed (tools/sunfish-install-filza.sh); a test MP3 copied to /var/mobile/Documents/Pirates.mp3 and Files' File Provider Storage (user content, for the AirPods audio test).

- 2026-10-30 Auto-rotate at boot: adspboot, sensors-start.sh, job org.iosonpixel.sensors (tools/sunfish-install-sensors.sh; not yet installed).
- 2026-10-30 Bluetooth for iOS (feature under work): btbridge, bluetool-stub, launchd edits for BlueTool/bluetoothd/org.iosonpixel.btbridge (tools/sunfish-install-btbridge.sh; not yet installed).
- 2026-10-30 Bluetooth: btup + crbtfw21.tlv/crnv21.bin in /var/local/bin, /var/local/fw/bt (tools/sunfish-install-bt.sh; not yet installed).

- 2026-10-30 audit: table above written from the phone vs the Oct 22 image; added tools/sunfish-setup-phone.sh for rows 9, 10, 12, 15-18.
- 2026-10-29 `L 1`: Location Services on (locationd state). setup.md step 9.
- 2026-10-29 Auto-Lock Never by fake taps (tap4). setup.md step 9.
- 2026-10-29 /var/root/Library/Preferences/com.apple.locationd.plist + LocationServicesEnabledIn7.0 (loc1, no effect; backup .bak-b32).
- 2026-10-29 SweetMetal2 reinstalled several times (rotation, touch, L command): sunfish-metal2-install.sh.
- 2026-10-29 /var/local/sweet-orient created (rotation commands).
- 2026-10-29 launchd: assetsd re-enabled with StandardErrorPath (Photos), lskdd/fairplayd.H2/wifip2pd dropped (B19), dropbear Interactive (D2).
