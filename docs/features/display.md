# Display
**State:** ✅ 1080x2340, ABL splash buffer 0x9c000000; GPU-composited (Metal → Turnip), vsync-paced. **Updated:** 2026-10-24.

## How to use
- Best present settings: kernel37+ with `sweetpresentwait=1`.

## Facts
- 1080x2340, ABL splash buffer 0x9c000000; GPU-composited (Metal → Turnip), vsync-paced ~21 updates/s (status.md).
- Panel Samsung SOFEF01, DSI command mode (not video), refreshed by MDP INTF1 autorefresh on TE (0x0ae6bab4 = 0x80000001) (plan step 7).
- Reading 0x0ae44000 hangs the bus. The MDP registers are at the catalog offset + 0x1000 (display-perf.md).
- Frame path, present path and the IOMFB/vsync findings: see [smoothness](smoothness.md) "Facts".
- Present switches (all off by default, all worse): `sweetfb_thread=1`, `sweetfb_onswap=1` (0161), 0158 non-blocking present, `sweetpresentwait=0`, `sweetpresentslots=4` (0164).
- sweet_a6xx.c sets up RB0 only and has no preemption support, so a second ringbuffer for the present is not feasible without a software context switch (2026-10-24).
- Sleep: "display off" only stops presenting frames (see sleep-wake).

## Open
- The present blit waits behind backboardd on the single ring (see smoothness).
- Scanout without the blit (risky: MDP registers).

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

### 2026-10-29 22:10 Safari rotates full width; re-apply waits for a stable front app (rot11, rot12; kernel98)
- rot11 (cbb2525): `R 3` sent with Safari already in front -> landscape, full width (rot11-b/c). Settings opened later stayed portrait: no "re-applied" line; Settings is portrait-only on iPhone (mask check skips it), so that is expected.
- Fix: the 1 s re-apply waits until the front set has stayed the same for 3 ticks (Safari was re-applied on its first front tick, still launching -> left ~45% drawn).
- rot12: Calculator `R 3` landscape (a); Safari opened afterwards: "R re-applied 3" once, landscape, ~100% width (b), stable 15 s later (c); `R 1` -> portrait, no bottom strip (d). No reset; battery 4454 mV.
- Open: touch mapping in landscape (needs a finger: U6, or `R N t`); SpringBoard home screen stays portrait; auto-rotate from the accelerometer.

### 2026-10-29 21:40 ONE-STEP ROTATE WORKS: `R 3` / `R 1` (rot7-rot10, kernel98; SweetMetal2 50a5730, cbb2525)
- rot7/rot8 (50a5730: front scenes first, then background): R still left Calculator portrait. rot9: plain `S 3` rotated it, `S 1` restored it, so the backboardd D step (and E) reset the app scene after the update.
- cbb2525: `R N` = scene updates only (front first, then the rest) + 1 s re-apply to apps that come to the front; D only with `R N t`, E only with `R N e`.
- rot10: `R 3` -> Calculator landscape, upright, scientific layout (rot10-b). Safari opened afterwards got the re-apply (logged) and drew landscape, but its content only fills the left ~45% (portrait width), right side black, while the toolbar spans the full width (rot10-c). `R 1` -> Safari portrait (an extra strip with up/down arrows at the bottom, rot10-d); Calculator portrait (rot10-e). No reset.
- Open: Safari's half-drawn landscape (try R while Safari is already in front); touch mapping in landscape (`R N t`); SpringBoard's own UI (home screen) stays portrait.

### 2026-10-28 22:30 SpringBoard scene-settings probe found no scenes
- Added to the SpringBoard probe: FBSceneManager sharedInstance -> allScenes/scenes -> each app scene's settings frame/orientation. On iOS 16 neither selector answers (no lines, no exception), so Safari's scene frame is still unknown. Next: find FBSceneManager's scene accessor (class dump in research/re/sb/objc.txt) or go through SBApplication -> sceneHandle -> scene.

### 2026-10-29 20:30 one-step rotate `R N` (rot4-rot6; SweetMetal2 2d1dc3c, bd6030b)
- 2d1dc3c `R N` = backboardd D N 0 + SpringBoard E N + front-scene update, 1 s re-apply to new front apps. rot4: Calculator rotated; Settings later came up landscape by itself; Safari never reached the screen in the window; `R 1` only reached the front scene (Settings), Calculator stayed landscape.
- bd6030b: R updates all app scenes (`externalApplicationSceneHandles`), re-apply incl. R 1, front-change logging. rot5/rot6: "R 3: applied to calculator-default" but the screen stayed portrait even after 20 s (regression vs rot3/rot4); Safari took > 15 s to come to the front; R 1 restored portrait. UIWDT 1 line in rot6's log (not checked whether a kill or the armed line).
- Next: front scenes via the foreground handles (as S/rot4), background ones from the all-handles set; log object identities.

### 2026-10-29 19:55 ROTATION WORKS: the app scene follows a direct settings update (rot3, kernel98; SweetMetal2 4963564)
- iOS 16.7 SpringBoard has no SBApplication -> scene-handle selector; the route: `+[SBSceneManagerCoordinator mainDisplaySceneManager]` -> `-externalForegroundApplicationSceneHandles` -> `-sceneIfExists` (research/notes/rotation-ios16.md; class-dumps in research/re/sb/).
- P (Calculator in front): handle SBDeviceApplicationSceneHandle currentInterfaceOrientation 1, supported 26; scene sceneID:com.apple.calculator-default, UIApplicationSceneSettings orientation 1, frame 375x812, deviceOrientation 0.
- `S 3` = `-[FBScene updateSettingsWithBlock:]` setting interfaceOrientation 3 + deviceOrientation 3, frame unchanged: Calculator re-lays out in the landscape scientific layout, upright and full screen (webcam rot3-b); settings stay 3 after 0.5 s and 2 s (no revert); SpringBoard's switcher host view becomes 812x375.
- `S 3 f` (frame swapped to 812x375) breaks it (half-screen portrait-style layout); `S 1` with the swapped frame left a rotated, clipped layout: keep the scene frame portrait-shaped (375x812).
- Next: wire the D/E device-orientation path (or a single command / Control Center toggle) to update the front scene, restore the frame on S 1; then sensors would drive it once the ADSP SSC works.

### 2026-10-29 11:00 rotation: SpringBoard probe P (rot2, kernel95; SweetMetal2 e63cb6d)
- New `P` command in /var/local/sweet-orient (SpringBoard, read-only): lines go to /var/local/mtl2.log (the per-boot live log did not show them).
- Calculator in front. After `D 3 0` + `E 3`: activeInterfaceOrientation 3; the SBMainSwitcherWindow's _UISceneLayerHostContainerView becomes 812x375 (was 375x812) with an identity layer transform and sublayerTransform, and that window's interfaceOrientation stays 1. The wallpaper and root-scene host views stay 375x812 portrait. Webcam: keypad unchanged.
- So SpringBoard sizes the app's host view for landscape but neither rotates the hosting window nor applies a 90 deg transform; Calculator does not relayout. `-[SBApplication sceneHandle]` does not exist on this iOS (scene settings not read yet; next: find the iOS 16 accessor, e.g. via SBSceneManager / FBSceneManager scenes by bundle id).

### 2026-10-29 10:45 rotation: Calculator does not relayout (rot0, rot1; kernel95)
- Research (research/notes/rotation-ios16.md, not disassembly-confirmed): on iPhone the app rotates its own pixels (UIWindow landscape bounds + 90 deg layer transform); backboardd/CA do not rotate the display. Calculator turns into the scientific keypad in landscape: a layout change visible on the webcam.
- rot0 (k95idle boot, Weather location alert on top): D 3 0 + E 3, E 4: 4x5 keypad stays. Confounded.
- rot1 (fresh boot, Calculator in front, no alert; ssh up 17 s, 4455 mV): `D 3 0` -> SpringBoard probe `notify orientation 3 raw 3 lock 0, UIDevice 3, app interface 3, locked 0` (no backboardd "effective" line); `E 3` logged; keypad stays 4x5 (webcam rot1-c). oslog: no Calculator "Scene will change" line.
- So SpringBoard believes orientation 3, but the front app's scene does not change orientation (or is counter-rotated). Next: a `P` probe in SpringBoard for the front app's scene settings/clientSettings interfaceOrientation and the scene host view transforms.

### 2026-10-28 22:20 rotation: where to look next
- RE opinion (no disassembly confirmed): on iOS 16 iPhones the app rotates its own picture (UIWindow _updateToInterfaceOrientation / _UIWindowRotationAnimationController: landscape bounds + a layer transform); the host and the display stay portrait. Safari logged `_endAnimatedResize` but no `_beginAnimatedResize`, so the window rotation step probably never runs, or the scene settings carry a portrait frame with orientation 3 (~60 / 25 %).
- An in-app probe (keyWindow bounds/transform, scene orientation/coordinate space) was added to SweetMetal2 for MobileSafari, but apps don't load SweetMetal2 (only SpringBoard, backboardd, com.apple.WebKit.GPU; MobileSafari only once, lazily), so it didn't run. Next: a small dylib injected into one app (DYLD_INSERT_LIBRARIES through its launchd/RunningBoard environment, or a tweak-loader style hook) to log those values, or read the scene settings frame from SpringBoard (FBSScene settings of Safari's scene).

### 2026-10-28 21:40 rotation: backboardd's side
- During `E 3`, backboardd logs `setPersistentProperties:{GraphicsOrientation: 4} ... sending to <BKIOHIDService ... SweetHIDTouch>` (touch coordinates follow the rotation) and `CBUIOrientation 3` to CoreBrightness; nothing about rotating the composited image (that's done by the app's own layer transform on iOS). So with method E the touch mapping rotates while the picture stays portrait: leave the phone at `E 1` unless testing. Next: why Safari's landscape layout doesn't show (Safari logged "Not push traits update to screen for new style 2"); compare a Metal-free app (Settings) and the app's layer tree.

### 2026-10-28 21:35 rotation: with oslog, method E rotates SpringBoard's and Safari's state; the screen stays portrait
- New phone tool `oslog` (phone/tools/oslog.c, ActivityStream SPI; `oslog -l -n PROC -t S`).
- After `E 3` with Safari in front, SpringBoard logs `[EmbeddedPipeline][Accelerometer] did change device orientation to: Ll`, the arbiter re-resolves (home screen supports Pu only; switcher overlay goes Lr), Safari's SBDisplayItemLayoutAttributes get `contentOrientation: landscapeRight (3)`, and an SBApplicationSceneUpdateTransaction runs.
- MobileSafari logs `Setting device orientation to 3`, `Scene will change interface orientation: landscapeRight (3)`, a scene-settings diff `orientation = landscapeRight (3)`, and `-[WKWebView _endAnimatedResize]`. The framebuffer shows 32 + 27 swaps in those 10 s.
- The webcam still shows the portrait layout (f3/f4). So the whole iOS-side chain works with method E; what's left is between the app's landscape scene and the composited screen (backboardd's display/scene transform vs our SweetFramebuffer / CA render path). Next: compare the app's layer bounds/transform in backboardd (CA debug) and check whether our IOMFB reports a display orientation/rotation property CA uses.

### 2026-10-28 21:45 rotation method E (traits pipeline callback): called, still no rotation (parked)
- RE (investigator, SpringBoard image in the shared cache): on iOS 16 app scenes follow the Traits arbiter, whose only device-orientation input is the BKSAccelerometer delegate callback `-[SBTraitsEmbeddedDisplayPipelineManager accelerometer:didChangeDeviceOrientation:]` (it rebuilds `_orientationInputs` via `_orientationInputsForDeviceOrientation:` and `_noteInputsDidChangeAnimated:reason:`); nothing calls it without an accelerometer. Orientation events are enabled only while `_isBacklightOn` (or prefersAlwaysOnOrientation).
- SweetMetal2 method E (`E N`): swizzles `initWithArbiter:sceneDelegate:` to capture the manager (captured: `E pipeline manager 0x28...`) and calls the callback with its own `_accelerometer`. After `D 3 0` + `E 3`: logged `E 3 (accelerometer 0x28...)`, Safari still portrait (webcam e3). With `_isBacklightOn` forced YES (it already was 1) + `_updateOrientationEventsEnabledState`: still portrait (e3b).
- Parked after methods A-E. Next would be reading SpringBoard's oslog (the "[EmbeddedPipeline][Accelerometer] did change device orientation" line and the arbiter's validator decisions); needs an oslog reader on the phone first.

### 2026-10-28 21:20 rotation probe: SpringBoard sees the orientation, Safari's scene doesn't follow
- SweetMetal2 in SpringBoard now probes 1.5 s after each `D` line. After `D 3 0` (backboardd effective 3): notify com.apple.backboardd.orientation = 3, rawOrientation = 3, orientationlock = 0, `UIDevice.orientation` = 3, SpringBoard's `activeInterfaceOrientation` = 3, SBOrientationLockManager isUserLocked = 0. After `D 1 0` all back to 1.
- Safari in front stays portrait (webcam p13, pb3), also after `B 3` (SBDeviceOrientationUpdateManager enqueue) on top.
- So the chain backboardd -> SpringBoard works; the step SpringBoard -> the app's FrontBoard scene (interface orientation in the scene settings) doesn't happen. Next: look at how SpringBoard decides to rotate the foreground scene (SBApplication supportedInterfaceOrientations, SBMainDisplaySceneLayout / SBOrientationTransaction, "rotation is disabled" device-class gates in SpringBoardHome/SpringBoard), read-only RE first.

### 2026-10-28 20:35 rotation without sensors: methods A-D, none rotates (parked)
- SweetMetal2 file watcher /var/local/sweet-orient: A `N` AX setDeviceOrientation (SpringBoard; the AX server isn't running), B `B N` SBDeviceOrientationUpdateManager enqueue, C `C N` BKSHIDServicesSetDeviceOrientationForAutomation after an unlock (needs com.apple.backboardd.setDeviceOrientation, which SpringBoard lacks), D `D N S` in backboardd: BKOrientationManager updateDeviceOrientation:N changeSource:S with CoreMotion updates dropped.
- or1 (kernel94): D worked only after backboardd was restarted on the new bundle (sunfish-test.sh -b installs after boot without restarting backboardd). Then `D 3 S` for S = 0..5 logs `effective 3`, but Safari stays portrait (webcam d3s1-5).
- Next: check whether SpringBoard's BKSAccelerometer client gets the change (hook _orientationDidChange / log SBDeviceOrientationUpdateManager's observer), and whether SpringBoard's orientation events are enabled.

Older entries (before 2026-10-25): [archive/features/display.md](../archive/features/display.md).
