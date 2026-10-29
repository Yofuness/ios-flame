# Bluetooth
**State:** ✅🟡 Bluetooth works in iOS Settings: on, discoverable, nearby devices listed (bt7, kernel105+, btbridge=1 default + btbridge). Pairing works (Xbox controller, U9); audio devices crash bluetoothd (AirPods; redfin 1016 next); 115200-baud polled UART drops bytes. **Updated:** 2026-10-30. **Updated:** 2026-10-30.

## Facts
| Item | Value | Ref (../recon/live.dts) |
|---|---|---|
| BT node | /soc/bt_wcn3990, "qca,wcn3990" | :884-897 |
| UART | qupv3_se3_4uart @0x88c000 (QUP0 SE3, msm-geni-serial-hs, 4-wire, alias hsuart0) | :9290-9300, :23626 |
| Supplies | vdd-io pm6150_l10 1.704-1.904 V; vdd-core pm6150l_l2 1.245-1.352 V; vdd-pa pm6150l_l10 3.2-3.4 V; vdd-xtal pm6150l_l1 1.704-1.904 V | :886-893 |
| Enable | no BT enable GPIO (regulators only) + UART pinctrl 0x177-0x179 | |
| SCO audio | over SLIMbus (btfmslim, slim@62e40000) | :997 |
| Firmware | /firmware/crbtfw21.tlv (210348 B), crnv21.bin (4722 B) in restore/factory/vendor.img (out of git) | |
- Our kernel has no QUP/GENI UART driver.
- iOS: bluetoothd expects Apple's Broadcom controller (BlueTool boots it with BCM vendor commands over an AppleSerial UART; details unverified: bluetoothd and BlueTool are on the phone's root, not in the shared cache). bluetoothd is exec'd every ~10.06 s with no logged reason: likely a clean exit when no transport exists + launchd's throttle. Harmless.

## Plan
1. RE BlueTool/bluetoothd from the root: what they open, the boot script (0.5-1 day).
2. A GENI UART driver (FIFO/PIO) + the 4 regulator votes; HCI_Reset reply from the chip (2-4 days; risk: QUP clocks / SE firmware state left by ABL).
3. QCA rampatch + NVM download (btqca.c: EDL_PATCH 0xFC00, NVM, HCI reset, baud 0xFC48 to 3.2 Mbaud) in a userland "btland"; a test scan (1-2 days).
4. An iOS transport shim: fake the BCM boot and vendor replies until bluetoothd stays up and Settings finds devices (3-10 days, high uncertainty).
5. Audio over SLIMbus: after the audio project.

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

### 2026-10-30 14:50 btbridge watchdog + SIGTERM, xnu 0231 Skywalk privileges (kernel108; untested)
- btbridge: SIGTERM/SIGINT -> chip off, close /dev/btbridge, remove the pid file; watchdog (-W off): an unanswered HCI_Reset (2 s) or 3 unanswered commands -> off + bring-up again, a local Command Complete for the pending reset; bt_uart restarts the RX sequencer after an overrun if it stopped (-R off).
- xnu 0231 (from ios-redfin 1016, MIT): PRIV_SKYWALK_REGISTER_USER_PIPE / _KERNEL_PIPE / _LOW_LATENCY_CHANNEL for platform binaries or holders of the matching entitlements (AirPods/A2DP crash); sweetskypipe=0 off.

### 2026-10-30 13:45 PAIRING WORKS: Xbox controller connected (U9 by the user; kernel107, btbridge)
- First try: the Settings switch only spun. btbridge log: bluetoothd sent scan disable (0x200c->0x2042 lext), then HCI_Reset + Read_Local_Version, and the chip never answered after the bridge had dropped 71 bytes with 4 RX FIFO overruns (polled UART at 115200). btbridge's SIGTERM was ignored (needed kill -9). Restarting btbridge (power cycle + firmware, 20 s) fixed it.
- Second try: an Xbox controller paired and showed connected (stats: acl up 183 / down 316 packets, cmd 598, evt 1693; 42 bytes dropped, 1 overrun).
- AirPods Pro 2: bluetoothd crashed (kernel "CRASH bluetoothd reason ns 2 code 0xb") during pairing, launchd restarted it 6 s later and it reconnected to the kernel control. Matches redfin's note for audio devices (their 1016 Skywalk user-pipe privilege for A2DP).
- Bugs to fix: (1) the chip goes silent after RX overruns (no recovery: btbridge should power-cycle/re-init on a host HCI_Reset that gets no answer, or on overrun); (2) btbridge ignores SIGTERM; (3) port redfin 1016; (4) 3 Mbaud or interrupt RX to stop the drops.

### 2026-10-30 10:40 DEVICES LISTED in Settings > Bluetooth (bt7, kernel105 btbridge=1; btbridge e739ed8)
- Legacy -> extended LE translation (from iOSPlus10 0338, MIT; phone/wifi/bt_lext.c, host test tools/bt-shim-test 41/41): bt6 showed bluetoothd had sent 0x204a (an extended command) before 0x200b, so the chip rejected legacy scans. Now 0x200b/0x200c -> 0x2041/0x2042, answers delivered as legacy; extended reports -> legacy reports.
- Settings > Bluetooth: 0 devices at 20 s, 1 device (a TV) at 40 s. 23 lext lines, all status 0; bluetoothd one pid (302), no restarts. Only error: 0x2007 (read adv TX power) 0x0c -> shim 0 dBm. No reset; 4455 mV.
- Stats: cmd 159, evt 120, local 92; uart dropped 56 bytes, 2 FIFO overruns (polled UART at 115200: open item).
- `btbridge=1` is now a sunfish-boot.sh default.
- Open: pairing (headset/keyboard/phone), audio profiles (A2DP: redfin 1016 Skywalk privilege), higher baud (QCA 0xfc48 to 3 Mbaud) or interrupt RX to stop the drops, boot-time firmware load (~52 s).

### 2026-10-30 10:10 BLUETOOTH ON IN SETTINGS (bt6, kernel105 btbridge=1; btbridge 629d50c)
- bt5b root cause: btbridge's poll() on /dev/btbridge never reported POLLIN, so bluetoothd's HCI_Reset/Read_Local_Version sat unread in the kernel ring (kernel "unread by the bridge" grew) -> timeouts, restart every ~21 s. Fix: read the device every pass (O_NONBLOCK).
- bt6: bluetoothd (pid 303) connects to the kernel control at 108.7 s, sends its init (0c03, 1001, 1002, 1009, 1003, 1004, 2003, 0c01, 2001, 0c56, 0c6d, 2002, 202f, 2031, 2024, 201c, 200f, 1005), all answered; Apple VSCs 0xfce9/0xfce2 answered by the shim (status 0 + 8 bytes). No restart over several minutes.
- Settings: Bluetooth row "On"; the page shows the switch ON, "This iPhone is discoverable as iPhone", a spinner at DEVICES, no devices listed.
- Classic inquiry runs (0x0401 -> Command Status 0 -> Inquiry Complete, repeated). LE: the chip answers 0x200b (Set Scan Params) and 0x200c (Set Scan Enable) with 0x0c Command Disallowed; the shim rewrites to 0, so bluetoothd thinks it scans but nothing comes ("controllerSupportsScanRx:0" in its scan agents). iOSPlus10 0338 (legacy LE -> extended LE translation) is the known fix.
- No reset; 4455 mV.

### 2026-10-30 09:40 bt5: bluetoothd takes the kernel transport; Settings spinner (kernel105 btbridge=1)
- Bridge at boot: patch 51.8 s (slow at boot), NVM 2.0 s; logging-off 0xfc17 -> `04 0e 05 01 17 fc 00 14` (status 0); HCI_Reset OK; drain: 0 dropped, 0 overruns (the bt4 framing slips are gone); "kdev: /dev/btbridge fd 4; btbridge: ready, pumping" at ~98 s.
- /dev: cu.bluetooth + uart.bluetooth (21,0, 0666), btwake (21,2), btbridge (21,1, 0600).
- bluetoothd[321] (started before the bridge was ready): "Searching for transport H4BC" / driver "POSIX" matched (no more "could not be matched"), then HCI_Reset 0x0c03 and Read_Local_Version timed out (STATUS 621) -> restart reason 108. bluetoothd[335] then stayed up (PowerManager power state 2, "Returning bluetooth power state: cafebabe") but sent no HCI command; no kernel "uart.bluetooth" line.
- bluetool-stub answered pid 306's boot/init ("btbridge ready after 0 ms"). Settings: Bluetooth row "Off"; the Bluetooth page shows a spinner instead of the switch. No reset; 4455 mV.
- Next: restart bluetoothd once the bridge is ready (bt5b); gate the kernel control connect until /dev/btbridge is open, so early starts fail fast instead of timing out.

### 2026-10-30 08:50 xnu 0226 kernel bridge (from ios-redfin 1010 + 1015, MIT) built: kernel105 (untested)
- `btbridge=1`: kernel control com.apple.uart.bluetooth (stream, not privileged, one client) + /dev/cu.bluetooth = /dev/uart.bluetooth (one pipe), /dev/btbridge (userland end, our btbridge), /dev/btwake (dummy); 16 KiB rings, termios stored. `btbridge=3`: nodes only. Off by default. Not taken: redfin 0982 nubs (our ADT node is seen), 1016 Skywalk privilege (A2DP, later).
- btbridge now opens /dev/btbridge (`-p` = old pty); bluetoothd env BT_HCI_TRANSPORT=H4BC:POSIX:com.apple.uart.bluetooth,115200 (rerun sunfish-install-btbridge.sh).

### 2026-10-30 08:30 bt4: bluetoothd has no plain H4 driver; bridge bring-up fails at boot (kernel98, ADT bluetooth node)
- Installed (sunfish-install-btbridge.sh): BlueTool job -> bluetool-stub, bluetoothd env BT_HCI_TRANSPORT=H4 BT_BAUDRATE=115200, job org.iosonpixel.btbridge; backup launchd.plist.pre-btbridge. ADT /arm-io/bluetooth (work/boot/sunfish-hi.adt; old one .pre-bt).
- bluetoothd (oslog): "DeviceTree transport = 0x00000003" (ADT node seen), "Searching for driver: H4" -> "FATAL ERROR - HCI Driver H4 could not be matched" -> "Transport layer initialization failed" -> crash reason=102, restart every ~10-20 s. So 16.7 bluetoothd has no plain-tty H4 driver: the pty route is dead; it needs the kernel control com.apple.uart.bluetooth (H4BC:POSIX:..., iOSPlus10 0332 / redfin 1015).
- bluetool-stub: gets RunBuiltinScript boot + init with btaddress (from the ADT), answers result 0 / crbtfw21 / 29 per bluetoothd pid. Works.
- btbridge at boot: version OK, patch 100 %, then H4 resyncs (166 bytes dropped), Read_Local_Version: none -> bring-up 1 failed; retry got no reply at all. The same code works when run by hand later (bt3). Timing at boot (Wi-Fi bring-up, clocks) is the suspect. /dev/cu.bluetooth never created.
- Settings: Bluetooth row "Off". No reset; 4455 mV. Install left in place (bluetoothd crash-loops as before B15; undo: sunfish-install-btbridge.sh --undo).
- Next: port redfin 0982 (nubs) + 1010 (kernel bridge char device) + 1015 (kernel control com.apple.uart.bluetooth), our btbridge as the userland side; investigate the boot-time bring-up failure.

### 2026-10-30 07:45 iOSPlus10 (MIT) has iOS Bluetooth on 20H392: we can reuse it
- github.com/akshaynexus/iOSPlus10 (OnePlus 10 Pro, QCA6490): bluetoothd reaches STACK_READY, Settings switch on, nearby devices listed. MIT licence, so code can be adapted with attribution (CREDITS.md).
- Their key facts for us: bluetoothd needs an IORegistry `bluetooth` entry with a non-zero local-mac-address (ADT /arm-io/bluetooth) or it exits "No bluetooth on this device"; transport via env BT_HCI_TRANSPORT (they use a kernel control socket; /dev/cu.bluetooth also works); BlueTool stand-in answers result 0; every Apple VSC (OGF 0x3f) needs Command Complete status 0 with >= 2 (they send 8) data bytes or bluetoothd times out and restarts (_BCM_ByteStreamEventParser); 0x2007 and 0x0c6d need rewrites.
- Our btbridge (userland pty) + BlueTool stand-in now follow those rules; their kernel transport is the fallback if a pty is refused.

### 2026-10-30 07:20 iOS side: how bluetoothd finds the controller (read-only; research/notes/bt-ios.md)
- Facts from our 16.7 bluetoothd (strings, research/re/bt/, out of git): it opens /dev/cu.bluetooth or /dev/uart.bluetooth ("No valid serial port found (tried cu.bluetooth and uart.bluetooth)"), reads env BT_HCI_TRANSPORT ("not set - Attempting to read from plist"), and talks to com.apple.BlueTool (job: /usr/sbin/BlueTool -R, mobile). iPhone X ADT: uart1 child "bluetooth,n88", 3 Mbaud, H4. No /etc/bluetool scripts on the root (inside BlueTool).
- From hakios-sm8150 (16.5, reference only, no licence): BlueTool reply {result: 0, firmware, manufacturer 29}; bluetoothd stops after 0x0c31 unless Supported_Commands octet 18 bit 3 is set (then 0x0c5b needs status 0); 0xfd48 must get the chip's own answer; other OGF 0x3f -> status 0. Their restart loop is unsolved.
- Plan: (1) BlueTool stub answering result 0; (2) bridge daemon = btup firmware load + openpty, /dev/cu.bluetooth -> pty slave, BT_HCI_TRANSPORT=H4 for bluetoothd; (3) an H4 shim in the bridge that logs every command and applies the fixes above, then new ones from our trace.

### 2026-10-30 06:50 LE SCAN WORKS: 26 devices (bt3, kernel98; btup 51134a5)
- Root cause of bt2's garbled reply: the chip also sends H4 ACL packets (handle 0xedc, ~497 B: the QCA debug log); the old parser (btup.c event(), old lines 420-440) skipped to the next 0x04 byte and framed "events" out of ACL payload. New reader parses events + ACL, resyncs one byte at a time, counts S_RX_FIFO_WR_ERR overruns.
- `btup fw /var/local/fw/bt --scan 10`: patch 210348 B in 20.0 s, NVM 4722 B; HCI_Reset OK; drain 0 events, 0 dropped, 0 overruns; Read_BD_ADDR OK (address not recorded in git); local version: HCI 9 (5.0), LMP 9, manufacturer 0x1d (Qualcomm), subversion 0x2be.
- Classic inquiry 10.24 s: complete, status 0, 0 devices (nothing discoverable nearby, or inquiry needs more work). LE passive scan 10 s: 26 devices, 1 with a name (a smart-light brand), RSSI -47 to -98; 7 ACL packets, 0 bytes dropped, 0 overruns. rc 0, no reset.
- Next: the iOS side: bluetoothd's transport (H4 over a tty, BlueTool boot script); research first (RE bluetoothd/BlueTool on our root; hakios-sm8150 vayu_bt.c as reference).

### 2026-10-30 06:20 Firmware loads; BD_ADDR read garbled (bt2, kernel98)
- `btup fw /var/local/fw/bt`: patch 210348 B in 866 segments, 20.1 s; NVM 4722 B, 20 segments, 0.6 s ("hci tag 82 01 11 -> ibs off, baud 115200"); HCI_Reset after -> `04 0e 04 01 03 0c 00` OK.
- Then a burst of mis-framed data ("dropped" bytes `c6 0b 00 20 14 00 ff ...`, a 98-byte "event" `04 05 5f 0e ...`), and Read_BD_ADDR read `04 fd 01 00` -> failed, rc 1. Version afterwards unchanged (patch 0x6699; the ROM version reply doesn't reflect the RAM patch). No reset.
- Guess: the polled RX loses bytes or mis-counts the last FIFO word, so H4 framing slips. Next: robust H4 reader + matching-opcode waits, then `btup fw DIR --scan N`.

### 2026-10-30 06:00 THE CHIP TALKS: HCI reset + version over SE3 (bt1, kernel98; btup 1aa1fea)
- Plan: research/notes/bt-plan.md (hakios-sm7150 bt_wcn3990.c as reference, no licence -> no code copied). Tool phone/wifi/btup.c, install tools/sunfish-install-bt.sh (btup + crbtfw21.tlv/crnv21.bin into /var/local/fw/bt).
- `btup status` (read-only): GCC vote 0x41bc1bc0 (wrapper-0 m/s ahb, core, core2x on; SE3 bit 13 off), SE3 cbcr 0x80004220 (off), RCG cfg 0x100000; gpio38-41 func 0 (not qup03). So ABL leaves SE3 unclocked.
- `uart-loop`: SE3 FW rev 0x20c, protocol 2 (UART) after btup's clock/vote setup; internal loopback 6/6 bytes OK.
- `power`: ldoa10 vote (was already on: "on -> 0"; Wi-Fi up), 0xC0 @2400 then 0xFC @115200; 1 byte 00 back.
- `hci-reset`: `04 0e 04 01 03 0c 00` = Command Complete, status 0. **First reply from the Bluetooth chip.**
- `version`: `04 ff 0e 00 02 0a 00 00 00 99 66 01 10 24 02 01 40` -> product 0xa, patch 0x6699, rom 0x1001, soc 0x40010224 (hakios' phone: rom 0x201, soc 0x40020140; different chip rev). Stock vendor has only crbtfw21.tlv / crnv21.bin, so those stay.
- `off`: ldoa10 off. No reset; 4438 mV.
- Next: `btup fw /var/local/fw/bt` (patch + NVM, ~22 s at 115200), then Read_BD_ADDR; then inquiry/LE scan.

### 2026-10-28 14:30 survey (read-only)
- Findings above; total 1-3 weeks before audio. Recommendation: later.
