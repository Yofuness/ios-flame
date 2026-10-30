# Haptics
**State:** 🟡 the vibration motor buzzes from a test tool (drvbuzz, kernel113 + sweetse9=2); not yet driven by iOS. **Updated:** 2026-10-30.

## How to use
- Boot with `sweetse9=2`. `/var/local/bin/drvbuzz --gpio` (read-only), `--probe` (ID + register dump, no writes),
  `--buzz MS LEVEL` (MS 1..500, LEVEL 1..0x40 = at most half scale). SE9 is shared: not while cs35l41/camid run.

## Facts
- TI DRV2624 LRA driver on SE9 I2C (qupv3_se9_i2c @0xa8c000) address 0x5a (../recon/live.dts:9641-9664); reset = TLMM gpio11
  active-high, TLMM north: CFG 0x390b000, IN_OUT 0x390b004. Under iOS the pin is at its power-on CFG 0x1 (input, pull-down:
  chip held in reset); drvbuzz sets the DT drv2624_rst value 0x200 (output, 2 mA) and restores 0x1 at exit.
- Open loop (DT ti,smart-loop=1 = OPEN_LOOP), LRA; OD_CLAMP 0xA1 = 3.42 V peak = full scale (RTP 0x7F). Init values = Google's
  sunfish drv2624.c (research/notes/haptics.md has the verified table, power-up order and do-not-write list).
- iOS side (hakios-xnu, MIT): haptics are 48 kHz mono 16-bit PCM on the "Actuator" audio device, only with an ADT
  /product/haptics node (closed-loop 1, supports-3rd-party-haptics 1). SunfishAudio.driver's Actuator already writes to
  /dev/sunfish-haptic (missing so far).

## Plan
1. [DONE] read-only ID probe, 2. [DONE] buzz from a test tool, 3. iOS path: ADT node (mkadt.py --haptics) + a daemon that
   turns the Actuator PCM into RTP pulses (taps <= 60 ms, longer = vibration envelope), 4. check AudioServicesPlaySystemSound.

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

### 2026-10-30 08:20 hp2: THE VIBRATION MOTOR BUZZES (kernel113 CAMERA=1 sweetse9=2 sweetcampmic=2; drvbuzz ab6f790)
- `--gpio`: TLMM_N gpio11 cfg 0x00000001 io 0 (power-on default); TLMM_S slot 0 (not gpio11's tile).
- `--probe`: "wr 0x390b000 = 0x200 (gpio11 output low), reads 0x200", release; "ID 0x03 (chip 0 rev 3) STATUS 0000", no I2C
  errors. Reset defaults: 07=44 08=88 0d=00 1f=3f 20=89 21=0d 22=6d 23=36 27=10 28=11 29=0c 2c=00 2f=c6. Restore: io 0, cfg 0x1.
- `--buzz 300 0x20` (quarter scale): STATUS 0x08 (PROCESS_DONE, no UVLO/OT/OC), no readback mismatch; mic at 155 Hz: 36.4 / 34.4
  vs neighbours 13-17 and 0.5 (2-3x, under the 8x TONE threshold); 172 Hz: nothing.
- `--buzz 300 0x40` (half scale): STATUS 0x08, no faults; mic: 1 TONE window, 155 Hz 61.3 vs 2.7 / 0.66 (rms 205.7).
- After: gpio11 cfg 0x1, io 0 (restored). No reset. LRA_PERIOD reads 0 (open loop: no back-EMF period, as expected).
- Earlier: hp1 stopped at the gpio check (the first drvbuzz only accepted a configured pin); the gpio11 tile/default came from
  pinctrl-sm7150.c + sdmmagpie (research/notes/haptics.md "gpio11 tile"). The DT also names gpio11 as the TE pin of a Qualcomm
  reference panel (nt35695b truly) and of qcom,dsi-display-secondary: not sunfish's panel (gpio10).
- Next: the iOS path (plan step 3).
