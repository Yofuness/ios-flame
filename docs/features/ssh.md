# SSH
**State:** ✅ dropbear + dash + toybox; `port/tools/sunfish-ssh`; boot to SSH 16-18 s. **Updated:** 2026-10-29.

## How to use
- `port/tools/sunfish-ssh` (status.md). Install: `tools/sunfish-install-ssh.sh`.
- Files: `python3 -I tools/flame-files.py get|put ...`.

## Facts
- dropbear + dash + toybox. The phone has no ifconfig/netstat/ping/ps in toybox, no `dd`, no nohup/pgrep/proc; `psx` takes a numeric signal (17 STOP, 19 CONT).
- ssh inside `while read` loops needs `</dev/null`.
- launchd starts RunAtLoad jobs with `ProcessType`/`POSIXSpawnType` = Interactive in its first daemon second (~22.5 s); without them dropbear waited until ~26 s. The plist order (alphabetical) does not matter. /var is mounted by ~20.9 s.

## Open
- None open.

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

### 2026-10-29 08:05 D2: dropbear gets Interactive priority (d2a, d2b; kernel94, default args)
- Investigation (rpc3 boot, archived log): launchd_cache_loader 21.865 s, wland 22.958, dropbear exec 26.280, first `exec dash` 32.025, ssh up 31 s. 287 execs before dropbear. Every early RunAtLoad Apple job has ProcessType or POSIXSpawnType Interactive; dropbear had neither. No ordering key exists in launchd.plist (top level: AppExtensions, AppRemovalServices, LaunchDaemons (523), Symlinks, SystemLibraryTreeState, VersionNumber).
- Change: `ProcessType: Interactive`, `POSIXSpawnType: Interactive` on org.iosonpixel.dropbear (phone plist + tools/sunfish-install-ssh.sh). Backups: phone /System/Library/xpc/launchd.plist.pre-d2, local work/userland/launchd.plist.pre-d2.
- Results (kernel s): d2a dropbear 22.486, first dash 24.425, ssh up 18 s; d2b dropbear 22.799, first dash 23.682, ssh up 16 s (battery 4456/4455 mV). Earlier range 24-41 s. Lock screen normal on the webcam.
- Observation: wland exec moved later (24.7 / 28.8 s vs 22.96); not on the SSH path; watch Wi-Fi join time.

No entries before 2026-10-25.
