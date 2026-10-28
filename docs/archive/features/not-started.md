# Not started: archived log (entries before 2026-10-25)
Current file: [features/not-started.md](../../features/not-started.md). Same format, newest first.

## Log

### 2026-10-23 09:00 Step 2 cheap items — triage (GPU blocked early, so started ahead of 11:30)
- **Brightness — deferred (not cheap).** sunfish panel sofef01 is OLED with `bl_ctrl_dcs`: brightness is a
  DSI DCS 0x51 command. The kernel has no DSI code; it needs the DSI controller's command-DMA path
  (Linux dsi_ctrl_hw_cmn kickoff) while ABL's autorefresh runs. A mistake can freeze the display.
- **Auto-rotate / sensors — deferred (not cheap).** Accelerometer etc. are behind the SLPI sensor DSP:
  needs SLPI boot (PAS like the modem) + QMI sensor service + an iOS motion HID.
- **Vibration — deferred (not cheap).** PM6150 vibrator @0x5300 (SID 1) is easy to drive, but iOS
  haptics go through Apple's Taptic stack (AppleHaptics/HID), which would also need emulating.
- **Sleep/wake stability — doing it.** New command `key N [MS]` (0137) lets the test press power remotely.
- (Recovered from `git show 1808cf9^:docs/sunfish/workday.md`; that commit deleted it.)
