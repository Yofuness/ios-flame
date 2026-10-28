# Boot
**State:** ✅ RAM-only `fastboot boot`; iOS 16.7.16 (iPhone X userland) root on userdata. **Updated:** 2026-10-27.

## How to use
- From fastboot (status.md "How to boot"):
  ```
  cd port && tools/sunfish-boot.sh NAME work/kernel37/kernel.development.sweet sweetpresentwait=1
  ```
  `tools/sunfish-boot.sh NAME KERNEL [bootargs]`; use the newest good kernel (kernel68+ for /var on ufsroot1, kernel69 for the torch).
- Never flash partitions, never relock, never select "Factory data reset" in recovery. Stop booting below 3600 mV
  (`fastboot getvar battery-voltage`).
- Default boot args from `tools/sunfish-boot.sh` (2026-10-27 uw2): `flamereboot=0 sweetuiwdt=30` (was `flamereboot=1800`), plus full GPU recovery and backboardd restart on by default (0b74443).
- Escape hatches: `sweetnofstab=1` (xnu 0186) hides /etc/fstab; repair with `toybox unlink /private/etc/fstab`. `sweetbootwdt=N` (xnu 0189) resets to fastboot if no swap N s after the pet thread starts.
- A hard hang needs a human: hold power (+vol down) to reach fastboot.

## Facts
- Key fixes found during the port (status.md):
  - ABL leaves UFS powered down → shim sends START STOP UNIT to W-LUN 0xd0.
  - RAM ends 0x1fcc00000 → XNU high range shrunk 64 MB; log stashes moved below.
  - IMEM restart reason at 0x146aa65c; panel 1080x2340 (was 2280 in kernel constants).
  - Modem XPU violation: rmtfs buffer above 4 GB → fixed at 0xE1001000.
  - `fastboot flash userdata` is slow on big FILL chunks, not hung → trailing FILL → SKIP.
- `flamereboot=N`: `reboot_s <= 0` pets the watchdog forever → no timed return to fastboot (watchdog stays armed, so a real hang still resets) (read of `iokit/Kernel/arm/SweetPlatformExpert.cpp`, 2026-10-23). status.md had listed "`flamereboot=0` — used for k10 to disable the auto-reboot; meaning unverified."
- UI watchdog (xnu 0179, `sweetuiwdt=30`): touch within 10 s + no swap for 30 s → kill backboardd (rung A/1); still frozen 30 s later → stop the watchdog pets (rung B/2) → reset to fastboot. Kernels before 55 ignore sweetuiwdt.
- The live log only starts ~27-60 s after boot; the RAM-log rescue (see storage, methods) has the rest.
- ABL boot image rules for flame are in `docs/archive/flame/NOTES.md`.

## Open
- UI watchdog: catches a frozen UI only; a kernel hang with swaps still running is not caught (broken.md).

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

### 2026-10-28 18:45 boot-arg line full
- tools/mkpayload.py refused `cmdline too long` (limit 608 - 16 bytes for the ramlog VAs) with sweetse9=2 sweetampreset=1 sweetcampmic=2 on top of the defaults. Dropped `sweetgpudelay=0` (the kernel default) from tools/sunfish-boot.sh; 562 bytes with sweetse9 + sweetcampmic. Next time: fold rarely changed sweet* args into kernel defaults.

### 2026-10-28 10:20-10:30 build VM hibernates instead of shutting down
- vm-stop.sh saves the VM (QEMU migrate to a zstd file) and vm-start.sh resumes from it. Hibernate 5.0 s (2.5 GB) / 11.3 s (4.1 GB after a build); resume 5.5 s / 13-15 s to SSH (boot was 26-35 s). Needed `tsc-frequency=2496000000` (invtsc blocked migration) and max-bandwidth 16 GiB/s (the default 128 MiB/s made the save take 33 s).
- tools/vm-build-xnu.sh 73 --stop from hibernation: 134 s total (159 s with boot + shutdown); kernel73 byte-identical to kernel72. vCPU pinning (8 on E-cores 4-11) and the guest clock are reapplied after resume.

### 2026-10-28 09:30-10:20 kernel builds move to a local macOS VM
- macOS 14.8.9 (OSX-KVM OpenCore, QEMU/KVM) in ~/VMs/macos, Xcode 15.0.1 (the user downloaded it with their Apple ID), 8 vCPUs as 8x1 cores pinned to host E-cores 4-11, 8 GB.
- tools/vm-build-xnu.sh (4515b6f) mirrors xnu.yml: full build 337 s, no-change rebuild 54-68 s. kernel71 (kernel70's patch set) has the same size (17895944), load commands and segment sizes as CI's kernel70; only the version string's "runner" becomes "builder".
- Cold cycle (VM off -> start -> incremental build -> VM off): 159 s; kernel72 byte-identical to kernel71.
- GitHub's macOS arm64 runners failed with "job not acquired" for 3 of 4 metal2 runs today; CI stays as the fallback.

### 2026-10-27 ~21:50- overnight run (user away until 05:00; plan.md "Overnight run") (escape hatch and boot watchdog part)
- Escape hatch 0186 `sweetnofstab=1` (kernel62): /etc/fstab hidden from LOOKUP; boot normal. `rm` fails (it stats first); `toybox unlink /private/etc/fstab` works -> that is the repair command.
- Boot watchdog 0189 `sweetbootwdt=N`: the counter starts when the pet thread starts (~26 s kernel time). N=20 never fired (UI drew first); N=5 (kernel66): "SWEET BOOTWDT: no swap 5 s after boot, resetting" at 31.3 s, phone in fastboot ~35 s later. Live log only starts ~27 s after boot (early kernel lines like "SWEET UFS" never appear).
- Lessons: the escape boot hides fstab from cat/stat too, so stage files elsewhere and copy them in on a normal boot; the live log starts ~27-60 s after boot, the RAM-log rescue has the rest.

### 2026-10-27 ~07:40-07:50 uw2: UI watchdog rung B (kernel57) + new boot default
- Freeze: a phone-side loop `psx -k backboardd 17` (SIGSTOP) every 0.1 s, started detached (`(sh /var/local/freeze.sh &)`; there is no nohup/pgrep/proc on the phone), so each respawned backboardd stops before its first swap. Taps every 5 s from the PC.
- Last swap ~216 s. **Rung 1** "killing backboardd" at 246.2 s; **rung 2** "still frozen: stopping watchdog pets" at 276.2 s (+30.0 s). The kernel log ran on to 325.6 s (~49 s pet-stop to bite), then the phone was in fastboot at 07:44:40. Works as designed.
- Two earlier attempts by the agent did not run the test (no pgrep; then an accidental SIGCONT + SIGTERM). psx takes a numeric signal: 17 STOP, 19 CONT, default 15 TERM.
- `tools/sunfish-boot.sh` default changed: `flamereboot=1800` -> `flamereboot=0 sweetuiwdt=30` (kernels before 55 ignore sweetuiwdt, so on those there is no auto-reboot at all now).
- Booted uw2b (kernel57, sweetpresentwait=1, new defaults): "UIWDT: armed, 30 s", home screen on the webcam, 4456 mV in fastboot.

### 2026-10-27 experiments.md lines
- 2026-10-27 uw2: kernel57 UI watchdog rung B: backboardd + respawns SIGSTOPped (psx -k backboardd 17 loop), taps every 5 s: rung 1 kill at 246.2 s, rung 2 (pets stopped) at 276.2 s, phone in fastboot (log ended 325.6 s). Works; now the sunfish-boot.sh default (flamereboot=0 sweetuiwdt=30).
- 2026-10-27 wd20 (kernel65, sweetbootwdt=20): did not fire (counter starts at ~26 s kernel time; UI drew first). wd5 (kernel66, sweetbootwdt=5): fired at 31.3 s, fastboot ~35 s later. PASS.

### 2026-10-26 ~22:14-22:24 kernel55 UI watchdog (xnu 0179) test (uw1)
- "SWEET UIWDT: armed, 30 s" at 27.7 s. No trigger in 90 s idle or during 6 swipe pairs.
- Rung A works: backboardd SIGSTOPped at 241.1 s (last swap) with taps every 5 s -> kill at 271.1 s (30.0 s), new pid, "swaps resumed" at 285.9 s. A second kill at 320.9 s fired after a real 30 s freeze of the respawned backboardd (taps arriving), recovered again.
- Rung B not reached: the respawned backboardd swapped before it could be stopped, which resets the rungs (by design: a respawn that draws counts as recovered). Rung B (stop pets -> fastboot) untested; to test, stop backboardd's respawn before its first swap, or add a debug arg.
- Phone left on night2 (kernel53, no injection, 5%), 4456 mV.

### 2026-10-26 ~22:00 Control: kernel54 (xnu 0178) sweetsmmureset=0, stall hangs, 565 MHz
- See [gpu](gpu.md) for this entry; it also touches this feature.

### 2026-10-26 experiments.md lines
- 2026-10-26 uw1: kernel55 UI watchdog: no false triggers (idle 90 s, swipes); rung A kills a frozen backboardd 30.0 s after the last swap, UI back; rung B untested.

Older entries (before 2026-10-25): [archive/features/boot.md](../archive/features/boot.md).
