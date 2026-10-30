# Your tasks at the phone

Things only a person at the phone can do (feel, listen, look, plug in). Claude keeps working on other items meanwhile and
reads the answers here or in chat. Moved out of docs/plan.md on 2026-10-30 (the user's ask). Done ones go to
[completed.md](completed.md).

- U1 flashlight button by hand: tap on/off, long-press for a lower level. ~2 min.
- U3 battery % while discharging: phone off the charger 30 min, on kernel106 with sweetbattery=2 (Claude boots it). (B13)
- U4 one boot on the TB4 port + 240 W cable, to read the charger status on both sources (optional). (B14)
- U10 auto-rotate: when Claude says it's ready, turn the phone on its side and back (Calculator open). ~1 min.
- U12 Camera app (when you're up): ready now (kernel111, camera on, camd idle, preview textures installed): open Camera (Utilities folder), say what you see (preview? colours?), switch to the front camera, take one photo each, check Photos. Keep Live Photo off. ~3 min. If backboardd misbehaves: Claude runs `tools/sunfish-metal2-install.sh --undo`.
- U13 Files app: open Files > Browse > On My iPhone (it opens now, kernel121): create a folder or save something (e.g. share a photo to Files): does it work? ~2 min.
- U14 Touch while plugged in (B36): on the charger, leave the phone idle 3 min, then tap a few times: does it react? Also: in yesterday's Calculator freeze, were you touching the screen? ~4 min.
- U15 Headphone jack: READY (jack6 passed): Claude starts a 60-s detect run; plug wired headphones in at ~15 s, out at ~35 s. ~2 min.
- U16 Haptics: toggle a switch in Settings (Sounds & Haptics): do you feel a tap? Is it too strong or weak? ~1 min (after Claude starts hapticd).
- U11 AirPods music: with the AirPods connected, play Pirates.mp3 (Filza: /var/mobile/Documents, or Music after import). ~2 min.
