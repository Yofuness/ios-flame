# Next steps: usability quick wins (plan of 2026-10-27)

Old plan: `docs/sunfish/plans/2026-10-27-gpu-stability-usability.md`. Rules from it still apply (RAM-only boots, stop below 3600 mV, one change per boot, push only to `sunfish`, webcam check after every visual change). Known-broken list: `docs/sunfish/broken.md`.

## Context
The GPU stability work is done apart from the 2 h exit soak (steps 1-5 of the old plan; kernel60 is current). The goal now is a phone that is usable day to day. This plan takes the quick items first: checks with no new code, then small changes of about half a day each. The big items (charging, sound/mic, Bluetooth, camera, sensors) come after, in the order under "Later".

## Steps (each: [ ] until done, with numbers)

### A. Checks, no new code (one boot of ~1.5 h on kernel60, phone-runner)
1. [ ] **Wi-Fi status.** Join the saved network, load a page in Safari, note DHCP address and the page load. Fix the contradiction in `status.md` ("working" vs "join fails").
2. [ ] **Battery %.** Patch 0128 (IR-compensated estimate) was never checked. Over 30 min, compare the on-screen % (webcam) with the `SWEET BATTERY` mV/mA lines: no jumps, direction matches the current.
3. [ ] **AegirPoster crash loop.** Drop it with `tools/sunfish-launchd-drop.sh` (find its launchd label or the job that launches it first; it may be an extension, not a job). Check the lock screen on the webcam; count crashes per 30 min before/after (487 in the long log).
4. [ ] **USB flap after boot (old step 5).** 3 fresh boots, watch the link for 5 min each: drops, time to recover.

### B. Small new code (~half a day each)
5. [ ] **Storage.** userdata is 0x1B65FFB000 bytes (~117 GB); the iOS root uses ~16 GB of it. Safe route: a second volume in the free space, mounted on `/var/mobile` (or `/var/mobile/Media` first), leaving the working root untouched. Back up first; incremental writes only. Done when Settings > General > Storage shows ~100 GB free.
6. [ ] **Flashlight, LED only.** Switch the PM6150L flash LED on/off from a phone tool via SPMI. Control Center hookup is a separate later step (iOS drives the torch through the camera stack).
7. [ ] **Brightness, panel only.** OLED brightness via the panel's DCS brightness command (guess: 0x51 over DSI) from a test tool; check on the webcam and the battery current. Hooking the Control Center slider is a separate later step. `sunfish-dim.sh` stays as the fallback.

### C. To do (deferred)
8. [ ] **2 h exit soak (from the old plan).** kernel60, app switches: 0 reboots, 0 unrecovered GPU hangs, pool flat. Battery will still drain until charging is fixed; note mV/mA anyway. Run when the user says so.

## Later (in this order, each its own plan step when reached)
- **Charging:** battery drains ~0.5 A under load on the charger. PMIC writes are risky: ask the user first. Notes: `docs/sunfish/charger-regs.md`.
- **Brightness slider and flashlight button** in Control Center (after 6/7).
- **Sound and mic:** ADSP firmware, codec, iOS audio driver. Weeks; one project covers speaker, earpiece, jack and mic.
- **Bluetooth, camera, sensors:** hard, uncertain. Cellular: skip.
- **Perf:** return to the ~21 updates/s ceiling with the soak as the regression gate.

## Files
- `tools/sunfish-launchd-drop.sh`, `docs/sunfish/broken.md` (step 3)
- `phone/tools/` (flashlight and brightness test tools), `work/src/xnu` if a kernel hook is needed → `xnu/patches/NNNN-sunfish-*.patch`
- docs: `docs/sunfish/plan.md`, `status.md`, `experiments.md`, `workday.md`

## Verification
Each step: numbers into experiments.md + plan.md in the same commit; webcam check for anything visible. Battery ≥3600 mV before each boot.
