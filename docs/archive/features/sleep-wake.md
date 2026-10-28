# Sleep and wake: archived log (entries before 2026-10-25)
Current file: [features/sleep-wake.md](../../features/sleep-wake.md). Same format, newest first.

## Log

### 2026-10-23 09:00 Step 2.4 sleep/wake stability — **pass** (0137)
- `flame-cmd.py key 0 150` (new, 0137) presses power remotely. 11 toggles over 5 min: every press →
  `SWEET HID: key 0x30` → `SWEET FB: display off/on`; SSH alive in every state; 0 panics.
  Screenshots logs/shots/sleep-1.png (wallpaper) / wake-1.png (black) — labels swapped (first press slept).
- **Needs you tonight:** does the OLED actually go dark when "display off"? We only stop presenting
  frames; real panel-off/brightness needs a DSI DCS command (same missing piece as brightness).
- (Recovered from `git show 1808cf9^:docs/sunfish/workday.md`; that commit deleted it.)

### 2026-10-23 09:00 Step 2 cheap items — triage (GPU blocked early, so started ahead of 11:30)
- See [not-started](not-started.md) for this entry; it also touches this feature.

### 2026-10-23 10:00 status.md update
- Sleep/wake via power button: stable (11/11).
