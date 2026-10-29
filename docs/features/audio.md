# Audio
**State:** 🟡 DSP up; APR + AFE + PCM stream; the left CS35L41 amp powers up on the I2S clock (PLL lock, PUP_DONE) and plays at -30 dB; not yet heard by the PC mic. **Updated:** 2026-10-29.

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

### 2026-10-29 18:45 SPEAKER AMP RUNS: PLL lock + PUP_DONE, 3 s on, no errors (sp2a, sp3a, sp3b; kernel98 sweetse9=2 sweetampreset=1; adspboot dc872fb, 423b675)
- sp2a: hw vote op 0x1027C -> status 3 (unsupported). 423b675: vote op 0x100F4 (block 2) -> ok, handle; LPI pins muxed only after DEVICE_START: gpio8 0x401->0xcc, gpio9 0x401->0xcc, gpio10 0x5c1->0xd0 (read back ok), restored before DEVICE_STOP; devote. All AFE steps status 0.
- sp3b (--pcm --amp --amp-db -30, tone -20 dBFS, gain 0.5 dB, boost bypass): `PLL_LOCK first after 1 ms`, `PUP_DONE after 1 ms (STATUS1 01400000 STATUS3 2000002f)`, on 3028 ms (3 s cap), no error bits, normal power-down; PCM 388/388. Laptop mic: no tone (rms ~800, 1 kHz bin 1-18): the output was ~-50 dB below full scale. No reset; 4455 mV.
- Next: -20 dB digital (the cap), then analog gain ~10 (10.5 dB) if still not heard.

### 2026-10-29 18:25 why no PUP_DONE: SEC MI2S pins never muxed, no LPASS hw vote (research/notes/amp-clock.md)
- Amps are on SEC MI2S (dai_mi2s1, port 0x1002, SD0; live.dts:20326). Its pins are LPASS LPI TLMM @0x62b40000: gpio8 SCK func3, gpio9 WS func3, gpio10 SD0 func4, 8 mA (live.dts:23261-23340). sp1 never muxed them nor cast AFE_CMD_REMOTE_LPASS_CORE_HW_VOTE_REQUEST 0x1027C (block 1) that the LPI registers need: the amp had no SCLK, so its PLL (ref = SCLK) can't lock -> no PUP_DONE. CS35L41 STATUS3 bit 1 = PLL_LOCK (to poll); STATUS1 bit 22 undefined in Linux.
- Next: --afe does hw vote + pin mux (CFG 0xCC/0xCC/0xD0, read back, restored at exit) before the clock set; then sp2.

### 2026-10-29 18:10 CS35L41: config verified, but no PUP_DONE (amp-check2/3, sp1; kernel98 sweetse9=2 sweetampreset=1; adspboot d161564)
- d161564: cs35l41.c per research/notes/cs35l41-plan.md (B2 errata + test key, pup/pdn patches 0x2084, 100 ms timeouts, no SP_RATE_CTRL, gain 0.5 dB, boost bypass only, left amp 0x40 only, volume clamp <= -20 dB) and `adspboot --amp-check` / `--amp` (GLOBAL_EN only after the first WRITE_DONE, <= 3 s, IRQ poll every 50 ms, power-down before teardown and on every exit).
- amp-check without the boot-args: SE9 clocks off -> refused. With sweetse9=2 sweetampreset=1 run too early: no ACK (the kernel releases the resets at ~31 s: `SWEET AMPRESET: after 0xc700 ... 0xca00`). After 31 s: devid 0x35a40 revid 0xb2, PWR_CTRL2 0x3321, IRQ1 status1 0 / status3 0x20000000 / status4 0x3; 19 registers written and verified; GLOBAL_EN never set; restored.
- sp1 (-30 dB, tone -20 dBFS, PCM 388 periods ok, AFE ok): GLOBAL_EN=1 -> `no PUP_DONE in 100 ms`, IRQ1_STATUS1 0x00400000 (bit 22) throughout, no error bits; powered down. Laptop mic: no tone (expected without PUP). No reset; 4456 mV.
- Suspect: no bit clock at the amp (LPI pin mux / LPASS hw vote missing, or a different MI2S/TDM port). Investigating.

### 2026-10-29 17:40 PCM plays the full 3 s through ASM/ADM/AFE (pc3; kernel98; adspboot bba8a7c)
- bba8a7c: remote intents kept with an in-use flag; the ADSP's RX_DONE_W_REUSE frees them again (RX_DONE deletes); grants logged; 1 s heartbeat during PCM (`--intent-once` = old behaviour).
- pc3: setup ok (handle b0a52a28, copp 0); `pcm: done: ok; 308 periods written, 308 done (0 errors) in 3055 ms, 0 underflows`; teardown (pause, close, ADM close, unmap) and AFE stop/clock off all status 0. apr_audio_svc: 116 remote intents, none stuck; rx 325 / rx_done_reuse 325. ADSP state 6 after; no SMMU fault; no reset; ssh 17 s; 4456 mV. Amps off: no sound expected.
- Next: CS35L41 (the only step with hardware risk): read-only ID/status first, then the left amp at -20 dB digital volume, boost bypass, 3 s, IRQ status before/after, tone-detect with the laptop mic.

### 2026-10-29 17:25 PCM 2x longer, still stalls (pc2; adspboot 2e4ad03)
- 2e4ad03: 512 remote intents per channel (pc1 had dropped 39 the ADSP gave), 64-packet tx queue, RX_DONE_W_REUSE for our intents, >= 8 local intents.
- pc2: setup all ok; 113 written, 105 WRITE_DONE in 1053 ms (was 53), then stall; t=30 s apr_audio_svc: remote intents 0, local 8, tx queued 14, intent reqs 14 (never granted); teardown no replies. ADSP state 6, no SMMU fault, no reset.
- Next: the ADSP's RX_DONE_W_REUSE to us is ignored (remote intents single-use in our table) — honour it.
- Mic check (the user's idea): laptop mic (lid closed) hears a 1 kHz tone from the laptop speakers ~50x above noise; Sonix webcam mic only ~5x. tone-detect defaults to the laptop mic.

### 2026-10-29 17:05 PCM through ASM/ADM: setup ok, 53 periods played, then the APR channel stalls (pc1; kernel98; adspboot 11c845c)
- 11c845c `--pcm`: 32 KB buffer for ADSP stream 0x1b21 through a translating bank (CB12, SMMU_REF values), DSP address 0x1a0000000 (IOVA 0xa0000000 | 1<<32); ASM map 0x10D92 -> handle b0a52a58; ADM open v5 0x10326 (port 0x1002, topology 0x10312, 16-bit 48 kHz 2 ch) status 0 copp 0; ASM open-write v3 0x10DB3, matrix route 0x10325, media fmt 0x10D98, run 0x10DAA: all status 0.
- Playback (1 kHz -20 dBFS, 10 ms periods, 8 in flight): 61 written, 53 WRITE_DONE in 504 ms, then nothing; teardown commands and AFE stop got no reply. ADSP state 6 (not crashed), no SMMU fault on 0x1b21 or 0x1b23, no reset. Amps off (no sound expected).
- Hypothesis: glink intent accounting under streaming (remote intents dropped when our table was full; local RX intents not refilled). Fix in progress.
- tools/sunfish-tone-detect.sh: the Sonix USB camera mic listens for the tone (Iriun gives video only on Linux), for the amp step.

### 2026-10-29 16:45 AFE port starts: SEC_MI2S_RX clock + config + start/stop all status 0 (af2-af4; kernel98; adspboot b8ed3ee, 7d3905c)
- Bugs fixed: GET_STATE is AVCS_CMD_ADSP_EVENT_GET_STATE 0x1290C (we sent 0x1326C, silently dropped); our APR hdr_field was 0x0150 (CMD_RSP type), now 0x0250 (SEQ_CMD); ADSP replies use an 8-word header (hdr_field 0x0081/0x0181, length in bits 7:4) — we read the payload 12 bytes early (af2: "not ready" was a header word; af3 raw dump: payload word 1 = ready). 10 s ready gate (ADSP_APR_NO_GATE=1 off); 0x100FC fallback removed; ADSP_APR_HEX=1 raw dumps.
- af4: `apr: adsp state words[0]=1 (ready)`, `apr: fwk 2.9.0.0, 8 services`; AFE (src/dest svc 4): 0x100F3 clock set (SEC_MI2S_IBIT 0x102, 1.536 MHz) ok; 0x100EF I2S config port 0x1002 (48 kHz, 16-bit, stereo, SD0) ok; 0x100E5 DEVICE_START ok; 2 s; 0x100E6 STOP ok; clock off ok. Every APR_BASIC_RSP 0x110E8 status 0. ADSP state 6 after; no crash, no reset; ssh 17 s; 4455 mV.
- Next: PCM source (ASM shared memory + ADM routing, or AFE loopback) with amps off; then CS35L41 at -20 dB for a 1 kHz tone (U5, the user listens).

### 2026-10-29 16:15 AFE silent (af1; kernel98; adspboot 2273261 --afe)
- `--afe`: clock set (AFE_SVC_CMD_SET_PARAM module 0x1028F param 0x10290, clk 0x102 SEC_MI2S_IBIT 1.536 MHz, 68-byte packets from APPS/svc 4) via 0x100F3 then 0x100FC: no reply in 3 s to either; config/start not reached. CORE still answers GET_FWK_VERSION (9 services), GET_STATE 0x1326C still no reply. ADSP stays up (state 6), no SMMU fault, no reset; ssh 17 s; 4455 mV.
- Next: compare the reference's sequence before its first AFE command (ready polling, audio PD, packet bytes).

### 2026-10-29 16:00 APR WORKS in adspboot (apr3; kernel100; adspboot e9610fd)
- e9610fd `--apr`: apr_audio_svc opened in adspboot's own glink loop (ADSP opened it, we answer OPEN + OPEN_ACK on lcid 5), 8 x 1024 RX intents; core queries. `adspboot boot --rpc --root --sensors --apr --unsafe --watch 40`:
  `apr: apr_audio_svc up (lcid 5, rcid 5)`; GET_STATE 0x1326c: no reply in 3 s; GET_FWK_VERSION 0x1292c -> `rx svc 3 op 1292d token 2 status 4000102 (len 148)`: 9 services: 0, 3 CORE (api 5), 4 AFE (api 7), 7 ASM (api 2), 8 ADM (api 4), d, 9, a, b.
- At exit our CLOSE of all channels (new) -> ADSP state 7 `fastrpc_port_s.c:176:Assertion 0 failed`. CLOSE is now off by default (ADSP_CLOSE=1 turns it on). No reset; ssh 17 s; 4438 mV.
- Next: AFE port start without amps (apr_tone step 2) inside adspboot.

### 2026-10-29 15:45 aprtest as a second process: ADSP glink assert again (apr2; kernel98; aprtest e89860d)
- e89860d: aprtest on its own lcid 9, ignores stale OPEN_ACK, 8 x 1024 RX intents, CLOSE at exit. apr2: plain `adspboot boot` (glink up, 6 channels), aprtest: OPEN "apr_audio_svc" lcid 9 sent, no OPEN_ACK in 3 s; ADSP state 7 `glink_channel_migration.c:667:Assertion status == GLINK_STATUS_SUCCESS failed`. No reset.
- Conclusion: a second process on adspboot's glink edge does not work; APR moves into adspboot (`--apr`, same edge loop as FastRPC).

### 2026-10-29 15:10 APR step 1 (aprtest): no reply, ADSP glink assert (apr1; kernel98)
- Plan: research/notes/apr-plan.md (aprtest.c = transport + core queries; apr_tone.c = AFE/PCM/CS35L41; fixes: AFE clock opcode 0x100F3 vs 0x100FC, ASM+ADM path as in the reference).
- apr1: plain `adspboot` (READY 121 ms), then `aprtest`: OPEN_ACK apr_audio_svc (lcid 1, rcid 5); tx GET_STATE 0x1326c, GET_FWK_VERSION 0x1292c, GET_VERSIONS 0x12905: no reply; ADSP INTENTs fill our table (16); ADSP state 7: `glink_core_intent.c:874:Assertion lc_intent != NULL failed`. No reset. Likely: aprtest queues no RX intents of its own on the channel. Fix in progress.

### 2026-10-29 14:50 stable ADSP, adspboot exits cleanly (au15; kernel97; adspboot e405bb6)
- e405bb6: with --rpc, --watch was paid twice (early watch never returned on READY, then a silent glink_watch): one deadline now, heartbeat every 10 s, summary on exit/SIGTERM, dropped glink intents logged.
- au15 `boot --rpc --root --sensors --unsafe --watch 60`: READY 111 ms; channels glink_ssr, IPCRTR, fastrpcglink-apps-dsp, LOOPBACK_CTL_LPASS, apr_audio_svc, apr_apps2; heartbeats t=10..50 s all `state 6, root serving 2 calls ctx 70, sensors serving 590 calls ctx d62, 602 invokes, pending tx 0` (DSP idle after init); exited rc 0 at ~60 s; no CRASH/PD FAILURE; SMMU no fault; 30 s later `adspboot status`: state 6 ready (SFR text "SFR Init: wdog or kernel error suspected." = the default string, presumably).
- Next: APR on apr_audio_svc (ADSP core version query first, read-only), then AFE port + CS35L41 for a tone (U5).

### 2026-10-29 14:05 ADSP survives: SMP2P "sleepstate" entry (au14; kernel95; adspboot f02e2a6)
- f02e2a6: outbound SMP2P item 429 now has "master-kernel" = 0 and "sleepstate" = 1 (bit 0 = AP awake) before the PAS reset (`--no-sleepstate` = old).
- au14 (ssh 16 s, 4454 mV): `smp2p out item 429: entries 2: "master-kernel" = 0, "sleepstate" = 1`, READY 114 ms, root + sensors PD attached and served (sns_reg_config, soc0 read ok). No CRASH, no PD FAILURE, no sar.cc line in adspboot's output or the kernel log; ~7 min later the ADSP's SMP2P state is still 6 (ready) — every earlier run went to 7 at 10.1 s.
- adspboot itself stopped printing after the attach phase (the 120 s watch printed nothing; killed by the runner's timeout). stdout is line-buffered, so it likely blocks in the listener; to fix.
- Next: fix the adspboot stall; then APR (apr_audio_svc channel) towards a speaker tone.

### 2026-10-29 13:50 the 10 s fatal decoded (Hexagon disassembly; research/notes/adsp-sarclient.md)
- llvm-objdump (hexagon) on the reassembled adsp.elf: SarClient init (b13 0xb2164e18) -> SeeHelper::init 0xb20660bc: findSuidSync("resampler"), then findSuidSync("remote_proc_state"), each 40 x 250 ms = 10 s; failure -> "Failed to initialize ASH SEE helper" -> fatal sar.cc.
- remote_proc_state driver (b13 0xb21d5268): smp2p_register(APPS, "sleepstate"); only on success does it set AVAILABLE. We only write "master-kernel" in our outbound SMP2P item, so it never becomes available.
- Fix: a "sleepstate" entry (bit 0 = 1 = awake) in our outbound item 429 before the ADSP starts. Implementing.

### 2026-10-29 13:25 stub modem SAR service: no effect (au13; adspboot ef1590b)
- `--stub-svc 17` announced (NEW_SERVER svc 17 at 1:20); the ADSP never sent it anything; no NEW_LOOKUP/DEL_CLIENT/BYE at all. Same fatal at 10127 ms. Missing remote service ruled out (as the 2026-10-28 18:45 analysis said: findSuidSync waits for sensor SUIDs). Likely: no physical sensor driver registers (power rails / interrupts / buses for the SSC sensors). Mi 9T firmware (non-Google) has no SarClient. Next: which sensor SarClient needs and what powers it (read-only).

### 2026-10-29 13:10 the 10 s fatal = SarClient in the static sensors image (firmware strings)
- ../restore/fw/vendor/adsp/adsp.b13 @1156273-1156520: "CHRE_util", "Failed to initialize ASH SEE helper", "chre_utils fatal", "sar.cc", then sns_qmi_client_alt.c "qmi_client_init_instance error %i"; adsp.b18 @10212: "9SarClient", "N4chre26SeeHelperCallbackInterfaceE", "Failed to find sensor '%s'". No SAR/CHRE config key or nanoapp file.
- au12: the ADSP sent no NEW_LOOKUP (HELLO, 12 NEW_SERVER, 4 DATA). Our adspboot announces only svc 64 to it; the modem's services (wland's edge) are never announced to the ADSP. Hypothesis: SarClient waits ~10 s for a QMI service (likely the modem's SAR service) and asserts. Next: stub-service option in adspboot, then real modem<->ADSP forwarding if confirmed.

### 2026-10-29 12:55 sensors registry builds; DSP still dies at 10.1 s (au12; adspboot 1530cd3)
- 1530cd3: persist paths with `..` were refused (EACCES); now normalised; remove/rename inside persist. tools/sunfish-soc0-files.sh: soc_id 365 (DT qcom,msm-id), hw_platform OEM, subtype id 14, version 2621440, revision 1.0 (guesses except soc_id).
- au12 (ssh 16 s, 4455 mV): both PDs attach/register/init2 0; sensors PD 905 calls (fwrite 206, frename 119, fopen 161 ...); 87 registry files in /var/local/sensors-persist/registry/registry; no file errors left except lowi_lp.conf; no PD FAILURE line, no SMMU fault. ADSP CRASH at 10124 ms: `sensor_process ... sar.cc:27 chre_utils fatal` (same as au10, where the sensors PD was never attached). iOS unaffected, no reset.

### 2026-10-29 12:40 root + sensors PD listeners serve files (au10, au11; kernel95; adspboot ae98d06)
- ae98d06: root PD listener after attach (remotectl open adsp_default_listener, register, init2, next2 loop with apps_std); `--sensors` then attaches the sensors PD (pd 2, client tgid+1, second buffer half). Sensors config (51 files from ../restore/fw/sensors/sensors) pushed to /var/local/fw/sensors (phone only).
- au10 (root only): attach 0, remotectl_open 0, register 0, init2 0, apps_std handle 11; only request `fopen_with_env ADSP_LIBRARY_PATH:.//lowi//lowi_lp.conf` -> missing; ADSP CRASH after 10149 ms (sar.cc:27 chre_utils fatal).
- au11 (root + sensors): sensors PD attach/register/init2 0; 718 calls served (fopen_with_env 311, stat 140, fread 138, readdir 42 ...); opened sns_reg_config + 28 config/*.json (bmp380, lis2mdl, lsm6dsr ...). Missing: 9 /sys/devices/soc0/* files, the persist registry dir (/mnt/vendor/persist/sensors/registry/registry: mkdir fails, temp.json x44). PD FAILURE sar.cc chre_utils fatal, then ADSP CRASH after 10079 ms "EX:kernel:0x0:sns_sig_remote_:0x109d". No reset; ssh 17 s; 4455 mV.

### 2026-10-29 12:20 FIRST FastRPC call: root PD attach -> 0 (au8, au9; kernel95; adspboot 6d4720f, afa2fd6)
- 6d4720f: the translating bank with the reference's exact values (SCTLR 0x1127, TCR 0x00803519 = cacheable inner-shareable walks, TCR2 0x38011, MAIR0 0xf404ff44, CBAR 0x1f300, leaf 0xe43). au8: refused by our read-back check (SCTLR reads 0x127: HUPCF is RAZ); afa2fd6 tolerates it.
- au9 (4454 mV, ssh 17 s): bank ok, 0x1b23 -> CB13; READY 117 ms; `rpc: <- rsp ctx 20 ret 0`, `rpc: root PD attach -> 0`. No SMMU fault (FSR 0x400 = format only), no reset. Then `sensor_process ... sar.cc:27 chre_utils fatal` again; `files opened: 0` (nobody serves the sensors files yet).
- The reference values are now the default (smmu.c; SMMU_OLD=1 = our old values). Difference that mattered (guess): cacheable/inner-shareable table walks and/or SCTLR TRE/AFE.
- Next: root PD listener (remotectl open, register, init2, next2) serving apps_std files, sensors files staged from the vendor partition onto /var/local (phone only, out of git), then the sensors PD.

### 2026-10-29 12:05 root PD attach with a 39-bit bank: same translation fault (au7; adspboot 0b14a57)
- Bank (CB12/13): SCTLR 0x61, TCR 0x00800019 (T0SZ 25), TCR2 0x62, MAIR0 0x44, CBAR 0x1f000, CBA2R 1; walk for 0x80000000 and 0x380000000 both L1->L2->L3 = DMA page | 0x443 (correct, before and after).
- READY 117 ms; attach sent (516 bytes); DSP crash after 1118 ms `frpck_0_0:0x95 PC=0xf02777ac`; FSR 0x402 (TF) FAR 0x80000000 FSYNR0 0x00110000. The SMMU strips the stream bits (FAR is 0x80000000). No SoC reset; ssh 17 s, 4455 mV.
- Next: the reference's exact bank values (cacheable inner-shareable walk TCR 0x00803619, MAIR0 0xf404ff44, SCTLR 0x1127) behind SMMU_REF=1; decode FSYNR0.

### 2026-10-29 11:50 DSP READY, sensors PD survives, root PD attach faults in the SMMU instead of resetting (au5, au6; kernel95; adspboot 3a275a6)
- 3a275a6: SMP2P items were swapped (we wrote our header into the ADSP's item 443 and waited on 429). Now in 443 / out 429; FSR status separates FORMAT (0x400) from faults.
- au5 `adspboot boot`: `smp2p in item 443: magic $SMP, entries 3: "smp2p" = 0, "slave-kernel" = 6`, `READY after 110 ms (state 6)`; pd-mapper avs/audio, tms/pdr_enabled; NO sensor_process PD failure (first time). `smmu --rpc` + `--status`: 0x1b23 -> CB13, FSR 0x400 no fault.
- Re-running boot on an up DSP: glink_core_intent.c:874 assertion (expected; needs a fresh boot).
- au6 fresh boot: READY after 48 ms; root PD attach (buffer 1062c8000, DSP sees 0x380000000): DSP crash after 1049 ms `EX:kernel:0x0:frpck_0_0:0x95`; CB13 FSR 0x402 (TF) FAR 0x80000000 FSYNR0 0x00110000. No SoC reset: the stream bits route the access to our bank now; the bank's table walk fails (guess: table coherency or TCR/MAIR). Fix in progress. ssh 21 s, 4455 mV.

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
