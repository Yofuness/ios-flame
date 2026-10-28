# iOS on Pixel 4a (sunfish): docs index

Port of ios-flame (Pixel 4) / ios-sweet (Redmi Note 10 Pro). Code: `port/` (branch `main`; the `sunfish` branch is frozen),
github.com/Yofuness/ios-flame. One file per feature under [features/](features/), each with a dated, append-only Log.

| Feature | State | Summary | Updated |
|---|---|---|---|
| [Boot](features/boot.md) | ✅ | RAM-only `fastboot boot`; iOS 16.7.16 (iPhone X userland) root on userdata | 2026-10-27 |
| [CPU](features/cpu.md) | ✅ | 8 cores; A55 1804 MHz, A76 2208 MHz (read from OSM registers) | 2026-10-24 |
| [Storage](features/storage.md) | ✅ | /private/var on /dev/ufsroot1 (94 GB HFSX) via /private/etc/fstab, kernel68+ | 2026-10-27 |
| [USB network](features/usb-network.md) | ✅ | Ethernet 192.168.7.2, live kernel log (UDP 5140, full backlog: 0127) | 2026-10-27 |
| [SSH](features/ssh.md) | ✅ | dropbear + dash + toybox; `port/tools/sunfish-ssh` | 2026-10-28 |
| [Display](features/display.md) | ✅ | 1080x2340, ABL splash buffer 0x9c000000; GPU-composited (Metal → Turnip), vsync-paced | 2026-10-24 |
| [Smoothness](features/smoothness.md) | 🟡 | ~21 screen updates/s ceiling while swiping; present blit waits behind backboardd | 2026-10-27 |
| [Brightness](features/brightness.md) | ✅ | Control Center slider drives the panel (DCS 0x51 32..511, sweetbl=2, xnu 0208-0216) | 2026-10-28 |
| [GPU](features/gpu.md) | ✅ | Adreno 618 at 565 MHz; full recovery from injected and real hangs (~26 ms) | 2026-10-28 |
| [GPU memory](features/gpu-memory.md) | ✅ | leak fixed by bridge 0105 (spare command pools capped at 8): pool flat over a 20 min app soak (198 / 324 peak / 245 MB), no speed cost | 2026-10-28 |
| [Metal](features/metal.md) | ✅ | Metal via SweetMetal2 (metal2vulkan + Turnip) on the A618 | 2026-10-26 |
| [Touch](features/touch.md) | ✅ | ST FTM5 over SE7 I²C 0x49 (patch 0125, `sweettouch=2`) | 2026-10-28 |
| [Buttons](features/buttons.md) | ✅ | power, vol-down (PON), vol-up (PM6150L GPIO2, patch 0126) | 2026-10-23 |
| [Flashlight](features/flashlight.md) | ✅ | Control Center button turns the LED on/off (85 mA, 300 s auto-off; sweettorch=1 default, SweetMetal2 AVFlashlight + module-caps hooks) | 2026-10-28 |
| [Clock](features/clock.md) | ✅ | PM6150 RTC + `sweetrtcoff`; timezone America/Toronto since 2026-10-28 | 2026-10-28 |
| [Wi-Fi](features/wifi.md) | 🟡 | wland joins at boot; iOS shows the status-bar icon and the network in Settings with sweetwifiev=1 (xnu 0199); the join from Settings is untested | 2026-10-28 |
| [Battery](features/battery.md) | 🟡 | real reading works; IR-compensated % (0128) shows no jumps at idle, discharge unchecked | 2026-10-28 |
| [Charging](features/charging.md) | 🟡 | charger already configured by XBL/ABL (FAST_CHARGE, FCC 2.0 A, FV 4.45 V, ICL 1.5 A); the drain under load is the USB source budget (TB4 + 240 W cable: +968 mA) | 2026-10-28 |
| [Apps](features/apps.md) | ✅ | 32 staged apps installed in /Applications; crash-looping launchd jobs dropped | 2026-10-28 |
| [Sleep and wake](features/sleep-wake.md) | ✅ | power-button sleep/wake; panel off via LM alpha blank + DCS 28/10, on via 11/29 (sweetpanelsleep=1, xnu 0202-0205, 10/10) | 2026-10-28 |
| [Audio](features/audio.md) | ⬜ | speaker amps answer on I²C (SE9 0215 + resets 0217); the ADSP boots and runs, then dies in its sensor PD (needs a FastRPC file listener) | 2026-10-28 |
| [Camera](features/camera.md) | ⬜ | SE9 I²C up (shared with the camera PMIC slg51000 @0x75); PMIC enable GPIOs behind sweetcampmic (0218), probe pending | 2026-10-28 |
| [Sensors](features/sensors.md) | ⬜ | survey done: all behind the SSC in the ADSP; waits for the ADSP boot | 2026-10-28 |
| [Bluetooth](features/bluetooth.md) | ⬜ | survey done: WCN3990 over a QUP UART + QCA firmware, then a Broadcom shim for bluetoothd; later | 2026-10-28 |
| [Not started](features/not-started.md) | ⬜ | Bluetooth, cellular, sensors (audio and camera have their own files now) | 2026-10-23 |

States: ✅ works, 🟡 partial, ❌ broken, ⬜ not started.

- [plan.md](plan.md): the current plan; old plans in [plans/](plans/).
- [methods.md](methods.md): tools and methods (boot, benches, logs, profiling, RE).
- [journal/](journal/): one file per day of work.
- [archive/](archive/): the old status.md tables ([archive/status-2026-10-28.md](archive/status-2026-10-28.md)), feature log entries before 2026-10-25 ([archive/features/](archive/features/)), the flame docs ([archive/flame/](archive/flame/)) and the sweet docs ([archive/sweet/](archive/sweet/)).
- [reference/](reference/): register dumps and hardware notes.
- [recovery.md](recovery.md): back to stock Android on the Pixel 4a.

## Known broken (from the old broken.md, as of 2026-10-28)
"plan.md step N" in the last column means the plan current when the row was written: steps 2 and 4 are in [plans/2026-10-27-gpu-stability-usability.md](plans/2026-10-27-gpu-stability-usability.md), steps 1 and 8 in [plan.md](plan.md).

| Area                                        | State                                                                                                                                                                                                                              | Where to start                                                            |
| ------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------- |
| Charging                                    | battery drains ~0.5 A under load even on the charger; the charger is never configured                                                                                                                                              | [reference/charger-regs.md](reference/charger-regs.md); PMIC writes are risky, ask the user first |
| Battery %                                   | IR-compensated estimate (0128) never checked                                                                                                                                                                                       | features/battery.md                                                                 |
| USB link                                    | port resets, replug and boot all fine since xnu 0184 (kernel60); ck1-ck3: 0 drops in 3x5 min                                                                                                                                       | -                                                                         |
| GPU hang recovery                           | works on injected hangs (5/5, ~26 ms) but never met a real hang since 0181/0182                                                                                                                                                    | plan.md step 2                                                            |
| UI watchdog                                 | catches a frozen UI only; a kernel hang with swaps still running is not caught                                                                                                                                                     | plan.md step 4                                                            |
| Speed                                       | ~21 screen updates/s ceiling while swiping; present blit waits behind backboardd                                                                                                                                                   | features/smoothness.md                                                           |
| Reduce Transparency                         | works but slow (~6 swaps/s)                                                                                                                                                                                                        | features/metal.md rtj/rtj2                                                       |
| Audio, Bluetooth, cellular, camera, sensors | not started                                                                                                                                                                                                                        | features/not-started.md                                                                 |
| Wi-Fi                                       | scan works, iOS never joins: it has no known network (no com.apple.wifi.known-networks.plist); 0193 answers COUNTRY_CODE and set 383, still 0 ASSOCIATE; get 103 CURRENT_NETWORK unanswered, get 423 not built                     | plan.md step 1; the user joins once in Settings                           |
| GPU pool                                    | grows in long app soaks: backboardd small BOs = Turnip command streams 16 -> 180 MB (first 15 min, then flat); own 1024 KB kernel BOs never freed, ~1 per 25-30 s while opening apps, not tracked by Turnip (bc1/bos1, 2026-10-28) | plan.md step 8                                                            |
| /var/local/mtl2.log                         | 321 MB (2026-10-28), appended at every Metal client start, never rotated                                                                                                                                                           | truncate now and then; rotate in the frontend                             |
| Block driver direct I/O                     | copyfile's fcopyfile data path gets EFAULT on /dev/ufsroot* files (vcopy uses its own read/write loop)                                                                                                                             | sweet_ufs.c strategy with user-space buffers                              |
