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

### 2026-10-28 13:30 M0 tool adspboot (8819af7, built, not run)
- phone/wifi/adspboot.c (own copies of wland's scm/slurp/cmd-db/QMP helpers; glink.c gets glink_select(host, ipc_bit), the modem default unchanged): PAS is-supported(1), lcx/lmx votes (first level >= 385), mdt -> metadata, PAS init, mem setup 0x96b00000+0x2200000, 27 segments relocated from 0x93c00000 (bounds-checked), SMP2P out item 443 (host 2, APCS bit 26), QMP `load_state adsp on`, auth-and-reset, wait for ready in item 429 (crash reason from SMEM 423 on err-fatal), then the glink lpass edge (items 478-480, IPC bit 24): VERSION and the channels the ADSP opens. `adspboot status` is read-only. Install: tools/sunfish-install-adsp.sh (not run). No kernel change: /dev/pmem passes the SMCs and the 0x96b00000 range (sweetpmem=1).
- Memory check: the DT's node names are stale (pil_adsp_region@95700000, wlan_msa_region@97500000); the reg values are adsp 0x96b00000-0x98d00000 and wlan_msa 0x98d00000-0x98f00000 (= wlfw.c MSA_PA): adjacent, no overlap.
- Not done: pd-mapper/QRTR over IPCRTR (link.c is modem-bound), OPEN_ACK, PAS shutdown for a rerun after a refused init.
### 2026-10-28 13:05 survey (read-only)
- Hardware table and plan above, from recon/live.dts, phone/wifi and the shared cache. Effort: M0 1 day, M1 1-2 weeks, M3 2-4 weeks.
