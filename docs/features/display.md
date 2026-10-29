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

### 2026-10-28 21:20 rotation probe: SpringBoard sees the orientation, Safari's scene doesn't follow
- SweetMetal2 in SpringBoard now probes 1.5 s after each `D` line. After `D 3 0` (backboardd effective 3): notify com.apple.backboardd.orientation = 3, rawOrientation = 3, orientationlock = 0, `UIDevice.orientation` = 3, SpringBoard's `activeInterfaceOrientation` = 3, SBOrientationLockManager isUserLocked = 0. After `D 1 0` all back to 1.
- Safari in front stays portrait (webcam p13, pb3), also after `B 3` (SBDeviceOrientationUpdateManager enqueue) on top.
- So the chain backboardd -> SpringBoard works; the step SpringBoard -> the app's FrontBoard scene (interface orientation in the scene settings) doesn't happen. Next: look at how SpringBoard decides to rotate the foreground scene (SBApplication supportedInterfaceOrientations, SBMainDisplaySceneLayout / SBOrientationTransaction, "rotation is disabled" device-class gates in SpringBoardHome/SpringBoard), read-only RE first.

### 2026-10-28 20:35 rotation without sensors: methods A-D, none rotates (parked)
- SweetMetal2 file watcher /var/local/sweet-orient: A `N` AX setDeviceOrientation (SpringBoard; the AX server isn't running), B `B N` SBDeviceOrientationUpdateManager enqueue, C `C N` BKSHIDServicesSetDeviceOrientationForAutomation after an unlock (needs com.apple.backboardd.setDeviceOrientation, which SpringBoard lacks), D `D N S` in backboardd: BKOrientationManager updateDeviceOrientation:N changeSource:S with CoreMotion updates dropped.
- or1 (kernel94): D worked only after backboardd was restarted on the new bundle (sunfish-test.sh -b installs after boot without restarting backboardd). Then `D 3 S` for S = 0..5 logs `effective 3`, but Safari stays portrait (webcam d3s1-5).
- Next: check whether SpringBoard's BKSAccelerometer client gets the change (hook _orientationDidChange / log SBDeviceOrientationUpdateManager's observer), and whether SpringBoard's orientation events are enabled.

Older entries (before 2026-10-25): [archive/features/display.md](../archive/features/display.md).
