# Haptics
**State:** ✅ iOS haptics work (switch taps pulse the motor); hapticd starts at boot (tools/sunfish-install-haptics.sh, boot with HAPTICS=1); the user's feel check (U16) open. **Updated:** 2026-10-30.

## How to use
- Boot with `sweetse9=2`. `/var/local/bin/drvbuzz --gpio` (read-only), `--probe` (ID + register dump, no writes),
  `--buzz MS LEVEL` (MS 1..500, LEVEL 1..0x40 = at most half scale). SE9 is shared: not while cs35l41/camid run.
- iOS haptics (step 3, not yet tested on the phone): boot with `HAPTICS=1` (ADT /product/haptics; composable with
  `CAMERA=1`) and `sweetse9=2`; install SunfishAudio.driver (`tools/sunfish-install-audio.sh`) and
  `python3 -I tools/flame-files.py put work/userland-linux/out/hapticd /var/local/bin/hapticd 755`; then by hand over
  ssh: `/var/local/bin/hapticd > /var/local/hapticd.log 2>&1 &` (launchd job later). It makes the FIFO
  /var/run/sunfish-haptic; the driver opens it within 1 s of the next Actuator write. Stop: `kill` it (gpio11 restored).
  Lines: `hapticd: tap N: 18 ms level 0x.. (peak ..)`, `hapticd: vibration N: start level ..`, `hapticd: vibration|cut N:
  .. ms level ..`, `hapticd: chip up` / `chip in reset (2 s idle)`; mediaserverd (camdev.log): `the actuator's FIFO ... open`.
- Open: after the first HAPTICS=1 boot, the MobileGestalt cache must be moved aside once so iOS sees the haptics node
  (as tools/sunfish-install-camera.sh step 5 does: systemgroup.com.apple.mobilegestaltcache/Library/Caches/
  com.apple.MobileGestalt.plist, keep a copy); not scripted yet.

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

### 2026-10-30 16:05 U16: the user feels the vibration (kernel127)
- "Vibration works as well" (the user, playing with the phone).

### 2026-10-30 09:30 hp4: hapticd starts at boot (launchd job installed), taps work; SE9 lock in (0ed877c)
- tools/sunfish-install-camera.sh (tools with the SE9 lock) + tools/sunfish-install-haptics.sh (job org.iosonpixel.hapticd) + reboot (kernel115 HAPTICS=1 CAMERA=1 sweetse9=2 sweetcampmic=2 sweetblnew=0): hapticd pid 72 under launchd; 3 switch taps -> "tap: 18 ms level 0x2e (peak 15750)", "ID 0x03 STATUS 0000", "chip in reset (2 s idle)", no faults. A mediaserverd restart during taps: no SE9 errors (no new camera open happened in the window, so the lock wasn't exercised against camid).
- Mic at 155 Hz heard nothing this time (18 ms taps at level 0x2e are short; hp3 saw small bumps): the feel check stays with the user (U16).

### 2026-10-30 09:00 Install script written (037da8f, 81d0fbb); NOT installed until SE9 has a lock
- tools/sunfish-install-haptics.sh: launchd job org.iosonpixel.hapticd (wrapper hapticd-start.sh, log /var/local/hapticd.log), MG cache aside once, --undo/--check; /var/local/haptics.off keeps hapticd off (touch it, kill hapticd: launchd restarts it, it exits 0).
- Hold: SE9 also carries the camera PMIC (slg51000) and the amps; with no lock a tap could interleave with a camera rail write. A cross-process flock in geni_i2c.c is being added first.

### 2026-10-30 08:45 hp3: iOS HAPTICS WORK (HAPTICS=1 CAMERA=1, kernel113 sweetse9=2 sweetcampmic=2; f84a058)
- Setup: SunfishAudio.driver with the Actuator FIFO, hapticd + drvbuzz installed, MobileGestalt cache moved aside (backup com.apple.MobileGestalt.plist.pre-haptics), ADT with /product/haptics. hapticd: "reading /var/run/sunfish-haptic (48 kHz mono s16)", FIFO prw-rw-rw-.
- 4 taps on a switch in Settings > Sounds & Haptics (fake taps `T`): hapticd "tap 1..4: 18 ms level 0x2e (peak 15750)"; each: "ID 0x03 (chip 0 rev 3) STATUS 0000", "chip up", later "chip in reset (2 s idle)"; 4/4 clean, no fault. mediaserverd: "the actuator's FIFO /var/run/sunfish-haptic open". Mic at 155 Hz: 4.5-6.2 at 3.5/6.5/9/12 s vs 0.5-1 elsewhere (the taps; short 18 ms pulses stay under the 8x TONE threshold).
- After: hapticd SIGTERM, gpio11 cfg 0x1 io 0 restored. No reset, 4456 mV.
- Open: launchd job + install script (and the MG cache step), boot recipe with HAPTICS=1 by default, SE9 shared with camid (camera sensor I2C) and the amp tools without a lock, feel check by the user.

### 2026-10-30 haptics step 3 written (untested on the phone)
- mkadt.py `--haptics` (/product/haptics closed-loop 1, supports-3rd-party-haptics 1: the two props of d22ap.json),
  sunfish-boot.sh `HAPTICS=1`. SunfishAudio.driver: Actuator -> FIFO /var/run/sunfish-haptic (non-blocking, 512-byte
  atomic writes, SIGPIPE off, reopen <= 1/s). hapticd (phone/wifi/hapticd.c): redwood_haptic.c's burst rules -> RTP
  (tap 18 ms at 0x60*peak, vibration 0x40*peak following 20 ms, 1000 ms cap + 1000 ms rest), chip in reset after 2 s
  idle. drvbuzz's chip code moved to phone/wifi/drv2624.c (same output and caps; RTP cap 0x40 there, 0x60 hard max).

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
