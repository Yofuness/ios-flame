# Backlog (2026-10-29)

Not loaded automatically: read it when waiting on a test or build, after finishing a priority item (to pick the
next easy item), and when adding something newly broken. Priority items live in [plan.md](plan.md); details and
history in the feature Logs. Finished items move to [completed.md](completed.md).

## Broken (cheapest first; [phone] = needs the phone)
- B46 Compass shows no heading: CoreLocation waits for a DeviceMotion HID service (page 0xFF0C usage 4, fused motion from Apple's motion coprocessor); accel/gyro/mag already stream (sensors.md cs1). Needs host-side fusion + Apple's event format: days.
- B44 Garbled image areas in Maps, TV, Mail, Voice Memos, Compass, Tips (sv1 shots): probably B29's texture-shadow problem in bigger images. 2026-10-30: 0109 shadow repack tried, no change (apps.md rp1). Round 2: UBWC off no change; parked (apps.md 14:00).
- B45 Measure (ARKit) and Watch (Bridge) don't stay open; Weather has no data (needs Apple's weather service).
- B42 Auto-brightness (parked 2026-10-30, sensors.md): lux reaches iOS, AAB ramps, but legacy mode writes only the slider value and new-arch mode needs a CADisplay brightness control we don't have. Option: our own lux -> brightness loop (night flag respected).
- B38 Boot cmdline limit: CAMERA=1 plus 3 extra boot args = 606 bytes > 592; sunfish-boot.sh refuses (sp16 dropped sweetrtcoff by hand). Shorten the defaults or raise the limit (D6 in plan.md). 2026-10-30: flamereboot=0 (kernel default) dropped and the double space gone (-15 B: 575 with sweetse9=2 sweetcampmic=2), sunfish-boot.sh now fails fast with the length. Still 1 B short for a third arg like sweetampreset=1 (592); real fix = a kernel-side second arg string (e.g. from the ADT /chosen), later.
- Camera black: iOS lists both cameras now (cam32); Camera.app test with the user = U12 (camera.md).
- B36 Touch silent while plugged in (was "Calculator froze the UI", kernel107 mo1, 2026-10-29 17:10 EDT): not a hang: kernel, GPU, backboardd kept running; no "SWEET HID: touch" from 150 s to 423 s, touch back right after the cable was replugged (research/notes/b36-calculator-freeze.md). Repro: plugged in (TB4, 240 W), idle 2-5 min, tap with a finger; check SWEET HID lines + the SE7 touch IRQ count. Needs the user (U14).
- B35 Privacy & Security has no Location Services row: CoreTelephony getSubscriptionInfo fails without CommCenter; the SweetMetal2 hook doesn't load in Preferences. Cosmetic; needs Preferences injection or a CommCenter stub (apps.md).
- B33 Home swipe: no injected bottom-edge swipe goes home (5 variants, b33); tests use `H` (home button hook: Calculator -> home confirmed, bt1) and `K` (lock); a real finger swipe works (U8, 2026-10-30): only the injected swipe fails.
- B29 Lock-screen flashlight/camera button icons and 5 Control Center tiles render as noise: icon-size backboardd buffer textures are shadows; forced shadow refresh (0108) no change. Parked 2026-10-29 (leads in apps.md). 2026-10-31: the lock-screen clock digits render as the same noise some minutes after boot (cam23, cam27: the camera was off in cam27, so not the camera).
- B8 PhotosFileProvider crashes (cause found 2026-10-30: Sandbox op 4 ENOENT, fix xnu 0240 = B37's): 0 in the kernel95 idle soak; confirm in a long app soak. (~2-4 per 2 h, uncaught ObjC exception in PlugInKit); low impact.
- B10 Reduce Transparency slow in app opens: RT on/off benches + one GPU trace. [phone]
- B11 UI watchdog misses a kernel hang with swaps still running.
- B12 Sensors: [MOSTLY DONE 2026-10-30] auto-rotate works via the ADSP SNS client (sensors.md); open: proximity/light into iOS.
- B13 Battery % while discharging unchecked (U3).
- B14 Charging drain under load = the USB source's budget; optional read-only TYPEC/APSD check (U4).
- B15 Bluetooth: [MOSTLY DONE 2026-10-30] pairing + AirPods connect, 3 Mbaud (bluetooth.md); first bring-up fixed 2026-10-30 (bt12); open: AirPods audio (U11).
- B16 Audio: see priority 1; then APR + CS35L41 for a beep (U5).
- B19 Respawn loops: lskdd, fairplayd.H2, wifip2pd dropped 2026-10-29; bluetoothd (10 s) kept until B15.
- B23 Any HLOS buffer the DSP touches resets the SoC: see priority 1.
- B30 Headphone jack (the user asked, 2026-10-29; not priority): WCD937x over SoundWire + MBHC; sound needs the ADSP audio path (B16/B23, audio.md M4). Jack-detect alone may work earlier (codec interrupt, no DSP).
- B32 Location Services: switched on with `echo "L 1" > /var/local/sweet-orient` (2026-10-29); survives a reboot (tap1). No position source yet (no GPS driver; Wi-Fi location needs Apple servers).
- B18 Cellular: not planned. iCloud: not expected to work.

## Later (needs repeated 1 h+ tests)
- B21 GPU busy-hang recovery + pool growth (no real hang seen since xnu 0207).
- B4 Silent resets in long app soaks (PON reason is sticky, no lead).

## Optimize (last)
- O11 UniXNU cd1b975: Turnip fence waits up to 100 ms instead of 1 ms slices (our turnip-darwin-kmd.patch line 453; check our kernel wait sleeps on the fence IRQ first; 0.5 h); redfin's ports 0131-0135 + 0137 of AdrenoMetal 0010-0016 (3-5 h, knobs default off). metal2vulkan: stay on 720ef1ce; its shadow dirty-page set (fee37100 etc.) only if traces show B29 shadow cost (research/notes/upstream-2026-10-30-gpu.md).
- O12 iOSPlus10 e23063c: slow USB/touch/key/BT polls while the display sleeps (their ~1270 timer wakeups/s with screen off). 2026-10-30 survey (research/notes/screen-off-power.md): ours ~3600 wakeups/s screen-off (wland 1 ms ~1000/s, USB 1 ms 1000/s, btbridge ~500-1000/s, sensors 2 ms ~500/s, hapticd 4 ms 250/s, touch 125/s, vsync 60/s, keys 50/s); plan: userland waits first (hapticd, wland), then a display-off flag for USB/touch/vsync/keys (iOSPlus10 0382 scheme, MIT). pw1 (e5cd72d installed): screen-off CPU per 60 s: hapticd 0.36 -> 0.00 s, wland 0.64 -> 0.51 s; adspboot (sensors) 2.80 s = 4.7 % of a core (the biggest): next. Sensors idle when blanked (408bcb5): adspboot 2.80 -> 0.05 s/min. Left: kernel display-off flag for USB 1 ms / touch / vsync / keys (iOSPlus10 0382 scheme), btbridge.
- O9 ios-redfin GPU line (survey 2026-10-30, research/notes/redfin-survey-2026-10-30.md), ranked: DEADFILL (plan item 0), bridge 0109+0110 COLOR_WRITES=fetch (1-2 h), 0111 scratch trim (1-2 h), TU_DEBUG=sysmem + Turnip 0108 retest (0.5 h), Turnip 0111 UBWC (1 h, A618 UBWC differs), bridge 0103-0108 rebase (4-8 h), 0130-0137 UniXNU ports (2-4 h), xnu 1041 CA threads on big cores (2-3 h), 1040 GPU IRQ fence waits (4-6 h, no fps gain), NILENC/PREWARM (2-3 h), rotfb4 landscape (2 h). Skip (no gain for them): ENCTHREAD 3, FP16, CONTRACT, COLOR_WRITES=plain, xnu 1035/1036/1037/1039/1042, triple buffering, gmem autotune.
- O10 ios-redfin xnu 1034/1045 USB Ethernet fast path + replug recovery (not GPU; could help our USB link).
- O2 zero-copy scanout: works by hand (kernel103 `sctl kern.sweet_zc_scan=2`), no fps gain (zc5), auto mode (kernel104, sweetfb_zerocopy=2) garbled once in two boots; parked 2026-10-30 (smoothness.md).
- O3 Reduce Transparency speed (with B10).
- O7 ~21 fps limit = backboardd's GPU 25-30 ms per frame (blur passes) + CPU/GPU never overlap (research/notes/fps-limit.md): A76 bias (0147) and GPU clocks (565/650 MHz) were tried in Oct 23-24 with no gain; left: no-blur bench, 2 frames in flight (QuartzCore RE). 2026-10-30: parked after DEADFILL/A76/CA-key tests (+0.5 at best) and an inconclusive CA_EMPTY_SHADERS run (smoothness.md); next real step = two frames in flight (QuartzCore RE, days).
- O5 display-off CPU fade when sweetpanelsleep is off.
- O6 failed GPU hang recovery takes 3.9-12.8 s: fail faster.
