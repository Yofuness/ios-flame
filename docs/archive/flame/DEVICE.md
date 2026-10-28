# Device

Test device for the ios-on-pixel project. All values below were read from the phone with `adb shell getprop` on 2026-10-19.

## Hardware

| Field | Value |
|---|---|
| Model | Google Pixel 4 |
| Codename | `flame` |
| SKU | `G020M` |
| Hardware revision | `MP1.0` |
| Serial | (not published) |
| SoC | Qualcomm Snapdragon 855 (SM8150) |
| Shipped with | Android 10 (`ro.product.first_api_level=29`) |

## Software (stock, as currently installed)

| Field | Value |
|---|---|
| Android version | 13 |
| Build ID | `TP1A.221005.002.B2` (final Pixel 4 release) |
| Fingerprint | `google/flame/flame:13/TP1A.221005.002.B2/9382335:user/release-keys` |
| Bootloader | `c2f2-0.5-8906123` |
| Radio / baseband | `g8150-00123-220708-B-8810441` |
| Active slot | `_a` (A/B device, slots `_a` and `_b`) |

## Lock state

| Field | Value | Meaning |
|---|---|---|
| `ro.boot.flash.locked` | `0` | Bootloader **unlocked** |
| `ro.boot.verifiedbootstate` | `orange` | Expected when unlocked; shows a warning screen at every boot |
| `sys.oem_unlock_allowed` | `1` | OEM unlocking switched on in Developer options |

Before this project the phone was locked (`flash.locked=1`, `green`) and OEM unlocking was off. It was unlocked on 2026-10-19 with `fastboot flashing unlock`, which wiped it.

## Host setup (macOS)

- Tools: `brew install --cask android-platform-tools`. This installs `adb` and `fastboot` 37.0.1 in `/opt/homebrew/bin`.
- On the phone: Settings → About phone → tap **Build number** 7 times, then Settings → System → Developer options → **USB debugging** on.
- Accept the "Allow USB debugging?" prompt on the phone after every wipe.

Quick check:

```sh
adb devices -l                     # expect: <serial>  device ... model:Pixel_4 device:flame
adb shell getprop ro.build.fingerprint
adb shell getprop ro.boot.flash.locked
```

## Button combos

| Action | How |
|---|---|
| Bootloader (fastboot mode) | Power off, then hold **Power + Volume Down**, or run `adb reboot bootloader` |
| Recovery | From the bootloader, use the volume keys to pick "Recovery mode" and press power, or run `adb reboot recovery`. At the "No command" screen, hold Power and tap Volume Up |
| Force restart | Hold **Power** for about 30 s |

See [RECOVERY.md](RECOVERY.md) for restoring the phone to stock.
