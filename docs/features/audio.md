# Audio
**State:** 🟡 partial: amps answer on I²C, the ADSP boots and runs, no sound yet (sensor PD needs a FastRPC file listener). **Updated:** 2026-10-29.

## Facts
Hardware (../recon/live.dts):

| Part | Node / compatible | Notes |
|---|---|---|
| ADSP | pil-tz-generic @0x62400000 (:4944), pas-id 1, firmware "adsp", mem 0x96b00000 size 0x2200000 (:23743) | firmware: vendor.img /firmware/adsp.mdt + adsp.b00-.b30 (31 files, ~15.9 MB; extracted to ../restore/fw/vendor/adsp/, outside git); the image is linked at 0x93c00000 and relocated into the carveout; not booted today; glink edge "lpass" (:4392) |
| Codec digital | bolero-codec (:20767): va-macro @62f20000, tx-macro @62ec0000, rx-macro @62ee0000, wsa-macro (:20851, disabled) | each macro has its own swr-mstr (:20795, :20829) |
| Analog codec | wcd937x-codec (:20909), wcd937x-slave on TX/RX SoundWire (:20810, :20842) | reset wcd-rst-gpio-node; supplies ldo-rxtx, vddpx-1, buck, mic-bias |
| Speaker + earpiece amps | cirrus,cs35l41 @0x40 (L) and @0x41 (R) on qupv3_se9_i2c @0xa8c000 (:9555, :9713-9760) | reset GPIO 8/11 on 0x2ea, irq 0x40/0x41; boost peak 3700 mA, 1000 nH, 15 uF; R is likely the earpiece as second speaker |
| Amp audio bus | sec-mi2s, msm-dai-q6-mi2s | per-amp tuning in vendor /firmware: *cs35l41-dsp1-spk-prot/cali/diag.bin (optional) |
| Jack | WCD937x HPHL/HPHR + MBHC, AMIC2 = headset mic | card "sm6150-bolero-snd-card" (:20937) |
| Mics | TX DMIC0-3 via tx/va macro, cdc-dmic01/23 GPIOs on lpi-pinctrl (:22898) | audio_ldo1/2 GPIOs 6/7 on 0x2e6, dcvdd supply |
| Clocks | qcom,audio-ref-clk (:21389, :23484-23549) | Q6/AFE-controlled on Android |

iOS side (~60%): CoreAudio has an IOAudio2 HAL backend (HALS_IOA2Manager::IOServiceMatchingHandler, HALS_IOA2Device/Stream/Control; research/re/CoreAudio.syms) and legacy IOAudioEngine classes. Strings: IOAudio2Device x5, IOAudioEngine x15, IOAudioDevice x7, one Apple codec name (AppleCS42L71A). The live log has no IOKit matching from mediaserverd/audiomxd yet.

## Plan
Architecture: boot the ADSP and talk APR over the glink "lpass" edge (the Linux q6afe/q6asm path). phone/wifi already has a userland TrustZone PAS loader (wland.c:374-451) plus glink, qmi, pd-mapper and smem, so the boot itself is small. Driving LPASS directly without the DSP (sc7180 lpass-cpu style) probably hits XPU or clock walls here (~60%); a 1 h read-only register probe (LPAIF/lpass_core at 0x62d84000, 0x62dc0000) can settle it. Never touch 0x0ae44000.
- **M0 (1 day): ADSP boot** through the wland PAS code (pas-id 1, adsp.mdt). Check: the glink "lpass" edge comes up, pd-mapper lists the audio PDs.
- **M1 (1-2 weeks): a speaker beep from a test tool.** APR client (Q6CORE, AFE clock/port start, a PCM buffer via ASM or AFE RT proxy), CS35L41 I²C init on SE9. Speaker safety: no DSP protection at first, digital volume at least 20 dB down, 1 kHz, at most 2-3 s, boost limit at or below the DT's 3700 mA, class-D gain unchanged, watch the CS35L41 IRQ status (overtemp, overcurrent).
- **M2 (1 week): mic capture** (DMIC via tx/va macro + AFE TX, LPI pinctrl vote); earpiece via the R amp.
- **M3 (2-4 weeks): iOS integration.** A kernel IOAudio2 service + the user client the HAL expects; done when AudioServicesPlaySystemSound is heard. Unknown: the private selectors (disassemble CoreAudio, one image).
- **M4 (1-2 weeks): headphone jack.** WCD937x over SoundWire (register init from Linux wcd937x), MBHC detect, routing.
Firmware (adsp.*, cs35l41 bins) stays on the phone in /var/local, out of git.

## Open
- recon/live.dts :20937-21000 (the full card, mi2s pins) and :4944-4975 (ADSP interrupts, mailbox).
- Linux q6afe.c, q6asm.c, apr.c, cs35l41.c (sm8250/sm6150 era) as references.
- Do audiomxd / mediaserverd run at all on the phone?

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

### 2026-10-29 11:35 audio steps 1-3 (au1, au3; kernel95; adspboot e111a25, c814f18)
- e111a25: pd-mapper adsp domains (instance 74), CB with ASID + TLBIASID before SCTLR, `smmu --status`, dsp addresses IOVA|3<<32, `boot --rpc --root`. Backups on the phone: /var/local/bin/adspboot.pre-e111, wland.pre-e111. Wi-Fi still joins with the new wland (au1).
- au1 `adspboot boot`: glink up (IPCRTR, fastrpcglink-apps-dsp, apr_audio_svc ...), "no ready in 10 s", sensor_process chre_utils fatal; no pd-mapper lines: the ADSP's IPCRTR is adspboot's, the locator lived only in wland.
- c814f18: adspboot answers the ADSP's servreg locator itself (NEW_SERVER svc 64 at 1:11 after HELLO; `--no-pdmap` off). Channel tx limit 64 -> 512 bytes.
- au3 (4454 mV, ssh 17 s): ADSP asks "avs/audio" -> 1, "tms/pdr_enabled" -> 1, "tms/pddump_disabled" -> 0; then `PD FAILURE "sensor_process": EF:sensor_process:0x1:rcinit_worker:0xf7:sar.cc:27:chre_utils fatal`, still "no ready in 10 s". ADSP services announced: 769, 66, 43, 366, 15, 400, 51, 24.
- `smmu --rpc --dry` ok; `smmu --rpc`: 0x1b23 -> CB13 via SMR14; `smmu --status`: sGFSR 0, but CB13 FSR 0x400, FAR 0x0001736fd050a02a (before any FastRPC buffer; stale?). Step 5 (root PD attach) not run; analysis running. No reset.

### 2026-10-29 11:15 why rpc1-3 failed (comparison with hakios-sm7150 + Linux fastrpc; research/notes/audio-sm7150-reference.md)
- The DSP emits stream 0x1b20 | (addr>>32 & 0xf) and uses the low 32 bits as the IOVA. rpc1 (0x1_062a0000) -> stream 0x1b21 (our identity bank) -> wrong memory -> sensors PD exception. rpc2 (0xe0f80000) / rpc3 (0x80000000) -> stream 0x1b20, no SMR, USFCFG=1 -> SoC reset. CB7 (0x1b23) was never reached. (Inference, ~80%.)
- hakios makes no SCM/hyp assign for audio/FastRPC memory (only rmtfs): the hypervisor track is likely unnecessary.
- Their setup: pd-mapper msm/adsp audio_pd/root_pd/sensor_pd at instance 74 (+ avs/audio, tms/pdr_enabled); root PD attach (pd 0) before the sensors PD (pd 2); CB with 39-bit TCR, ASID = bank, TLB invalidate + sync before SCTLR. Speaker on Mi 9T: PRIMARY_MI2S_RX; ours likely SECONDARY_MI2S_RX (0x1002), unchecked.
- Plan (baby steps, each logged): 1 pd-mapper entries; 2 --dry SMMU plan; 3 CB7 for 0x1b23 only + FSR/sGFSR read-out; 4 dsp addresses = IOVA | 3<<32; 5 root PD attach with one 4 KB page (the reset-risk step); then listener/init2, sensors PD, speaker.

### 2026-10-29 11:05 references from other ports (survey; research/notes/hackios-survey.md)
- xorbidden/hakios-sm7150 (Mi 9T, SM7150, no licence: facts only): DSP buffers in kernel memory mapped by its own page tables in a free apps-SMMU bank; the address handed to the DSP is IOVA 0x10000000 | (stream SID low 4 bits) << 32 (audio SID 0x1b21 -> 1<<32, FastRPC 0x1b23 -> 3<<32); one SMR per stream. pd-mapper answers `tms/pdr_enabled` on msm/adsp/root_pd (instance 74); FastRPC attaches the root PD before the sensors PD. Linux fastrpc.c also adds sid << 32. Our rpc3 handed 0x80000000 without SID bits (guess: the DSP used a stream with no SMR -> reset).
- oosexgang/hakios-xnu-kernel (APSL-2.0): APR ASM/ADM/AFE playback + software speaker guard; userland CoreAudio HAL plug-in (MIT). Not verified by us.

### 2026-10-29 07:50 notes moved out of the scratchpad
- The FastRPC source excerpts (downstream adsprpc.c, upstream fastrpc.c, quic listener + apps_std) are now in research/notes/fastrpc/ (gitignored); the camx IMX363 decode is in research/notes/camx/.

### 2026-10-29 06:10 rpc3: translated 32-bit buffer also resets the SoC (U8 option a, the user's go)
- New smmu.c smmu_translate(): AArch64 S1 bank, 4 KB pages, T0SZ 32, IOVA 0x80000000+512 KB -> a pm_alloc buffer (managed DRAM above 4 GB), MAIR0 0x44 (normal NC), SCTLR 0x61 (terminate on fault). This SMMU has no ATS ops (ID0 0x4c017e4f: ATOSNS), so it checks by read-back; TCR2 reads back 0x62 for 0x02 (bits 5-6 set by the hardware). `adspboot smmu --rpc` with the DSP off: CB9, L3 106364443..1063e3443, all six FastRPC SIDs (SMR9-14) -> CB9; phone fine.
- rpc3 (rebooted, kernel94): the same mapping at boot (CB8, buffer 0x106288000, DSP sees 0x80000000), PAS boot, 6 channels, attach sent (ctx 1012, 516 bytes) -> **SoC reset to fastboot** right after, as in rpc2. No SMMU fault line in the kernel log before the reset. Recovery rcv2: 4433 mV, SSH 25 s, lock screen, brightness 0x1.
- So: rpc1 (an address the DSP truncated) = a DSP-side exception; rpc2 (unmanaged RAM, identity) and rpc3 (managed RAM via a 32-bit IOVA) = a whole-SoC reset as soon as the DSP reaches the buffer. Leading guess (~65%): an access-control violation (XPU / hyp stage 2) because the memory belongs to HLOS only; downstream's buffers are reachable because the compute cbs' SIDs are covered by the hypervisor's stage-2 for the HLOS VM, which it set up through Linux's SMMU driver on Android and which our banks don't get. A fix would need the hypervisor (SCM mem assign / the hyp's SMMU stage 2): high risk, the user's call.
- `adspboot --rpc` now refuses without `--unsafe`. Audio phone work stopped (two resets).
### 2026-10-29 05:40 why the sensor PD faults: analysis (read-only, investigator)
- Our attach matches downstream's kernel attach (pid 0, tid, ctx|pd 2, handle 1, sc 0x00010000, one 4-byte tgid arg; no mem donation, no static-process create; PDR only for restarts). The one difference: the address. Downstream's page.addr is a compute-cb IOVA, 32-bit (no dma-ranges on the ADSP cbs, live.dts:4304-4341; adsp_mem pool limited below 4 GB, live.dts:23799); ours was 0x1062a0000. Guess (~55%): the DSP truncated it -> the frpck_0_0 exception.
- Below 4 GB there is no XNU-managed RAM (tools/mkadt.py: XNU takes 0x1_0000_0000 up), and unmanaged RAM there reset the SoC (rpc2; likely an XPU/hyp stage-2 violation: that RAM belongs to HLOS but not to anything shared with the ADSP).
- Options left, all need the user's OK (after a hard reset, and they touch the hypervisor or SMMU translation): (a) a translating context bank for 0x1b23 with a 32-bit IOVA (e.g. 0x80000000) mapping a pm_alloc buffer above 4 GB (SMMU page tables; same SMMU as UFS/USB); (b) hyp-assign a buffer to the ADSP VMIDs 0x16/0x25 (qcom,adsp-remoteheap-vmid, live.dts:4249) via SCM; (c) the 12 MB adsp_mem CMA region (live.dts:4240, 23799) with a hyp assign. (a) is the lowest risk: no hyp call, and a wrong mapping gives an SMMU fault, not an XPU reset (unproven).
### 2026-10-29 05:20-05:31 FastRPC listener: first runs (rpc1, rpc2); rpc2 reset the phone (kernel94)
- New phone/wifi/fastrpc.c (790 lines, written by the implementer agent from the downstream adsprpc.c / quic fastrpc sources): `adspboot boot --rpc --watch S` (implies --smmu) serves `fastrpcglink-apps-dsp`: attach to the sensors PD (handle 1 method 0, in: tgid, pd 2, pid 0), then remotectl_open("adsp_default_listener"), listener_init2, the next2 loop with apps_std fopen/fread/readdir/stat/... from /var/local/fw/sensors (vendor.img /etc/sensors, 51 files staged) and a writable /var/local/sensors-persist. 13 guesses listed in the source (`guess:`).
- rpc1 (fresh boot, 4426 mV): SMMU 8/8 ok, PAS boot as before, 6 channels, attach sent (DMA buffer pm_alloc 0x1062a0000, 516 bytes, ctx 1012). No response. Crash reason changed: `err_qdi.c:1040:EX:sensor_process:0x1:frpck_0_0:0x93:PC=0xb21b0b8c` (EX = an exception in the PD's FastRPC thread) instead of rcinit's `chre_utils fatal`. So the message reaches the sensor PD and something in it makes it fault. All 8 ADSP banks: FSR 0x400 only (FORMAT, sticky), no translation/permission fault.
- Guess for rpc2: the buffer above 4 GB. rpc2 (rebooted, 4431 mV): buffer at 0xe0f80000 (the unused top of radio.c's unmanaged pool). **The SoC reset into fastboot right after the attach was sent** (last log line uptime 68.9 s; the partial rpc2.log ends at `rpc: -> h 1 m 0 ... ctx 1012`). Phone work on audio stopped (project rule after a hard reset). Recovery boot rcv1 fine (SSH 25 s, lock screen, brightness 0x1).
- Reverted to the pm_alloc buffer (rpc1 behaviour, no reset). The 0xe0... pool is not safe for DSP access (likely an XPU/hyp stage-2 violation -> reset); never give the DSP memory outside managed DRAM.
- Open: why the PD faults on the rpc1 message. Leads, read-only: (1) the attach message shape for sensorspd (pid/tid, tid session bit, ctx index bits) against a real Android trace; (2) whether the DSP needs the buffer hyp-assigned to it (downstream hyp_assign_phys for the adsp heap / remote heap) or in a 32-bit IOVA window (DMA mask), not a raw PA.
### 2026-10-29 05:15 apps SMMU: all audio-DSP streams mapped (U7, the user's go, 2026-10-29)
- New phone/wifi/smmu.c: radio.c's WLAN SMMU code (in use every boot) taken out and given a SID parameter; `adspboot smmu [--dry] [--sid SID MASK]`, and `adspboot boot --smmu` does it before the PAS boot. Runtime only: gone after a reboot.
- Dry run (kernel94, uptime ~27100 s): sCR0 002d0406, 79 SMRs, 61 banks; 0x1b20/0xf would take SMR7 + CB6; no existing SMR overlaps.
- The masked SMR 0x800f1b20 reads back 0 (the write is dropped; S2CR7 and CB6 did take). A single SID sticks: 0x1b23/0 -> SMR7 80001b23 -> CB7 ok. So one SMR per SID: 0x1b21 (audio-ion) SMR8/CB8, 0x1b23-0x1b28 (FastRPC cbs) SMR7, 9-13 / CB7, 9-13, 0x1b2f (usb-audio) SMR14/CB14, all read back as written (S2CR type 0, SCTLR e0 = M off, CBAR 1f000).
- Phone fine after: ping, /var/local reads, Wi-Fi up. CB6 is left set up but unused (no SMR points at it).
- Next: the FastRPC reverse-RPC listener (sensor PD files), then retry adspboot past the 10 s sensor_process fatal.

### 2026-10-28 21:00 FastRPC listener: protocol spec done; blocked on the apps SMMU (needs the user's decision)
- Spec (from the sunfish downstream adsprpc.c, quic/fastrpc and recon/live.dts:4246-4345; source excerpts kept in the session scratchpad, not in git): 40-byte smq_msg over glink `fastrpcglink-apps-dsp`, a contiguous metadata buffer (remote_arg64 + page lists), sensorspd = static PD 2, attach = handle 1 method 0 (in: tgid); then remotectl_open("adsp_default_listener"), adsp_listener_init2 (handle 3 mid 3), loop next2 (mid 4) answering apps_std fopen/fread/fclose/opendir/readdir/stat for /vendor/etc/sensors (+ a scratch persist registry dir). Effort ~3-5 days.
- Blocker (read-only pmem dump of the apps SMMU at 0x15000000): sCR0 0x002d0406 (USFCFG = 1: unmatched streams fault), IDR0 0x4c017e4f (79 SMRs), valid SMRs 0-6 and 117-127, none matching the ADSP FastRPC SIDs 0x1b23-0x1b28. So any AP buffer handed to the DSP would fault: a stream entry + pass-through context bank must be added first. That SMMU also carries UFS/USB DMA, so this change is held for the user's OK (plan U7); not started.

### 2026-10-28 18:45 why the ADSP dies (read-only, strings in adsp.b13/b18/b04 + the .jsn files)
- The ~10 s is `chre::SeeHelper::findSuidSync` (b13 0xf7615; b18 0x2ac2 "Waited %lu ms for %s (found %zu, required %hhu)") waiting for sensor SUIDs: the SAR module (sar.cc) needs them, gets none, "Failed to initialize ASH SEE helper" -> fatal. svc 400 (SNS client, published) is local, so the QMI side works.
- No sensor driver registers because the SEE registry reads /vendor/etc/sensors/sns_reg_config, sensors_list.txt and /vendor/etc/sensors/config/*.json from the AP through reverse FastRPC (apps_std via adsp_listener, HAP_rfs_wait; b13 0x13f8e0/0x115b5f/0xf91a3). On Android that is `adsprpcd sensorspd`. Nobody serves it here, which fits "no FastRPC data".
- Domains (adsps.jsn/adspr.jsn): msm/adsp/sensor_pd and msm/adsp/root_pd, qmi instance 74; pd-mapper (svc 64) is probably not what blocks this (~70%).
- Next (big, parked as a dream step, ~2-5 days, ~65% that it removes the fatal): an AP-side FastRPC reverse-RPC listener on `fastrpcglink-apps-dsp` serving apps_std fopen/fread/fclose/stat/opendir read-only from a staged copy of vendor.img:/etc/sensors plus an empty persist registry. Needs the FastRPC wire format and SMMU-mapped buffers for the ADSP's context bank.
- Meanwhile SE9 works, so camera M1 can start: phone/wifi/i2cprobe (read-only I2C reads + ACK scan) for the slg51000 @0x75.

### 2026-10-28 18:35 adsp3/adsp4 (kernel93): early glink up, QRTR services seen, same sensor fatal
- adsp3: PAS shutdown (SCM PIL 6) on a never-started ADSP blocked in the SMC forever (process unkillable; reboot cleared it). d70ac33: shutdown only when SMP2P shows an earlier run.
- adsp4 (fresh boot, a184a48 + d70ac33): PAS as before (all 0/0). glink lpass edge at once: version 1 features 7; the ADSP opens 6 channels: glink_ssr, fastrpcglink-apps-dsp, LOOPBACK_CTL_LPASS, apr_audio_svc, apr_apps2, IPCRTR (all OPEN_ACKed; IPCRTR asks for 52-byte intents x3).
- QRTR: HELLO from node 5 (answered), then 11 NEW_SERVER from node 5 (svc/inst/port): 769/1/f, 66/18945/3, 43/5122/2, 366/1/10, 15/8193/1, 15/8449/9, 15/8705/a, 400/1/e, 51/1281/6, 51/3073/d, 24/257/8. No NEW_LOOKUP, no data on any other channel (FastRPC silent).
- Still no ready; ~10 s in: `sensor_process ... rcinit_worker:0xf7:sar.cc:27:chre_utils fatal`. So it isn't waiting on a QRTR lookup. Leading guess (~50%): the SEE registry reads its files through FastRPC reverse calls (apps_std via adsprpcd on the AP), which nobody serves, and CHRE's SEE helper gives up. A FastRPC listener is a large job (SMMU-mapped buffers): parked as a dream item; next is read-only analysis of what the fastrpc channel would carry.
- Note: the webcam showed the screen off at the end: iOS auto-lock on the idle lock screen (sweetpanelsleep=1), not a fault.

### 2026-10-28 17:55 adsp1/adsp2 (kernel93, booted k93a): the ADSP authenticates and runs, then err-fatal in the sensor PD
- adsp1: `pas supported(1) -> 0/0` stopped the tool; 1cf0cf5 only logs it now (wland never asks).
- adsp2: votes lcx/lmx 384 -> 0/0; mdt 28 phdrs, linked 0x93c00000-0x95e00000, relocated to 0x96b00000; pas init (8860 B metadata) 0/0; mem setup 0x96b00000+0x2200000 0/0; 26 segments 15963961 bytes; smp2p out 443; QMP `load_state adsp on` acked; auth-and-reset 0/0. Then no ready in 10 s; glink items 478/480 present, 479 none; crash reason (SMEM 423): `err_qdi.c:1040:EF:sensor_process:0x1:rcinit_worker:0xf9:sar.cc:27:chre_utils fatal`. The phone stayed up, no SMMU/XPU lines.
- So: the firmware runs; the sensors user PD (CHRE, SAR sensor init) fails fatally during rcinit and takes the ADSP down. Next: what it wants from the AP (files over TFTP/FastRPC, servreg/PDR, sensor registry).

### 2026-10-28 17:50 k93a (kernel93, sweetse9=2 sweetampreset=1): both CS35L41 amps answer
- xnu 0217: PM6150L GPIO 8 (0xc700) and 11 (0xca00) are TYPE 0x10 SUBTYPE 0x11 (MV); before MODE 0x00 OUT_SRC 0x00 (input), after MODE 0x01 OUT_SRC 0x80 EN 0x80 (output high) at 30.67 s.
- `apr_tone --probe --amp both`: "2 amp(s) answer"; each: devid 0x35a40, revid 0xb2, pwr_ctrl1 0, pwr_ctrl2 0x3321, amp_dig_vol 0x8000, amp_gain_ctrl 0x273, irq1_status1 0. The resets were the blocker.
- Next: M0 ADSP boot (tools/sunfish-install-adsp.sh, adspboot, aprtest).

### 2026-10-28 17:30 k92b (kernel92, sweetse9=2): SE9 pins muxed; amps don't answer
- gpio46/47 cfg 0x4 (func 1 = qup13), io 1; S3 CBCR 0x4220; fw 0x303.
- `apr_tone --probe --amp both`: gi2c_init OK (gcc vote 0x433c1bc0, s3 cbcr 0x4220, fw 0x303, status 0); `cs35l41 0x40: read 00000000: err 6` -> no ACK on either amp. Likely held in reset (PM6150L GPIO 8 / 11, never driven). Next: xnu 0217 `sweetampreset=1` (PM6150L GPIO 8/11 output high), then probe again.

### 2026-10-28 17:20 k92a: SE9 clocks on (xnu 0215, sweetse9=1); apr_tone --probe
- The boot lines were not in the captured log (printed too early), so read back with pmem: GCC vote 0x15200c = 0x43bc1bc0 (bit 25 set), S3 CBCR 0x1183a4 = 0x4220 (on), RCG DFS on (0x1183bc = 0x21, left alone), SE9 fw 0xa8c068 = 0x303 (protocol 3 = I2C, like SE7), 0x48 = 0x11, CLK_SEL 0.
- TLMM: gpio46/47 on the north tile 0x3900000 read cfg 0 (GPIO mode), io 1 (lines high: idle I2C with pull-ups); the 0x3500000 / 0x3d00000 offsets read 0 for every pin. So north is the right tile; sweetse9=2 muxes them.
- apr_tone --probe: amp IDs over I2C, read-only, no ADSP needed. Next: k92b sweetse9=2 + `apr_tone --probe --amp both`.

### 2026-10-28 17:30 M1b tool apr_tone (8f5e166, built, dry-run checked on the PC, not run on the phone)
- No ASM stream: a 1 kHz -20 dBFS PCM buffer (pm_alloc, mapped to the DSP with SHARED_MEM_MAP_REGIONS) written in 10 ms chunks to an AFE RT proxy port (0x2001) looped back to SECONDARY_MI2S_RX (0x1002); clock set SEC_MI2S_IBIT 1.536 MHz, I2S config 48k/16; amp CS35L41 over SE9 I²C from userland via pmem (geni_i2c.c); amp limits enforced in cs35l41.c: digital volume >= 20 dB down, boost peak only lowered to <= 3700 mA, AMP_GAIN never written, IRQ1 error bits polled every 100 ms, amp on <= 3 s, tone <= 2.5 s, power-down on any error/signal. Opcodes 35-85% (from memory), amp registers 30-85%.
- Possible blockers: the amp reset pins (PM6150L GPIO 8/11) may be held low (no userland SPMI write); the DSP may not reach a plain physical buffer (no LPASS SMMU map); the LPI pins need a hw vote first (--hwvote --lpi-pins, off by default).
- Test order: adspboot -> aprtest -> apr_tone --dry-run -> --no-amp (which AFE commands are accepted) -> apr_tone (L amp, bypass, -20 dB; the user listens) -> only then --boost / --amp both. Don't run kern.sweet_camprobe at the same time (same SE9 bus).

### 2026-10-28 16:50 M1a tool aprtest (84c9f0f, built, not run) + an adspboot fix (20c8be3)
- phone/wifi/aprtest.c: reattaches to the lpass glink edge without resetting it (glink_keep_state), opens "apr_audio_svc" (OPEN / OPEN_ACK, intents, TX_DATA with RX_INTENT_REQ when out of intents, TX_DATA_CONT reassembly, RX_DONE), then APR (5-word header, hdr_field 0x0150, APPS 5 -> ADSP 4): AVCS_CMD_ADSP_EVENT_GET_STATE 0x1326C (-> 0x1326D, 1 = ready), AVCS_CMD_GET_FWK_VERSION 0x1292C (-> 0x1292D, services list), fallback AVCS_GET_VERSIONS 0x12905. Exit 0 if ready. Opcode confidence 65-85% (from memory of q6core.c / apr.h).
- adspboot: a glink INTENT command is sized 8 + 8 * param2 (intent count), not param1 (channel id); the wrong size could have desynced the RX FIFO.
- Test: adspboot (glink up), then `/var/local/bin/aprtest` right after.

### 2026-10-28 13:30 M0 tool adspboot (8819af7, built, not run)
- phone/wifi/adspboot.c (own copies of wland's scm/slurp/cmd-db/QMP helpers; glink.c gets glink_select(host, ipc_bit), the modem default unchanged): PAS is-supported(1), lcx/lmx votes (first level >= 385), mdt -> metadata, PAS init, mem setup 0x96b00000+0x2200000, 27 segments relocated from 0x93c00000 (bounds-checked), SMP2P out item 443 (host 2, APCS bit 26), QMP `load_state adsp on`, auth-and-reset, wait for ready in item 429 (crash reason from SMEM 423 on err-fatal), then the glink lpass edge (items 478-480, IPC bit 24): VERSION and the channels the ADSP opens. `adspboot status` is read-only. Install: tools/sunfish-install-adsp.sh (not run). No kernel change: /dev/pmem passes the SMCs and the 0x96b00000 range (sweetpmem=1).
- Memory check: the DT's node names are stale (pil_adsp_region@95700000, wlan_msa_region@97500000); the reg values are adsp 0x96b00000-0x98d00000 and wlan_msa 0x98d00000-0x98f00000 (= wlfw.c MSA_PA): adjacent, no overlap.
- Not done: pd-mapper/QRTR over IPCRTR (link.c is modem-bound), OPEN_ACK, PAS shutdown for a rerun after a refused init.
### 2026-10-28 13:05 survey (read-only)
- Hardware table and plan above, from recon/live.dts, phone/wifi and the shared cache. Effort: M0 1 day, M1 1-2 weeks, M3 2-4 weeks.
