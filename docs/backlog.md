# Backlog (2026-10-29)

Not loaded automatically: read it when waiting on a test or build, after finishing a priority item (to pick the
next easy item), and when adding something newly broken. Priority items live in [plan.md](plan.md); details and
history in the feature Logs.

## Broken (cheapest first; [phone] = needs the phone)
- Camera black (no camera stack, B17).
- B26 [DONE 2026-10-30] Tap survey: Notes keyboard, Control Center, cover sheet, Settings panes (tap2-tap4).
- B34 [DONE 2026-10-30] `openapp`: 10/10 rc 0 with the right app in front (b33 run); the tap4 misses were while locked or busy.
- B36 Calculator froze the UI on kernel107 (sweetmotion=0x77, fake orientation events running) while plugged in, 2026-10-30; read the RAM-log rescue of that boot.
- B35 Privacy & Security has no Location Services row: CoreTelephony getSubscriptionInfo fails without CommCenter; the SweetMetal2 hook doesn't load in Preferences. Cosmetic; needs Preferences injection or a CommCenter stub (apps.md).
- B33 Home swipe: no injected bottom-edge swipe goes home (5 variants, b33); tests use `H` (home button hook: Calculator -> home confirmed, bt1) and `K` (lock); a real finger swipe works (U8, 2026-10-30): only the injected swipe fails.
- B29 Lock-screen flashlight/camera button icons and 5 Control Center tiles render as noise: icon-size backboardd buffer textures are shadows; forced shadow refresh (0108) no change. Parked 2026-10-29 (leads in apps.md).
- B8 PhotosFileProvider crashes: 0 in the kernel95 idle soak; confirm in a long app soak. (~2-4 per 2 h, uncaught ObjC exception in PlugInKit); low impact.
- B10 Reduce Transparency slow in app opens: RT on/off benches + one GPU trace. [phone]
- B11 UI watchdog misses a kernel hang with swaps still running.
- B12 Sensors: all behind the ADSP's SSC; wait for audio. Fallback: a sysctl that injects rotation events.
- B13 Battery % while discharging unchecked (U3).
- B14 Charging drain under load = the USB source's budget; optional read-only TYPEC/APSD check (U4).
- B15 Bluetooth: WCN3990 over a QUP UART + QCA firmware + a Broadcom shim for bluetoothd; 1-3 weeks.
- B16 Audio: see priority 1; then APR + CS35L41 for a beep (U5).
- B17 Camera frames (M2): camcc PLL for 24 MHz MCLK, CSIPHY/CSID/IFE, camera SMMU; weeks.
- B19 Respawn loops: lskdd, fairplayd.H2, wifip2pd dropped 2026-10-29; bluetoothd (10 s) kept until B15.
- B20 Screen rotation: see priority 2.
- B23 Any HLOS buffer the DSP touches resets the SoC: see priority 1.
- B30 Headphone jack (the user asked, 2026-10-29; not priority): WCD937x over SoundWire + MBHC; sound needs the ADSP audio path (B16/B23, audio.md M4). Jack-detect alone may work earlier (codec interrupt, no DSP).
- B31 oslog segfaults (rc 139) mostly in the first minutes after boot (b31: old 6/8, new 4/8 back to back right after boot, then 0/6 a few minutes later); _exit fix didn't cure it, no crash report written. Workaround: rerun. Next: a 20-run loop right after boot with the kernel fault line.
- B32 Location Services: switched on with `echo "L 1" > /var/local/sweet-orient` (2026-10-29); survives a reboot (tap1). No position source yet (no GPS driver; Wi-Fi location needs Apple servers).
- B18 Cellular: not planned. iCloud: not expected to work.

## Later (needs repeated 1 h+ tests)
- B21 GPU busy-hang recovery + pool growth (no real hang seen since xnu 0207).
- B4 Silent resets in long app soaks (PON reason is sticky, no lead).

## Optimize (last)
- O2 zero-copy scanout: works by hand (kernel103 `sctl kern.sweet_zc_scan=2`), no fps gain (zc5), auto mode (kernel104, sweetfb_zerocopy=2) garbled once in two boots; parked 2026-10-30 (smoothness.md).
- O3 Reduce Transparency speed (with B10).
- O7 ~21 fps limit = backboardd's GPU 25-30 ms per frame (blur passes) + CPU/GPU never overlap (research/notes/fps-limit.md): A76 bias (0147) and GPU clocks (565/650 MHz) were tried in Oct 23-24 with no gain; left: no-blur bench, 2 frames in flight (QuartzCore RE).
- O4 [DONE 2026-10-29] lskdd respawn cost: dropped.
- O5 display-off CPU fade when sweetpanelsleep is off.
- O6 failed GPU hang recovery takes 3.9-12.8 s: fail faster.
