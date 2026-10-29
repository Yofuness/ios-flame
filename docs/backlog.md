# Backlog (2026-10-29)

Not loaded automatically: read it when waiting on a test or build, after finishing a priority item (to pick the
next easy item), and when adding something newly broken. Priority items live in [plan.md](plan.md); details and
history in the feature Logs.

## Broken (cheapest first; [phone] = needs the phone)
- B24 Notes, Photos, Camera, Wallet black, Maps grey: not the mtl2-clients list (Notes/Photos never ask the bridge; listing all five stalled backboardd -> UIWDT kill). Next: Maps listed alone; for Notes, what draws its layer (CA remote layer / IOSurface errors in oslog). [phone]
- B25 Music SIGSEGV: MPMediaLibrary init says "app not authorized for access" -> nil library -> strlen(NULL). A TCC MediaLibrary row did not help; next: disassemble the authorization check (parked 2026-10-29).
- B26 Survey not done for tap-only paths: Settings sub-panes, Notes keyboard, Notification/Control Center. [phone]
- B28 AegirPoster crash loop at idle lock (~300-630 s, then stops): no Metal device. Listing it stops the loop but garbles the lock-screen clock (bridge texture/format issue); reverted. Next: what the clock texture needs (mtl2.log for AegirPoster, the A008 surface format 0x41303038).
- B29 Lock-screen flashlight/camera button icons render as noise: icon-size backboardd buffer textures are shadows; forced shadow refresh (0108) no change. Parked 2026-10-29 (leads in apps.md).
- B27 App Store stuck on "LOADING" (network/account; may need iCloud).
- B8 PhotosFileProvider crashes (~2-4 per 2 h, uncaught ObjC exception in PlugInKit); low impact.
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
- B18 Cellular: not planned. iCloud: not expected to work.

## Later (needs repeated 1 h+ tests)
- B21 GPU busy-hang recovery + pool growth (no real hang seen since xnu 0207).
- B4 Silent resets in long app soaks (PON reason is sticky, no lead).

## Optimize (last)
- O2 panel rate ~21 updates/s: only a second, preemptible GPU ring is left (big).
- O3 Reduce Transparency speed (with B10).
- O4 [DONE 2026-10-29] lskdd respawn cost: dropped.
- O5 display-off CPU fade when sweetpanelsleep is off.
- O6 failed GPU hang recovery takes 3.9-12.8 s: fail faster.
