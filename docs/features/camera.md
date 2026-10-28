# Camera
**State:** ⬜ not started; survey done 2026-10-28. **Updated:** 2026-10-28.

## Facts
Hardware (recon/live.dts):

| Item | Node / compatible | Facts |
|---|---|---|
| Rear sensor | `qcom,cam-sensor@0` (:12970) | CCI0 master 0, CSIPHY0, MCLK0 = GPIO13, reset GPIO43, camcc 0x50 at 24 MHz; actuator@0, eeprom@0, ois@0, flash |
| Front sensor | `qcom,cam-sensor@1` (:13014) | CCI0 master 1, CSIPHY2, MCLK2 = GPIO15, reset GPIO68, camcc 0x54 at 24 MHz |
| Sensor IDs | not in the DT | expected IMX363 (rear), IMX520 (front); addresses from Google's camx sensor modules or a CCI scan |
| Rear supplies | slg51000@75 on SE9 I²C 0xa8c000 (:9563) | vio ldo7 1.8 V, vana ldo3 2.85 V, vdig ldo1 2.85 V (check), custom1 ldo4 1.8 V, custom2 ldo6 1.05 V; vaf gpio-regulator "camera_ldo" 2.85 V (:22402) |
| Front supplies | same PMIC | vio ldo7 1.8 V, vana ldo2 2.8 V, vdig ldo5 1.2 V |
| CCI0 / CCI1 | `qcom,cci` @0xac4a000 / 0xac4b000 | IRQ 0x1cc, GPIO17-20, 37.5 MHz source |
| CSIPHY0-3 | `qcom,csiphy-v1.2` @0xace0000/2000/4000/6000 | lane data at :12686 |
| CSID0/1, VFE0/1 | `qcom,csid175_200` @0xacb3000, `qcom,vfe175_130` @0xacaf000 | Titan 175 |
| CSID-lite, VFE-lite | @0xacc8000, `qcom,vfe-lite175` @0xacc4000 | |
| Clocks, power, bus | `qcom,camcc-sdmmagpie` (:621), titan_top GDSC @0xad0c1c4, `qcom,cam-cpas` @0xac40000 | CAMNOC/AHB/AXI votes |
| SMMU | `qcom,msm-cam-smmu`, cb "ife" (:13144) | stream IDs 0x900/0xd00/0x880, mask 0x460 |

iOS side: the iPhone X userland (d22ap) uses AppleH10CamIn (research/ipsw-x/d22ap.adt: AppleH10CamIn, isp0, isp0-config); the capture plugin is /System/Library/MediaCapture/H10ISP.mediacapture (IOConnect to AppleH10CamIn + the ISP firmware protocol; needs the ADT camera-rear/front calibration). The live log has no CamIn or cameracaptured lines: nothing has tried to open a camera yet.

## Plan
Path: raw Bayer through RDI (CSIPHY -> CSID -> IFE RDI write master -> memory), with register sequences from mainline camss (csiphy-3ph-1-0, csid-gen2, vfe-170 RDI; VFE 175 is close to sdm845's vfe-170), not the downstream CAM-ISP stack. Skip ICP/IPE/BPS/JPEG.
1. **M1 (1-1.5 weeks): read the rear chip ID over CCI** (IMX363 0x0016 = 0x0363). xnu: slg51000 regulator driver over SE9 I²C (DT voltages exactly, read back before enable), camcc + titan_top GDSC, CCI master, MCLK pinmux, reset GPIO, a probe sysctl. Power order: vio, vana/vdig, MCLK, reset high; never above the DT maximums.
2. **M2 (3-5 weeks): one raw frame to the PC.** IMX363 init tables (from the camx sensor module bins in the backed-up vendor image; out of git), CSIPHY0 lanes, CSID RDI0, VFE RDI write master into a static buffer, SMMU context bank, cpas vote. Risks: SMMU faults and bus hangs.
3. **M3 (1-2 weeks): a debayered preview** in a test app (IOSurface + Metal), without AVFoundation.
4. **M4 (4-8+ weeks, uncertain): the Camera app.** A fake AppleH10CamIn user client and an ISP-firmware-protocol stub for H10ISP.mediacapture.

## Open
- Does ABL leave camcc on? (read CAMCC / GDSC registers read-only first)
- Find the sensor module bins in backup/ (vendor image).
- Disassemble H10ISP.mediacapture (one image, 8 GB scope) and count its IOConnect selectors.

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

### 2026-10-28 13:00 survey (read-only)
- Hardware table and the RDI path above, from recon/live.dts and the shared cache. Effort: M1 ~1 week, M2 3-5 weeks, M4 4-8+ weeks.
