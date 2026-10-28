# ios-flame

**Apple's iOS 16 running on a Google Pixel 4.**

A port of [ios-sweet](https://github.com/dipa-studio/ios-sweet) to the Pixel 4 (`flame`, Qualcomm SM8150 /
Snapdragon 855, Adreno 640). Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation), with ios-sweet's
patches and the flame ones on top, runs the **unmodified iOS 16.7.16 (20H392) userland of the iPhone X**. The
phone is booted with `fastboot boot` from Google's own bootloader. Nothing is flashed to the boot chain; the iOS
root lives in the `userdata` partition.

![Lock screen, home screen, Settings, General, Control Center, Today view, Spotlight, Clock, Calendar, Notes, Photos and Safari, all rendered through Metal on the Adreno 640](docs/img/metal2-tour.jpg)

> Experimental bring-up, not a usable phone OS. It **erases Android's user data**, Apple's security model is
> mostly switched off (permissive AMFI and sandbox, stub keystore), and many parts are missing. Do not put
> anything personal on it.

## What works (2026-10-22, kernel 0124)

| Area | State |
| --- | --- |
| Boot | Google ABL (unlocked) -> `fastboot boot` of the shim -> XNU. Every reset lands back in fastboot (the shim sets the PMIC reboot reason) |
| CPU | All 8 cores (4x Cortex-A55 + 4x Cortex-A76) at full clock, PSCI |
| Storage | UFS 3.0; the iOS root is an HFS+ image in `userdata`, read/write |
| Display | The 1080x2280 command-mode panel ABL leaves running. iOS sees an iPhone X (1125x2436); the Adreno 640 scales each frame to fit, with Apple's continuous corners |
| GPU / Metal | Metal on Mesa's Turnip Vulkan driver through metal2vulkan (`sweetmtl=2`): backboardd, SpringBoard and apps render on the GPU, shaders compile on the phone. Swiping home screens runs at about 20-26 fps |
| Touch, buttons | ST FTS touch over SPI; power and volume keys |
| USB | Ethernet to the Mac: SSH (dropbear), a kernel file server, the live kernel log, internet sharing |
| Wi-Fi | The WCN3990 radio driven from userland (`wland`): scan, WPA2-PSK, joins at boot. **Settings -> Wi-Fi works** (wifid on en0, iOS's own DHCP) |
| Apps | Settings, Safari (renders full pages), Clock, Calendar, Notes, Photos and the other stock apps. An ad hoc-signed third-party IPA (YouTube) launches and browses |
| Time | PM8150 RTC |

**Not working yet:** audio, video playback (mediaserverd crashes), Bluetooth, cellular, camera, sensors, GPS,
SEP / Face ID, HEIC/HEVC decoding (no software decoder), Today-view widgets with Metal (they use the CPU path),
real battery readings.

## How it boots

```
ABL (Google's bootloader, unlocked)
  -> fastboot boot shim-boot.img          LZ4 kernel + AVB hash footer, or ABL refuses it
       shim (shim/): acts as iBoot. Logs to UFS, sets the PON reason, arms the watchdog,
       builds the boot args and jumps to XNU with an Apple device tree (tools/mkadt.py)
  -> XNU (xnu/patches): every driver built in: UFS, display, GPU, touch, keys, USB, RTC,
       RPMh, /dev/pmem for the userland drivers, en0 for wifid
  -> launchd and the iPhone X userland from userdata
       + phone/: SweetMetal2.bundle (Metal), wland (Wi-Fi), debug tools
```

## Repository

| Path | What |
| --- | --- |
| `xnu/patches/` | 0001-0081 from ios-sweet, 0082-0124 for flame ([list](docs/archive/flame/NOTES.md#flame-patches), [how to build](xnu/README.md)) |
| `shim/` | The bare-metal shim ABL boots (`shim/build.sh`) |
| `tools/` | Host tools: ADT and payload, boot-and-collect-logs loop, SSH/file/command access to the running phone, GPU and Metal debugging, ios-sweet's root-image scripts |
| `phone/` | Code that runs in iOS: `wifi/` (wland), `metal2/` + `turnip/` (Metal on Turnip), `metal/` (the older template driver), `gpu/`, `tools/` (psx, pstack, openapp, lsreg, netsvc, oslog, ...) |
| [NOTES.md](docs/archive/flame/NOTES.md) | Hardware facts, Google ABL boot image rules, test loop, one line per flame patch |
| [HANDOFF.md](docs/archive/flame/HANDOFF.md) | The engineering log, newest first: every step, what broke and why |
| [METAL-NEXT.md](docs/archive/flame/METAL-NEXT.md) | The plan for Metal at user quality (metal2vulkan + Turnip) and why |
| [RECOVERY.md](docs/archive/flame/RECOVERY.md), [DEVICE.md](docs/archive/flame/DEVICE.md) | Back to stock Android; the test phone and host setup |
| `docs/archive/sweet/` | ios-sweet's README and notes (the history of patches 0001-0081) |

## Not in git: what you need to get yourself

Scripts expect these at the paths shown (all gitignored). None of them can be shared: they are Apple's or
Google's/Qualcomm's files, or data from your own phone.

- **The phone**: a Pixel 4 (`flame`; the 4 XL `coral` is untested) with the bootloader unlocked, on the final
  Android 13 build `TP1A.221005.002.B2`. Keep Google's factory image in `images/` ([RECOVERY.md](docs/archive/flame/RECOVERY.md)).
  Back up the per-device partitions first (persist, modemst1/2, fsg, fsc, ...) into `backup/`. They hold your
  IMEI and calibration data, so keep them private.
- **Boot image tools**: AOSP `mkbootimg` in `downloads/mkbootimg/`, `avbtool.py` in `downloads/avb/`, and the
  stock DTB (`python3 tools/unpack_bootv2.py boot.img stock/` on the factory image's boot.img) as `stock/dtb`.
- **TWRP** 3.7.1_12-0 for flame in `downloads/twrp/`. It writes the root image and reads the logs back. The
  test loop keeps it in `boot_a` (see docs/archive/flame/NOTES.md).
- **The iOS root**: the iPhone X iOS 16.7.16 (20H392) IPSW. `tools/remotezip.py` fetches single files from
  Apple's CDN. Build the HFS+ image with `tools/mkiosroot.sh` in a Linux VM (apfs-fuse, hfsprogs), then run
  `tools/sweet-root-tweaks.py`. Write it from TWRP with `adb exec-in 'dd of=/dev/block/sda15 bs=4194304' < IMG`,
  not `fastboot flash userdata` (ABL hangs on the last chunk).
- **Firmware** from the factory image's `vendor.img` (`/firmware`):
  - GPU: `a640_gmu.bin`, `a640_zap.mdt`/`.b02` and `a630_sqe.fw` in `research/gpu/fw/` (`flame-run.sh -G`
    puts them in the device tree).
  - Wi-Fi: `wlanmdsp.mbn`, `modemuw.jsn` and `bdwlan-flame.bin`, plus the modem's `modem.mdt`/`.bNN` and
    `modemr.jsn`, in `/var/local/fw/modem` on the phone.
  - Your own modemst1/modemst2/fsg/fsc images go in `/var/local/fw/rmtfs` on the phone (docs/archive/flame/HANDOFF.md, "WiFi, part 1").
- **Toolchains**: Xcode with the iOS SDK (`work/userland/bin/ios-cc`), Homebrew `llvm`, `lld` and `lz4` for the
  shim, [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build) for the first kernel build,
  Mesa and metal2vulkan checkouts for `tools/build-turnip-ios.sh` and `tools/build-m2v-bridge.sh` (pins in the
  script headers), and dropbear, toybox and dash built for iOS (docs/archive/flame/HANDOFF.md, "SSH into iOS works").

## Run it

```sh
python3 tools/mkadt.py work/boot/flame-hi.adt --highmem     # Apple device tree for flame
tools/build-xnu.sh $PWD/work/build.log                       # kernel (after one full darwin-xnu-build run)
cp work/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet work/boot/kernel-0124
tools/flame-run.sh first -k work/boot/kernel-0124 -s 600 -G -a "sweetusb=2 sweetmtl=2"   # one boot + logs
# once, with the phone up and FlameBridge running (tools/flamebridge/build.sh, open the app):
ssh-keygen -t ed25519 -N '' -f work/userland/ssh/id_ed25519 && tools/flame-install-ssh.sh
tools/flame-fresh-run.sh second 1800 sweetmtl=2              # from then on: boot and wait for SSH
tools/flame-ssh                                              # root shell in iOS
```

`tools/flame-run.sh` is the loop underneath. It builds the payload and boot image, runs `fastboot boot`, waits for
the run to end back in fastboot, boots TWRP, then pulls and decodes the kernel log. docs/archive/flame/NOTES.md and HANDOFF.md
("How to run things", "Tools") cover each step and the boot args.

## Credits

Built on ios-sweet by dipa-studio, with Wi-Fi from ios-topaz and HackiOS, and Metal from UniXNU, metal2vulkan
and Mesa. Licences and details in [CREDITS.md](CREDITS.md).
