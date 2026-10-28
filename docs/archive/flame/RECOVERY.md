# Recovery

How to get the Pixel 4 (`flame`) back to stock Android, whatever state it's in. This procedure was tested on 2026-10-19: unlock, wipe, then `flash-all.sh` brought the phone back to `TP1A.221005.002.B2`.

Device details are in [DEVICE.md](DEVICE.md).

## Images

Stored in `images/` and downloaded from Google ([factory images](https://developers.google.com/android/images#flame), [full OTAs](https://developers.google.com/android/ota#flame)). Both are the final Pixel 4 build, `TP1A.221005.002.B2`.

| File | Purpose | SHA-256 |
|---|---|---|
| `flame-tp1a.221005.002.b2-factory-38e4f49a.zip` | Factory image: full reflash through fastboot. Needs an unlocked bootloader | `38e4f49a8f72a8f9522f86874187de47add539372fa8c951d11ebde4f9c21103` |
| `flame-ota-tp1a.221005.002.b2-4d76895a.zip` | Full OTA: sideload from stock recovery. Works with a locked or unlocked bootloader | `4d76895aac34a8bb816a338439ee62ca0794628228cf4c1330b4e40a5c8cb4af` |

Check them with:

```sh
cd images && shasum -a 256 -c SHA256SUMS
```

The factory zip is already unpacked in `images/flame-tp1a.221005.002.b2/`.

To download them again: the URLs are `https://dl.google.com/dl/android/aosp/<filename>`.

## Primary method: factory image through fastboot (erases data)

Use this for almost any problem, including a phone that won't boot, as long as you can get into the bootloader.

1. Get into the bootloader:
   - If Android is running with USB debugging on: `adb reboot bootloader`
   - Otherwise: power off (hold Power for about 30 s if it's stuck), then hold **Power + Volume Down**.
2. Confirm the Mac sees it:
   ```sh
   fastboot devices          # expect <serial>  fastboot
   ```
3. Flash:
   ```sh
   cd images/flame-tp1a.221005.002.b2
   ./flash-all.sh
   ```
   This flashes the bootloader and radio firmware, reboots into the bootloader, then runs `fastboot -w update image-flame-tp1a.221005.002.b2.zip`, which flashes every partition and wipes user data. It takes about 5 minutes and the phone reboots several times. **Don't unplug it while this runs.**
4. It's finished when the phone shows **Welcome to Pixel**. Set it up, turn on USB debugging again and accept the prompt.
5. Check:
   ```sh
   adb shell getprop ro.build.fingerprint
   # google/flame/flame:13/TP1A.221005.002.B2/9382335:user/release-keys
   ```

Notes:
- `flash-all.sh` only writes the currently active slot. If the other slot was modified too, fix it by running `fastboot --set-active=other` and then running `flash-all.sh` again, or with `fastboot --slot=all` when flashing individual partitions.
- If fastboot stalls on the Mac, try a different USB port or cable. Plug directly into the Mac rather than through a hub.

## Fallback method: full OTA sideload

Use this if fastboot flashing isn't an option, for example if the bootloader got locked again. It reinstalls the system **without** wiping data and doesn't need an unlocked bootloader. It still needs stock recovery to be bootable.

1. `adb reboot recovery`, or from the bootloader pick **Recovery mode**.
2. At "No command", hold **Power** and tap **Volume Up** to show the menu.
3. Choose **Apply update from ADB**.
4. ```sh
   adb sideload images/flame-ota-tp1a.221005.002.b2-4d76895a.zip
   ```
5. Choose **Reboot system now**. If it still won't boot, choose **Wipe data/factory reset** in recovery.

## Relocking the bootloader (optional, erases data)

Only relock **after** confirming the phone boots the unmodified stock factory image. Locking on top of modified software can brick the phone permanently.

```sh
adb reboot bootloader
fastboot flashing lock      # confirm on the phone with the volume keys and power
```

Afterwards: `ro.boot.flash.locked=1`, verified boot `green`. To unlock again, OEM unlocking must still be on in Developer options, then run `fastboot flashing unlock`, which wipes the phone again.

## Rules for staying recoverable

- **Don't relock** while anything is modified.
- **Don't flash the bootloader, radio, or other firmware partitions** (`abl`, `xbl`, `tz`, `modem`, etc.) unless they come from the factory image. A bad bootloader means no fastboot, and then only Qualcomm EDL mode is left.
- Prefer `fastboot boot <img>` (boots the image temporarily, nothing is written) over `fastboot flash` when testing kernels or boot images.
- Keep the `images/` folder and confirm the checksums match before relying on it.
