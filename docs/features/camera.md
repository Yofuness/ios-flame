# Camera
**State:** 🟡 CAMERA.APP WORKS: live colour preview on BOTH cameras and saved HEIC photos (2026-10-30 cam35-cam37); auto-exposure (cam40), autofocus on open and on tap, sharp photos (af5-af7) work; Live Photo off by default (it filled mediaserverd until a kill); open: slow camera switch, front colour cast, preview CPU. iOS lists the Back Camera (16 formats) and the Front Camera (18 formats) (camera ADT + camd + SunfishAudio.driver camera code, cam26/cam32); Camera.app not tried yet (U12). The FRONT camera streams too (IMX355 1640x924 @30, cam31). The rear camera streams continuously into RAM at 60.1 fps (2-slot ring, `tools/sunfish-cam-rx.sh --stream N`) and takes complete raw photos (`--capture`), no resets; exposure/gain settable (camid); no phone-side ISP / iOS camera yet; **Updated:** 2026-10-31.

## Facts
Hardware (recon/live.dts):

| Item | Node / compatible | Facts |
|---|---|---|
| Rear sensor | `qcom,cam-sensor@0` (:12970) | CCI0 master 0, CSIPHY0, MCLK0 = GPIO13, reset GPIO43, camcc 0x50 at 24 MHz; actuator@0, eeprom@0, ois@0, flash |
| Front sensor | `qcom,cam-sensor@1` (:13014) | CCI0 master 1, CSIPHY2, MCLK2 = GPIO15, reset GPIO68, camcc 0x54 at 24 MHz |
| Sensor IDs | not in the DT | expected IMX363 (rear), IMX520 (front); addresses from Google's camx sensor modules or a CCI scan |
| Rear supplies | slg51000@75 on SE9 I²C 0xa8c000 (:9563) | vio ldo7 1.8 V, vana ldo3 2.85 V, vdig ldo1 2.85 V (check), custom1 ldo4 1.8 V, custom2 ldo6 1.05 V; vaf gpio-regulator "camera_ldo" 2.85 V (:22402) |
| Front supplies | same PMIC | vio ldo7 1.8 V, vana ldo2 2.8 V, vdig ldo5 1.2 V |
| CCI0 / CCI1 | `qcom,cci` @0xac4a000 / 0xac4b000 | IRQ 0x1cc, GPIO17-20, 37.5 MHz source |
| CSIPHY0-3 | `qcom,csiphy-v1.2` @0xace0000/2000/4000/6000 | lane data at :12686 |
| CSID0/1, VFE0/1 | `qcom,csid175_200` @0xacb3000, `qcom,vfe175_130` @0xacaf000 | Titan 175 |
| CSID-lite, VFE-lite | @0xacc8000, `qcom,vfe-lite175` @0xacc4000 | |
| Clocks, power, bus | `qcom,camcc-sdmmagpie` (:621), titan_top GDSC @0xad0c1c4, `qcom,cam-cpas` @0xac40000 | CAMNOC/AHB/AXI votes |
| SMMU | `qcom,msm-cam-smmu`, cb "ife" (:13144) | stream IDs 0x900/0xd00/0x880, mask 0x460 |

iOS side: the iPhone X userland (d22ap) uses AppleH10CamIn (research/ipsw-x/d22ap.adt: AppleH10CamIn, isp0, isp0-config); the capture plugin is /System/Library/MediaCapture/H10ISP.mediacapture (IOConnect to AppleH10CamIn + the ISP firmware protocol; needs the ADT camera-rear/front calibration). The live log has no CamIn or cameracaptured lines: nothing has tried to open a camera yet.

## Plan
Path: raw Bayer through RDI (CSIPHY -> CSID -> IFE RDI write master -> memory), with register sequences from mainline camss (csiphy-3ph-1-0, csid-gen2, vfe-170 RDI; VFE 175 is close to sdm845's vfe-170), not the downstream CAM-ISP stack. Skip ICP/IPE/BPS/JPEG.
1. **M1 (1-1.5 weeks): read the rear chip ID over CCI** (IMX363 0x0016 = 0x0363). xnu: slg51000 regulator driver over SE9 I²C (DT voltages exactly, read back before enable), camcc + titan_top GDSC, CCI master, MCLK pinmux, reset GPIO, a probe sysctl. Power order: vio, vana/vdig, MCLK, reset high; never above the DT maximums.
2. **M2 (3-5 weeks): one raw frame to the PC.** IMX363 init tables (from the camx sensor module bins in the backed-up vendor image; out of git), CSIPHY0 lanes, CSID RDI0, VFE RDI write master into a static buffer, SMMU context bank, cpas vote. Risks: SMMU faults and bus hangs.
3. **M3 (1-2 weeks): a debayered preview** in a test app (IOSurface + Metal), without AVFoundation.
4. **M4 (4-8+ weeks, uncertain): the Camera app.** A fake AppleH10CamIn user client and an ISP-firmware-protocol stub for H10ISP.mediacapture.

## Open
- Does ABL leave camcc on? (read CAMCC / GDSC registers read-only first)
- Find the sensor module bins in backup/ (vendor image).
- Disassemble H10ISP.mediacapture (one image, 8 GB scope) and count its IOConnect selectors.

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

### 2026-10-30 23:45 mem1/lp1: LIVE PHOTO was filling mediaserverd; off by default now: 4 of 4 shutter presses save
- mem1 (kernel128, footprint tool): Camera closed/background/screen off: mediaserverd rss 38-43 MB, flat. Rear preview with Live Photo on: rss 1.7 GB, footprint 33 GB (MALLOC_LARGE, 937 regions, ~35 MB each), killed ~33 s after the stream starts ("low swap ... size 49 GB"). Kills per boot today: 155135: 2, 161539: 2, 164250: 1, 170558: 7, 173029: 2, 173818: 6, 175437: 11, 183748: 7.
- lp1: tap on the Live Photo button: footprint 14 GB -> 136 MB at once, then flat (MALLOC_LARGE 68-94 MB, 36 s). Shutter x4, 12 s apart: IMG_0008..0011 all saved, 0 new kills. tools/sunfish-camera-prefs.sh writes com.apple.camera.plist: CAMUserPreferenceDesiredIrisMode 0, CAMUserPreferencePreserveLivePhoto true (backup .bak-lp, cfprefsd restarted). After a relaunch: footprint 136-159 MB, button crossed out, preview sharp (work/shots/lp1-s.png).
- What leaks in Live Photo mode is not known (Apple's Iris path + our software HEVC encoder inject is a guess; the allocations are not ours). Live Photo itself stays unsupported (O15).
- CPU: mediaserverd still takes ~5 cores while previewing (64 CPU-s per 13 s, probably our software ISP; not profiled): a likely battery cost (O14).

### 2026-10-30 23:15 B48 analysis (work/af7/af7a.log): the kernel keeps killing mediaserverd (low swap); a stale START then blocks recovery
- af7a.log: 14.991 photo made; 17.720 "low swap: killing largest compressed process with pid 571 (mediaserverd) and size 49502 MB" (jetsam low-swap, vm_compressor.c:619 -> kern_proc.c:4469/4521); Camera gets AVAudioSessionMediaServicesWereLost, 20.623 session runtime error -11819, "startRunning succeeded (took 9.006 seconds)", 31.083 "START -> 0 after 12058 ms", "opened after stop: closed at once". No stream-start timeout. Kills recur in recent boots (20261030-175437: 11, about every 53 s; 183748: 6; first in 155135, the show1 boot).
- Second bug: a stale worker sits in camera_client_open (camera_client.c:24, :198) holding device_owner (camera_device.c:2185), so the wanted worker waits; stream_stop blocks 1.5 s per flip (:2344). Fix idea: START wait abortable (poll worker_wanted).
- Open: what fills mediaserverd's compressed ledger (~0.9 GB/s of something highly compressible, or an accounting bug). mem1 samples it.

### 2026-10-30 23:00 af6/af7: focus on open works; first shutter after reopening can be ignored (port 4e3e807)
- af6 (kernel128, camera_af.h: sweep after AE is stable, lock, per-session reset, 1 s photo timeout): no tap: F 400, 440, 481, 562, 643, 725, settles 714 (focus 991); sharp ~25 s after openapp (+9 s blurry, +16 s soft); locked, no F lines for 20+ s. After front -> rear: black ~10 s, soft at +20 s, sharp at +30 s, sweep ran again. Front: pale blue-lilac, bright, not clipped. 5 camclient sessions in a row rc 0. DEVLOST 0, FAULT 0.
- Photo: af6's two shutter taps saved nothing, no "a photograph asked for". af7: A (openapp, 35 s, no tap): nothing; B (tap focus, 6 s, shutter): IMG_0005.HEIC; C (openapp again, 35 s, no tap): IMG_0006.HEIC, StillImage in 381 ms. So a tap isn't needed; AF isn't the cause. A's log has "Viewfinder still closed N seconds after ViewfinderClosed (CameraStopped, PreviewStopped, RuntimeError)", "stream: its worker is still in the kernel: left to finish on its own"; B/C don't. Cause open (restart race? guess); backlog B48. Logs: work/af7/.

### 2026-10-30 22:30 af5: TAP-TO-FOCUS AND SHARP PHOTOS IN CAMERA.APP (kernel128)
- Tap on the board: yellow AF box; F 704, 684, 715, 664, 719 (hunts, settles ~719); preview sharp. Shutter: IMG_0004.HEIC (1164081 B) in ~2 s, no stall; work/cam/af5-s.jpg: every board marking readable, colours right. Exposure and focus by the app itself.
- Open: continuous AF sweeps F 420, 481, 542, 623, 643, 725 but the preview is soft 15 s later. After front -> rear: F 420, 405, soft, no search. Being tuned. DEVLOST 0, no FAULT.

### 2026-10-30 22:00 af3/af4: THE LENS FOCUSES under software control (kernel128)
- af3 sweep looked flat: dark frames, and most FOCUS commands were lost (a FOCUS sent while the previous session was stopping is cleared at its end; only 3 reached camid). Rerun with sunfish-sharpness2.py (8x8 block mean of high bytes) and -e 6000 448.
- af4, board ~10 cm: centre gradient energy per code: 400: 15.2, 440: 18.4, 481: 22.0, 562: 32.1, 643: 52.5, 725: 71.0 (the sessions for 525, 608, 689 didn't move: those FOCUS were lost, energy = 400's). Higher code = nearer; the peak is at or past the 725 limit for a 10 cm subject. work/cam/af4-best.png: "CS 100 16V" on the capacitors, crystal marking, pins, "V1.024" readable.
- Still open: Camera.app's own AF (af5); 725 may not be the true macro end (the per-unit calibration in the OIS flash isn't read).

### 2026-10-30 21:30 af2: the lens moves (read-back OK); found + fixed: every camera session leaked 2 SMMU banks
- `camid --lens-move 560 / 725 / 400` (kernel128, port 6aeb4ee): "0xB3 = 00 after 30.1 / 26.0 / 49.6 ms", read back 0x84 after init 0, at target and after 2 s = the code written, "parked at 0", camvaf 0. No lens errors.
- Then camd FAULT: "smmu: no dedicated mapping (conflict 0, free SMRs 8 of 4, banks 1 of 2)". Cause: cb_restore's write of CBAR back to 0 doesn't stick (reads 0x0001f000), so the free-bank scan skipped every bank we had used: sessions marched CB54+55, 56+57, 58+59, then none left. So the camera stopped after a number of opens per boot (and may explain earlier FAULTs). Fix: a bank with CBAR == CAM_CBAR, SCTLR.M off and no SMR on it is reusable. Verified: 4 sessions in a row rc 0, all on CB4 + CB14.
- camd.c still latches FAULT on a setup failure (safe; the reason was this leak).

### 2026-10-30 21:00 af1: the lens chips answer (kernel128, xnu 0247 kern.sweet_camvaf)
- `camid --lens-probe` (read-only): "AF 0x72 reg 0xB3: ACK, 00" (raw e5 00 00 00), "OIS 0x3e reg 0xF100: ACK, 00 00 00 00". Kernel: "SWEET CAMVAF: 0xcb00 TYPE 0x10 SUBTYPE 0x10, gpio1 0xc000 TYPE 0x10 SUBTYPE 0x10", before 40 00 41 00 42 04 44 00 45 01 46 80, on, off 18 ms later. So the VAF rail and address are right. Camera preview fine afterwards. 4455 mV. Next: init + one small move.

### 2026-10-30 20:30 Autofocus plan (static decode, research/notes/camera-af.md; nothing run yet)
- AF chip LC898219XI at 7-bit 0x72 on CCI0 m0; position = 12-bit big-endian write to register 0x84; init E0=01, poll B3==00, 8A=A5, 89=3F, 87=1D, 84=0000; range codes 400..725 (inferred). OIS/EEPROM chip LC898123F40 at 0x3e stays off. VAF rail = PM6150L GPIO12 (0xcb00, inferred): needs a kernel sysctl (0247). Steps: read-only probe -> init -> one small move -> camid F + camd FOCUS -> AF tuning. Hard limits: 400..725, max 20 codes per write, >= 2 ms apart; no writes to 0x3e.

### 2026-10-30 20:15 cam38-cam40: AUTO-EXPOSURE WORKS; camera switch 16 s -> 6-8 s (kernel127)
- cam38 (port ace0d24): headers change 3 frames after the write (read-back pairing ran AE away), AE meters the shaded picture, frame length extends down to 15 fps (0x0340 in camid's group hold), one E in flight and no repeats, SIGTERM 2 s after the loop. Host test test_ae settles rear/front dark/bright within 13-23 frames. The 3-frame latency and front line time are not measured.
- cam39: target read 767 (should be 144): Camera.app sends ExposureBias as integer 256; camera_device read it as +256 stops, clamped to +8. Pictures overexposed. START: rear 3897 ms, to front 6265 ms, back to rear 8490 ms.
- cam40 (port 96b6ecb, integer bias = 8.8 linear, 256 = no change): rear AE mean 37 -> 98 -> 127 of target 144, 33.3 ms, gain 6.5-7.1x; rear preview and IMG_0003.HEIC normally exposed, colours right, out of focus (no autofocus). Front: START 7879 ms, then ~10 s more until the picture shows; AE mean 118-134 of 144, 33.3 ms, gain 1.8-2.1x; pale blue/lavender cast (front white balance untuned). Back to rear START 6873 ms. DEVLOST 0.

### 2026-10-30 19:30 cam37: THE FRONT CAMERA WORKS IN CAMERA.APP; it takes ~16 s to switch, exposure is off
- kernel127. Rear first (START 0 after 3936 ms), tap on the switch button: "the Back Camera's frames stop for the Front Camera's", "START -> 0 after 16068 ms", "frames of 1640x924 (the Front Camera, 30.0 fps mode)", imx355 tuning loaded, "PrimaryScaler: the first picture given". camd.log: rear "session end: ok, 97 frames published, 1 dropped"; front "+4.231 s first frame published (exposure 2604 lines)". Preview (work/shots/cam37-front.png): ceiling and the iPhone 11 at the bottom, bright, blown out. Earlier "black" = the tests and the user gave up before 16 s. Back to rear works. DEVLOST 0.
- AE: front "frame 4: exposure 33.2 ms, gain 1.0x, green 44 above black" with camd gain code 512 -> 636 (rising on a white picture); rear "exposure 16.5 ms, gain 1.0x, green 4 above black", camd stuck at 1568 lines, gain code 448, preview almost black. Fix in progress (cam38).

### 2026-10-30 17:30 cam36: PHOTOS WORK (rear); front through camd works, Camera.app front preview black (kernel127, port 4009f97)
- camd no longer latches a false FAULT: `camclient -s 1 -n 2` rc 0 then `camclient -s 2 -n 2 -o` rc 0 (front 1640x924 stride 2064, START after 8.39 s, 2 frames, 0 torn); camd.log "session end: ok" for both, "past the ring: 0 nonzero". Front raw is dim (gray 15..17).
- Photo: camdev.log "a photograph asked for", "StillImage: 4032x3024 made of every pixel in 450 ms", "the photograph is given"; oslog "quality prioritization 2 taken as 1 (speed)", "a photograph of 4032x3024 encoded in 3018 ms". File /var/mobile/Media/DCIM/100APPLE/IMG_0001.HEIC (902569 B, 3024x4032 per heif-convert on the host): CLEAN picture of the red board and the gold letters, correct colours, dark, blurry (no autofocus, subject close). Tile corruption is only in the Photos.app display (B44). (Earlier cam35 "no photo" = camd in FAULT.)
- Camera.app front: black preview, no FAULT; camdev.log ends "the graph starts the stream (format 0)"; back to rear works without killing camd. Rear preview dark/green in this run (low light, exposure not adapting yet).

### 2026-10-30 17:00 cam35: CAMERA.APP LIVE PREVIEW WORKS (rear, kernel127, port 5af97b4)
- camd/camrx/camclient/camisp replaced in /var/local/bin (5af97b4 touched only comments/constants in camera_device.c + sunfish_camera.h, so SunfishAudio.driver kept), camd restarted by launchd. Camera.app: a live full-colour picture of the red circuit board (and the printed letters behind it), work/shots/cam35-rear.png, webcam agrees. Horizontal bands in the screenshot also cut the UI text and shutter button, so they are flame-shot reading during an update, not the camera.
- Front: switching in Camera.app -> "START -> -60 after 30065 ms (camd pid 379, state 4)", "frames stopped (0)", camd.log "session end: FAULT (no more starts), 2 frames published", then every START -> -5 until camd is killed (kill; launchd restarts; rear comes back). tools/sunfish-cam-rx.sh --front --capture works, so the difference is in camd's front path.
- Photo: the shutter tap saved nothing (Photos: "No Photos or Videos"); camdev.log "frames begin (format 0)", "streams given back (2)". Cause open.

### 2026-10-30 16:40 cam34: BOTH CAMERAS GIVE REAL PICTURES; the static is a wrong row stride (lines are 16-byte aligned)
- kernel127, daylight. Rear (camclient -n 2 -o; camd had been in FAULT after the Camera.app session: killed, launchd restarted it, START 0 after 4.35 s): with stride 2528 the frame shows the red circuit board right in front of the lens (blurry); with our stride 2520 it is diagonal stripes = Camera.app's static. Front (tools/sunfish-cam-rx.sh --front --capture, 1894200 B): with stride 2064 (data 2050 B) it shows the ceiling and the user's iPhone 11 with its flash dot; green cast (no white balance). Shots: work/cam/cam34-rear-s2528.png, cam34-front-view.png.
- Stride measured by the minimum vertical row difference of the high bytes: rear 2520 -> 37.9, 2528 -> 0.49; front 2050 -> ~20, 2064 -> 4.96. Rule: stride = align16(width * 10 / 8).
- Rear raw: high-byte mean 16.5 (black ~16), max 24: dark scene at 1592 lines, gain 1, 16.5 ms. Two frames 1 s apart nearly identical (mean abs diff 0.16), 0 torn.
- Camera.app (20 s): "START -> 0 after 3543 ms", 2016x1512 60.1 fps, PrimaryScaler 1504x1128 420f, "frame 6: exposure 16.5 ms, gain 1.0x, ... lux 150, 5351 K"; preview = green/cyan diagonal streaks (same shear). The front camera shows nothing in Camera.app: camd has no front path yet.
- Fix in progress: stride helper everywhere + camd copies height*stride.

### 2026-10-30 16:30 Camera static, read-only analysis: nobody has looked at a picture from this camera yet
- Path (code): sensor 2016x1512 MIPI RAW10 packed, stride 2520 -> unpack camera_isp.c:853-862 -> camd copies frames from the carveout via /dev/pmem 64 KB at a time (camd.c:265) into a shared slot -> camera_client copies out (camera_client.c:256) -> camera_device announces 420f/420v/BGRA, render_into fills NV12 with CoreVideo's per-plane bytesPerRow (camera_device.c:1292-1298, 1453) -> backboardd planarFill converts 420f to RGBA (NativeBridgeMetalFrontend.mm:1964-2018). Formats agree on paper.
- Every raw2png/camisp output since cam16 is logged "not viewed"; only mean, zero blocks and md5 were checked. Ranked: (1) the raw data is noise/stale (cam19 comb histogram, "exposure 0.0 ms", /dev/pmem reads of the DMA carveout without cache maintenance, RGGB/black level guessed); (2) backboardd reads the IOSurface planes wrongly (plane bytesPerRow, xnu 0235 offset); (3) torn slots (would give bands, least likely).
- Next (when the phone is back on USB, daylight): dump one camd frame, raw2png on the host and LOOK at it; then dump plane 0 of one CVPixelBuffer after render_into. Notes: research/notes/camera-static.md.

### 2026-10-30 16:05 U12 (part): Camera.app shows static (the user, kernel127)
- Camera opens and shows a moving static/noise picture: frames reach the preview, the pixel data or format is wrong. Pending: front camera, a photo in Photos.

### 2026-10-30 12:30 B43: camera start 11 s -> 0.4 s, exposure starts from the real value (2e4f7cc)
- research/notes/camera-start.md: 15 stage() sleeps of 300 ms (crash-bisect aid) = 4.5 s; AE started from cur_lines -1 -> 0 -> the 4-line floor (0.04 ms, "exposure 0.0 ms").
- 2e4f7cc: CAMRX_FAST=1 (set by camd) skips the stage sleeps and zeroes the carveout only once per camd process; camd.log lines carry "+S.mmm s" since START; cur_lines starts at the table's 1592 lines; isp_ae makes no request while the exposure is unknown; START queues 1500 lines at 2x gain.
- On the phone (kernel121, by hand): two `camclient -n 30` runs: "first frame published" at +0.425 s and +0.379 s after START (was ~11 s); "camd: sensor exposure set: frame_length 1608, exposure 1500 lines, gain code 256" right after; 31 frames each, 0 torn; whole session incl. power-down 4.27 s. The zeroing took 0.003 s (not the cost).
- camd exited 0 on SIGTERM, so launchd (KeepAlive SuccessfulExit false) never restarted it after a kill: now exits 1 (installed; takes effect for the launchd job from the next boot).

### 2026-10-30 12:00 Camera.app on kernel121 (sv1): frames flow, but the start takes 11 s and the first frames are unexposed
- camdev.log: "open: START -> 0 after 11220 ms (camd pid 42, state 2)", "frames of 2016x1512 from the kernel (the Back Camera, 60.1 fps mode)", colour tuning loaded, PrimaryScaler 4032x3024 420f and SecondaryScaler 1504x1128 420f pictures given; "frame 5: exposure 0.0 ms, gain 1.0x, digital 1.00x, green 40 above black ... lux index -494"; stopped after 4 frames / 3 pictures (the survey went home at 5 s, before the preview could show). camd: 15 frames published, 0 dropped.
- So the "black preview" in the survey was the 11 s bring-up. Open: start time (camd bring-up ~11 s: Optimize), exposure 0.0 ms at the start (AE start values), Magnifier the same.

### 2026-10-30 11:40 B40 pre-checks (read-only)
- c26ce1c StartRecorderServer: have (sunfish-install-camera.sh:63, same plists/key; sweet-root-tweaks.py:120 undoes it on a root rebuild).
- 51f6f0b ADT nodes: have, superset (mkadt.py:86-91 adds camera-front + flash; iOSPlus10 only camera-rear + empty /product/camera).
- 3b0eca8 sandbox ext ops 5/6: have (xnu 0219); no TCC.db/entitlement change needed by their fix.
- 86a5df1 plane bpr +0x10: have (xnu 0235); our plug-in uses CVPixelBuffer plane APIs, no hard-coded offsets; their bpr<width frame skip optional.
- hakios 0021 select-before-read: have for front (camera_client.c:194); back sends no SENSOR, the read loop switches (camera_device.c:2090). Notes: research/notes/camera-precheck.md.

### 2026-10-30 06:55 Plug-in camera opens work again (2ed9d22 on the phone, boot sp16r)
- After a mediaserverd restart: camdev.log "open: START -> 0 after 11713 ms (camd pid 42, state 2)" and "START -> 0 after 18286 ms"; the earlier "START -> -22 after 30 s" is gone. (The opens are iOS prewarm: "opened after stop: closed at once".) U12 (Camera.app by the user) is unblocked.

### 2026-10-30 06:45 Plug-in camera open stalled camd: SENSOR fields in the wrong order (fixed in 2ed9d22, not yet on the phone)
- Seen (kernel112 CAMERA=1, after mediaserverd restarts in jack8): camdev.log "open: START -> -22 after 30064 ms (camd pid 42, state 0)" twice; camd.log only "client fd 6 connected/gone". camd alone works: `camclient -n 30` START 0 after 10.80 s, 30 frames, 14.19 fps read, 0 torn, STOP 0.
- Cause (investigator, code reading): camera_client.c built SENSOR as { SENSOR, sensor, ~0, ~0, 0, 0 }, i.e. the sensor in `bytes`; camd skipped 1-2 bytes of the following START as payload, replied -EINVAL to a = ~0, then blocked in read_full() for the rest of START, freezing its whole loop until the client closed (30 s). The -22 logged was SENSOR's reply left in `r`.
- Fix 2ed9d22: { SENSOR, 0, sensor, ~0, ~0, 0 }; non-START replies are read into a scratch reply (a SENSOR refusal is logged). Hardening left open: camd trusts `bytes` and reads blocking, so one bad client can still freeze it.
- Next: install the driver after sp16, restart mediaserverd, expect "open: START -> 0" in /var/local/camdev.log; then U12 can go ahead.

### 2026-10-30 04:30 O8: camera_isp renders split into row bands (9e8aa03): 8x faster on the phone, identical output
- isp_render_nv12 / _bgra / _full_nv12 / _still_nv12: a whole-frame call is cut into isp_threads bands (dispatch_apply_f on iOS, pthreads on the host); partial-row calls stay single-threaded (camera_device.c's render_into already splits frames 8 ways with dispatch_apply_f in mediaserverd, so the live preview was already parallel; camisp's 83 ms was single-threaded). isp_measure stays single-threaded (float sums). isp_threads 0/1 = old path; default ISP_THREADS env or hw.ncpu capped at 8.
- Phone (camisp on cam21.raw, 8 threads): binned 1008x756 84.79 -> **10.61 ms/frame**; full 2016x1512 NV12 371.75 -> **47.12 ms**; md5 of 1- vs 8-thread output identical (78396d4a...). Host: 13.3 -> 2.53 ms, 35.2 -> 8.87 ms, all byte-identical.
- SunfishAudio.driver rebuilt with it but not reinstalled (mediaserverd's path doesn't use the whole-frame split).

### 2026-10-30 04:00 Colour tuning files installed (research/notes/camera-tuning.md)
- ios-redfin's tools/redfin-camtune.py works as-is on our com.google.tuned.s5_imx363.bin / s5_imx355.bin (vendor.img /lib64/camera): imx363 10 colour matrices + 83 lens-shading grids (~1.0 centre, 3.1-3.8 corners) -> work/cam/camera-imx363.isp / camera-imx355.isp (21572 B each, RWCISP02, 1 light x 6 entries: no CCT triggers found, so the ISP uses file order; no AWB reference lights; imx355's first matrix equals imx363's: check). Installed to /private/var/local/fw/ (camera_isp.c isp_load_tuning); tools/sunfish-install-camera.sh now makes + puts them and the front table.
- Untested: whether the "no colour tuning" log line goes away and colours improve: with U12 (remove the .isp files if colours look worse).

### 2026-10-31 08:30 cam33: Camera.app preview support installed (Metal bridge 615c313); UI unchanged
- `tools/sunfish-metal2-install.sh work/metal2-linux/out/phone/metal2/out` (now keeps the old bundle as SweetMetal2.bundle.prev; `--undo` swaps it back, b480234): backboardd restarted (pid 61 -> 366), lock screen and home screen render as before (B29 noise only), backboardd crash reports 5 before / 5 after, 2 min stable. "planar iosurface refused": 0 (no planar surface yet; Camera.app not opened).
- Ready for U12 (Camera.app, both cameras): kernel111 CAMERA=1, camera setup installed, camd idle, preview textures in.
- Correction (times): the "2026-10-31 HH:MM" headings of tonight's camera entries (from "00:15 cam13" to this one) are estimates that ran ahead of the clock; the PC clock says these runs happened between about 22:00 on 2026-10-29 and 01:35 on 2026-10-30 EDT. Run tags and order are right.

### 2026-10-31 08:15 cam32 (kernel111 CAMERA=1): iOS LISTS BOTH CAMERAS; camd idle after the boot prewarm (1d93eee)
- camera_device.c front port restored (redfin's, half-turned; centre-crop of 1640x924 to the plist's 4:3 photo / 16:9 video sizes; SENSOR a=2 before START); camd race fixed (front chosen before the session thread; it would have stopped every front session). Off switches: /var/local/camdev.nofront (no front port), CAMD_NO_FRONT=1 (camd refuses SENSOR 2).
- Battery 4439 mV, lock screen at ~55 s. camd by launchd (pid 42), idle (0.06-0.08 s CPU, no camid). camdev.log: "the device made: 2 streams, the back camera, the front one", the prewarm (back then front), socket not yet up, "streams given back (2)", "reopen skipped", "stop (0 frames)". **camera_avtest: Back Camera (built-in_video:0, position 1, 16 formats) + Front Camera (built-in_video:1, position 2, 18 formats).** 2 min stable, no reset.

### 2026-10-31 07:55 cam31 (kernel111 CAMERA=1): FRONT CAMERA STREAMS: the shared csiphy0 clock was missing (c5a24d6)
- Cause (research/notes/camera-front-csiphy2.md): live.dts:12730-12749 lists csiphy0_clk among CSIPHY2's (and 1's, 3's) clocks; downstream enables it for every PHY. camrx's front path now runs the csiphy0 branch 0xad06020 too (read back 0x1) and turns it off after.
- `sunfish-cam-rx.sh --front --stream 5`: settle 24, sensor frame_count 30..181, **151 SOF / 150 done, 30.00 fps, CSID bytes 1894200/1894200 (one 1640x924 RAW10 frame)**, 0 faults, GFSR 0, SMMU restored, powered down. `--front --capture`: 1894200 B, 0 zero 1 KB blocks, mean byte 18.1; raw2png 1640x924 gray 0..195 (files in work/cam/cam31-front*, not viewed). The script exited 2 after the capture (a missing file-name argument; the capture itself finished): to fix.
- Rear regression: 60.00 fps, 3810240/3810240, 0 faults. camd restored and idle. No reset.
- Next: camd SENSOR 2 (front) through camera_device (front camera in Camera.app: ports[1] is NULL in our camera_device.c; redfin turned it half round).

### 2026-10-31 07:30 cam29 + cam30 (kernel111 CAMERA=1): front CSIPHY2 sees lane activity, CSID-lite still 0 packets with every PHY select
- cam29 (manual front sequence): sensor frame_count 30/60/90 at 1 s steps; `rx report` RX_PKTS 0, ECC 0, CRC 0, bytes 0/0, top/RX/RDI0 IRQ 0. CSIPHY2 status 0x8b0..0x8d8 after the stream: settle 24 `00 01 00 20 00 00 04 00 80 40 00` (= the rear's pattern after a working stream, cam8), settle 14 and 40 `04 01 80 20 00 10 04 00 82 40 04`. So the PHY sees the lanes; nothing reaches CSID-lite.
- cam30 (camrx afd3bfe, CAMRX_PHY_SEL): RX_CFG0 PHY_NUM_SEL 3, 1, 0, 2 -> RX_PKTS 0 every time (sensor streaming each time). Cleanup OK (GDSC off), camd restored and idle. No reset.
- Open: what else routes CSIPHY2 to a CSID (a combo/C-PHY mode bit in CSIPHY2, its own lane-enable/positions, a different CSIPHY table for this PHY, or CSID-lite only fed from PHY0/1). Read-only investigation queued; front camera parked behind U12 / the rear Camera.app.

### 2026-10-31 07:05 cam28 (kernel111 CAMERA=1): front IMX355 streams, but CSIPHY2 -> CSID-lite gets 0 bytes; rear regression OK (a22e43f)
- Front path: module file com.qti.sensormodule.primax_imx355.bin from ../restore/factory/vendor.img (debugfs; research/notes/camx/, not in git); modes (4-lane RAW10): 0/3 3280x2464@30 688 Mb/s/lane, 1 3280x1848@30 720, 2 1640x924@30 (2x2) 360 -> mode 2 (1894200 B, stride 2050). CSIPHY2 0xace4000 (DT cam-sensor@1: cci-master 1, csiphy-sd-index 2), csi2phytimer RCG 0xad0604c 300 MHz, branches 0xad06064/0xad06068 on; settle 24; RX_CFG0 0x00232103 (4 lanes, map 0x3210, PHY_NUM_SEL 2 as Linux csid-gen2 does). laneAssign in both module files = 0x3210, isComboMode 0.
- camid --front: ID 35 03 55 55, 103 writes, readback 0x0136 0x18 (24 MHz), 0x0112 0x0a, 0x0114 0x03, stream on, **frame_count 30/60/90/.../181 at 1 s steps (sensor streams 30 fps)**; but CSID bytes 0/0, 0 SOF, CSIPHY2 status after phy on all 0. No faults, no reset. Rear regression (`--stream 5`): 60.00 fps, 0 faults.
- Next (cam29): manual front sequence with rx report (CSIPHY2 status + RX IRQ during the stream) and settle 14/40 overrides.

### 2026-10-31 06:45 Camera preview support in the Metal bridge built, not installed (615c313)
- NativeBridgeMetalFrontend.mm (0112): newTextureWithDescriptor:iosurface:plane: no longer refuses planar IOSurfaces for MTLPixelFormatYCBCR8_420_2P(_sRGB) 500/501 on 420v/420f (CPU BT.601 conversion into an RGBA8 copy; video/full range from the fourcc) and RGB10A8_2P_XR10 550/551 (XR10 colour + plane-1 alpha into BGRA8); refreshed on bind when the IOSurface seed changes; other planar kinds refused and logged once. Off: `M2V_PLANAR=0` in backboardd's env. After ios-redfin (MIT).
- Risks: CPU cost in backboardd's encode thread (~5-15 ms per 1080p refresh), possible tearing (replaceRegion while in flight), double colour conversion if CA's shader expects YCbCr. Install by day with the Camera.app test (U12): tools/build-metal2-linux.sh done; `tools/sunfish-metal2-install.sh work/metal2-linux/out/phone/metal2/out --no-restart` then reboot.

### 2026-10-31 06:20 cam27 (kernel111, CAMERA=1): the worker fix holds, no unwanted stream; camd enabled + idle
- 7de5516 + ebf31dd installed (SunfishAudio.driver), camd re-enabled. Battery 4455 mV, boot OK, lock screen at +60 s.
- camdev.log: the boot prewarm "stream: frames begin (format 0, a photo mode)" -> socket not there yet -> "device: streams given back (1)" -> **"reopen skipped: stream given back (1)"** -> "stream: stop (0 frames, 0 pictures given)". camd.log: only its start; no START/STOP; no camid child. camd CPU 0.06 / 0.11 / 0.13 / 0.19 s over ~4 min (idle polling). camera_avtest: "Back Camera", 16 formats; still idle after it. 2 min stable.
- State left for the user's Camera.app test (U12): kernel111 CAMERA=1 boot, camera setup installed, camd enabled and idle.
- The lock-screen clock digits turned to noise again later in this boot with the camera never streaming: B29, not the camera.

### 2026-10-31 05:55 cam26 (kernel111, CAMERA=1 ADT): iOS LISTS THE BACK CAMERA; a stream kept running unwanted (fixed by hand, bug being fixed)
- `tools/sunfish-install-camera.sh` (ffe92d6): 7 tools + table; launchd job org.iosonpixel.camd (backup launchd.plist.pre-camera); camdev.on + camdev.log 0666; StartRecorderServer = true in MediaToolbox D22/D221/Default (backups .before-camera); MobileGestalt cache moved aside (copy .pre-camera). Battery 4440 mV. `CAMERA=1 tools/sunfish-boot.sh cam26 work/kernel111/... sweetse9=2 sweetcampmic=2`: SSH ~10 s, **SpringBoard up at ~60 s** (no boot-logo hang), lock screen at +60/+120/+240 s; no new mediaserverd crash.
- camdev.log: "plug-in loaded in mediaserverd (pid 29)" (at boot), "gate: camera on", swizzles replaced, "the camera's device vendor made", "the device made: 1 streams". **camera_avtest: 1 video device "Back Camera" (built-in_video:0), position 1 (back), 16 formats (420v/420f, 960x540 .. 4032x3024, up to 60 fps), active 1920x1080.** Front camera not listed (by design).
- **Unwanted stream**: at boot mediaserverd asked for the stream (prewarm, "frames begin (format 0, a photo mode)"); camd's socket wasn't up yet, the worker started reopening with backoff; "streams given back" came but the worker kept reopening, connected after 11.6 s and streamed 60 fps for minutes with no client (7034 frames; AE ran: exposure 16.2 ms, gain 1.0x, ~5400 K, lux index 149). Nothing displayed or saved. Stopped by hand: /var/local/bin/camd renamed camd.disabled (launchd can't restart it), SIGTERM to camd + its camid child: "powered down", GDSC collapsed. Fix being written (worker stops reopening once the stream is stopped/given back; a late START is closed at once).
- Phone left: kernel111 CAMERA=1 boot, camera setup installed, camd disabled (renamed), camera powered off; mediaserverd's camera code retries the socket every 5 s (harmless).

### 2026-10-31 05:35 Camera.app prerequisites built (43bff35): kernel111, mkadt --camera, camera_avtest
- xnu 0234 (redfin 0925: DisplayPipePlaneBaseAlignment = {PlaneBaseAlignmentLinear = 64}; `sweetclcdalign=0` = old number) + 0235 (redfin 0927: plane bytes per row also at +0x10 of the IOSurface lock result; `sweetplanebpr=0` off). kernel111 = kernel110 + 0234 + 0235 (37 s).
- tools/mkadt.py `--camera`: /arm-io/isp (camera-rear 1, camera-front 1) + /product/camera flash 1; boot with `CAMERA=1 tools/sunfish-boot.sh ...` (work/boot/sunfish-hi-camera.adt), `ADT=PATH` for any tree; default unchanged.
- Our inject_media.c already has the portrait-prewarm no-op and the filter-thumbnail fix. camera_avtest (lists AVCaptureDevices + formats only, no session) built.
- Caution from redfin (r15a/b): camera ADT flags + a rebuilt MobileGestalt cache without a working capture device at mediaserverd start = SpringBoard stuck at the boot logo. So camd (launchd), the gate, StartRecorderServer and the MG cache move go in together: tools/sunfish-install-camera.sh (with --undo/--check) being written.

### 2026-10-31 05:20 Camera.app prerequisites gap check (read-only, research/notes/camera-stage-f-gaps.md)
- Have: 0924's tccd camera sandbox extension (our 0219). Missing: (1) ADT /arm-io/isp + /product/camera nodes (redfin mkadt.py --camera) + a MobileGestalt cache made on them; (2) MediaToolbox SubServerConfiguration.plist StartRecorderServer = true (tools/sweet-root-tweaks.py sets it false); (4) 0925 DisplayPipePlaneBaseAlignment dictionary; (5) 0927 IOSurface plane bytes-per-row at +0x10; (6) SweetMetal2 YCbCr 420_2P / RGB10A8_2P textures for the preview (1-2 d); small: front camera rotation, filter-thumbnail hook, alerts, Live Photo off. Order: 0925+0927 -> ADT camera -> StartRecorderServer -> driver hooks -> SweetMetal2 YCbCr.
- Being written: 0234/0235 (kernel111), mkadt --camera, camera_avtest (list devices only).

### 2026-10-31 05:05 cam25 (kernel110): camera code in SunfishAudio.driver installed; gate off/on harmless; Camera.app not opened yet
- `phone/audio/build.sh` + `tools/sunfish-install-audio.sh` (41a11d4: camera_device.c + inject_media.c over camera_client.c, gate /var/local/camdev.on). Gate off: mediaserverd restarted (pid 29 -> 471), no crash report in 80 s. Gate on (camd running, mediaserverd restarted 471 -> 490): running, no crash report.
- **/var/local/camdev.log (0666) stayed empty in both cases**: no "gate:" line, so there's no sign the plug-in's camera constructor ran. mediaserverd runs as uid 501 (psx), so permissions aren't it. Guess: the HAL plug-in (and so inject_media's constructor) loads only when CoreAudio first initialises in mediaserverd, not at process start; or the constructor isn't reached. To check by day: open Camera.app with the gate on and read camdev.log/camd.log.
- Opening Camera.app at night was declined (the tap into the Utilities folder was denied by the auto-mode safety check; the phone is in the user's bedroom): left for the user (U12).
- End state: gate OFF (camdev.on removed), camd stopped ("powered down"), mediaserverd restarted (pid 512); the new driver stays installed (audio only while the gate is off). The phone has no killall/pgrep/ps: use /var/local/bin/psx + kill.

### 2026-10-31 04:45 cam24 (kernel110, sweetse9=2 sweetcampmic=2): 0233 makes camd ~6x faster
- Battery 4440 mV; boot OK. `camclient -n 300`: first frame 6.26 s after START, **22.91 fps read** (was 4.0), seq 1..480 (180 gaps), 0 torn, published 300, dropped 3; copy_us bimodal ~4 ms / 34-43 ms (was ~276 ms); mean byte ~24. Teardown clean. Regression on kernel110: `--stream 5` 60.00 fps 0 faults; `--capture` full frame (0 zero 1 KB blocks). 3 min stable, no reset.
- The capture script didn't fetch the frame: camrx failed the run on the ~12 KB "guard" writes (the ~4.8 extra lines the WM always writes). Fixed: only guard writes beyond +16 KB fail a capture.
- kernel110 is the camera kernel now (boot with sweetse9=2 sweetcampmic=2).

### 2026-10-31 04:30 Stage f symbol check (read-only, research/notes/camera-stage-f-symbols.md)
- 49 of 53 symbols redfin's camera_device.c / inject_media.c need are in our dyld cache: 33/36 C exports (CMDerivedObjectCreate, CMBaseObjectGetDerivedStorage, FigCaptureDeviceGetClassID, FigCaptureStreamGetClassID, VT encoder calls, port types, 20 metadata keys in CMCaptureCore), 16/16 ObjC classes/methods (BWFigCaptureDeviceVendor +sharedCaptureDeviceVendor / -initWithDeviceCreateFunction:, BWNodeOutput emitSampleBuffer:, ...). Missing: 4 optional metadata keys (AWBCCT, ColorTemperature, FocusPosition, LensPosition), which camera_device.c treats as NULL. kFigCaptureSampleBufferAttachmentKey_MetadataDictionary unconfirmed (lookup timed out; stills need it: watch for "metadata disabled" in the f2 log).
- Stream class vtable: -[BWFigCaptureStream start]/stop call slots +0x8/+0x10 of the class's third word = redfin's layout. Device class version/slots inferred from the same build (16.7.x). Risk of loading the camera code into mediaserverd: low.

### 2026-10-31 04:25 xnu 0233 + kernel110: the camera ring gets a write-combined pmem mapping
- sweet_pmem.c pm_rw: reads/writes of 0x9cb00000..+0xB00000 go through one ml_io_map_unmappable(..., VM_WIMG_WCOMB) mapping with uiomove (wide loads) instead of 2 MB device windows + 32-bit loads; pm_io refuses that range. Both mappings are uncached, so IFE DMA needs no cache maintenance. kernel110 = kernel109 + 0233 (built 40 s). To test: camd copy_us (was ~276 ms) and client fps (was 4.0).

### 2026-10-31 04:10 cam23 (kernel109, cam20r boot): f1 DONE, camd + camclient work end to end (9acd78a)
- A (split camrx regression): `--stream 5` 60.00 fps (301 SOF / 300 done), 0 faults, past ring 0, restored: same as cam22.
- B camd (started `(/var/local/bin/camd --log /var/local/camd.log ... &)`; the phone has no nohup/pgrep/timeout): /var/run/sunfish-camera.shm 12587008 B (3 x 4 MB slots, 2016x1512 RAW10) + .sock. `camclient -n 120`: START reply after 6.67 s, 120 frames read, seq 1..219, 0 torn, published 120, dropped 6, **client 4.0 fps (sensor 60.1): copy_us ~276 ms per frame through /dev/pmem pread (~14 MB/s)**; header 2016x1512 stride 2520 fmt 1 exp 1592 lines (16.47 ms) gain 256 frame_length 1608; mean byte 23.4. `camclient -n 60 -e 800 512`: exposure change while streaming took effect (headers: exp 1592 -> 800 lines, gain 256 -> 512; mean byte 24.6 -> 28.8). Teardown on last client: camid power-down, SMMU restored, "powered down"; SIGTERM exit clean. 3 min stable; camd.raw 3810240 B, 0 zero 1 KB blocks.
- Bottleneck: the copy out of the carveout (uncached device mapping, 32-bit loads). Next: xnu patch: pmem reads of the splash carveout through a normal-non-cacheable (write-combined) mapping with 64-bit/bcopy loads; then camera_client.c shim + camera_device.c in SunfishAudio.driver.
- Screen: the lock-screen button icons are noise (B29, already right after boot: cam20r frame); in cam23's frame the clock digits were noise too after midnight (also B29-like, not the camera: the camera writes only 0x9cb00000.. via its SMRs).

### 2026-10-31 03:55 Stage f design (read-only, research/notes/camera-stage-f.md)
- Chosen: a root daemon `camd` (camrx's proven code) -> /var/run/sunfish-camera.shm (3 slots, redwood_camera.h structs) + /var/run/sunfish-camera.sock (commands); camera_device.c in SunfishAudio.driver reads it through a ~150-line shim (camera_client.c) replacing its 8 device calls. Reasons: no sandbox enforced here (xnu 0050 stand-in allows all; 0219 already covers redfin 0924's tccd camera token); mediaserverd opening /dev/pmem rejected (root 0600, and a camera fault would kill audio).
- Same iOS build as redfin (16.7.16 20H392 iPhone X), so their FigCaptureDevice vtables/selectors should match; a read-only symbol check comes before the first load. Kernel 0923 port stays the fallback (~2-3 days).
- f1 (camd + camclient, no mediaserverd) being written.

### 2026-10-31 03:40 e3: phone-side ISP runs (camisp, 7b4bc20; ios-redfin camera_isp.c, MIT)
- phone/camera: camera_isp.c/.h + redwood_camera.h verbatim from redfin, camisp.c wraps a bare 2016x1512 RAW10 file in a redwood_camera_frame header (stride 2520, format 1, Bayer --bayer, default RGGB unverified, black 64 assumed). Needs only libc + libm.
- Phone (/var/local/cam/frame.raw from cam22, night scene): binned 1008x756 **83.4 ms/frame** (~12 fps); full 2016x1512 NV12 **371.9 ms/frame**. Host (PC) 13.9 / 73.6 ms. Guess for the gap: CPU clocks low (no CPU governor yet, backlog). Stats: mean R/G/B 87.0/89.0/88.1 above black, AWB gains 1.022/1.000/1.010; AE numbers meaningless (no real exposure metadata yet).
- On-device: /var/local/bin/camisp (phone tool; tools/build-userland-linux.sh builds it), /var/local/cam/isp.ppm + isp-full.nv12 (test output, not viewed).
- Next: f (Camera.app): redfin's /dev/redwood-camera kernel device (0923) + sandbox extension (0924) vs a userland daemon; decide, then camera_device.c + inject_media.c into SunfishAudio.driver.

### 2026-10-31 03:20 cam22 (kernel109, cam20r boot): e1 DONE, continuous streaming 60.1 fps; e2 exposure/gain written (2e94b32)
- `sunfish-cam-rx.sh --stream 10`: SOF 602 / done 601, **60.10 fps** (per second 60-61), late address 0, 0 trap/bank faults, GFSR 0, VIOLATION 0, COMP_ERROR 0, CSID 3810240/3810240; slots 3647329 / 3648057 nonzero, past the ring 0; last frame saved; late writes N; restored. 2 min stable.
- `--stream 30`: SOF 1805 / done 1804, **60.13 fps**, no second below 60, 0 faults, past the ring 0, saved, restored; 3 min stable.
- Exposure/gain (single captures, `--stream-cmd '... --exposure E --gain G'`): frame_length 0x0340 = 1608 lines, exposure clamp 4..1568, gain code max 448 (8x); all written values read back. Mean byte: E200 G0 31.17; E1000 G0 31.14; E1500 G256 (2x) 31.29; E1500 G448 (8x) 37.56 (histogram spreads out). Exposure barely moves the mean at night: guess, a dark room shows only the black level + noise (8x gain spreads the noise). Needs a daytime test.
- Next: e3 phone-side ISP (redfin camera_isp.c + camera_test as a phone tool on stream.raw), then f (Camera.app via SunfishAudio.driver).

### 2026-10-31 03:05 Stage e plan (read-only, research/notes/camera-stage-e.md)
- redfin's path: kernel /dev/redwood-camera (0923+: 3 slots, 1 ms SOF/WM0_DONE poll, IMAGE_ADDR swap per SOF, exposure in group hold) -> camera_device.c (FigCaptureDevice via a BWFigCaptureDeviceVendor swizzle, inject_media.c) + camera_isp.c (CPU ISP: AE/AWB/shading/CCM/gamma) linked into their audio HAL plug-in inside mediaserverd. Our SunfishAudio.driver is that plug-in's port, so the camera files can go in the same way.
- Stages: e1 continuous streaming (2 carveout slots: 3 x 3.82 MB doesn't fit 0xB00000), e2 exposure/gain (0x0202/0x0204 in group hold), e3 phone ISP (camera_isp.c + camera_test), f Camera.app (camera_device.c in SunfishAudio.driver; high risk: mediaserverd crash takes audio down). A kernel driver is probably not needed (guess; mediaserverd sandbox may force it).
- e1 + e2 being written (camrx stream, camid --hold/--exposure/--gain).

### 2026-10-31 02:55 cam21 (kernel109, cam20r boot): STAGE d DONE, one capture = full frame, no reset
- `CAPMODE="--bypass --sid 0x8c0 --sid2 0xcc0"` (b5e38a9): SMR15 0x8c0 + SMR16 0xcc0 (both mask 0) -> CB14 bypass; trap CB15: SMR17-22. buf done ~220 ms; 0 faults on either bank, GFSR 0; **frame 3777917 of 3810240 nonzero, 0 of 3720 1 KB blocks zero**; guard 11997 nonzero (+0..+12095: the frame is ~4.8 lines of 2520 B longer than 2016x1512, both halves now); late writes N; restored; "saved, fsync ok". **No reset: 8 of 8 SSH checks over 4 min.** Screen fine (webcam).
- **Cause of cam17/cam18B/cam20's resets: the single masked SMR 0x8c0/0x400**, not SID 0xcc0 itself (guess why: the hypervisor's SMMU emulation or the SMR mask width; not investigated). Rule: exact SMRs only. sunfish-cam-rx.sh's default CAPMODE is now this config.
- Frame (numbers only, not viewed): work/cam/cam21.raw (+ PNGs).
- Next: the extra ~5 lines (embedded data? set FRAME_BYTES / WM height from the CSID), exposure/gain so frames aren't dark (IMX363 registers 0x0202/0x0204), then stage e (ISP + iOS camera device, redfin camera_isp.c / camera_device.c).

### 2026-10-31 02:40 cam20 (kernel109, cam18r boot): sf_axi on + one SMR 0x8c0/0x400: frame ALL ZERO, RESET a few min later
- camid: camera_sf_axi 0x80000000 -> 0x1 after the GDSC (hf_axi 0x1, xo 0x1). SMRs: 0x8c0/0x400 -> CB18 bypass, 6 others -> CB19 trap. buf done ~200 ms; 0 faults sampled; **frame 0 of 3810240 nonzero, guard 0**: not even the 0x8c0 half landed. Late writes N, restored. Then SSH timed out a few minutes after "powered down"; fastboot, 4439 mV; rescued cam20r (23:13). Per-boot log 20261029-230036.log ends at ~1425 s, no panic.
- New reading: every run with the one masked SMR (0x8c0 mask 0x400: cam17, cam18B, cam20) lost all data and reset; cam15 (exact SMR 0x8c0 only, 0xcc0 unmatched) landed the 0x8c0 half and reset; cam16/18A/19 (exact 0x8c0 + 0xcc0 on an exact trap SMR) landed it and did not reset. So the masked SMR may itself be the problem (unverified). sf_axi stays on (redfin does it; harmless).
- Next (cam21): two exact SMRs, 0x8c0 and 0xcc0 (`--sid 0x8c0 --sid2 0xcc0`), both into the bypass bank.

### 2026-10-31 02:25 cam20 prep (read-only, kernel109 cam18r boot, camera powered down): TBUs are on; camera_sf_axi is off
- pinsample reads: TBU vote GDSCs 0x17d040 (hf0) / 0x17d044 (sf) / 0x17d048 (hf1) all 0x80000001 (bit31 PWR_ON = 1, our SW_COLLAPSE vote off: someone else keeps all three TBUs on); TBU status 0x15182210/18/20 = 1, 1, 1. So the hf1 TBU behind SID 0xcc0 is powered: the research's first guess is out.
- GCC camera_hf_axi 0x10b020 0x80000001 (camera off now); **camera_sf_axi 0x10b06c = 0x80000000 (disabled)** (Linux v6.15 gcc-sm7150.c: halt/enable 0xb06c; research/re/gcc-sm7150.c). redfin 0921 enables sf_axi; we never did.
- camid now enables camera_sf_axi next to hf_axi. Next (cam20): `--bypass --sid 0x8c0 --sid-mask 0x400` (cam17's config) with sf_axi on.

### 2026-10-31 02:10 cam19 (kernel109, cam18r boot): FULL FRAME from two half captures, no reset
- A `--bypass --sid 0x8c0`: buf done ~210 ms, frame 1889325 nonzero, guard 5917 (+1..+12095), late writes N, restored; 2 min SSH OK. B `... --addr-offset 0x400`: buf done ~220 ms, frame 1888380 nonzero, guard 6083 (+64..+11327), late writes N, restored; 3 min SSH OK.
- `sunfish-cam-merge.py cam19-a.raw cam19-b.raw cam19-full.raw`: 3721 blocks of 1 KB, **empty even 0, odd 0**; merged: 0 all-zero 1 KB blocks, mean byte 31.2, max 255, histogram [142516, 326650, 3507, 12443, 11779, 6189, 1574, 6233, 3527, 1554, 53, 1543, 12445, 6150, 1537, 6620]; raw2png gray 0..245, colour (RGGB guess) R 0..245 G 7..134 B 0..245. Files in work/cam/ (cam19-full*.png), not viewed (privacy, night).
- Root cause research for SID 0xcc0 (research/notes/camera-sid-cc0.md): 0x8c0 goes through TBU mnoc_hf_0 (SIDs 0x800-0xbff), 0xcc0 through TBU mnoc_hf_1 (0xc00-0xfff) (live.dts:13930-13958); TBU power votes GCC 0x17d040 (hf0), 0x17d044 (sf), 0x17d048 (hf1); redfin also turned on GCC camera_sf_axi (we don't). Next: read-only reads of those + the TBU status registers, then sf_axi on, then the hf1 TBU vote, one per boot.

### 2026-10-31 01:45 cam18 (kernel109, cam17r boot): 0xcc0 writes stay in the buffer; bounded translation still RESETS
- A (`--bypass --sid 0x8c0`, cadab15, FAR sampling): 3 trap faults sampled, all SID 0xcc0, FAR 0x9cb00400, 0x9cb88c00, 0x9ce76400 (all inside the buffer); frame 1889287 nonzero, guard 5909 (+1..+12095); late writes N; restored; **no reset** (6 of 6 SSH checks in 3 min).
- B (`--xlate`: SMR 0x8c0/0x400 -> stage-1 bank, tables map only IOVA 0x9ca00000..0x9d600000 identity in 2 MB blocks; L1 [2] = 0x110b84003, L2 [0xe5..0xea] = 0x006000009ca00441..0x006000009d400441, read back OK): the log ends at "cleanup: smmu park on the trap bank, 2 s"; **RESET** (per-boot log 20261029-223822.log ends at 1185.4 s, no panic); 4438 mV; rescued cam18r. (The runner started B while A's stability loop still ran; A had passed 6/6 already.)
- So the reset isn't 0xcc0 writing outside the buffer. Pattern over cam12-cam18: every run where SID 0xcc0's transactions were NOT context-faulted in our trap bank (unmatched: cam12, cam15; passed: cam17 bypass, cam18B translated) reset within seconds; every run with 0xcc0 trapped (cam14, cam16, cam18A) did not. Guess: the second camnoc port/TBU behind SID 0xcc0 (bit 10 = address bit 10 interleave) lacks a vote, clock or GDSC (e.g. an MMU TBU HF1 GDSC) and its traffic stalls the bus -> watchdog. Research item.
- Workaround now: two captures with SID 0x8c0 only, the second with `--addr-offset 0x400` (the frame 1 KB up, so the odd blocks land on even addresses), joined by tools/sunfish-cam-merge.py.

### 2026-10-31 01:20 cam17 (kernel109): bypass for SID 0x8c0 + 0xcc0 (mask 0x400) -> RESET at stream start
- `CAPMODE="--bypass --sid 0x8c0 --sid-mask 0x400"` (e4445e1): SMR15 0x840008c0 -> CB16 (bypass); trap CB17: 0x900/0x460, 0x820/0x440, 0x880, 0xc80, 0x8a0, 0xca0. The script hung at "sensor stream" (camid --stream); the per-boot log (20261029-221640.log) ends at 1121.2 s with ordinary IOKit lines, no panic. Phone in fastboot, 4439 mV; rescued cam17r (22:38). No frame. Note: the banks chosen were CB16/17, not CB14/15: cam16's S2CR restore wrote 0x000200ff but read back 0x0000000f (restore of S2CR doesn't stick; harmless with the SMRs invalid, but banks 14/15 then look used).
- cam16's frame (numbers only): zero blocks at 1 KB granularity exactly alternate (3720 blocks, 1860 zero, pattern 1010...; 2 KB blocks: 0 zero): **the WM interleaves by address bit 10 across SID 0x8c0 (even KB) and 0xcc0 (odd KB)**, so 0xcc0 is needed for a full frame.
- So letting 0xcc0 through resets the phone, 0x8c0 alone doesn't. Cause unknown (0xcc0 writes elsewhere too? something about that port?). Next: sample every 0xcc0 trap FAR during a cam16-style run (safe), then `--xlate`: both SIDs through a stage-1 bank that maps only 0x9ca00000..0x9d600000 (everything else faults).

### 2026-10-31 01:00 cam16 (kernel109, cam15r boot): FIRST FRAME CAPTURED AND SAVED, no reset (3ed415d)
- `CAPMODE="--bypass --sid 0x8c0" sunfish-cam-rx.sh --capture`: 8 SMRs (SMR15 0x8c0 -> CB14 bypass; SMR16-22 0x900/0x460, 0x820/0x440, 0x880, 0xc80, 0x8a0, 0xca0, 0xcc0 -> CB15 trap). buf done ~200 ms; CB14 FSR 0x400 clean, sGFSR 0; IFE VIOLATION 0, BUS_IRQ 0x2/0x1/0, COMP_ERROR 0; CSID 3810240/3810240.
- **Trap bank during the capture: FSR 0x402 (TF), FAR 0x9cb00400, write, SID 0xcc0**: the IFE-lite WM also writes through SID 0xcc0 (= 0x8c0 | 0x400), apparently alternating bursts (first fault at +0x400). Frame region 1889395 of 3810240 nonzero (49.6% of sampled bytes): consistent with every 0xcc0 burst being dropped (guess). Next: one bypass SMR 0x8c0 mask 0x400 (covers both; no overlap with SMR5 0x800/0x440).
- Guard: 5906 nonzero bytes at +0..+12095 past 3810240 (4 pages, 4.8 lines of 2520 B): the frame is ~5 lines taller than 1512 (embedded/extra lines, guess), contiguous, not scattered.
- Cleanup: IFE-lite reset, SID parked on the trap bank 2 s: **late writes after stop: N**; restore OK; camnoc back to XO. "saved, fsync ok". **No reset: SSH OK 8 of 8 checks over 4 min.** So cam12/cam15's resets came with unmatched-stream faults (cam12: all SIDs; cam15: SID 0xcc0 was never matched).
- Frame (numbers only, not viewed: the phone is in the user's bedroom at night): mean byte 15.5, max 255; 16-bin histogram [343219, 163856, 1772, 6318, 5973, 3008, 747, 2999, 1778, 798, 29, 758, 6103, 3047, 743, 3172] (dark scene); raw2png gray 0..240. Files: work/cam/frame-cam16.raw, -gray.png, -color.png (gitignored, for the user).

### 2026-10-31 00:45 cam15 (kernel109): FRAME LANDS in RAM (SID 0x8c0 bypass), then RESET ~10 s later
- `CAPMODE="--bypass --sid 0x8c0" sunfish-cam-rx.sh --capture`: SMR15 0x800008c0 -> S2CR 0xf -> CB15 (SCTLR 0xe0); no read-back mismatch. Stream ~1040 ms, buf done ~200 ms, CSID 3810240/3810240, IFE VIOLATION 0, BUS_IRQ 0x2/0x1/0, COMP_ERROR 0, CB15 FSR 0x400 (no fault), sGFSR 0.
- **Frame region: 1889286 of 3810240 bytes nonzero** (data landed); **guard: 5896 of 7724096 nonzero** (the IFE wrote past 3810240; where is still unknown). SMMU restore lines OK, camnoc back to XO.
- **RESET**: the live log stops at 790.8 s, ~10 s after the capture (780 s), no panic line; phone in fastboot at 22:09-22:12 (4438 mV). Rescued as cam15r (22:13). The frame file written on the phone was lost (not flushed; /var/local/cam/frame.raw is still cam14's from 22:05). Same pattern as cam12 (log stops ~7 s after the capture). cam14 (trap) did not reset.
- Hypothesis (unverified): unmatched-stream (global) SMMU faults from the IFE trigger the reset (hypervisor); cam12 had them from the start, cam15 probably after its SMR was removed while the IFE still wrote. Next: cleanup keeps the SID on a trap bank (late writes -> context faults, reported), IFE global reset before the SMMU teardown, fsync the frame; trap SMRs for the other IFE SIDs.
- Live log: the per-boot split stopped at the 20:57 boot (boots/current.log -> 20261029-205717.log); ../logs/usb-live.log has all boots.

### 2026-10-31 00:30 cam14 (kernel109, same boot): trap capture finds the IFE-lite write SID = 0x8c0
- `sunfish-cam-rx.sh --install --capture` (CAPMODE --trap): SMR15 0x900/0x460, SMR16 0x880/0x460, SMR17 0x820/0x440 -> CB14 (SCTLR 0x21 M|CFRE, TCR EPD0|EPD1); every write read back as written.
- Stream ~1060 ms, buf done at ~210 ms, CSID 3810240/3810240. **CB14 FSR 0x402 (TF), FAR 0x9cb00000 (= our IMAGE_ADDR), FSYNR0 0x00400010 (write), CBFRSYNRA 0x8c0 -> SID 0x8c0** (in the DT list, 0x8c0/0x460). sGFSR 0. Buffer/guard nonzero 0/0.
- Restore: 17 lines OK (SMRs invalid, S2CR 0x000200ff, CB14, camnoc back to XO); `camrx smmu` afterwards: only SMR0-14 valid. **No reset in 3+ min** (cam12's ~1 min reset came after unmatched-stream faults, sGFSR 0: the hypervisor likely handles those; context faults in our bank are harmless).
- Next (cam15): `CAPMODE="--bypass --sid 0x8c0"`: one SMR (mask 0) -> bypass bank -> the frame should land at 0x9cb00000.

### 2026-10-31 00:15 cam13 (kernel109, cam12r boot): read-only SMMU dump explains cam12's empty frame
- `camrx smmu` (88dde8e, no writes): sCR0 0x00280406 (CLIENTPD 0, GFRE/GFIE 1, USFCFG 1 = unmatched streams fault and are terminated, SMCFCFG 1); 79 SMRs, 61 banks; sGFSR 0.
- Valid SMRs 0-14, all translate-type S2CRs: 0x340/0xf, 0x2a0/0xf, 0x2c0/0xf -> CB0; 0x540 -> CB1; 0x300/0xf -> CB2; 0x800/0x440 -> CB3; 0x240/0x1 -> CB5; 0x1b21/24/25/26/27/28/2f -> CB6-12; 0x1b23 -> CB13. All banks bypass (SCTLR 0xe0, CBAR 0x1f000) except CB13 (SCTLR 0x127, M=1, CBAR 0x1f300, TTBR0 set).
- **All 14 DT IFE SIDs: no SMR -> unmatched -> fault.** So cam12's IFE writes were terminated by the SMMU (buffer stayed 0). sGFSR 0 after cam12 although USF faults should set it: guess, the hypervisor handles global faults, and may be what reset the phone ~1 min later (unverified).
- Next (cam14): `camrx capture --trap`: SMRs 0x900/0x460, 0x880/0x460, 0x820/0x440 -> a free bank with M=1 and no table walks (EPD0/EPD1): context faults record the SID, nothing lands.

### 2026-10-30 23:55 cam12 (kernel109, same boot): IFE wrote "a frame" but not to our buffer; RESET ~1 min later
- `sunfish-cam-rx.sh --install --capture` (a47990d): slot1 0x9cb00000 + 0xB00000 zeroed in 0.126 s (2816 pages read back 0). camnoc 150 MHz then back to XO at cleanup. Stream ~1090 ms; **buf done at ~210 ms**; IFE VIOLATION 0, IRQ0 0x8000220, BUS_IRQ 0x2/0x1/0, COMP_ERROR 0, WM0_STATUS0 0x9cb00000; CSID bytes 3810240/3810240. camrx exit 0.
- **Frame region 0 of 3810240 nonzero, guard 0 nonzero**: the write did not land in the buffer.
- SMMU: smmu_identity found SMR5 0x84400800 (SID 0x800 mask 0x440, valid) already matching 0x800 -> S2CR 3 -> CB3 (SCTLR 0xe0 bypass, CBAR 0x1f000) and wrote nothing (by design: never touches an existing SMR). GFSR 0 before/after, CB3 FSR 0x400 (no fault bits). But the IFE SIDs in our DT (live.dts:13150: 0x900, 0xd00, 0x880, 0xc80 mask 0x460; 0x820, 0xc20 mask 0x440; 0x920, 0xd20, 0x8a0, 0xca0, 0x940, 0xd40, 0x8c0, 0xcc0 mask 0x460) are NOT covered by SMR5's mask 0x440. So redfin's "0x800/0x5e0" assumption failed here: the stream went through some other SMR or the unmatched-stream path.
- **RESET (hard)**: `sunfish-ssh true` was OK right after the capture; ~1 min later SSH timed out and the phone was in fastboot ("reboot bootloader"), 4438 mV; last boot-log lines are the camclk/camid execs. Rescued: cam12r (kernel109 sweetse9=2 sweetcampmic=2, 21:57). Guess (unverified): the IFE's 3.8 MB went to IOVA 0x9cb00000 through another context bank's translation (wrong RAM), or the hypervisor caught the camera SID's write.
- Smaller next step: read-only SMMU dump (sCR0 USFCFG, every valid SMR/S2CR/CB, which SMR each DT IFE SID hits); then a dedicated SMR for the IFE SIDs into a FAULTING bank first (TCR EPD0/EPD1 = no walk: a write becomes a translation fault with FAR + SID in CBFRSYNRA, no data lands) to learn the exact SID, and only then a bypass bank for that SID.

### 2026-10-30 23:40 cam11 (kernel109, same boot): camid ABORT camnoc_axi (leftover camnoc RCG on PLL0)
- `sunfish-cam-rx.sh --install --capture` (fbaddc1): camid stopped at "ABORT: camnoc_axi" after GDSC on and cpas_ahb 0x1. Cause: cam10's camrx set the camnoc RCG to 150 MHz (PLL0 even), then `camclk off` turned PLL0 off; RCG settings survive, so the camnoc_axi branch has no running source. No capture, no DMA, no reset.
- Fix: camrx's cleanup puts the camnoc RCG back on XO while PLL0 still runs; sunfish-cam-rx.sh starts PLL2 + PLL0 before camid (PLLs need no GDSC, cam2).

### 2026-10-30 23:25 cam10 (kernel109, sweetse9=2 sweetcampmic=2): PMEM_ALLOC32 ENOMEM; buffer moves to the display carveout
- Battery 4455 mV; kernel109 boots (SSH 7 s). `sunfish-cam-rx.sh --install --capture`: power-up, camnoc 150 MHz, CSIPHY ok, then "pmem: alloc32 8388608: Cannot allocate memory"; powered down. No DMA, no reset. (The runner's first boot try passed the kernel directory to sunfish-boot.sh: give it work/kernel109/kernel.development.sweet.)
- Cause (known since the audio work, audio.md): XNU manages no RAM below 4 GB here (mkadt.py gives XNU 0x1_0000_0000 up); all 680 logged PMEM allocations are at 0x106..-0x12. So 0232 can't succeed; it stays harmless.
- Next: capture into the splash carveout's zero-copy slot1 (PA 0x9cb00000, 0xB00000; fb + GPU map use 0x9c000000..0x9ca00000; slot1 unused without sweetfb_zerocopy). Open risk: whether the hypervisor lets the camera SIDs write that RAM (a reset if not).

### 2026-10-30 23:05 cam9 (kernel108, same boot): capture aborted safely, PMEM buffer above 4 GB; xnu 0232 PMEM_ALLOC32 -> kernel109
- `tools/sunfish-cam-rx.sh --install --capture` (e6d65ae): powered up, camnoc_axi RCG set to 150 MHz (PLL0 even /4, CFG 0x207), then PMEM_ALLOC 8 MB returned PA 0x1afde0000: above 4 GB, IFE IMAGE_ADDR is 32 bit -> "abort", PMEM_FREE 0, powered down. No DMA, no SMMU change, no reset. (Privacy: the phone is in the user's bedroom at night; capture results are checked by numbers only, the picture stays unopened in work/cam/ for the user.)
- Fix: xnu 0232 `PMEM_ALLOC32` (ioctl 'P' 6): kmem_alloc_contig with max_pnum = 4 GB - 1 page; phone/wifi/pmem.c pm_alloc32(); camrx capture uses it. Kernel: kernel109 = kernel108 + 0232.

### 2026-10-30 22:45 cam8 (kernel108, same boot): stage c DONE, real sensor frames reach CSID-lite (c5e17d7)
- `camclk pll0 on` (Fabia, Linux v6.15 sequence): L 0x3e, FRAC 0x8000, TEST_CTL 0x40000000, USER_CTL[15:8] 0x31; lock, MODE 0xd0800004 -> 0xd0800005 after OUTCTRL (VCO 1200 MHz). camclk has no --dry-run.
- `camrx phy on`: settle count 25 (Linux formula: 85 ns + 6 UI at 304 Mb/s/lane, 300 MHz timer; the notes' 36 was wrong); csi0phytimer 300 MHz (src 2 PLL0 even, div 3) + csiphy0 branches on; CSIPHY0 v1.2 redfin 0922 table, lane map 0x3210 (guess, worked). Status 0x8b0..0x8d8 all 0 after phy on.
- `camrx rx arm` (RX_CFG0 0x32103 4 lanes, RDI0 VC 0 DT 0x2b), then `camid --addr 0x1a --keep --mclk-asis --stream` (frame counter 1 ... 63 in 1 s), then `camrx rx report`: **RX_PKTS 119448, ECC 0, CRC 0, RDI0_STATUS 0, bytes ping/pong 3810240/3810240 = exactly one 2016x1512 RAW10 frame**. RX IRQ 0x004000ff (DL0-3 SOT + EOT; bit 22 UNMAPPED_VC_DT, likely the sensor's embedded-data packets), RDI0 0x1ff8 (INPUT SOF/SOL/EOF, SUBSAMPLED SOF/EOF). CSIPHY status after: 00 01 00 20 00 00 04 00 80 40 00 (meaning unknown).
- Cleanup: rx stop, phy off, clocks off, `camclk off`, `camid --addr 0x1a` (GDSC collapse). No reset.
- Next: stage d (IFE-lite write master RDI0 -> a RAM buffer through the SMMU; dump one RAW10 frame to the PC).

### 2026-10-30 22:20 cam7 (kernel108, same boot): stage c1 DONE, CSID-lite test pattern runs (camrx, ba7ebf7)
- Base: `camid --addr 0x1a --keep`, `camclk pll2 on`. Before: cphy_rx/ife_lite_csid/ife_lite/csi0phytimer RCGs CMD 0x80000000 CFG 0; branches off. Refgen (read-only, offsets are guesses) BIAS_EN 0x4040404, BG_CTRL 0x1010101.
- `camrx clocks on`: cphy_rx CFG 0x504 + ife_lite_csid 0x504 (384 MHz, PLL2 early/2.5), ife_lite 0x502 (320 MHz); RCG updates done; branches lite_cphy_rx 0x1, lite_csid 0x221, ife_lite 0x1 (none stuck).
- CSID-lite HW_VERSION 0x20000000. `camrx tpg 2` (640x480 RAW10 colour bars, VC 0xa, 4 lanes, redfin 0921 values): 20 polls; **RX_PKTS 0 -> 205814** (~213 frames/s at ~482 packets/frame, a guess at the packet count), **bytes ping/pong 384000/384000 = exactly one 640x480 RAW10 frame**; SOF/EOF bits set in every poll (INPUT + SUBSAMPLED), RDI0_STATUS 0, CRC 0, ECC reads 0xffff (unverified: saturated or misread field). RX bits 0-7 and 14 latched (not decoded).
- Cleanup: clocks off, `camclk off`, `camid --addr 0x1a` (GDSC collapse). No reset. Note: toybox on the phone has no `sync`.
- Next (c2): PLL0 (Fabia, 1200 MHz) for csi0phytimer 300 MHz (DT), CSIPHY0 v1.2 4-lane init (redfin 0922), CSID RX from the PHY with the sensor streaming (camid needs a keep-streaming option).

### 2026-10-30 22:00 cam6 (kernel108, sweetse9=2 sweetcampmic=2, same boot): stage b DONE, the IMX363 streams
- Tools: tools/sunfish-camtab.py (after ios-redfin's redfin-camtab.py; our module bin is "Parameter Parser V1.3.2", resolutionData 35 words vs redfin's 37) -> work/cam/imx363-mode8.txt (30 init + 82 mode 8 writes, all delays 0); camid `--stream FILE` (commit a55102c).
- Modes in metric_imx363.bin (all 4-lane RAW10 0x2b, 82 writes): 0/7 4032x3024@30 552 Mb/s/lane; 1 4032x2272@30 432; 2 2016x1136@120 464; 3 2016x1136@240 828; 4 4032x2272@30 836; 5/6 4032x2272@60 836; 8 2016x1512@60.13 304 (2x2 bin, default); 9 2016x1136@60 (rate field 0).
- Run: `camid --addr 0x1a --keep`, `camclk pll2 on`, `camclk mclk 0 24`, `camid --addr 0x1a --mclk-asis --stream /var/local/cam/imx363-mode8.txt`: ID OK, 112/112 writes, readbacks 0x0136 0x18, 0x0137 0x00 (24 MHz), 0x0112 0x0a, 0x0114 0x03 (4 lanes); stream on, 0x0100 = 1.
- **Frame counter 0x0005 every 100 ms: 1, 6, 12, 18, 25, 31, 38, 44, 50, 57, 63 = ~62 frames/s** (mode 8 is 60.13). Stream off, 11 pd lines, `camclk off`. No reset.
- Not done: the stream-on table's 0x0138 = 1 (temperature sensor enable) left out. Nothing receives the MIPI data yet.
- Next: stage c (CSIPHY0 v1.2 tables + CSID-lite RDI0; CSID test pattern first).

### 2026-10-30 21:50 cam5 (kernel108, sweetse9=2 sweetcampmic=2, same boot as cam4): stage a2 DONE, IMX363 ID at MCLK 24 MHz
- `camid --addr 0x1a --keep` (MCLK on XO 19.2 MHz): bytes 35 03 63 63, IMX363 ID OK.
- `camclk pll2 on` (lock, MODE 0xc0000007), `camclk mclk 0 24`: CBCR 0x1, CFG 0x2501 M 1 N 0xec D 0xeb.
- `camid --addr 0x1a --keep --mclk-asis`: "mclk0 rcg left as is: cfg 0x2501 m 0x1 n 0xec"; **bytes 35 03 63 63 at 24 MHz**.
- `camclk off` (CBCR 0x80000000, PLL off), `camid --addr 0x1a`: ID OK at XO, then 11 pd lines (reset, mclk, 4 pins, cci_0, 3 branches, GDSC collapse). No reset.
- On-device: /var/local/bin/camid + camclk (phone tools, built by tools/build-userland-linux.sh); nothing left powered.
- Next: stage b (sunfish-camtab.py from redfin-camtab.py + `camid --stream FILE`: init + 2016x1512 mode, 0x0100 = 1, frame counter 0x0005).

### 2026-10-30 21:40 cam4 (kernel108, sweetse9=2 sweetcampmic=2): MCLK0 runs at 24 MHz from PLL2
- Battery 4455 mV, SSH 18 s. campmic GPIOs set at ~30.6 s of boot (not 146 s as on kernel94): "after ... 40 01 ... 44 80 45 01 46 80".
- `pll2 on`: lock, MODE 0xc0000007. `camid --keep`: SLG LDOs on (STATUS 0x02), TITAN GDSC 0x22f001 -> 0xf822f000, cpas/core/camnoc/cci_0 on, MCLK0 on XO (CBCR 0x1), reset high; CCI: 0x10 NACK (status 0x8000000), then 0x1a timeout (status 0). Same as 2026-10-28, where 0x1a only answered when probed alone (`--addr 0x1a`).
- **`camclk mclk 0 24` with the GDSC up: branch on** (CBCR 0x1, CLK_OFF clear), CFG 0x2501 (src 5 = PLL2 out_aux 480 MHz, div 1), M 1, N 0xec, D 0xeb (= 480/20 = 24 MHz). cam2/cam3's "stuck off" was the GDSC being off. `camclk status`: PLL2 lock 1, VCO 1920 MHz. `camclk off` + `camid`: everything powered down (pd lines). No reset.
- Not measured: the actual MCLK frequency on the pin (no scope); the ID read at 24 MHz.
- camid gets `--mclk-asis` (keeps camclk's 24 MHz RCG instead of resetting to XO) for cam5.

### 2026-10-30 21:25 cam3 (kernel108, sweetse9=2; no sweetcampmic): camera PMIC off, sensor not reached
- Battery 4455 mV; boot 5 s, SSH 16 s; webcam: Apple logo (dim, night mode). No reset.
- `pll2 on`: locked again (MODE 0xc0000006, 0xc0000007 after OUTCTRL), same register values as cam2.
- `camid --keep`: "slg rd 0x1105 err 6", "ABORT: slg read" (SE9 fw 0x303 status 0, so the I2C bus is up). Cause: the boot lacked `sweetcampmic=2` (xnu 0218, the PM6150L GPIOs that enable the SLG51000); cm1/cam2 on 2026-10-28 had both args.
- `mclk 0 24`: RCG updated (CFG 0x2501, M 1, N 0xec, D 0xeb), branch stuck off (CBCR 0x80000001) as in cam2: consistent with the TITAN GDSC still off (camid aborted before it).
- `camclk off`: all back off. Next (cam4): same with `sweetse9=2 sweetcampmic=2`.

### 2026-10-30 21:15 cam2 (kernel108, no reboot; no sweetse9): PLL2 locks; MCLK0 branch stays off
- camclk (commit c8633dc) installed. `pll2 on`: L=0x64, USER_CTL 0x30b, CONFIG_CTL 0x20000954, CONFIG_CTL_U 0x400003d6, then BYPASSNL/RESET_N/OUTCTRL; all read back; "pll2 locked, MODE 0xc0000006" (VCO 1920 MHz, aux 480 MHz). Lock Y.
- Correction to cam1: the Agera PLL2 register offsets differ from Fabia; the as-read status0 was USER_CTL 0x1, USER_CTL_U 0x20000954, CONFIG_CTL 0x3d6, CONFIG_CTL_U 0x04000400 (reset values).
- `camid --keep` stopped before any write: "SE9 clocks off (no GCC writes from here)": this boot had no `sweetse9=2`, so no sensor power/GDSC.
- `mclk 0 24`: RCG update done (M 1, N 0xec, D 0xeb, CFG 0x2501 src 5 div 1; CMD 0x80000000), but "mclk0 branch stuck off (CBCR 0x80000001)". Guess (unverified): the TITAN_TOP GDSC is off without camid's power-up.
- `camclk off`: CBCR 0x80000000, PLL2 off (lock 0, outctrl 0). No reset. The RCG keeps the 24 MHz config until reboot.
- Next (cam3): boot kernel108 with sweetse9=2, `camid --keep`, `pll2 on`, `mclk 0 24`, re-read the ID.

### 2026-10-30 21:00 Camera port plan from ios-redfin; camcc read-only dump (cam1, kernel108)
- Plan: research/notes/camera-port.md (redfin MIT: sweet_cam.c via patches 0919/0921-0934, software ISP camera_isp.c, FigCaptureDevice camera_device.c; same sensors/CSID-lite/IFE-lite/CSIPHY 1.2 addresses as ours; different camcc (sdmmagpie) and SMMU SIDs). Six stages, ~2-3 weeks.
- cam1 (read-only): camcc base 0x0ad00000 (+0x10000); GCC_CAMERA_AHB_CBCR 0x10b008 = 0x1 (on). PLL0-4 (0x0..0x4000): MODE/L/ALPHA 0 (off, no lock); PLL0/1/3/4 identical reset-looking CONFIG (USER_CTL 0x4805, CONFIG_CTL 0x20485699), PLL2 different (USER_CTL 0x20000954, CONFIG_CTL 0x3d6). MCLK0-3 RCGs (0x5004+0x20n) off (CMD 0x80000000, CFG/M/N/D 0), CBCRs 0x80000000 (off). Linux camcc-sm7150.c: all Fabia; 24 MHz MCLK = pll2 OUT_AUX, M/N 1/20. Next: a camclk tool to configure/enable PLL2 + MCLK RCG at 24 MHz, then camid at 24 MHz.

### 2026-10-28 22:10 M2 prep (read-only): the IMX363 init/mode tables are in the camx module file
- vendor.img /lib64/camera/: com.qti.sensormodule.metric_imx363.bin (1,044,640 B; sensor + actuator + OIS + EEPROM), com.qti.sensor.imx363.so, com.qti.sensormodule.primax_imx355.bin + .so, Google tuning bins. Google's files: decoded only in the session scratchpad (camx/dec.py), not in git.
- Format: a symbol table (0x48-byte records: index, name, data offset, size) then a data area at 0xacd50; register lists are 80-byte items with a 16-bit address, an 8-bit value and a delay.
- IMX363: init table 30 registers, starting with EXCK_FREQ 0x0136/0x0137 = 24.00 MHz; 10 modes of 82 registers each, all 4-lane RAW10 (0x0114 = 3, 0x0112 = 0x0a0a), pre-PLL divider 4: 4032x3024 @30 (~1.37 Gb/s/lane), 4032x2272 @30, 2016x1136 @120/240, 2016x1512 @60 (2x2 binning). Stream on 0x0100 = 1. I2C 0x34 (8-bit) = 0x1a. Power order in the module: VIO first, MCLK 24 MHz last, reset released before it.
- camid runs MCLK at 19.2 MHz (XO, no camcc PLL). For streaming either bring up the camcc PLL for 24 MHz (cleanest, the tables then apply as-is) or patch EXCK_FREQ to 19.2 MHz and scale the PLL multipliers by 1.25.

### 2026-10-28 21:30 camf1 (kernel94): the front sensor answers: IMX355
- `camid --front` (1bfeea3): LDO2 MISC1 bit0 = 1 (2.4 V base); VSEL LDO7 0x3c (1.8 V), LDO2 0x82 -> 0x50 (2.8 V), LDO5 0x8c -> 0xa0 (1.2 V), all STATUS 0x02. MCLK2 (0xad05044 / 0xad0505c) on XO, gpio19/20 (cci_i2c, south) 0 -> 0x7, gpio15 (cam_mclk) 0x1 -> 0x44, reset gpio68 (north).
- CCI0 master 1 at 7-bit 0x1a: reg 0x0016 = bytes `35 03 55` -> **0x0355 = Sony IMX355**; reg 0x0000 = 00 00. rc 0, all powered down after.

### 2026-10-28 20:45 cam2 (kernel94, sweetse9=2 sweetcampmic=2): M1 done, the rear IMX363 answers (ID 0x0363 at 7-bit 0x1a)
- phone/wifi/camid (98952da + the hf_axi fix): SLG51000 PATN_ID 05 35 a9, LDO1 MISC1 bit0 = 1 (2.4 V base). VSEL LDO7 0x3c (1.8 V), LDO3 0xa5 (2.85 V), LDO1 0x82 -> 0x5a (2.85 V), LDO4 0x3c (1.8 V), LDO6 0x8c -> 0x82 (1.05 V); enable via MATRIX_CONF_A 0x110d bits 6,2,0,3,5: every STATUS = 0x02 (VOUT_OK, no ILIM).
- GCC camera_ahb 0x1, camera_xo -> 0x1, hf_axi read CLK_OFF until the GDSC was up (first run aborted on it), then 0x1. TITAN_TOP GDSC 0x22f001 -> 0xf822f000 (PWR_ON). camcc cpas_ahb 0x1, core_ahb on, camnoc_axi 0x221, slow_ahb + cci_0 + mclk0 RCGs on XO (19.2 MHz), cci_0 and mclk0 branches 0x1. TLMM gpio17/18 (south) 0 -> 0x7 (cci_i2c, pull-up), gpio13 (south) 0x1 -> 0x44 (cam_mclk, 4 mA), gpio43 (north) 0x1 -> 0x200 then reset high.
- CCI0: HW_VERSION 0x10070000, reset done (status 0x1000000). Address 0x10: status 0x8000000 (NACK). Address 0x1a (run alone): READ_BUF_LEVEL 1, bytes `35 03 63 63` = slave echo 0x35 + **0x0363 = IMX363**. rc 0; everything powered down in reverse after.
- Next (M2, weeks): the sensor's init tables (camx sensor module bins in the vendor image), CSIPHY0 lanes, CSID RDI0, IFE RDI write master into a buffer (SMMU context bank), one raw frame to the PC.

### 2026-10-28 18:50 cm1 (kernel94, sweetse9=2 sweetcampmic=2): camera PMIC SLG51000 answers
- `i2cprobe scan` on kernel93 (before): 0x40/0x41 (amps), 0x50-0x53 (an EEPROM-like device, 4 blocks: reg 0 = 31/04/2a/70; maybe the camera module EEPROM), 0x75 silent.
- xnu 0218 (kernel94) `sweetcampmic=2`: PM6150L GPIO 1/5 (enable) then 4/6 (cs), all TYPE 0x10 SUBTYPE 0x10 (LV), before MODE 0 OUT 0, after MODE 0x01 OUT 0x80 EN 0x80 (output high), at 146.3 s.
- Then 0x75 answers. SYSCTL 0x1100..0x110f = 00 ff 00 02 05 05 35 a9 03 75 00 00 40 00 00 00: PATN_ID (0x1105-07) = 05 35 a9, SYS_CONF_A 0x1109 = 0x75 (its own address: the register map is right). MATRIX_CONF 0x1118/19 = 00 00 (no LDO enabled). LDO VSEL 0x2000..0x2600 (LDO1-7) = 82 00 82 a5 00 3c 00.
- Next (M1, bigger): LDO enables at the DT voltages (vio LDO7 1.8 V, vana LDO3 2.85 V, vdig LDO1, custom1 LDO4 1.8 V, custom2 LDO6 1.05 V; vaf = PM6150L GPIO12), MCLK0 (TLMM gpio13, camcc 24 MHz), reset gpio43, then a CCI master driver to read the IMX363 ID (0x0016 = 0x0363). Read the 0x50-0x53 EEPROM (read-only) first: it may hold the module ID.

### 2026-10-28 16:55 k88a camera probe (kernel88, sweetcamprobe=1, read-only)
- gcc_camera_ahb_cbcr = 0x1 (on), titan_top_gdscr = 0x22f001, mclk/csiphy/ife CBCRs = 0x80000000 (off), cci_0 = 0; SE9 skipped (its clocks are off: needs a clock vote before any I²C to the slg51000).

### 2026-10-28 13:00 survey (read-only)
- Hardware table and the RDI path above, from recon/live.dts and the shared cache. Effort: M1 ~1 week, M2 3-5 weeks, M4 4-8+ weeks.
