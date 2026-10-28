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

## Status (2026-10-28)

States: ✅ works, 🟡 partial, ❌ broken, ⬜ not started. Details in [docs/README.md](docs/README.md).

| Area | State | Summary |
|---|---|---|
| [Boot](docs/features/boot.md) | ✅ | RAM-only `fastboot boot`; iOS 16.7.16 (iPhone X userland) root on userdata |
| [CPU](docs/features/cpu.md) | ✅ | 8 cores; A55 1804 MHz, A76 2208 MHz |
| [Storage](docs/features/storage.md) | ✅ | /private/var on a 94 GB HFSX volume (ufsroot1) via /private/etc/fstab |
| [USB network](docs/features/usb-network.md) | ✅ | Ethernet 192.168.7.2, live kernel log |
| [SSH](docs/features/ssh.md) | ✅ | dropbear + dash + toybox |
| [Display](docs/features/display.md) | ✅ | 1080x2340, GPU-composited (Metal → Turnip), vsync-paced |
| [Smoothness](docs/features/smoothness.md) | 🟡 | ~21 screen updates/s ceiling while swiping |
| [Brightness](docs/features/brightness.md) | 🟡 | panel brightness (DCS 0x51) from a tool; no Control Center slider |
| [GPU](docs/features/gpu.md) | ✅ | Adreno 618 at 565 MHz; recovers from GPU hangs (~26 ms) |
| [GPU memory](docs/features/gpu-memory.md) | ✅ | leak fixed (spare command pools capped): pool flat over a 20 min app soak |
| [Metal](docs/features/metal.md) | ✅ | Metal via SweetMetal2 (metal2vulkan + Turnip) |
| [Touch](docs/features/touch.md) | ✅ | ST FTM5 over I²C |
| [Buttons](docs/features/buttons.md) | ✅ | power, volume down, volume up |
| [Flashlight](docs/features/flashlight.md) | 🟡 | LED works from a tool (`sweettorch=1`); no Control Center button |
| [Clock](docs/features/clock.md) | ✅ | PM6150 RTC |
| [Wi-Fi](docs/features/wifi.md) | 🟡 | wland joins a network at boot from a config file; joining from Settings fails |
| [Battery](docs/features/battery.md) | 🟡 | real reading; % shows no jumps at idle, discharge unchecked |
| [Charging](docs/features/charging.md) | ❌ | drains ~0.5 A under load on the charger; charger not configured |
| [Apps](docs/features/apps.md) | ✅ | 32 stock apps installed in /Applications |
| [Sleep and wake](docs/features/sleep-wake.md) | 🟡 | power-button sleep/wake stable (11/11); the panel stays on |
| [Audio](docs/features/not-started.md) | ⬜ | speaker, earpiece, headphone jack, mic: not started (survey under way) |
| [Camera](docs/features/not-started.md) | ⬜ | rear and front cameras: not started (survey under way) |
| [Bluetooth](docs/features/not-started.md) | ⬜ | not started |
| [Cellular](docs/features/not-started.md) | ⬜ | not planned |
| [Sensors](docs/features/not-started.md) | ⬜ | accelerometer, gyro, proximity, ambient light: not started |

## Quick start

Safety rules (from [AGENTS.md](AGENTS.md)):
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

Everything else (building, benches, logs, profiling): [docs/methods.md](docs/methods.md).
Back to Android: [docs/recovery.md](docs/recovery.md).

## Repository layout

| Path | What |
|---|---|
| `xnu/patches/` | XNU patches: ios-sweet's, flame's, then the `NNNN-sunfish-*` ones |
| `shim/` | The bare-metal shim ABL boots; it acts as iBoot |
| `phone/` | Code that runs in iOS: `tools/` (debug tools), `wifi/` (wland), `metal2/` + `turnip/` (Metal on Turnip), `metal/`, `gpu/` |
| `tools/` | Host tools: boot, live log, SSH and file access, benches, GPU debugging |
| `docs/` | [README.md](docs/README.md) (feature index), [features/](docs/features/), [journal/](docs/journal/) (one file per day), [plan.md](docs/plan.md), [archive/](docs/archive/), [reference/](docs/reference/) |

## Credits and license

Built on ios-sweet by dipa-studio, with Wi-Fi from ios-topaz and HackiOS, and Metal from UniXNU, metal2vulkan
and Mesa. The repository keeps ios-sweet's [MIT License](LICENSE); `xnu/patches/` modify Apple's XNU and stay
under the APSL 2.0, and files from other projects keep their own licence. Details in [CREDITS.md](CREDITS.md).
Nothing from Apple's iOS, Google's images or Qualcomm's firmware is in this repository.
