# Bluetooth
**State:** 🟡 the WCN3990 runs from a phone tool: firmware loaded, BD_ADDR read, LE scan finds 26 devices (btup, 2026-10-30); iOS side not started. **Updated:** 2026-10-30.

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
