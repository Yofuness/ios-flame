# Camera
**State:** 🟡 Stage d DONE: one capture gives a complete raw IMX363 frame (2016x1512 RAW10) in RAM and a file (`tools/sunfish-cam-rx.sh --capture`: SMMU SMRs 0x8c0 + 0xcc0 exact -> bypass), no reset; front IMX355 answers; no ISP / iOS camera yet. **Updated:** 2026-10-31.

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

### 2026-10-31 02:55 cam21 (kernel109, cam20r boot): STAGE d DONE, one capture = full frame, no reset
- `CAPMODE="--bypass --sid 0x8c0 --sid2 0xcc0"` (b5e38a9): SMR15 0x8c0 + SMR16 0xcc0 (both mask 0) -> CB14 bypass; trap CB15: SMR17-22. buf done ~220 ms; 0 faults on either bank, GFSR 0; **frame 3777917 of 3810240 nonzero, 0 of 3720 1 KB blocks zero**; guard 11997 nonzero (+0..+12095: the frame is ~4.8 lines of 2520 B longer than 2016x1512, both halves now); late writes N; restored; "saved, fsync ok". **No reset: 8 of 8 SSH checks over 4 min.** Screen fine (webcam).
- **Cause of cam17/cam18B/cam20's resets: the single masked SMR 0x8c0/0x400**, not SID 0xcc0 itself (guess why: the hypervisor's SMMU emulation or the SMR mask width; not investigated). Rule: exact SMRs only. sunfish-cam-rx.sh's default CAPMODE is now this config.
- Frame (numbers only, not viewed): work/cam/cam21.raw (+ PNGs).
- Next: the extra ~5 lines (embedded data? set FRAME_BYTES / WM height from the CSID), exposure/gain so frames aren't dark (IMX363 registers 0x0202/0x0204), then stage e (ISP + iOS camera device, redfin camera_isp.c / camera_device.c).

### 2026-10-31 02:40 cam20 (kernel109, cam18r boot): sf_axi on + one SMR 0x8c0/0x400: frame ALL ZERO, RESET a few min later
- camid: camera_sf_axi 0x80000000 -> 0x1 after the GDSC (hf_axi 0x1, xo 0x1). SMRs: 0x8c0/0x400 -> CB18 bypass, 6 others -> CB19 trap. buf done ~200 ms; 0 faults sampled; **frame 0 of 3810240 nonzero, guard 0**: not even the 0x8c0 half landed. Late writes N, restored. Then SSH timed out a few minutes after "powered down"; fastboot, 4439 mV; rescued cam20r (23:13). Per-boot log 20261029-230036.log ends at ~1425 s, no panic.
- New reading: every run with the one masked SMR (0x8c0 mask 0x400: cam17, cam18B, cam20) lost all data and reset; cam15 (exact SMR 0x8c0 only, 0xcc0 unmatched) landed the 0x8c0 half and reset; cam16/18A/19 (exact 0x8c0 + 0xcc0 on an exact trap SMR) landed it and did not reset. So the masked SMR may itself be the problem (unverified). sf_axi stays on (redfin does it; harmless).
- Next (cam21): two exact SMRs, 0x8c0 and 0xcc0 (`--sid 0x8c0 --sid2 0xcc0`), both into the bypass bank.

### 2026-10-31 02:25 cam20 prep (read-only, kernel109 cam18r boot, camera powered down): TBUs are on; camera_sf_axi is off
- pinsample reads: TBU vote GDSCs 0x17d040 (hf0) / 0x17d044 (sf) / 0x17d048 (hf1) all 0x80000001 (bit31 PWR_ON = 1, our SW_COLLAPSE vote off: someone else keeps all three TBUs on); TBU status 0x15182210/18/20 = 1, 1, 1. So the hf1 TBU behind SID 0xcc0 is powered: the research's first guess is out.
- GCC camera_hf_axi 0x10b020 0x80000001 (camera off now); **camera_sf_axi 0x10b06c = 0x80000000 (disabled)** (Linux v6.15 gcc-sm7150.c: halt/enable 0xb06c; research/re/gcc-sm7150.c). redfin 0921 enables sf_axi; we never did.
- camid now enables camera_sf_axi next to hf_axi. Next (cam20): `--bypass --sid 0x8c0 --sid-mask 0x400` (cam17's config) with sf_axi on.

### 2026-10-31 02:10 cam19 (kernel109, cam18r boot): FULL FRAME from two half captures, no reset
- A `--bypass --sid 0x8c0`: buf done ~210 ms, frame 1889325 nonzero, guard 5917 (+1..+12095), late writes N, restored; 2 min SSH OK. B `... --addr-offset 0x400`: buf done ~220 ms, frame 1888380 nonzero, guard 6083 (+64..+11327), late writes N, restored; 3 min SSH OK.
- `sunfish-cam-merge.py cam19-a.raw cam19-b.raw cam19-full.raw`: 3721 blocks of 1 KB, **empty even 0, odd 0**; merged: 0 all-zero 1 KB blocks, mean byte 31.2, max 255, histogram [142516, 326650, 3507, 12443, 11779, 6189, 1574, 6233, 3527, 1554, 53, 1543, 12445, 6150, 1537, 6620]; raw2png gray 0..245, colour (RGGB guess) R 0..245 G 7..134 B 0..245. Files in work/cam/ (cam19-full*.png), not viewed (privacy, night).
- Root cause research for SID 0xcc0 (research/notes/camera-sid-cc0.md): 0x8c0 goes through TBU mnoc_hf_0 (SIDs 0x800-0xbff), 0xcc0 through TBU mnoc_hf_1 (0xc00-0xfff) (live.dts:13930-13958); TBU power votes GCC 0x17d040 (hf0), 0x17d044 (sf), 0x17d048 (hf1); redfin also turned on GCC camera_sf_axi (we don't). Next: read-only reads of those + the TBU status registers, then sf_axi on, then the hf1 TBU vote, one per boot.

### 2026-10-31 01:45 cam18 (kernel109, cam17r boot): 0xcc0 writes stay in the buffer; bounded translation still RESETS
- A (`--bypass --sid 0x8c0`, cadab15, FAR sampling): 3 trap faults sampled, all SID 0xcc0, FAR 0x9cb00400, 0x9cb88c00, 0x9ce76400 (all inside the buffer); frame 1889287 nonzero, guard 5909 (+1..+12095); late writes N; restored; **no reset** (6 of 6 SSH checks in 3 min).
- B (`--xlate`: SMR 0x8c0/0x400 -> stage-1 bank, tables map only IOVA 0x9ca00000..0x9d600000 identity in 2 MB blocks; L1 [2] = 0x110b84003, L2 [0xe5..0xea] = 0x006000009ca00441..0x006000009d400441, read back OK): the log ends at "cleanup: smmu park on the trap bank, 2 s"; **RESET** (per-boot log 20261029-223822.log ends at 1185.4 s, no panic); 4438 mV; rescued cam18r. (The runner started B while A's stability loop still ran; A had passed 6/6 already.)
- So the reset isn't 0xcc0 writing outside the buffer. Pattern over cam12-cam18: every run where SID 0xcc0's transactions were NOT context-faulted in our trap bank (unmatched: cam12, cam15; passed: cam17 bypass, cam18B translated) reset within seconds; every run with 0xcc0 trapped (cam14, cam16, cam18A) did not. Guess: the second camnoc port/TBU behind SID 0xcc0 (bit 10 = address bit 10 interleave) lacks a vote, clock or GDSC (e.g. an MMU TBU HF1 GDSC) and its traffic stalls the bus -> watchdog. Research item.
- Workaround now: two captures with SID 0x8c0 only, the second with `--addr-offset 0x400` (the frame 1 KB up, so the odd blocks land on even addresses), joined by tools/sunfish-cam-merge.py.

### 2026-10-31 01:20 cam17 (kernel109): bypass for SID 0x8c0 + 0xcc0 (mask 0x400) -> RESET at stream start
- `CAPMODE="--bypass --sid 0x8c0 --sid-mask 0x400"` (e4445e1): SMR15 0x840008c0 -> CB16 (bypass); trap CB17: 0x900/0x460, 0x820/0x440, 0x880, 0xc80, 0x8a0, 0xca0. The script hung at "sensor stream" (camid --stream); the per-boot log (20261029-221640.log) ends at 1121.2 s with ordinary IOKit lines, no panic. Phone in fastboot, 4439 mV; rescued cam17r (22:38). No frame. Note: the banks chosen were CB16/17, not CB14/15: cam16's S2CR restore wrote 0x000200ff but read back 0x0000000f (restore of S2CR doesn't stick; harmless with the SMRs invalid, but banks 14/15 then look used).
- cam16's frame (numbers only): zero blocks at 1 KB granularity exactly alternate (3720 blocks, 1860 zero, pattern 1010...; 2 KB blocks: 0 zero): **the WM interleaves by address bit 10 across SID 0x8c0 (even KB) and 0xcc0 (odd KB)**, so 0xcc0 is needed for a full frame.
- So letting 0xcc0 through resets the phone, 0x8c0 alone doesn't. Cause unknown (0xcc0 writes elsewhere too? something about that port?). Next: sample every 0xcc0 trap FAR during a cam16-style run (safe), then `--xlate`: both SIDs through a stage-1 bank that maps only 0x9ca00000..0x9d600000 (everything else faults).

### 2026-10-31 01:00 cam16 (kernel109, cam15r boot): FIRST FRAME CAPTURED AND SAVED, no reset (3ed415d)
- `CAPMODE="--bypass --sid 0x8c0" sunfish-cam-rx.sh --capture`: 8 SMRs (SMR15 0x8c0 -> CB14 bypass; SMR16-22 0x900/0x460, 0x820/0x440, 0x880, 0xc80, 0x8a0, 0xca0, 0xcc0 -> CB15 trap). buf done ~200 ms; CB14 FSR 0x400 clean, sGFSR 0; IFE VIOLATION 0, BUS_IRQ 0x2/0x1/0, COMP_ERROR 0; CSID 3810240/3810240.
- **Trap bank during the capture: FSR 0x402 (TF), FAR 0x9cb00400, write, SID 0xcc0**: the IFE-lite WM also writes through SID 0xcc0 (= 0x8c0 | 0x400), apparently alternating bursts (first fault at +0x400). Frame region 1889395 of 3810240 nonzero (49.6% of sampled bytes): consistent with every 0xcc0 burst being dropped (guess). Next: one bypass SMR 0x8c0 mask 0x400 (covers both; no overlap with SMR5 0x800/0x440).
- Guard: 5906 nonzero bytes at +0..+12095 past 3810240 (4 pages, 4.8 lines of 2520 B): the frame is ~5 lines taller than 1512 (embedded/extra lines, guess), contiguous, not scattered.
- Cleanup: IFE-lite reset, SID parked on the trap bank 2 s: **late writes after stop: N**; restore OK; camnoc back to XO. "saved, fsync ok". **No reset: SSH OK 8 of 8 checks over 4 min.** So cam12/cam15's resets came with unmatched-stream faults (cam12: all SIDs; cam15: SID 0xcc0 was never matched).
- Frame (numbers only, not viewed: the phone is in the user's bedroom at night): mean byte 15.5, max 255; 16-bin histogram [343219, 163856, 1772, 6318, 5973, 3008, 747, 2999, 1778, 798, 29, 758, 6103, 3047, 743, 3172] (dark scene); raw2png gray 0..240. Files: work/cam/frame-cam16.raw, -gray.png, -color.png (gitignored, for the user).

### 2026-10-31 00:45 cam15 (kernel109): FRAME LANDS in RAM (SID 0x8c0 bypass), then RESET ~10 s later
- `CAPMODE="--bypass --sid 0x8c0" sunfish-cam-rx.sh --capture`: SMR15 0x800008c0 -> S2CR 0xf -> CB15 (SCTLR 0xe0); no read-back mismatch. Stream ~1040 ms, buf done ~200 ms, CSID 3810240/3810240, IFE VIOLATION 0, BUS_IRQ 0x2/0x1/0, COMP_ERROR 0, CB15 FSR 0x400 (no fault), sGFSR 0.
- **Frame region: 1889286 of 3810240 bytes nonzero** (data landed); **guard: 5896 of 7724096 nonzero** (the IFE wrote past 3810240; where is still unknown). SMMU restore lines OK, camnoc back to XO.
- **RESET**: the live log stops at 790.8 s, ~10 s after the capture (780 s), no panic line; phone in fastboot at 22:09-22:12 (4438 mV). Rescued as cam15r (22:13). The frame file written on the phone was lost (not flushed; /var/local/cam/frame.raw is still cam14's from 22:05). Same pattern as cam12 (log stops ~7 s after the capture). cam14 (trap) did not reset.
- Hypothesis (unverified): unmatched-stream (global) SMMU faults from the IFE trigger the reset (hypervisor); cam12 had them from the start, cam15 probably after its SMR was removed while the IFE still wrote. Next: cleanup keeps the SID on a trap bank (late writes -> context faults, reported), IFE global reset before the SMMU teardown, fsync the frame; trap SMRs for the other IFE SIDs.
- Live log: the per-boot split stopped at the 20:57 boot (boots/current.log -> 20261029-205717.log); ../logs/usb-live.log has all boots.

### 2026-10-31 00:30 cam14 (kernel109, same boot): trap capture finds the IFE-lite write SID = 0x8c0
- `sunfish-cam-rx.sh --install --capture` (CAPMODE --trap): SMR15 0x900/0x460, SMR16 0x880/0x460, SMR17 0x820/0x440 -> CB14 (SCTLR 0x21 M|CFRE, TCR EPD0|EPD1); every write read back as written.
- Stream ~1060 ms, buf done at ~210 ms, CSID 3810240/3810240. **CB14 FSR 0x402 (TF), FAR 0x9cb00000 (= our IMAGE_ADDR), FSYNR0 0x00400010 (write), CBFRSYNRA 0x8c0 -> SID 0x8c0** (in the DT list, 0x8c0/0x460). sGFSR 0. Buffer/guard nonzero 0/0.
- Restore: 17 lines OK (SMRs invalid, S2CR 0x000200ff, CB14, camnoc back to XO); `camrx smmu` afterwards: only SMR0-14 valid. **No reset in 3+ min** (cam12's ~1 min reset came after unmatched-stream faults, sGFSR 0: the hypervisor likely handles those; context faults in our bank are harmless).
- Next (cam15): `CAPMODE="--bypass --sid 0x8c0"`: one SMR (mask 0) -> bypass bank -> the frame should land at 0x9cb00000.

### 2026-10-31 00:15 cam13 (kernel109, cam12r boot): read-only SMMU dump explains cam12's empty frame
- `camrx smmu` (88dde8e, no writes): sCR0 0x00280406 (CLIENTPD 0, GFRE/GFIE 1, USFCFG 1 = unmatched streams fault and are terminated, SMCFCFG 1); 79 SMRs, 61 banks; sGFSR 0.
- Valid SMRs 0-14, all translate-type S2CRs: 0x340/0xf, 0x2a0/0xf, 0x2c0/0xf -> CB0; 0x540 -> CB1; 0x300/0xf -> CB2; 0x800/0x440 -> CB3; 0x240/0x1 -> CB5; 0x1b21/24/25/26/27/28/2f -> CB6-12; 0x1b23 -> CB13. All banks bypass (SCTLR 0xe0, CBAR 0x1f000) except CB13 (SCTLR 0x127, M=1, CBAR 0x1f300, TTBR0 set).
- **All 14 DT IFE SIDs: no SMR -> unmatched -> fault.** So cam12's IFE writes were terminated by the SMMU (buffer stayed 0). sGFSR 0 after cam12 although USF faults should set it: guess, the hypervisor handles global faults, and may be what reset the phone ~1 min later (unverified).
- Next (cam14): `camrx capture --trap`: SMRs 0x900/0x460, 0x880/0x460, 0x820/0x440 -> a free bank with M=1 and no table walks (EPD0/EPD1): context faults record the SID, nothing lands.

### 2026-10-30 23:55 cam12 (kernel109, same boot): IFE wrote "a frame" but not to our buffer; RESET ~1 min later
- `sunfish-cam-rx.sh --install --capture` (a47990d): slot1 0x9cb00000 + 0xB00000 zeroed in 0.126 s (2816 pages read back 0). camnoc 150 MHz then back to XO at cleanup. Stream ~1090 ms; **buf done at ~210 ms**; IFE VIOLATION 0, IRQ0 0x8000220, BUS_IRQ 0x2/0x1/0, COMP_ERROR 0, WM0_STATUS0 0x9cb00000; CSID bytes 3810240/3810240. camrx exit 0.
- **Frame region 0 of 3810240 nonzero, guard 0 nonzero**: the write did not land in the buffer.
- SMMU: smmu_identity found SMR5 0x84400800 (SID 0x800 mask 0x440, valid) already matching 0x800 -> S2CR 3 -> CB3 (SCTLR 0xe0 bypass, CBAR 0x1f000) and wrote nothing (by design: never touches an existing SMR). GFSR 0 before/after, CB3 FSR 0x400 (no fault bits). But the IFE SIDs in our DT (live.dts:13150: 0x900, 0xd00, 0x880, 0xc80 mask 0x460; 0x820, 0xc20 mask 0x440; 0x920, 0xd20, 0x8a0, 0xca0, 0x940, 0xd40, 0x8c0, 0xcc0 mask 0x460) are NOT covered by SMR5's mask 0x440. So redfin's "0x800/0x5e0" assumption failed here: the stream went through some other SMR or the unmatched-stream path.
- **RESET (hard)**: `sunfish-ssh true` was OK right after the capture; ~1 min later SSH timed out and the phone was in fastboot ("reboot bootloader"), 4438 mV; last boot-log lines are the camclk/camid execs. Rescued: cam12r (kernel109 sweetse9=2 sweetcampmic=2, 21:57). Guess (unverified): the IFE's 3.8 MB went to IOVA 0x9cb00000 through another context bank's translation (wrong RAM), or the hypervisor caught the camera SID's write.
- Smaller next step: read-only SMMU dump (sCR0 USFCFG, every valid SMR/S2CR/CB, which SMR each DT IFE SID hits); then a dedicated SMR for the IFE SIDs into a FAULTING bank first (TCR EPD0/EPD1 = no walk: a write becomes a translation fault with FAR + SID in CBFRSYNRA, no data lands) to learn the exact SID, and only then a bypass bank for that SID.

### 2026-10-30 23:40 cam11 (kernel109, same boot): camid ABORT camnoc_axi (leftover camnoc RCG on PLL0)
- `sunfish-cam-rx.sh --install --capture` (fbaddc1): camid stopped at "ABORT: camnoc_axi" after GDSC on and cpas_ahb 0x1. Cause: cam10's camrx set the camnoc RCG to 150 MHz (PLL0 even), then `camclk off` turned PLL0 off; RCG settings survive, so the camnoc_axi branch has no running source. No capture, no DMA, no reset.
- Fix: camrx's cleanup puts the camnoc RCG back on XO while PLL0 still runs; sunfish-cam-rx.sh starts PLL2 + PLL0 before camid (PLLs need no GDSC, cam2).

### 2026-10-30 23:25 cam10 (kernel109, sweetse9=2 sweetcampmic=2): PMEM_ALLOC32 ENOMEM; buffer moves to the display carveout
- Battery 4455 mV; kernel109 boots (SSH 7 s). `sunfish-cam-rx.sh --install --capture`: power-up, camnoc 150 MHz, CSIPHY ok, then "pmem: alloc32 8388608: Cannot allocate memory"; powered down. No DMA, no reset. (The runner's first boot try passed the kernel directory to sunfish-boot.sh: give it work/kernel109/kernel.development.sweet.)
- Cause (known since the audio work, audio.md): XNU manages no RAM below 4 GB here (mkadt.py gives XNU 0x1_0000_0000 up); all 680 logged PMEM allocations are at 0x106..-0x12. So 0232 can't succeed; it stays harmless.
- Next: capture into the splash carveout's zero-copy slot1 (PA 0x9cb00000, 0xB00000; fb + GPU map use 0x9c000000..0x9ca00000; slot1 unused without sweetfb_zerocopy). Open risk: whether the hypervisor lets the camera SIDs write that RAM (a reset if not).

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
