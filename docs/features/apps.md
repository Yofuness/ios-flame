# Apps
**State:** ✅ 32 staged apps installed in /Applications, plus CocoaTop (process viewer, tools/build-cocoatop-linux.sh); crash-looping launchd jobs dropped. **Updated:** 2026-10-28.

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
