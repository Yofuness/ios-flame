# Plan (2026-10-29)

Rules: CLAUDE.md ("How to work"). Old plan: `docs/plans/2026-10-28-priorities-broken-list.md`. One line per item;
details and history in the feature Logs (`docs/features/<name>.md`).

## Priority (a couple at a time, easy first, dev speed on top; ▶ = being worked; ~half a day max each)
1. ▶ **Quick backlog items:** B1 mtl2.log truncation check on the next Metal install; B28 AegirPoster
   (listing it garbles the clock; find why); B24 Maps listed alone in mtl2-clients. (Parked: B25 Music "app not authorized", B29 noisy lock buttons.)
2. **Screen rotation** (B20): SpringBoard and Safari rotate (method E), the screen stays portrait; next: the
   compositing side (backboardd display transform / our framebuffer). (display.md)
3. **Audio: share memory with the DSP via the hypervisor** (B16/B23): read-only research of the SCM mem-assign
   call in the background now (while phone tests run); phone steps only once it's understood, as baby steps: a
   no-op assign on one page, a shared page the CPU still uses, then the FastRPC attach. `adspboot --rpc` needs
   `--unsafe`. (audio.md)

Done 2026-10-29: B19 lskdd/fairplayd.H2/wifip2pd dropped (xpcproxy 633 -> 314 per 10 min), app survey (11 of 18 apps fine; B24-B27 in backlog), D2 SSH earlier (dropbear Interactive priority: boot to SSH 16-18 s, was 24-41), VMware bridge pinned (boot to SSH 24-29 s, 0 lost sessions), SMMU streams for the DSP (U7),
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
- D2 [DONE 2026-10-29] SSH earlier at boot: dropbear Interactive, SSH up 16-18 s.
- D4 [DONE 2026-10-29] live log per boot (`../logs/boots/current.log`); the 474 MB log gzipped in `../logs/archive/`.

## Backlog
The Broken, Later and Optimize lists are in [backlog.md](backlog.md) (not auto-loaded).

## Verification
Each item: numbers in the feature Log, this plan and the README status table in the same commit, a journal line,
a webcam check for anything visible, battery ≥ 3600 mV before each boot.
