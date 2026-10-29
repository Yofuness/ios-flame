# Apps
**State:** ✅ 32 staged apps installed in /Applications; 17 of 18 surveyed apps open fine (Maps, Notes, Wallet, Music, Photos, App Store fixed/confirmed 2026-10-29); only Camera (no camera stack), plus CocoaTop (process viewer, tools/build-cocoatop-linux.sh); crash-looping launchd jobs dropped. **Updated:** 2026-10-29.

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
