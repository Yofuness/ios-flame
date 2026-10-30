# Camera
**State:** 🟡 Stage c done: real IMX363 frames arrive in CSID-lite over CSIPHY0 (2016x1512 RAW10, 3810240 bytes per frame, ECC/CRC 0); no frame in RAM yet (IFE-lite write master + SMMU next); front IMX355 answers. **Updated:** 2026-10-30.

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

### 2026-10-30 23:05 cam9 (kernel108, same boot): capture aborted safely, PMEM buffer above 4 GB; xnu 0232 PMEM_ALLOC32 -> kernel109
- `tools/sunfish-cam-rx.sh --install --capture` (e6d65ae): powered up, camnoc_axi RCG set to 150 MHz (PLL0 even /4, CFG 0x207), then PMEM_ALLOC 8 MB returned PA 0x1afde0000: above 4 GB, IFE IMAGE_ADDR is 32 bit -> "abort", PMEM_FREE 0, powered down. No DMA, no SMMU change, no reset. (Privacy: the phone is in the user's bedroom at night; capture results are checked by numbers only, the picture stays unopened in work/cam/ for the user.)
- Fix: xnu 0232 `PMEM_ALLOC32` (ioctl 'P' 6): kmem_alloc_contig with max_pnum = 4 GB - 1 page; phone/wifi/pmem.c pm_alloc32(); camrx capture uses it. Kernel: kernel109 = kernel108 + 0232.

### 2026-10-30 22:45 cam8 (kernel108, same boot): stage c DONE, real sensor frames reach CSID-lite (c5e17d7)
- `camclk pll0 on` (Fabia, Linux v6.15 sequence): L 0x3e, FRAC 0x8000, TEST_CTL 0x40000000, USER_CTL[15:8] 0x31; lock, MODE 0xd0800004 -> 0xd0800005 after OUTCTRL (VCO 1200 MHz). camclk has no --dry-run.
- `camrx phy on`: settle count 25 (Linux formula: 85 ns + 6 UI at 304 Mb/s/lane, 300 MHz timer; the notes' 36 was wrong); csi0phytimer 300 MHz (src 2 PLL0 even, div 3) + csiphy0 branches on; CSIPHY0 v1.2 redfin 0922 table, lane map 0x3210 (guess, worked). Status 0x8b0..0x8d8 all 0 after phy on.
- `camrx rx arm` (RX_CFG0 0x32103 4 lanes, RDI0 VC 0 DT 0x2b), then `camid --addr 0x1a --keep --mclk-asis --stream` (frame counter 1 ... 63 in 1 s), then `camrx rx report`: **RX_PKTS 119448, ECC 0, CRC 0, RDI0_STATUS 0, bytes ping/pong 3810240/3810240 = exactly one 2016x1512 RAW10 frame**. RX IRQ 0x004000ff (DL0-3 SOT + EOT; bit 22 UNMAPPED_VC_DT, likely the sensor's embedded-data packets), RDI0 0x1ff8 (INPUT SOF/SOL/EOF, SUBSAMPLED SOF/EOF). CSIPHY status after: 00 01 00 20 00 00 04 00 80 40 00 (meaning unknown).
- Cleanup: rx stop, phy off, clocks off, `camclk off`, `camid --addr 0x1a` (GDSC collapse). No reset.
- Next: stage d (IFE-lite write master RDI0 -> a RAM buffer through the SMMU; dump one RAW10 frame to the PC).

### 2026-10-30 22:20 cam7 (kernel108, same boot): stage c1 DONE, CSID-lite test pattern runs (camrx, ba7ebf7)
- Base: `camid --addr 0x1a --keep`, `camclk pll2 on`. Before: cphy_rx/ife_lite_csid/ife_lite/csi0phytimer RCGs CMD 0x80000000 CFG 0; branches off. Refgen (read-only, offsets are guesses) BIAS_EN 0x4040404, BG_CTRL 0x1010101.
- `camrx clocks on`: cphy_rx CFG 0x504 + ife_lite_csid 0x504 (384 MHz, PLL2 early/2.5), ife_lite 0x502 (320 MHz); RCG updates done; branches lite_cphy_rx 0x1, lite_csid 0x221, ife_lite 0x1 (none stuck).
- CSID-lite HW_VERSION 0x20000000. `camrx tpg 2` (640x480 RAW10 colour bars, VC 0xa, 4 lanes, redfin 0921 values): 20 polls; **RX_PKTS 0 -> 205814** (~213 frames/s at ~482 packets/frame, a guess at the packet count), **bytes ping/pong 384000/384000 = exactly one 640x480 RAW10 frame**; SOF/EOF bits set in every poll (INPUT + SUBSAMPLED), RDI0_STATUS 0, CRC 0, ECC reads 0xffff (unverified: saturated or misread field). RX bits 0-7 and 14 latched (not decoded).
- Cleanup: clocks off, `camclk off`, `camid --addr 0x1a` (GDSC collapse). No reset. Note: toybox on the phone has no `sync`.
- Next (c2): PLL0 (Fabia, 1200 MHz) for csi0phytimer 300 MHz (DT), CSIPHY0 v1.2 4-lane init (redfin 0922), CSID RX from the PHY with the sensor streaming (camid needs a keep-streaming option).

### 2026-10-30 22:00 cam6 (kernel108, sweetse9=2 sweetcampmic=2, same boot): stage b DONE, the IMX363 streams
- Tools: tools/sunfish-camtab.py (after ios-redfin's redfin-camtab.py; our module bin is "Parameter Parser V1.3.2", resolutionData 35 words vs redfin's 37) -> work/cam/imx363-mode8.txt (30 init + 82 mode 8 writes, all delays 0); camid `--stream FILE` (commit a55102c).
- Modes in metric_imx363.bin (all 4-lane RAW10 0x2b, 82 writes): 0/7 4032x3024@30 552 Mb/s/lane; 1 4032x2272@30 432; 2 2016x1136@120 464; 3 2016x1136@240 828; 4 4032x2272@30 836; 5/6 4032x2272@60 836; 8 2016x1512@60.13 304 (2x2 bin, default); 9 2016x1136@60 (rate field 0).
- Run: `camid --addr 0x1a --keep`, `camclk pll2 on`, `camclk mclk 0 24`, `camid --addr 0x1a --mclk-asis --stream /var/local/cam/imx363-mode8.txt`: ID OK, 112/112 writes, readbacks 0x0136 0x18, 0x0137 0x00 (24 MHz), 0x0112 0x0a, 0x0114 0x03 (4 lanes); stream on, 0x0100 = 1.
- **Frame counter 0x0005 every 100 ms: 1, 6, 12, 18, 25, 31, 38, 44, 50, 57, 63 = ~62 frames/s** (mode 8 is 60.13). Stream off, 11 pd lines, `camclk off`. No reset.
- Not done: the stream-on table's 0x0138 = 1 (temperature sensor enable) left out. Nothing receives the MIPI data yet.
- Next: stage c (CSIPHY0 v1.2 tables + CSID-lite RDI0; CSID test pattern first).

### 2026-10-30 21:50 cam5 (kernel108, sweetse9=2 sweetcampmic=2, same boot as cam4): stage a2 DONE, IMX363 ID at MCLK 24 MHz
- `camid --addr 0x1a --keep` (MCLK on XO 19.2 MHz): bytes 35 03 63 63, IMX363 ID OK.
- `camclk pll2 on` (lock, MODE 0xc0000007), `camclk mclk 0 24`: CBCR 0x1, CFG 0x2501 M 1 N 0xec D 0xeb.
- `camid --addr 0x1a --keep --mclk-asis`: "mclk0 rcg left as is: cfg 0x2501 m 0x1 n 0xec"; **bytes 35 03 63 63 at 24 MHz**.
- `camclk off` (CBCR 0x80000000, PLL off), `camid --addr 0x1a`: ID OK at XO, then 11 pd lines (reset, mclk, 4 pins, cci_0, 3 branches, GDSC collapse). No reset.
- On-device: /var/local/bin/camid + camclk (phone tools, built by tools/build-userland-linux.sh); nothing left powered.
- Next: stage b (sunfish-camtab.py from redfin-camtab.py + `camid --stream FILE`: init + 2016x1512 mode, 0x0100 = 1, frame counter 0x0005).

### 2026-10-30 21:40 cam4 (kernel108, sweetse9=2 sweetcampmic=2): MCLK0 runs at 24 MHz from PLL2
- Battery 4455 mV, SSH 18 s. campmic GPIOs set at ~30.6 s of boot (not 146 s as on kernel94): "after ... 40 01 ... 44 80 45 01 46 80".
- `pll2 on`: lock, MODE 0xc0000007. `camid --keep`: SLG LDOs on (STATUS 0x02), TITAN GDSC 0x22f001 -> 0xf822f000, cpas/core/camnoc/cci_0 on, MCLK0 on XO (CBCR 0x1), reset high; CCI: 0x10 NACK (status 0x8000000), then 0x1a timeout (status 0). Same as 2026-10-28, where 0x1a only answered when probed alone (`--addr 0x1a`).
- **`camclk mclk 0 24` with the GDSC up: branch on** (CBCR 0x1, CLK_OFF clear), CFG 0x2501 (src 5 = PLL2 out_aux 480 MHz, div 1), M 1, N 0xec, D 0xeb (= 480/20 = 24 MHz). cam2/cam3's "stuck off" was the GDSC being off. `camclk status`: PLL2 lock 1, VCO 1920 MHz. `camclk off` + `camid`: everything powered down (pd lines). No reset.
- Not measured: the actual MCLK frequency on the pin (no scope); the ID read at 24 MHz.
- camid gets `--mclk-asis` (keeps camclk's 24 MHz RCG instead of resetting to XO) for cam5.

### 2026-10-30 21:25 cam3 (kernel108, sweetse9=2; no sweetcampmic): camera PMIC off, sensor not reached
- Battery 4455 mV; boot 5 s, SSH 16 s; webcam: Apple logo (dim, night mode). No reset.
- `pll2 on`: locked again (MODE 0xc0000006, 0xc0000007 after OUTCTRL), same register values as cam2.
- `camid --keep`: "slg rd 0x1105 err 6", "ABORT: slg read" (SE9 fw 0x303 status 0, so the I2C bus is up). Cause: the boot lacked `sweetcampmic=2` (xnu 0218, the PM6150L GPIOs that enable the SLG51000); cm1/cam2 on 2026-10-28 had both args.
- `mclk 0 24`: RCG updated (CFG 0x2501, M 1, N 0xec, D 0xeb), branch stuck off (CBCR 0x80000001) as in cam2: consistent with the TITAN GDSC still off (camid aborted before it).
- `camclk off`: all back off. Next (cam4): same with `sweetse9=2 sweetcampmic=2`.

### 2026-10-30 21:15 cam2 (kernel108, no reboot; no sweetse9): PLL2 locks; MCLK0 branch stays off
- camclk (commit c8633dc) installed. `pll2 on`: L=0x64, USER_CTL 0x30b, CONFIG_CTL 0x20000954, CONFIG_CTL_U 0x400003d6, then BYPASSNL/RESET_N/OUTCTRL; all read back; "pll2 locked, MODE 0xc0000006" (VCO 1920 MHz, aux 480 MHz). Lock Y.
- Correction to cam1: the Agera PLL2 register offsets differ from Fabia; the as-read status0 was USER_CTL 0x1, USER_CTL_U 0x20000954, CONFIG_CTL 0x3d6, CONFIG_CTL_U 0x04000400 (reset values).
- `camid --keep` stopped before any write: "SE9 clocks off (no GCC writes from here)": this boot had no `sweetse9=2`, so no sensor power/GDSC.
- `mclk 0 24`: RCG update done (M 1, N 0xec, D 0xeb, CFG 0x2501 src 5 div 1; CMD 0x80000000), but "mclk0 branch stuck off (CBCR 0x80000001)". Guess (unverified): the TITAN_TOP GDSC is off without camid's power-up.
- `camclk off`: CBCR 0x80000000, PLL2 off (lock 0, outctrl 0). No reset. The RCG keeps the 24 MHz config until reboot.
- Next (cam3): boot kernel108 with sweetse9=2, `camid --keep`, `pll2 on`, `mclk 0 24`, re-read the ID.

### 2026-10-30 21:00 Camera port plan from ios-redfin; camcc read-only dump (cam1, kernel108)
- Plan: research/notes/camera-port.md (redfin MIT: sweet_cam.c via patches 0919/0921-0934, software ISP camera_isp.c, FigCaptureDevice camera_device.c; same sensors/CSID-lite/IFE-lite/CSIPHY 1.2 addresses as ours; different camcc (sdmmagpie) and SMMU SIDs). Six stages, ~2-3 weeks.
- cam1 (read-only): camcc base 0x0ad00000 (+0x10000); GCC_CAMERA_AHB_CBCR 0x10b008 = 0x1 (on). PLL0-4 (0x0..0x4000): MODE/L/ALPHA 0 (off, no lock); PLL0/1/3/4 identical reset-looking CONFIG (USER_CTL 0x4805, CONFIG_CTL 0x20485699), PLL2 different (USER_CTL 0x20000954, CONFIG_CTL 0x3d6). MCLK0-3 RCGs (0x5004+0x20n) off (CMD 0x80000000, CFG/M/N/D 0), CBCRs 0x80000000 (off). Linux camcc-sm7150.c: all Fabia; 24 MHz MCLK = pll2 OUT_AUX, M/N 1/20. Next: a camclk tool to configure/enable PLL2 + MCLK RCG at 24 MHz, then camid at 24 MHz.

### 2026-10-28 22:10 M2 prep (read-only): the IMX363 init/mode tables are in the camx module file
- vendor.img /lib64/camera/: com.qti.sensormodule.metric_imx363.bin (1,044,640 B; sensor + actuator + OIS + EEPROM), com.qti.sensor.imx363.so, com.qti.sensormodule.primax_imx355.bin + .so, Google tuning bins. Google's files: decoded only in the session scratchpad (camx/dec.py), not in git.
- Format: a symbol table (0x48-byte records: index, name, data offset, size) then a data area at 0xacd50; register lists are 80-byte items with a 16-bit address, an 8-bit value and a delay.
- IMX363: init table 30 registers, starting with EXCK_FREQ 0x0136/0x0137 = 24.00 MHz; 10 modes of 82 registers each, all 4-lane RAW10 (0x0114 = 3, 0x0112 = 0x0a0a), pre-PLL divider 4: 4032x3024 @30 (~1.37 Gb/s/lane), 4032x2272 @30, 2016x1136 @120/240, 2016x1512 @60 (2x2 binning). Stream on 0x0100 = 1. I2C 0x34 (8-bit) = 0x1a. Power order in the module: VIO first, MCLK 24 MHz last, reset released before it.
- camid runs MCLK at 19.2 MHz (XO, no camcc PLL). For streaming either bring up the camcc PLL for 24 MHz (cleanest, the tables then apply as-is) or patch EXCK_FREQ to 19.2 MHz and scale the PLL multipliers by 1.25.

### 2026-10-28 21:30 camf1 (kernel94): the front sensor answers: IMX355
- `camid --front` (1bfeea3): LDO2 MISC1 bit0 = 1 (2.4 V base); VSEL LDO7 0x3c (1.8 V), LDO2 0x82 -> 0x50 (2.8 V), LDO5 0x8c -> 0xa0 (1.2 V), all STATUS 0x02. MCLK2 (0xad05044 / 0xad0505c) on XO, gpio19/20 (cci_i2c, south) 0 -> 0x7, gpio15 (cam_mclk) 0x1 -> 0x44, reset gpio68 (north).
- CCI0 master 1 at 7-bit 0x1a: reg 0x0016 = bytes `35 03 55` -> **0x0355 = Sony IMX355**; reg 0x0000 = 00 00. rc 0, all powered down after.

### 2026-10-28 20:45 cam2 (kernel94, sweetse9=2 sweetcampmic=2): M1 done, the rear IMX363 answers (ID 0x0363 at 7-bit 0x1a)
- phone/wifi/camid (98952da + the hf_axi fix): SLG51000 PATN_ID 05 35 a9, LDO1 MISC1 bit0 = 1 (2.4 V base). VSEL LDO7 0x3c (1.8 V), LDO3 0xa5 (2.85 V), LDO1 0x82 -> 0x5a (2.85 V), LDO4 0x3c (1.8 V), LDO6 0x8c -> 0x82 (1.05 V); enable via MATRIX_CONF_A 0x110d bits 6,2,0,3,5: every STATUS = 0x02 (VOUT_OK, no ILIM).
- GCC camera_ahb 0x1, camera_xo -> 0x1, hf_axi read CLK_OFF until the GDSC was up (first run aborted on it), then 0x1. TITAN_TOP GDSC 0x22f001 -> 0xf822f000 (PWR_ON). camcc cpas_ahb 0x1, core_ahb on, camnoc_axi 0x221, slow_ahb + cci_0 + mclk0 RCGs on XO (19.2 MHz), cci_0 and mclk0 branches 0x1. TLMM gpio17/18 (south) 0 -> 0x7 (cci_i2c, pull-up), gpio13 (south) 0x1 -> 0x44 (cam_mclk, 4 mA), gpio43 (north) 0x1 -> 0x200 then reset high.
- CCI0: HW_VERSION 0x10070000, reset done (status 0x1000000). Address 0x10: status 0x8000000 (NACK). Address 0x1a (run alone): READ_BUF_LEVEL 1, bytes `35 03 63 63` = slave echo 0x35 + **0x0363 = IMX363**. rc 0; everything powered down in reverse after.
- Next (M2, weeks): the sensor's init tables (camx sensor module bins in the vendor image), CSIPHY0 lanes, CSID RDI0, IFE RDI write master into a buffer (SMMU context bank), one raw frame to the PC.

### 2026-10-28 18:50 cm1 (kernel94, sweetse9=2 sweetcampmic=2): camera PMIC SLG51000 answers
- `i2cprobe scan` on kernel93 (before): 0x40/0x41 (amps), 0x50-0x53 (an EEPROM-like device, 4 blocks: reg 0 = 31/04/2a/70; maybe the camera module EEPROM), 0x75 silent.
- xnu 0218 (kernel94) `sweetcampmic=2`: PM6150L GPIO 1/5 (enable) then 4/6 (cs), all TYPE 0x10 SUBTYPE 0x10 (LV), before MODE 0 OUT 0, after MODE 0x01 OUT 0x80 EN 0x80 (output high), at 146.3 s.
- Then 0x75 answers. SYSCTL 0x1100..0x110f = 00 ff 00 02 05 05 35 a9 03 75 00 00 40 00 00 00: PATN_ID (0x1105-07) = 05 35 a9, SYS_CONF_A 0x1109 = 0x75 (its own address: the register map is right). MATRIX_CONF 0x1118/19 = 00 00 (no LDO enabled). LDO VSEL 0x2000..0x2600 (LDO1-7) = 82 00 82 a5 00 3c 00.
- Next (M1, bigger): LDO enables at the DT voltages (vio LDO7 1.8 V, vana LDO3 2.85 V, vdig LDO1, custom1 LDO4 1.8 V, custom2 LDO6 1.05 V; vaf = PM6150L GPIO12), MCLK0 (TLMM gpio13, camcc 24 MHz), reset gpio43, then a CCI master driver to read the IMX363 ID (0x0016 = 0x0363). Read the 0x50-0x53 EEPROM (read-only) first: it may hold the module ID.

### 2026-10-28 16:55 k88a camera probe (kernel88, sweetcamprobe=1, read-only)
- gcc_camera_ahb_cbcr = 0x1 (on), titan_top_gdscr = 0x22f001, mclk/csiphy/ife CBCRs = 0x80000000 (off), cci_0 = 0; SE9 skipped (its clocks are off: needs a clock vote before any I²C to the slg51000).

### 2026-10-28 13:00 survey (read-only)
- Hardware table and the RDI path above, from recon/live.dts and the shared cache. Effort: M1 ~1 week, M2 3-5 weeks, M4 4-8+ weeks.
