# SSH
**State:** ✅ dropbear + dash + toybox; `port/tools/sunfish-ssh`. **Updated:** 2026-10-28.

## How to use
- `port/tools/sunfish-ssh` (status.md). Install: `tools/sunfish-install-ssh.sh`.
- Files: `python3 -I tools/flame-files.py get|put ...`.

## Facts
- dropbear + dash + toybox. The phone has no ifconfig/netstat/ping/ps in toybox, no `dd`, no nohup/pgrep/proc; `psx` takes a numeric signal (17 STOP, 19 CONT).
- ssh inside `while read` loops needs `</dev/null`.

## Open
- None open.

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

No entries before 2026-10-25.
