# Sleep and wake
**State:** 🟡 power-button sleep/wake stable (11/11); the panel is not switched off. **Updated:** 2026-10-23.

## How to use
- `flame-cmd.py key 0 150` presses power remotely (xnu 0137).

## Facts
- Every press → `SWEET HID: key 0x30` → `SWEET FB: display off/on`; "display off" only stops presenting frames.

## Open
- Does the OLED actually go dark on "display off"? Real panel-off needs a DSI DCS command (see brightness).

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

Older entries (before 2026-10-25): [archive/features/sleep-wake.md](../archive/features/sleep-wake.md).
