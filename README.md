# iOS 16 on a Google Pixel 4a (sunfish)

**Apple's iOS 16 userland running on a Pixel 4a, booted from RAM with Google's own bootloader.**

Pixel 4a (`sunfish`): Qualcomm SM7150 (Snapdragon 730G), Adreno 618 GPU, 1080x2340 OLED panel.

## Lineage

A port of [ios-flame](docs/archive/flame/) (the Pixel 4 port), which is itself a port of
[ios-sweet](https://github.com/dipa-studio/ios-sweet) (Redmi Note 10 Pro). Apple's open-source XNU kernel
(xnu-8796, the iOS 16 generation), with ios-sweet's, flame's and the sunfish patches on top, runs the iOS 16.7.16
userland of the iPhone X. The phone is booted with `fastboot boot`; nothing is flashed to the boot chain, and the
iOS root lives in the `userdata` partition.

- flame docs (Pixel 4): [docs/archive/flame/](docs/archive/flame/)
- sweet docs (Redmi Note 10 Pro): [docs/archive/sweet/](docs/archive/sweet/)

> Experimental bring-up, not a usable phone OS. It **erases Android's user data**, Apple's security model is
> mostly switched off (permissive AMFI and sandbox, stub keystore), and many parts are missing. Do not put
> anything personal on it.

## Status (2026-10-30)

States: ✅ works, 🟡 partial, ❌ broken, ⬜ not started. Each row links to the feature file with the details; the
plan is [docs/plan.md](docs/plan.md).

| Area | State | Summary |
|---|---|---|
| [Boot](docs/features/boot.md) | ✅ | RAM-only `fastboot boot`; iOS 16.7.16 (iPhone X userland) root on userdata |
| [CPU](docs/features/cpu.md) | ✅ | 8 cores; A55 1804 MHz, A76 2208 MHz |
| [Storage](docs/features/storage.md) | ✅ | /private/var on a 94 GB HFSX volume (ufsroot1) via /private/etc/fstab; direct (uncached) I/O fixed |
| [USB network](docs/features/usb-network.md) | ✅ | Ethernet 192.168.7.2, live kernel log; boot to SSH 16-18 s |
| [SSH](docs/features/ssh.md) | ✅ | dropbear + dash + toybox; boot to SSH 16-18 s |
| [Display](docs/features/display.md) | ✅ | 1080x2340, GPU-composited (Metal → Turnip), vsync-paced; apps rotate to landscape and back on command (`R 3`/`R 1`), touch follows; auto-rotate waits for sensors |
| [Smoothness](docs/features/smoothness.md) | 🟡 | ~34 swaps/s in steady scrolling; the limit is 22-24 ms of CPU per frame in backboardd's render thread, half of it in our Metal bridge (2026-10-30) |
| [Haptics](docs/features/haptics.md) | ✅ | iOS haptics work: switch taps pulse the motor (DRV2624 via hapticd, launchd job, `HAPTICS=1` boot); feel check by the user open |
| [Brightness](docs/features/brightness.md) | ✅ | Control Center slider drives the panel brightness (DCS 0x51, `sweetbl=2`) |
| [GPU](docs/features/gpu.md) | ✅ | Adreno 618 at 565 MHz; recovers from GPU hangs (~26 ms) |
| [GPU memory](docs/features/gpu-memory.md) | ✅ | leak fixed (spare command pools capped): pool flat over a 20 min app soak |
| [Metal](docs/features/metal.md) | ✅ | Metal via SweetMetal2 (metal2vulkan + Turnip) |
| [Touch](docs/features/touch.md) | ✅ | ST FTM5 over I²C |
| [Buttons](docs/features/buttons.md) | ✅ | power, volume down, volume up |
| [Flashlight](docs/features/flashlight.md) | ✅ | Control Center button turns the LED on and off |
| [Clock](docs/features/clock.md) | ✅ | PM6150 RTC |
| [Wi-Fi](docs/features/wifi.md) | ✅ | joins the home network at boot (from a config file on the phone); iOS shows the Wi-Fi icon and the network in Settings (`sweetwifiev=1`); joining a new network from Settings is untested |
| [Battery](docs/features/battery.md) | 🟡 | real reading; % shows no jumps at idle, discharge unchecked |
| [Charging](docs/features/charging.md) | 🟡 | charges (set up by the bootloader: 2 A charge, 1.5 A input); a weak USB port cannot keep up under load, a strong one gives ~+1 A |
| [Apps](docs/features/apps.md) | ✅ | 32 stock apps installed; survey 2026-10-29: 17 of 18 open fine; Files "On My iPhone" opens; app extensions work: widgets, share sheet, emoji keyboard (2026-10-30) |
| [Sleep and wake](docs/features/sleep-wake.md) | ✅ | power-button sleep/wake; the OLED panel really switches off and back on (`sweetpanelsleep=1`, 10/10) |
| [Audio](docs/features/audio.md) | 🟡 | AirPods play music; on the speaker path a test tone from the audio DSP now reaches both amps and echoes back correctly (lines swapped, sp20, 2026-10-30), not heard yet: audible test next |
| [Camera](docs/features/camera.md) | 🟡 | iOS lists both cameras (Back 16 formats, Front 18); Camera.app works: live colour preview on both cameras, auto-exposure, autofocus and sharp saved photos (2026-10-30); switching cameras takes ~7 s + ~10 s until the picture shows; Live Photo off; both cameras stream from tools (rear 60 fps, front 30 fps) |
| [Bluetooth](docs/features/bluetooth.md) | ✅ | iOS Settings: devices listed, Xbox controller and AirPods Pro 2 pair and connect (noise control works); AirPods play music (2026-10-30) |
| [Cellular](docs/features/not-started.md) | ⬜ | not planned |
| [Sensors](docs/features/sensors.md) | ✅ | Compass works (heading + dial, 2026-10-30, kernel126); auto-rotate works (accelerometer -> rotation); proximity turns the screen off when near (kernel122); all 6 sensors answer on the audio DSP; auto-brightness parked |

## Quick start

Safety rules (all rules: [CLAUDE.md](CLAUDE.md)):
- Never flash ABL, XBL, TZ, modem or other firmware partitions.
- Never relock the bootloader on modified software.
- Stop booting below 3600 mV (`fastboot getvar battery-voltage`).

From `port/`, with the phone in fastboot:

```sh
tools/sunfish-livelog.sh ../logs/usb-live.log &      # live kernel log + USB network link (phone 192.168.7.2)
tools/sunfish-boot.sh NAME KERNEL [bootargs]         # RAM-only boot
tools/sunfish-install-ssh.sh                          # once
tools/sunfish-ssh                                     # root shell in iOS
```

From a stock Pixel 4a: [docs/setup.md](docs/setup.md); every on-device change: [docs/device-changes.md](docs/device-changes.md).
Everything else (building, benches, logs, profiling): [docs/methods.md](docs/methods.md).
Back to Android: [docs/recovery.md](docs/recovery.md).

## Repository layout

| Path | What |
|---|---|
| `xnu/patches/` | XNU patches: ios-sweet's, flame's, then the `NNNN-sunfish-*` ones |
| `shim/` | The bare-metal shim ABL boots; it acts as iBoot |
| `phone/` | Code that runs in iOS: `tools/` (debug tools), `wifi/` (wland), `metal2/` + `turnip/` (Metal on Turnip), `metal/`, `gpu/` |
| `tools/` | Host tools: boot, live log, SSH and file access, benches, GPU debugging |
| `docs/` | [README.md](docs/README.md) (docs index), [features/](docs/features/), [journal/](docs/journal/) (one file per day), [plan.md](docs/plan.md), [archive/](docs/archive/), [reference/](docs/reference/) |

## Credits and license

Built on ios-sweet by dipa-studio, with Wi-Fi from ios-topaz and HackiOS, and Metal from UniXNU, metal2vulkan
and Mesa. The repository keeps ios-sweet's [MIT License](LICENSE); `xnu/patches/` modify Apple's XNU and stay
under the APSL 2.0, and files from other projects keep their own licence. Details in [CREDITS.md](CREDITS.md).
Nothing from Apple's iOS, Google's images or Qualcomm's firmware is in this repository.
