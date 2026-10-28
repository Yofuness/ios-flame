# Recovery (Pixel 4a, sunfish)

How to get out of trouble on the Pixel 4a. This file only collects what the sunfish docs already said; nothing here
was tested as a full restore on this phone. The old Pixel 4 (flame) procedure, with its factory-image steps and
caveats, is in [archive/flame/RECOVERY.md](archive/flame/RECOVERY.md); read it before a real restore, but its
file names and build are for flame, not sunfish.

## Rules (from CLAUDE.md / AGENTS.md)
- Never flash xbl/abl/tz/etc. (ABL, XBL, TZ, modem or other firmware partitions); never relock the bootloader.
- Never select "Factory data reset" in recovery.
- Writes to userdata are incremental only, with a backup first.
- Confirm each destructive step with the user before running it.

## Backups
- `backup/` (+ Lily) (status.md). In the project root (`../backup` from `port/`): the partition images and
  `backup/SHA256SUMS`. They contain per-device data (modemst1/2, fsg, fsc, persist): keep them out of git.

## Restore Android
- status.md: `restore/factory/…s2…zip` → `flash-all.sh`.
- The file in the project root is `restore/factory/sunfish-tq3a.230805.001.s2-factory-aafe4e24.zip` (unpacked next
  to it in `restore/factory/sunfish-tq3a.230805.001.s2/`). `flash-all.sh` comes from the factory zip; it erases data.

## Getting to fastboot
- A hard hang (no SSH, no ARP, no fastboot) needs a human: hold power (+vol down) to reach fastboot
  (2026-10-23, [features/boot.md](features/boot.md)).
- Boot-time watchdog: `sweetbootwdt=N` resets to fastboot if no swap N s after boot (xnu 0189); the UI watchdog
  (`sweetuiwdt=30`, default) ends in fastboot when the UI stays frozen.

## iOS-side escapes
- /var on ufsroot1 fails to boot: boot with `sweetnofstab=1`, then `toybox unlink /private/etc/fstab`
  ([features/storage.md](features/storage.md)). The old /var is still on the root underneath.
- launchd jobs dropped with `tools/sunfish-launchd-drop.sh`: the original is `/System/Library/xpc/launchd.plist.pre-drop`,
  the copy from before step 6 is `/var/local/launchd.plist.pre-step6`; before the CA_FORCE_TIMER change
  `/var/local/launchd.plist.pre-vbl` ([features/apps.md](features/apps.md)).
- Network preferences: `tools/sunfish-netprefs.py` keeps a backup in `logs/preferences.plist.bak-*`.
