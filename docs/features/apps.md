# Apps
**State:** ✅ 32 staged apps installed in /Applications; 12 of 18 surveyed apps open fine (Maps since 2026-10-29), plus CocoaTop (process viewer, tools/build-cocoatop-linux.sh); crash-looping launchd jobs dropped. **Updated:** 2026-10-29.

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
| com.apple.assetsd | 2026-10-27 | crash loop (88 per 30 min, SIGABRT) | Photos opens to its first-run sheet (it rendered black before); photo library likely not usable |
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
| AegirPoster | crash loop | 487 in the long log | lock-screen poster (wallpaper) extension; lock screen still draws, left enabled |
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
