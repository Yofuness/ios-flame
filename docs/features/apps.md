# Apps
**State:** ✅ 32 staged apps installed in /Applications; 17 of 18 surveyed apps open fine (Maps, Notes, Wallet, Music, Photos, App Store fixed/confirmed 2026-10-29); only Camera (no camera stack), plus CocoaTop (process viewer, tools/build-cocoatop-linux.sh); crash-looping launchd jobs dropped. App extensions work (2026-10-30 ext1: widgets, widget gallery, share sheet, Files "On My iPhone", emoji keyboard; 0 crashes). **Updated:** 2026-10-30.

## How to use
Removed from `/System/Library/xpc/launchd.plist` with `tools/sunfish-launchd-drop.sh LABEL...` (applies on reboot).
Backups on the phone: the original is `/System/Library/xpc/launchd.plist.pre-drop`; the copy from before step 6
is `/var/local/launchd.plist.pre-step6` (also `work/userland/` on the PC). To undo one job, copy its dict back
from the backup with plistlib.
- Note from broken.md: numbers come from the 30 min app soaks (pp1, dd1, dd2 on 2026-10-27). Add a line when you find something; move it to "Fixed" (here: a Log entry with the date and patch) instead of deleting it.

## Facts
- 32 staged apps installed in /Applications (status.md).
- Safari needs com.apple.WebKit.GPU + MobileSafari in /var/local/mtl2-clients (2026-10-23).

### Disabled launchd jobs
| Label | Disabled | Why | Known side effect |
|---|---|---|---|
| com.apple.voicemail.vmd | 2026-10-23 | spun one CPU core without cellular | no visual voicemail (no cellular anyway) |
| com.apple.ReportCrash | 2026-10-23 | busy with TTS synthesizer crashes | no crash reports on the phone; "Failed to send exception EXC_CORPSE_NOTIFY" lines (~400/h) |
| com.apple.accessibility.axassetsd | 2026-10-23 | crash loop / CPU | accessibility asset downloads |
| com.apple.assetsd | 2026-10-27, RE-ENABLED 2026-10-29 | crash loop (88 per 30 min, SIGABRT: 'requires TCCPhotosService access', fixed by kernel95 sandbox extensions) | none now |
| com.apple.assetsd.nebulad | 2026-10-27 | assetsd helper (camera bursts) | none seen (no camera) |
| com.apple.driverkitd | 2026-10-27 | crash loop (58 in the long log) | no DriverKit drivers (none used) |
| com.apple.spotlight.IndexAgent | 2026-10-27 | thought to launch the Spotlight index extensions (it did not) | none seen |
| com.apple.corespotlightservice | 2026-10-27 | same | none seen |
| com.apple.lskdd | 2026-10-29 | respawn every 1 s from ~400 s (215 per 10 min), exits without a crash | none seen |
| com.apple.fairplayd.H2 | 2026-10-29 | KeepAlive, exits every 10 s (59 per 10 min) | App Store / DRM media (did not work anyway) |
| com.apple.wifip2pd | 2026-10-29 | respawn every 10 s after the Wi-Fi join (48 per 10 min) | no AirDrop / AWDL; Wi-Fi join still works |
| com.apple.searchd | 2026-10-27 | launched ~0.4 s before every Spotlight index extension (21 times in 30 min) | Spotlight screen still opens (Siri Suggestions + search field); search results not checked. dd2: 0 Spotlight extension launches, 0 of their crashes |

### Broken services to look at later
| Process | Symptom | Count | Notes |
|---|---|---|---|
| assetsd | SIGABRT at start (`reason ns 2 code 0x6`) | 88 / 30 min | disabled; Photos needs it for a real library. Backtrace in the live log (`CRASH assetsd bt ...`) |
| *SpotlightIndexExtension (Tips, PassKit, Contacts, MobileSMS, Music, Maps, iBooks, TV, Reminders) | SIGABRT ~40 ms after exec | ~85 / 30 min before searchd was dropped, 0 after (dd2) | launched via searchd (now disabled); why they abort is unknown |
| AegirPoster | crash loop (fixed 2026-10-29 by kernel95 sandbox extensions: 0 in an 11 min idle soak) | 487 in the long log | lock-screen poster (wallpaper) extension |
| FitnessWidget, com.apple.VoiceMemos, com.apple.podcasts (widget/extension) | crash loop | 9-10 / 30 min each in dd1, 0 in dd2 (15 min, searchd off), 160 in the long log | probably Spotlight-launched too (gone with searchd) |
| PhotosFileProvider, fileproviderd | occasional crash | 1 / 30 min each | |
| wifid | `apple80211: wifid gets 103 (CURRENT_NETWORK) ... error 6` | ~270 / h | ioctl not implemented in our Wi-Fi shim |
| wifid | `apple80211: wifid sets 383 ... REFUSED` | ~19 / 30 min | |
| SpringBoard / CA | `CATransformLayer ... allowsGroupOpacity in transform-only layer` | ~150 / h | log noise, probably harmless |

## Open
- /var/local/mtl2.log is 321 MB (2026-10-28), appended at every Metal client start, never rotated. Truncate now and then; rotate in the frontend (broken.md).
- Photos needs assetsd for a real library; why the Spotlight index extensions abort is unknown.
- Reduce Transparency: see metal.

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

### 2026-10-30 19:30 bl1: B47 not reproducible: the Control Center blur is there without any Turnip option
- bl1a (kernel126, no TU_DEBUG knob, backboardd restarted, 25 s): CC shows the blurred wallpaper; Utilities folder panel translucent with blurred icons/dock. gm0's "flat blue, no blur" was probably the first CC open before the wallpaper/blur was ready (guess).
- bl1b: per-process knob /var/local/mtl2-tu-debug.backboardd = sysmem (port fc0fef7; bridge log "MTL2[415 backboardd]: TU_DEBUG=sysmem" only for backboardd): identical look; fps (fpsbench 30 s home swipes) 20.3 -> 19.1 swaps/s, 19.4 -> 18.3 panel updates/s (-6%, one run each). Knob removed. B44 garbled CC tiles present in both.
- B47 closed. The per-process knob stays (useful for one-process Turnip A/Bs).

### 2026-10-30 19:00 tx1 + round 4: texture-upload census; row pitch is not the cause; B44 parked
- tx1 (kernel126, bridge a77f0f7 with /var/local/mtl2-b44log, backboardd restarted): 234 B44 lines (backboardd: linearTexture 121, texture 78, iosurface 9, replaceRegion 8, blitTexToTex 3, iosurfacePlanar 2, getBytes 1; SpringBoard: texture 7, replaceRegion 3, layoutUpload 2). Log: work/tx1-mtl2.log. Garbled areas as in gm0 (shots work/shots/tx1-*.png); sizes ~115x70 (CC glyph), ~90x70 (lock glyph), ~235x210 (VM icon), ~1030x680 (Tips banner).
- "linearTexture" = Apple SPI newLinearTextureWithDescriptor:offset:bytesPerRow:bytesPerImage: (NativeBridgeMetalFrontend.mm:321), same route as buffer textures: alias (DRM_FORMAT_MOD_LINEAR with Metal's exact row pitch) or, when Turnip refuses the pitch, the B29 shadow (optimal image + vkCmdCopyBufferToImage, bufferRowLength = row/bpp).
- Census of 384 B29 lines: every row%64 == 0 texture is an alias, every row%64 != 0 one is a shadow, without exception. Shadowed A8 (49, glyph atlas) render fine; shadowed RGBA8/BGRA8/RG8 (19) include the garbled art. So Turnip never picks its own pitch; my 64-B pitch guess is refuted.
- Left (not tested): misaligned buffer offsets (offset % texel size -> staging() path, buffer_texture.rs:1227), bytesPerImage ignored for slices (buffer_texture.rs:308-320). UBWC was already ruled out (round 2).
- Parked (about half a day over 4 rounds without a visible fix): backlog B44 has the next step (log offset % texel bytes for shadowed 4-byte textures; one screen per run to tie textures to items).

### 2026-10-30 18:20 gm0-gm2: Turnip `sysmem` and `flushall,syncdraw` don't fix the garbled images; `sysmem` brings back the Control Center blur
- Knob /var/local/mtl2-tu-debug (read once per process at launch, SweetMetal2.mm:213; was absent, removed again), backboardd restarted per run, kernel125, flame-shot of CC, Voice Memos, Tips, lock screen (work/shots/gm{0,1,2}-*.png).
- gm0 baseline: 6 CC tiles garbled (2 top-left circles, 3 bottom tiles, right tile of the flashlight row), Voice Memos welcome icon garbled, Tips banner garbled with black bands, both lock-screen buttons garbled; CC background flat blue (no blur).
- gm1 `sysmem` (no GMEM tiling) and gm2 `flushall,syncdraw`: the same areas stay garbled (Tips bands change colour/pattern). Both show the CC blur/vibrancy background, and the brightness slider looks dark/unfilled. So causes 1 (GMEM) and 2 (sync) are ruled out for B44; new finding: the missing blur depends on Turnip's render path (GMEM vs sysmem / flushes) -> backlog B47.
- No crashes seen (count 1123 at the end; not counted before). No obvious fps change.
- Next for B44: cause 3 (an unlogged upload path): log pixelFormat/size/bytesPerRow in newTextureWithDescriptor / replaceRegion / IOSurface texture creation and match the garbled sizes.

### 2026-10-30 17:45 B44 round 3 (read-only): not compressed textures; correction to rp1
- Correction to rp1 (13:00): "the repack fired 8 times, none in the apps" is wrong: 8 is only the log cap in bridge patch 0109 (repack_log, LOGGED >= 8); the repack ran for every shadow and still changed nothing.
- Log census (all ../logs/boots, ~60.7k "B29" lines): only uncompressed formats reach the buffer-texture path (A8, RGBA8, BGRA8; RGBA16F/RG8/BGR10XR as aliases), all drawn by backboardd; no ASTC/ETC/PVRTC anywhere (the bridge handles ASTC/ETC2 separately, NativeBridgeMetalFrontend.mm:1727-1870). Colours in the garbled art are right; only the geometry is broken.
- Ranked guesses: 1 Turnip GMEM tiling bug for Core Animation's offscreen image draws; 2 missing sync (texture read before its copy/render ends); 3 an unlogged path (IOSurface decodes, replaceRegion). Next: /var/local/mtl2-tu-debug = `sysmem`, then `flushall,syncdraw` (gm1/gm2), flame-shot Voice Memos, Tips, Control Center. Notes: research/notes/garbled-textures.md "Round 3".

### 2026-10-30 16:50 ext1: APP EXTENSIONS WORK (kernel123, fake taps + flame-shot, no reboot)
- Widgets (Today view): Notes widget shows real notes, Battery widget 100%; a third large widget stays a grey skeleton. Widget gallery ("+" in jiggle mode) lists Smart Stack, Notes, Weather (skeleton), Reminders, Calendar ("FRIDAY 30").
- Share sheet (from Notes): AirDrop (blank icon), Messages, Mail, Reminders, Copy, Markup, Print, Save to Filza, Save to Files.
- Files > Browse: iCloud Drive, On My iPhone, Recently Deleted, Tags; On My iPhone opens ("Folder is Empty"). No Photos location listed.
- Keyboards: Settings lists English (US) + Emoji; Notes keyboard has predictions; the emoji key switches to the Emoji keyboard and back.
- Notification Center swipe: cover sheet drawn (date, clock, "no notifications"), but its flashlight and camera buttons are garbled glyphs (likely B44 textures).
- Crash reports: 1123 before and after (0 new). Not checked: the PlugInKit/pkd oslog grep printed nothing at all (grep may not have matched). Photos library is empty, so PhotosFileProvider was not exercised. Screens: work/shots/ext1-*.png.
- Open: skeleton widgets (Weather needs Apple's signed service; the other unknown), cover-sheet button glyphs (B44), AirDrop icon blank.

### 2026-10-30 14:00 B44 round 2: UBWC off (TU_DEBUG=noubwc) doesn't fix it either; parked
- research/notes/garbled-textures.md "round 2": every garbled widget's texture takes the bridge's "-> shadow" path (row pitch not a multiple of 64 or offset 32: 600x600 RGBA8 offset 32 row 2400, A8 566x79 / 543x358 / 480x65 / 224x192, BGRA8 194x237 row 784, ...); no 16-bit/compressed formats; colours right (no channel swap); rows slope gently and break into dashes.
- /var/local/mtl2-tu-debug = noubwc + backboardd restart: Voice Memos' waveform icon still sheared dashes (work/shots/ub-VoiceMemos.png). Removed again.
- Ruled out so far: stale shadows (0108), the 0109 repack (fired only for 8 other backboardd textures: the garbled ones may use another path, e.g. offset-32 or IOSurface-backed textures it skips), UBWC. Next lead when resumed: why 0109 didn't touch e.g. the 600x600 offset-32 texture, and Turnip's per-line 2D copy for unaligned pitches on A6xx (tu_clear_blit.cc ~2811-2876). Parked (~45 min today).

### 2026-10-30 13:00 rp1: bridge 0109 (M2V_SHADOW_REPACK=1, acfbc93) does not fix the garbled images
- A/B with flame-shot (work/shots/rp1-A-*/rp1-B-*): Voice Memos header stripes, Compass needle, Mail banner, Control Center tiles identical; Tips hero banner as torn (B a bit worse). The repack fired 8 times, all in backboardd (180x180 4B row 720->768, 142x62 / 156x62 1B row 160->192), none in the apps. So unaligned shadow copies are not (or not the only) cause; the garbled images are app-side. Knob left off (mtl2-env empty); 0109 stays in the build, default off. B44 stays open (next idea: which format/path the apps' images take: compressed/ASTC, CGImage decode into IOSurface, 16-bit formats).

### 2026-10-30 12:00 sv1: app survey with screenshots (kernel121, flame-shot; work/shots/sv1-*.png)
- OK (real UI or first-run sheet): Safari (UI; last page can't reach 192.168.7.1), Settings, Reminders, Calendar, Camera (UI; preview black at 5 s: camera start takes 11 s, see camera.md), Maps, Clock, Calculator, Music, Podcasts, TV, News, App Store, Health, Wallet, Mail, FaceTime (sign-in), Fitness, Voice Memos, Compass, Translate, Find My, Shortcuts, Tips. EMPTY-OK: Notes, Photos, Contacts, Files (On My iPhone "Folder is Empty"), Messages.
- Broken: Measure (back to home within 5 s, no crash report; ARKit app), Watch/Bridge (home + "Watch would like to send notifications"), Magnifier (preview black: same 11 s camera start), Weather ("Cupertino --", no data).
- Garbled image areas (not icons): Maps (torn textures), TV (one icon), Mail banner, Voice Memos header (red/blue stripes), Compass needle/logo, Tips hero image (torn black line): looks like B29's texture shadows.
- No new crash reports during the survey. Location prompts (Reminders, Camera, Weather, Wallet, Compass, Find My) and Watch notifications were answered "Don't Allow"/"Not Now" by fake taps (device-changes.md).

### 2026-10-30 11:15 ext1: extension survey on kernel121: no extension dies any more; softwareupdated dropped
- 150 s `oslog -l -g sandbox` while using the widgets page, Photos, Files and Notes: "Terminating app due to uncaught exception" 0, "failed to get my own sandbox container" 0. Works: Today view widgets (Notes, Battery; a third large widget shows grey placeholder bars), the Notes keyboard, Files On My iPhone. Home-screen Photos widget "No content available" = empty library (chronod schedules it, no errors). Notification Center swipe showed the lock screen instead (gesture, not checked further). Share sheet not tested (no photos).
- New find: 796 OTAUpdate-*.ips restore reports, one every 2-5 min, from softwareupdated ("Cleaning up unused prepared updates", waiting for an IOKit service with EmbeddedDeviceTypeRoot, restore_error 60): job com.apple.mobile.softwareupdated dropped (next boot). Software Update can't work here anyway.

### 2026-10-30 11:00 sb5: "ON MY IPHONE" OPENS (kernel121 = xnu 0240 v5 + fpfix): "Folder is Empty", no error
- 0240 history: v1 (container from the spawn blob: xpcproxy passes none), v2 (HOME=...Containers/Data/... recorded at exec), v3 (lookup by pid only), v4 (exit notify kept), v5 (the later exec hook had wiped the entry the HOME scan had just stored: the clear moved to the start of the scan).
- sb5 (kernel121 HAPTICS=1 CAMERA=1 sweetse9=2 sweetcampmic=2, no reset): `sbcont 269` (Calculator) -> /private/var/mobile/Containers/Data/Application/91283062-E420-4FBF-8D0B-1CC8C6A50C30; kernel "op4 by sbcont for pid 269 (Calculator) -> 0"; 53 op4 lines, all Calculator/Files ones -> 0.
- Files: On My iPhone shows a folder view "Folder is Empty"; LocalStorageFileProvider (pid 322) launched and did not terminate (no "failed to get my own sandbox container"). PhotosFileProvider check inconclusive (no log lines in 15 s).
- Open: create/save a file there (U13 by the user or fake taps), PhotosFileProvider (B8), other extension types (widgets, share sheet, keyboards) now that op 4 answers.

### 2026-10-30 10:40 sb2 (kernel118, 0240 v2): the HOME fallback finds every container; op 4 still says ENOENT -> v3
- sb2: spawn blobs of the extensions (LocalStorageFileProvider 307/308/310, PhotosFileProvider 309/311, all ppid 1): "len 1104 profile yes container no", no strings past "plugin" @0x10 (the profile name): xpcproxy passes no container there. "HOME container" lines: LocalStorage -> .../PluginKitPlugin/85108458-61BF-4B47-BAAA-3A5F30781965, Photos -> .../PluginKitPlugin/A9E8417C-..., apps (Files 309A017E-...) -> Data/Application/<UUID>. op4: 23 lines, all early boot (xpcproxy/keybagd queries, ENOENT); none from the extensions; extensions still die, Files "Content Unavailable".
- By hand (new phone tool phone/tools/sbcont.c: sandbox_container_path_for_pid): self, Files (276) and Weather (247) -> ENOENT, and no op4 log line (the 32-line cap was likely used up by an early burst whose printf lines were partly lost). Reading: the lookup compared the uniqueid recorded at exec with the running proc's, which differs.
- v3: lookup by pid only (entries cleared at exit and at the next exec of the pid), "op4 pid N uniqueid A, stored B" logged on a mismatch, op-4 calls after 60 s logged too. kernel119 (first build skipped 0240: the regenerated patch had kern_exec.c as a new file; fixed).

### 2026-10-30 10:20 sb1 (kernel117 = + xnu 0240): no reset, but no spawn carried a container; extensions still die
- Boot kernel117 sweetse9=2 sweetcampmic=2 (4454 mV), SSH 17 s, UI up. "SWEET Sandbox: spawn blob" 16 lines, all "(launchd) len 1104 profile yes container no" (pids 222-261: the 16-line cap was used up by launchd's early spawns); "container for pid" 0.
- LocalStorageFileProvider[309] and PhotosFileProvider[313] still "Bootstrapping; failed to get my own sandbox container" + "invalid sandbox or container: error 2"; Files: "Content Unavailable"; fileproviderd "must have pid" x5.
- Next (0240 v2): log the blobs of extension spawns (strings + offsets), a fallback from the exec's HOME=/private/var/mobile/Containers/Data/... env string, and log each op-4 call.

### 2026-10-30 10:05 Cause of the extension deaths: our kernel's Sandbox op 4 (container path) always says ENOENT
- research/notes/files-on-my-iphone.md "extension sandbox container": PlugInKit -[PKService checkEnvironment:] (0x1aaf39c94, unconditional from _prepareToRun) calls sandbox_container_path_for_pid(getpid()) = __sandbox_ms("Sandbox", 4, {pid,0,buf,len,0}); on failure "Bootstrapping; failed to get my own sandbox container" + NSInternalInconsistencyException. Our permissive policy (sweet_amfi.c:425, patch 0050) returns ENOENT for op 4.
- On a real phone Sandbox.kext keeps the container from the spawn's sandbox attributes (+0x50). Fix being written: xnu 0240 remembers it at exec (mpo hook with macpolicyattr) and answers op 4; it should fix every PlugInKit extension (LocalStorage, PhotosFileProvider B8, likely widgets/share/keyboards).

### 2026-10-30 09:55 fp2/fp3: the LocalStorage extension starts and dies: "failed to get my own sandbox container"
- Filtered captures (`oslog -l -t 35 -n fileproviderd / -n LocalStorage / -n Files`; the unfiltered oslog dies with SIGSEGV on the PhotosFileProvider crash backtrace) while opening On My iPhone and tapping Try Again: LocalStorageFileProvider[751] "Hello, I'm launching as euid = 501, uid = 501, personaid = 1000", "Bootstrapping; failed to get my own sandbox container", "*** Terminating app due to uncaught exception 'NSInternalInconsistencyException', reason: 'plugin has invalid sandbox or container: error 2 path ?'". fileproviderd: "Plugin ... must have pid! Extension request will fail", "Failed to aquire assertion for plugin: ... pid: 0", NSCocoaErrorDomain 4097. PhotosFileProvider dies the same way (the long-known B8 crash).
- So the remaining Files blocker is generic: app extensions can't learn their own sandbox container (probably also B8 and other extensions). Investigator on the container lookup path.
- The storage dir exists: .../AppGroup/E1DA353E-.../File Provider Storage (mobile:mobile, empty).

### 2026-10-30 09:45 hp5: "ON MY IPHONE" IS LISTED (fpfix via the sh exec wrapper, kernel116); opening it gives "Content Unavailable"
- kernel116 HAPTICS=1 CAMERA=1 sweetse9=2 sweetcampmic=2. fileproviderd starts through the wrapper (kill -> one restart, no crash loop). The oslog capture showed no "fpfix"/"not eligible" lines either way (os_log from the constructor not seen).
- fpdump: LocalStorage "isEnabled=1 ... supportsEnumeration=1 ... isConnectedToAppExtension=1", storage .../Containers/Shared/AppGroup/E1DA353E.../File Provider Storage (no --enable needed). Files > Browse > Locations: On My iPhone, iCloud Drive, Recently Deleted.
- Tapping On My iPhone: "Content Unavailable - The folder contents could not be displayed because of an unknown error." (Try Again). Next: the enumeration error (LocalStorageFileProvider / fileproviderd log while tapping): probably an APFS-only call on HFSX.

### 2026-10-30 09:30 fpfix (2f74fc1): launchd drops DYLD_* from the job env; a sh exec wrapper instead
- hp4: with EnvironmentVariables DYLD_INSERT_LIBRARIES on com.apple.FileProvider, fileproviderd (pid 396) still logs "Volume /dev/ufsroot1 not eligible to store FP library, skipping", no "fpfix" line; fpdump: LocalStorage isEnabled=0, --enable -> NSCocoaErrorDomain Code=4. Files Browse: no Locations at all.
- By hand: DYLD_INSERT_LIBRARIES works for our tools and for fileproviderd started from a shell (DYLD_PRINT_LIBRARIES shows /private/var/local/lib/fpfix.dylib loaded right after the main image). So launchd strips DYLD_* from the job's environment.
- tools/sunfish-install-fpfix.sh now sets ProgramArguments ['/bin/sh','-c','DYLD_INSERT_LIBRARIES=/var/local/lib/fpfix.dylib exec .../fileproviderd'] (exec keeps the pid for the MachServices check-in); installed, takes effect at the next boot.

### 2026-10-30 08:05 phone/tools/shot (iOSPlus10 negroni-shot port, 52ad05b): wrong image
- hp1: `tools/sunfish-shot.sh hp1` -> "1125x2436" (the fallback size: CADisplay mainDisplay bounds not read) and a PNG with a blue gradient block (~710x368) on white, not the lock screen. D7 stays open; webcam meanwhile.

### 2026-10-30 07:30 U13 by Claude (fake taps + webcam, boot sp16r): Files still has no "On My iPhone"
- Browse > Locations lists only "Recently Deleted" (plus Tags); no On My iPhone, no iCloud Drive. `lsreg -i com.apple.FileProvider.LocalStorage`: dataContainerURL .../PluginKitPlugin/85108458-61BF-4B47-BAAA-3A5F30781965 (the link survived the reboot).
- oslog -l across a Files relaunch: Files[558] "Wanted to use com.apple.FileProvider.LocalStorage, but it isn't available" (and the same for CloudDocs); pkd "Candidate plugin count from LaunchServices: 1".
- So the files7 state (pkd + fileproviderd accept LocalStorage after a restart) does not hold after a reboot. Next (als1, in progress): restart pkd + fileproviderd + Files on a fresh boot and look again.
- tools/flame-shot gives skewed/garbage images on this boot (default and explicit 0x9c400000 1080x2280 stride 4320); the webcam is readable at minimum brightness (image rotated 90 degrees). D7 added.

### 2026-10-30 04:00 Correction to files6/7
- PluginKitPlugin/85108458-61BF-4B47-BAAA-3A5F30781965 has mtimes 1969-12-31 (from the root image): it existed all along; the `lsreg -r` after the clean files6 boot only linked it into the LaunchServices record. So earlier rebuilds missed it (they may have run with containermanagerd busy/wedged, or before it was up), and the hand-made container was not needed. Setup step: a full `lsreg -r` on a settled boot.

### 2026-10-30 03:55 files6/7: LocalStorage gets a real data container; pkd accepts it; Files' list unverified (night)
- files6 boot (kernel111 CAMERA=1, 4438 mV): `lsreg -i` still (none) after boot; `lsreg -r` (63 s, "rebuild -> 1") then gave dataContainerURL /private/var/mobile/Containers/Data/PluginKitPlugin/85108458-61BF-4B47-BAAA-3A5F30781965 (made by containermanagerd: metadata 638 B; not the hand-made 718EBA02, which is now deleted).
- files7 (pkd + fileproviderd + Files restarted): pkd: no "mis-configured" / "negative cache add" lines any more; fileproviderd discovers com.apple.FileProvider.LocalStorage (FPDLocalStorageExtension created, "default provider remains com.apple.FileProvider.LocalStorage"), "returning 3 providers"; Files' log has no "isn't available" line any more. LocalStorageFileProvider not running yet (it runs on browse). Files' Browse list not readable by the webcam at night brightness: user check (U13).
- Reproduce (setup step): after a boot, `lsreg -r` once (makes the missing plug-in data containers), then restart pkd/fileproviderd or reboot. Whether the hand-made container was needed is unknown (containermanagerd was also wedged before the reboot).

### 2026-10-30 03:35 files5b: hand-made data container for LocalStorage (not yet picked up)
- Copied PhotosFileProvider's container layout to PluginKitPlugin/718EBA02-812E-42A0-B1C1-CCEE1BC84BB9 with a metadata plist naming com.apple.FileProvider.LocalStorage (content class 4, uid/gid 501, no ParentBundleID). `lsreg -p`, then `lsreg -r` (rebuild -> 0): `lsreg -i` still dataContainerURL (none); pkd still "no sandbox data container". containermanagerd (process name "containermanager", pid 69) ignored SIGTERM (CPU rose 25.7 -> 32.2 s, then idle) and an lsreg call waited on it for ~1 min. Next: reboot so containermanagerd indexes the container at start (files6).

### 2026-10-30 03:20 files5: the data container can't be made from our tools: containermanagerd gets no code-sign identifier (lsreg 0bfc13d)
- `lsreg -i com.apple.FileProvider.LocalStorage`: dataContainerURL (none), containingBundle (none); PhotosFileProvider for comparison: /private/var/mobile/Containers/Data/PluginKitPlugin/A9E8417C-..., containing com.apple.mobileslideshow.
- `lsreg -c com.apple.FileProvider.LocalStorage <appex>` (MCMPluginKitPluginDataContainer +containerWithIdentifier:createIfNecessary:existed:error:), as uid 501 and as root: MCMErrorDomain 98. containermanagerd: "Could not get code sign identifier from audit token to initialize a client object for pid [681]", "Error creating client identity ... (98|2|0)". Our tools are ldid -S ad-hoc signed (identifier = file name), so the kernel likely doesn't hand out an identity for them (csops CS_OPS_IDENTITY / audit-token signing id).
- Options: (a) a kernel fix so ad-hoc /var/local/bin binaries report their signing identifier (helps every tool that talks to MCM / identity-checking daemons); (b) a hand-made plug-in data container (copy Photos' container layout + metadata plist, owner mobile) and re-register; (c) do the MCM call from a trusted process. Being scoped (research/notes/files-on-my-iphone.md).

### 2026-10-30 03:00 files4: the reason: "this plug-in has no sandbox data container" (oslog fixed, a671ac2)
- oslog no longer exits when its stream stops (reconnects until -t; SIGSEGV handler; B31's early stops = the old exit-on-STOPPED). `oslog -l -n pkd -t 70` across a pkd restart: pkd[653] "rejecting; Ignoring mis-configured plugin at [/System/Library/Frameworks/FileProvider.framework/PlugIns/LocalStorageFileProvider.appex]: this plug-in has no sandbox data container." then "negative cache add"; after it "Candidate plugin count from LaunchServices: 7"; fileproviderd "returning 2 providers".
- Cause: no data container for com.apple.FileProvider.LocalStorage (our /var never went through MobileInstallation, so containermanagerd never made one). Fix being written: `lsreg -c BUNDLEID APPEX` (create the plug-in data container via MobileContainerManager, re-register so LaunchServices records it) and `lsreg -i` (read-only check).

### 2026-10-30 02:50 files3: pkd drops LocalStorage with a "negative cache hit"; parked
- fp5 (oslog -l while Files + fileproviderd restart, work/files3/fp5-all.log, research/notes/files-on-my-iphone.md): fileproviderd asks PKHost for com.apple.fileprovider-nonui; pkd[184] "Will enumerate 8 candidate plugins", candidate 1 "negative cache hit", "Candidate plugin count from LaunchServices: 7", "Final plugin count: 7"; fileproviderd's "discovered extensions" list has the 7 others (Photos, CloudDocs x4, SMB, UserFS), NOT com.apple.FileProvider.LocalStorage; "found no value for key fpfs-providers / fpfs-disabled-providers / provider-filter"; "default provider remains com.apple.FileProvider.LocalStorage"; "returning 2 providers"; Files "Wanted to use com.apple.FileProvider.LocalStorage, but it isn't available". Side: "/dev/ufsroot1: non-apfs ... not eligible to store FP library".
- pkd kill -9 (pid 184 -> 599), Files + fileproviderd restarted: LocalStorageFileProvider still never runs. No kernel AMFI/Sandbox lines for pkd. oslog -l stopped after ~7 s in both later captures (B31-like), so pkd's reason wasn't caught.
- Parked. Next: a pkd capture that survives (oslog -n pkd started right after pkd's restart, or several short captures), grep "negative|reject|invalid|sign|entitle" + the bundle id; suspects: pkd's on-disk plug-in database / annotation, the appex's signature/entitlement check inside pkd, or the non-APFS data volume.

### 2026-10-30 02:35 files2: LaunchServices does know the LocalStorage extension; fileproviderd leaves it out
- exq (installed to /var/local/bin/exq from phone/tools/out): com.apple.fileprovider-nonui has 8 records incl. com.apple.FileProvider.LocalStorage -> FileProvider.framework/PlugIns/LocalStorageFileProvider.appex (191 app / 517 extension records). `lsreg -p` on it -> 0 = success (files1's "-> 0" was success too, misread). lsd logs "_LSServerRegisterItemInfo begin", no refusal; AMFI trusted. So the 3-provider answer is fileproviderd's own filter; reading work/files1/fp-fileproviderd.log for the reason.
- On-device: /var/local/bin/exq (phone tool).

### 2026-10-30 02:20 files1: Files has no "On My iPhone": the LocalStorage provider isn't registered
- The extension exists (/System/Library/Frameworks/FileProvider.framework/PlugIns/LocalStorageFileProvider.appex) and its group container group.com.apple.FileProvider.LocalStorage exists; /var/mobile/Library/Application Support/FileProvider holds only LegacyEnabledStateHasBeenMigrated (re-made at boot); /var/db/PlugInKit-Annotations has no LocalStorage entry; 0 execs of LocalStorageFileProvider in 137 logged boots.
- oslog while opening Files (openapp com.apple.DocumentsApp; logs in work/files1/): Files "Wanted to use com.apple.FileProvider.LocalStorage, but it isn't available"; fileproviderd "providerDomainsCompletionHandler: ... returning 3 providers"; pkd enumerates 1 com.apple.fileprovider-ui plugin.
- `lsreg -p <appex>` -> registerPlugin: NO. On-device change: none that took (registration failed); fileproviderd and Files were restarted. Research (research/notes/files-on-my-iphone.md) on how framework plug-ins get into LaunchServices and whether our `lsreg -r` rebuilds dropped them.

### 2026-10-30 03:50 B35 Location Services row: gate found, hook not loaded (b35, kernel98)
- Gate (research/notes/b35-location-row.md, PrivacySettingsUI disassembly): -[PUIPrivacyController specifiers] removes the "LOCATION" row unless refreshLocationServicesAvailable cached YES; that method asks CoreTelephony getSubscriptionInfo, which fails without CommCenter (removed from launchd.plist since image v13) -> nil -> row hidden.
- SweetMetal2 hook (Preferences: refreshLocationServicesAvailable -> cache @YES, via a dyld add-image callback): no "b35:" line at all in mtl2.log, so the SweetMetal2 bundle never loads in Preferences (it only loads in Metal clients). Row still missing.
- Parked: cosmetic (Location Services itself is on). Needs a way to inject into Preferences (e.g. DYLD_INSERT_LIBRARIES for that app) or a CommCenter stub that answers getSubscriptionInfo with zero subscriptions.

### 2026-10-30 00:20 Auto-Lock Never; last Settings panes (tap4, kernel98)
- Auto-Lock set to Never by fake taps (was 1 minute). Battery pane fine (100 %, graph); Wallpaper pane opens (previews washed out white); Privacy & Security opens but has NO Location Services row (list starts at Tracking) -> B35.
- `openapp` was ignored twice with the phone unlocked and Settings in front (B34); earlier in a boot it works. Injected home swipes from Settings only scrolled the list (B33 stays open).
- Tap y on the Settings list ran ~0.05 higher than predicted from the frame; aim at the row's centre.

### 2026-10-29 23:55 Settings panes by fake taps (tap3, tap3b; kernel98)
- tap3 got stuck (Notes left in landscape with the keyboard, `openapp` ignored); a fresh boot cleared it. tap3b: `openapp` is ignored while the screen is locked (Auto-Lock is 1 minute); `W 0.5 0.99 0.5 0.5 600` goes from the lock screen to the home screen.
- Settings rows (portrait y): Wi-Fi 0.37, Bluetooth 0.42, Sounds 0.57, General 0.78, Display & Brightness 0.89; back = `T 0.08 0.075`.
- Wi-Fi pane fine (joined network, list); Bluetooth "Unavailable" (B15, expected); General > About fine (iOS 16.7.16, iPhone X, serial N/A); Display & Brightness fine; Sounds fine except one garbled row label. Panes are slow to appear. Battery, Location Services, Wallpaper not reached (locked).
- Next: Auto-Lock -> Never (Display & Brightness, by taps) so tests don't lock; then the 3 missing panes and the home swipe from inside an app.

### 2026-10-29 23:40 Tap survey (B26) with fake taps (tap2, kernel98)
- Landscape touch works: after `R 3`, `T 0.42 0.69` (portrait coords where the landscape "5" sits) typed "5" first try; no `t` touch-mapping step needed.
- Notes: Welcome -> Continue -> iCloud alert (first "Not Now" tap ignored, second dismissed it) -> compose -> keyboard shows; two taps on "a" typed "Aa". Keyboard typing works.
- Control Center (swipe down from top-right): opens; media, volume, brightness, orientation lock, mirroring, Focus, flashlight, bell, timer fine; five tiles (connectivity, camera, ...) render as static noise (same family as B29 icon noise).
- Swipe down from top-left: the cover sheet (lock-screen look with Notification Center); iOS 16 behaviour, two bottom-corner icons are noise (B29).
- Injected home swipe (`W 0.5 0.995 0.5 0.6 250`) did not leave the app: unknown whether the gesture needs a slower/edge-flagged swipe or the real gesture is broken (not tested by finger).

### 2026-10-29 23:20 Fake taps work: `T X Y` / `W ...` (tap1, kernel98; SweetMetal2 touch commands)
- backboardd dispatches IOHID digitizer events (hand + finger child) through an admin IOHIDEventSystemClient, sender = the touchscreen service (0x100000126).
- tap1: Calculator `T 0.38 0.75` -> "2", `T 0.62 0.75` -> "23"; Settings `W 0.5 0.8 0.5 0.3 300` scrolled the list to Battery..FaceTime. Landscape tap after `R 3` at 0.5,0.5 hit a gap (inconclusive). Location Services stayed on after a reboot (Maps: no alert). No reset; 4438 mV.
- Unblocks B26 (tap-only survey paths) and U6 (landscape touch) without a finger.

### 2026-10-29 22:40 B32 Location Services on via `L 1` (loc1, loc2; kernel98)
- loc1: plist edit `LocationServicesEnabledIn7.0`=true in /var/root/Library/Preferences/com.apple.locationd.plist (backup .bak-b32) + locationd restart (kill; no launchctl on the phone): no effect, Maps still showed "Turn On Location Services".
- New SweetMetal2 command `L 0|1` (SpringBoard calls +[CLLocationManager setLocationServicesEnabled:]). loc2: `orient: L: class 0x2174c8528 set 1, enabled 0 -> 1`; Maps opened to "What's New in Maps" with no Location Services alert (loc2-a). The root plist still has no Enabled key (the state lives elsewhere).
- Open: survives a reboot? Maps' own "Allow location" prompt needs a tap; no position source yet (no GPS driver, Wi-Fi location needs Apple servers).

### 2026-10-29 10:20 kernel95 idle soak: 0 crashes, AegirPoster and App Store fixed (k95idle; ssh up 18 s, 4455 mV)
- 11 min untouched, whole boot (to 719 s): 0 CRASH lines (kernel94 boots today: 33-66, mostly AegirPoster). AegirPoster exec'd at 387.9 s ("not listed ... no Metal device") and did not abort.
- Lock screen: clock and date clean; the two buttons still noisy (B29).
- App Store: "Welcome to the App Store" with Continue (was "LOADING" forever). Weather: opens, "Turn On Location Services" dialog.
- Sandbox issues logged (cap 16): com.apple.app-sandbox.read-write x11, kTCCServiceAddressBook x4, StreamingUnzipService x1. 5 "Failed to send exception EXC_CORPSE_NOTIFY" (ReportCrash disabled).

### 2026-10-29 10:10 Photos works: assetsd re-enabled on kernel95 (ph1)
- Job cache: assetsd entry copied back from launchd.plist.pre-drop (+ StandardErrorPath /var/local/assetsd.err); nebulad stays dropped. Backup: phone /System/Library/xpc/launchd.plist.pre-photos, local work/userland/launchd.plist.pre-photos; new: work/userland/launchd.plist.photos.
- ph1 (kernel95; ssh up 17 s, 4455 mV): 0 CRASH assetsd (assetsd.err: only CoreData first-run index setup), Photos opens to "No Photos or Videos" and stays up 30 s, 0 CRASH MobileSlideShow. Safari fine.

### 2026-10-29 10:05 Music works: kernel95 emulates sandbox extensions (sb1; xnu 0219, default args)
- xnu 0219 (sweet_amfi.c): __mac_syscall("Sandbox") op 5 (issue) copies out a well-formed token (< 0x800, class + data), op 6 (consume) returns handle 1, 2, ...; ops 4 / 0x43 still ENOENT. Boot-arg `sweetsbext=0` restores the old ENOENT. ABI notes: research/notes/sandbox-extension-abi.md.
- sb1 (kernel95; boot 6 s, ssh up 17 s, 4455 mV): no panic; lock screen and Safari fine. Issues seen (first 16): nsurlsessiond com.apple.StreamingUnzipService, tccd kTCCServiceAddressBook x5, cloudd com.apple.app-sandbox.read-write x9.
- Music: opens to "Listen Now" / "You're Offline" and stays up (webcam sb1-m5, sb1-m15), 0 CRASH Music. Was SIGSEGV on launch.
- oslog itself crashed (sig 11, far 0x10) during the capture; the capture held no tccd lines.

### 2026-10-29 10:00 TCC works, sandbox extensions don't: shared cause of Music and Photos (b24e, b24f; kernel94)
- Photos with MobileSlideShow in mtl2-clients: same SIGABRT (same pc/lr/far) as without; not Metal. Reverted.
- b24f: assetsd re-enabled for one boot with StandardErrorPath /var/local/assetsd.err: `*** Terminating app due to uncaught exception 'NSInternalInconsistencyException', reason: 'assetsd requires TCCPhotosService access'` (12 assetsd crashes, 4 MobileSlideShow). Job cache restored from launchd.plist.pre-assetsd (applies next boot). /var/mobile/Media/PhotoData has only Thumbnails/.
- Music (b25 capture): tccd `Granting ... com.apple.Music ... kTCCServiceMediaLibrary via entitlement 'com.apple.private.tcc.allow'`, AUTHREQ_RESULT authValue=2 authReason=11, then `Failed to issue sandbox extension for class: com.apple.tcc.kTCCServiceMediaLibrary`. The client then reports not authorized.
- Cause: our Sandbox MAC policy stub (xnu patch 0050, sweet_amfi.c:292-310) returns ENOENT for __mac_syscall("Sandbox") op 5 (extension issue) and op 6 (consume). Next: emulate issue/consume in the kernel (ABI research running). Likely unlocks Music, Photos (assetsd) and any other TCC-gated service.

### 2026-10-29 09:50 B24: black apps re-checked on a fresh boot (b24d, kernel94; ssh up 17 s, 4456 mV)
- Notes: works ("Welcome to Notes", drawn correctly; `MTL2[MobileNotes]: not listed` = CPU rendering is fine). Wallet: works (empty-state screen). Their black screens in the survey came from the session state, not the apps.
- Photos: SIGABRT at launch (74.621, reason none) right after `exec fileproviderd`; assetsd is disabled (crash loop, 2026-10-27), likely the cause.
- Camera: black, alive, no crash (no camera stack, B17). oslog itself crashed (sig 11) during that capture.
- openapp fails with FBSOpenApplicationServiceErrorDomain Code=5 after an app is kill -9'd or while Maps' Location alert is up; a reboot clears it.

### 2026-10-29 09:40 B24: Maps opens (b24a-c, kernel94; bridge 2250f8f, e1ab0a8, 91b7ee2)
- b24a (Maps listed in mtl2-clients): device granted, then SIGABRT 0.1 s after the first draw right after `MISSING NBRenderEncoder -setDepthClipMode:`. No side effects (lock clock, Safari fine, 0 UIWDT/hangs).
- Added to NativeBridgeMetalFrontend.mm: setDepthClipMode: (Clip = default, Clamp logged and ignored: no depth clamp in the runtime), useResources:/useHeap:/useHeaps:/memoryBarrierWithScope:afterStages:beforeStages: no-ops, setScissorRects:/setViewports: (first entry only).
- b24b: next abort on `MISSING NBLibrary -newFunctionWithName:constantValues:pipelineLibrary:error:`. Added it (forward, pipelineLibrary ignored) plus newFunctionWithDescriptor:/newRenderPipelineStateWithDescriptor:/newComputePipelineStateWithFunction: completionHandler: forwards.
- b24c (ssh up 17 s, 4455 mV): Maps runs, 0 crashes, 0 MISSING; draws "What's New in Maps" + "Turn On Location Services" alert (webcam b24c-3). One `native-metal: texture refused: texture: OutOfMemory(Device)`; one feature icon on the sheet is noise (like B29). Map tiles not seen yet (sheet in front, no taps).
- Maps stays in mtl2-clients; tools/sunfish-metal2-install.sh now writes Maps too. Still missing selectors: setTessellationFactorBuffer, setVertexAmplificationCount:viewMappings:, setColorStoreAction:atIndex:, setDepthStoreAction:, newDynamicLibrary:.

### 2026-10-29 09:35 B29: shadow refresh switch, no change; parked (b29c, kernel94, bundle 561e6cd)
- shadow.rs: every encoder starts with all shadows stale (new ShadowSync per encoder), so each encoder copies buffer -> shadow before first use; nothing CPU-side marks entries stale.
- Bridge 0108 `M2V_SHADOW_REFRESH=always` (copy-in before every use) set in /var/local/mtl2-env: backboardd logged the switch, the two lock-screen buttons still noisy (webcam b29c), clock clean, no UIWDT/hang. Switch removed again (mtl2-env empty, as before).
- Parked. Open leads: CA writes after the command buffer ran (race), the image sampled with no copy-in in that encoder, or the icons are not these shadow textures at all (only backboardd logs buffer textures; SpringBoard none).

### 2026-10-29 09:25 B29: lock-screen button noise, storage of buffer textures (b29a, b29b; kernel94; bundle f6550d2)
- a980246 logs (bytesNoCopy / IOSurface, 1-byte formats): 0 lines; the textures come through `-[MTLBuffer newTextureWithDescriptor:offset:bytesPerRow:]`.
- f6550d2 (bridge 0107) logs each buffer texture's storage: backboardd only, 32 lines: 17 wider alias (e.g. 1125x2436 BGRA8, RG8 23x23/32x40 row 64, A8 rows 64-aligned), 15 shadow. Shadows: RGBA8 101x101 / 103x103 / 97x76 row 416, BGRA8 194x237 r784, 1x1, A8 216x61 r224, 463x54 r480, 712x234 / 732x174 / 736x234 r736, 981x245 r992, big BGRA8 1326x1326 / 1271x1823 / RGBA8 1105x1160. Reason: no-copy buffer, Turnip refuses the row pitch (needs 64; host-allocation linear needs 256).
- Copy regions and Turnip's row copies are correct (investigator). Hypothesis: CA writes the icon pixels on the CPU after the shadow was filled and the tracker does not see it -> stale/uninitialised shadow. Next: M2V_SHADOW_REFRESH=always test switch.
- Lock-screen clock clean on both boots (AegirPoster not listed); the two buttons noisy.

### 2026-10-29 09:20 B25: Music crash (b29a boot, kernel94)
- Disassembly: `_MediaLibrary_coreLibrary` does `[[self databasePath] lastPathComponent] UTF8String` -> std::string; databasePath (MPMediaLibrary -> MPMediaLibraryDataProviderML3 -> ML3MusicLibrary) is nil. Listings: research/re/MediaPlayer.s, MusicLibrary.s.
- Present on the phone: /etc/passwd mobile (home /var/mobile), /var/mobile/Media/iTunes_Control/iTunes/MediaLibrary.sqlitedb (532 KB), medialibraryd and tccd running.
- oslog at launch: `-[MPMediaLibrary initWithClientIdentity:userIdentity:]: app not authorized for access`, then `<MPMediaLibrary> [nil] - Singleton`, crash 1 ms after the first library request. Account DSID (null) everywhere. No medialibraryd / ML3 log lines.
- TCC.db had no kTCCServiceMediaLibrary rows. Added (com.apple.Music, auth_value 2, reason 4) on a host copy (WAL checkpointed), swapped in with tccd stopped, tccd restarted. Music still crashes (578, 591, 616, 638 s). Backup: /var/local/tcc-pre-b25/ (TCC.db + wal + shm). Row kept (harmless).
- Parked: next would be the authorization check itself in MediaPlayer (which call answers "not authorized": TCC preflight vs an entitlement / client-identity check).

### 2026-10-29 09:30 B28: AegirPoster crash loop vs mtl2-clients (b28a, kernel94, default args)
- Symbolized (b19a, 66 crashes 305.8-626.5 s, all the same bt): uncaught exception on an FBSSerialQueue block -> std::terminate -> _objc_terminate -> abort. 60 ms before each: `MTL2[pid AegirPoster]: not listed (/var/local/mtl2-clients): no Metal device`. Pairs ~0.4 s apart every ~10 s; starts at the idle auto-lock (~300 s) and stops on its own (~626 s).
- b28a (AegirPoster added to mtl2-clients, reboot, 11 min untouched; ssh up 17 s, 4456 mV): 2 crashes (sig 9, 310.0 / 315.1 s), then none to 756 s. Device created ("_MTLDevice is 528 bytes") but many "no device answer" queries. 0 UIWDT, 0 GPU hangs.
- But the lock-screen clock rendered as noise (webcam b28a vs a clean "8:16" on mc2); the two bottom lock-screen buttons are noise in both. Reverted mtl2-clients to SpringBoard, com.apple.WebKit.GPU, MobileSafari (backup /var/local/mtl2-clients.pre-b28 = same).

### 2026-10-29 09:05 B19: three looping jobs dropped (b19a, kernel94, default args)
- Analysis (mc1 boot, ~620 s): lskdd 215 execs (1 s, from 400 s, ThrottleInterval 0), fairplayd.H2 59 (10 s, KeepAlive), bluetoothd 54 (10 s, no controller), wifip2pd 48 (10 s, from the Wi-Fi join), xpcproxy 633. None crash; they exit on their own (no exit status in the kernel log).
- Dropped lskdd, fairplayd.H2, wifip2pd (tools/sunfish-launchd-drop.sh). Backup: phone /System/Library/xpc/launchd.plist.pre-b19, local work/userland/launchd.plist.pre-b19. bluetoothd kept (Settings BT pane, BT audio HAL plugin; re-check with B15).
- b19a (first 620 s): lskdd 0, fairplayd.H2 0, wifip2pd 0, bluetoothd 60, xpcproxy 314 (was 633). ssh up 17 s, 4455 mV. Wi-Fi joined (87.5 s wland link up). Settings and Safari draw (webcam).
- Crashes: AegirPoster sig 6 x66 from 305.8 s (known intermittent loop, 4427 in the archived logs, 0 in mc1); MusicWidgets sig 11 x5; IntentsExtension, MobileTimerInten, WeatherIntents sig 6 x1 each. Not linked to the drops (guess; AegirPoster predates them).

### 2026-10-29 08:45 Black apps vs mtl2-clients (mc1, mc2; kernel94, default args)
- Process names: MobileNotes, MobileSlideShow, Camera, Passbook, Maps. mtl2.log before: `MTL2[277 Maps]: not listed (/var/local/mtl2-clients): no Metal device`; no lines at all for the other four.
- Relaunch test on the survey boot: after `kill -9` of MobileNotes, `openapp` failed for every app (FBSOpenApplicationServiceErrorDomain Code=5, XPC error on reply); screen black. Not a good relaunch method.
- mc1 (all five added to mtl2-clients, reboot; ssh up 17 s, 4456 mV): Notes still black, and no MTL2 line for MobileNotes/MobileSlideShow even when listed (they never ask the bridge for a device). Display stopped updating; at 125.677 the UI watchdog killed backboardd ("no swap for 30 s with touch input"; GPU pool 93 MB held, peak 115 MB), SpringBoard restarted (pid 27 -> 365); later openapp calls failed with Code=5. No GPU fault, no kgsl/VK errors.
- So: listing does not fix Notes/Photos (they do not use the bridge themselves); Maps does want a device (untested alone). Reverted mtl2-clients to SpringBoard, com.apple.WebKit.GPU, MobileSafari (backup /var/local/mtl2-clients.pre-survey).
- mc2 (reverted, reboot; ssh up 18 s, 4455 mV): lock screen and Safari draw normally.
- Music: /var/mobile/Media/iTunes_Control/iTunes already exists (mobile 755) and medialibraryd runs (uid 501); so neither is the NULL. No launchctl on the phone.

### 2026-10-29 08:20 App survey (kernel94, d2b boot; `openapp`, webcam, psx)
- 17 of 18 apps launch. Render normally: Settings main list, Safari (example.com over Wi-Fi), Calendar, Clock (Alarm tab), Messages, Mail, Calculator, Files, Phone, Contacts, Health.
- Black screen but alive: Notes, Photos, Camera, Wallet. Grey blank: Maps. Guess was mtl2-clients: wrong for the four (see 08:45 entry).
- Music: SIGSEGV (sig 11, far 0x0) on launch at 294.142 s. Symbolized (ipsw a2s): _platform_strlen <- std::string(const char*) <- -[MPMediaLibrary(MediaLibraryCore) _MediaLibrary_coreLibrary]+140 <- MPMediaLibraryView initWithLibrary <- MPModelLibraryRequestOperation. A NULL library path/identifier (guess: no iTunes_Control library or medialibraryd).
- App Store: "LOADING" spinner only. Weather: "Cupertino --", no data (expected without location/iCloud).
- Not run (no taps): Settings sub-panes, Notes keyboard, Notification Center, Control Center.

### 2026-10-28 20:15 CocoaTop (third-party process viewer) installed (ct1, kernel94)
- Built on Linux from github.com/D0m0/CocoaTop (bf193b8) with tools/build-cocoatop-linux.sh: no Xcode, so loose PNG icons instead of the asset catalog, UILaunchScreen instead of the storyboard, an xpc.h stub (the theos 16.5 SDK has none); ldid with the app's own entitlements (task_for_pid-allow, no-sandbox, platform-application, persona-mgmt). Out: work/cocoatop/CocoaTop.app.
- Installed to /Applications/CocoaTop.app (tar over sunfish-ssh, root:wheel, sha1s match). `lsreg APP` returned 0 and did nothing (as on flame); `lsreg -r` (rebuild -> 1) registered it as a System app `ru.domo.cocoatop64`.
- `openapp ru.domo.cocoatop64`: the process list renders (246 processes, Free 1573.5 MB, CPU 1.7%; webcam ct1 launch2.jpg); no mtl2-clients entry needed. CocoaTop-helper (root via persona spawn) exec'd 65 ms after the app; one line `SWEET Sandbox: CocoaTop privilege 1002 denied`, effect unknown. Home-screen icon not seen yet (frame showed the App Library).

### 2026-10-28 13:55 PhotosFileProvider sig 6: symbolized
- Same backtrace in every crash (slides differ): ___pthread_kill, _pthread_kill, abort, demangling_terminate_handler, _objc_terminate, std::__terminate, objc_exception_throw, __CFDictionaryCreateGeneric, PlugInKit func_1aaf39c64+248. An uncaught exception while PlugInKit builds a CFDictionary (a nil value, likely). Impact: 2-4 crashes per 2 h, the extension restarts. Fix deferred.

### 2026-10-28 02:05-03:15 overnight run, part 3: the GPU pool leak
- See [gpu-memory](gpu-memory.md) for this entry; it also touches this feature.

### 2026-10-28 00:00-02:10 overnight run, part 2 (crash counts from sk68)
- sk68 2 h exit soak (kernel68, /var on ufsroot1, 15 chunks, 120.7 min): 408/408 app opens, 0 resets, 0 GPU hangs, 0 SMMU faults, 0 UIWDT, CRASH only PhotosFileProvider x4 (AegirPoster 0). Pool 192 -> 709 peak -> 631 MB end: "pool flat" not met. Battery 4431 -> 4429 mV, 0 mA, 100% (full on the charger; discharge still untested).

### 2026-10-28 experiments.md lines
- (see [gpu-memory](gpu-memory.md), experiments line for 2026-10-28) 2026-10-28 sk68: kernel68 (/var on ufsroot1), 2 h app soak (15 chunks sk68a-n, 120.7 min, 6 apps): 408/408 opens, 0 resets, 0 hangs, 0 SMMU faults, 0 UIWDT, 0 pool warnings; CRASH only PhotosFileProvider x4 (AegirPoster 0); pool 192 -> 709 peak -> 631 MB end, drifting up (exit criterion "pool flat" NOT met); battery 4431 -> 4429 mV, 0 mA, 100% on screen (on charger, full: discharge still unchecked); swaps 12.8/s, panel 5.7/s mean (idle-diluted); df /private/var 3.66 GB used of 98 GB.

### 2026-10-28 broken.md "Fixed" row
| Area | Fixed | How |
|---|---|---|
| AegirPoster crash loop | 2026-10-28 | not seen in 50 min idle nor the 2 h sk68 app soak |

### 2026-10-27 ~19:00-20:07 plan steps 1-4 checks (ck1-ck3, kernel60 defaults) (step 3 AegirPoster part)
- Step 3 AegirPoster: 0 crashes, 0 CRASH lines of any kind in ~50 min idle. Bundle `/System/Library/CoreServices/AegirProxyApp.app/PlugIns/AegirPoster.appex`. Recount during an app soak.

### 2026-10-27 ~10:50-11:10 step 6 dd2: searchd launches the Spotlight extensions
- In dd1's log every *SpotlightIndexExtension exec follows `exec searchd` by ~0.4 s (searchd started 21 times in 30 min). Dropped com.apple.searchd.
- dd2 (kernel59): lock screen and Spotlight screen normal on the webcam (scratchpad dd2-lock.jpg, dd2-spotlight.jpg). 15 min appsoak: 54/54 opens; 1 CRASH line (PhotosFileProvider), 0 extension launches. The VoiceMemos/podcasts/SMS crashes are gone too.
- New file docs/sunfish/broken.md: disabled launchd jobs (9 so far), broken services, other broken things.

### 2026-10-27 ~10:05-10:45 plan step 6: crash-looping daemons (dd1)
- Dropped launchd jobs com.apple.spotlight.IndexAgent, com.apple.corespotlightservice, com.apple.assetsd, com.apple.assetsd.nebulad, com.apple.driverkitd with tools/sunfish-launchd-drop.sh. Previous plist: /var/local/launchd.plist.pre-step6 (phone), work/userland/launchd.plist.pre-step6 (PC); original still /System/Library/xpc/launchd.plist.pre-drop.
- dd1 (kernel59, 30 min appsoak): 102/102 opens, 0 resets/hangs, 10.8 swaps/s, pool end 317 MB (peak 385), battery 4434 mV flat.
- CRASH lines dd1 vs pp1 (same 30 min mix): assetsd 0 vs 88; TipsSpotlightInd 19/19, PassKitSpotlight 19/19, ContactsCoreSpot 10/9, VoiceMemos 10/9, MobileSMSSpotlig 9/9, podcasts 9/9, PhotosFileProvider 1/1, fileproviderd 1/1.
- Photos opens to "What's New in Photos" + a notification prompt (it rendered black before). Home screen normal. Spotlight UI and lock screen not checked (the agent's swipes missed).
- Open: the Spotlight index extensions are launched by something other than the two dropped jobs (candidates: searchd, spotlightknowledged, duetexpertd).

### 2026-10-27 ~07:55-09:00 kernel58 (xnu 0182: SMMU stall-on-fault by default)
- See [gpu](gpu.md) for this entry; it also touches this feature.

### 2026-10-27 experiments.md lines
- 2026-10-27 dd1: dropped launchd jobs spotlight.IndexAgent, corespotlightservice, assetsd, assetsd.nebulad, driverkitd (tools/sunfish-launchd-drop.sh; backup /var/local/launchd.plist.pre-step6). 30 min app soak on kernel59: 102/102 opens, assetsd crashes 88 -> 0, Photos now opens to its first-run sheet (was black); Spotlight index extension crashes unchanged (Tips 19, PassKit 19, Contacts/VoiceMemos/SMS/Podcasts 9-10 each), so another daemon launches them. 10.8 swaps/s, no hangs.
- 2026-10-27 dd2: also dropped com.apple.searchd (it execs ~0.4 s before every Spotlight index extension). 15 min app soak: 54/54 opens, CRASH lines 1 (PhotosFileProvider) vs ~45 per 15 min in dd1; 0 extension launches. Lock screen and Spotlight screen (Siri Suggestions, search field, keyboard) still work on the webcam.

### 2026-10-25 soak0 — baseline stability soak (plan step 0), aborted at ~16 min
- See [gpu](gpu.md) for this entry; it also touches this feature.

Older entries (before 2026-10-25): [archive/features/apps.md](../archive/features/apps.md).
