# Bluetooth
**State:** ⬜ not started; survey done 2026-10-28 (later: after charging, probably after audio). **Updated:** 2026-10-28.

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

### 2026-10-28 14:30 survey (read-only)
- Findings above; total 1-3 weeks before audio. Recommendation: later.
