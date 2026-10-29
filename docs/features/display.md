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

### 2026-10-28 22:30 SpringBoard scene-settings probe found no scenes
- Added to the SpringBoard probe: FBSceneManager sharedInstance -> allScenes/scenes -> each app scene's settings frame/orientation. On iOS 16 neither selector answers (no lines, no exception), so Safari's scene frame is still unknown. Next: find FBSceneManager's scene accessor (class dump in research/re/sb/objc.txt) or go through SBApplication -> sceneHandle -> scene.

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
