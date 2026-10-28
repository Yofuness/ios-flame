# Not started
**State:** ⬜ audio, Bluetooth, cellular, camera, sensors. **Updated:** 2026-10-23.

## Facts
- Sound and mic: ADSP firmware, codec, iOS audio driver. Weeks; one project covers speaker, earpiece, jack and mic (plan "Later").
- Bluetooth, camera, sensors: hard, uncertain. Cellular: skip.
- Auto-rotate / sensors: behind the SLPI sensor DSP (SLPI boot + QMI sensor service + an iOS motion HID).
- Vibration: PM6150 vibrator @0x5300 (SID 1) is easy to drive, but iOS haptics go through Apple's Taptic stack.

## Open
- All of the above.

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

Older entries (before 2026-10-25): [archive/features/not-started.md](../archive/features/not-started.md).
