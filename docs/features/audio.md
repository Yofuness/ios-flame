# Audio
**State:** ⬜ not started; survey done 2026-10-28. **Updated:** 2026-10-28.

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
