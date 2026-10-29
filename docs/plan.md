# Plan (2026-10-29)

Rules: CLAUDE.md ("How to work"). Old plan: `docs/plans/2026-10-28-priorities-broken-list.md`. One line per item;
details and history in the feature Logs (`docs/features/<name>.md`).

## Priority (a couple at a time, easy first, dev speed on top; ▶ = being worked; ~half a day max each)
1. ▶ **D2: SSH earlier at boot** (dev speed, ~1 h): dropbear starts ~33 s into boot, after ~540 execs; give it an
   early launchd position (a root write, with a backup and the escape boot). Every test pays the wait. (ssh.md)
2. **App survey** (~1 h, no risk): open every app and core function (keyboard, Safari over Wi-Fi, Settings panes,
   Notes, Photos, Calendar, Maps, Clock alarms, notifications, App Store without iCloud), webcam frames, and add
   each failure to `docs/backlog.md` ranked by cost. (apps.md)
3. **Quick backlog items:** B1 mtl2.log truncation check on the next Metal install; B19 restart loops (lskdd ~1 s,
   bluetoothd / fairplayd.H2 / wifip2pd ~10 s: exit reasons via `oslog` / `launchctl print`, drop what nothing
   needs); then the easiest finds from the survey.
4. **Screen rotation** (B20): SpringBoard and Safari rotate (method E), the screen stays portrait; next: the
   compositing side (backboardd display transform / our framebuffer). (display.md)
5. **Audio: share memory with the DSP via the hypervisor** (B16/B23): read-only research of the SCM mem-assign
   call in the background now (while phone tests run); phone steps only once it's understood, as baby steps: a
   no-op assign on one page, a shared page the CPU still uses, then the FastRPC attach. `adspboot --rpc` needs
   `--unsafe`. (audio.md)

Done 2026-10-29: VMware bridge pinned (boot to SSH 24-29 s, 0 lost sessions), SMMU streams for the DSP (U7),
Wi-Fi auto-join accepted (U2), FastRPC listener written. Done 2026-10-28: camera IDs, brightness slider, flashlight
button, SE9 I2C + amps, boot-arg defaults.

## Questions for you (Claude took the recommended path meanwhile)
- (none open)

## Your tasks at the phone (when you're available)
- U1 flashlight button by hand: tap on/off, long-press for a lower level. ~2 min.
- U3 battery % while discharging: phone off the charger 30 min. (B13)
- U4 one boot on the TB4 port + 240 W cable, to read the charger status on both sources (optional). (B14)
- U5 speaker beep: listen for a 2 s tone once audio works.

## Dev speed (on top of the priority list when found; ~1 h max each)
- D2 SSH earlier at boot: priority 1.
- D4 [DONE 2026-10-29] live log per boot (`../logs/boots/current.log`); the 474 MB log gzipped in `../logs/archive/`.

## Backlog
The Broken, Later and Optimize lists are in [backlog.md](backlog.md) (not auto-loaded).

## Verification
Each item: numbers in the feature Log, this plan and the README status table in the same commit, a journal line,
a webcam check for anything visible, battery ≥ 3600 mV before each boot.
