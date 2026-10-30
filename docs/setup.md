# Setup from scratch (Pixel 4a, sunfish)

How to go from a stock, bootloader-unlocked Pixel 4a to the current iOS state. Every on-device change is listed in
[device-changes.md](device-changes.md); this file puts them in order. Steps marked **(by hand)** are written-down steps
that a script will replace later. Run everything from `port/` unless it says otherwise.

Status: written 2026-10-30 from the audit of the working phone. Steps 1-4 were done once on Oct 22 and are
reconstructed from the files and notes of that day; the full sequence has not been replayed on a second phone yet.

## What you need (not in git)
- Pixel 4a (sunfish), bootloader unlocked, charged above 3600 mV.
- Linux PC with `apfs-fuse`, `hfsprogs` (fsck.hfsplus, mkfs.hfsplus), `img2simg`, `rsync`, `fastboot`, sudo; the cross
  toolchains used by `tools/build-*-linux.sh` (see [methods.md](methods.md) "Building").
- A macOS VM for kernel builds (`tools/macos-vm/README.md`, `tools/vm-build-xnu.sh`), or the GitHub workflow.
- Apple: the iPhone X (iPhone10,3) iOS 16.7.16 (20H392) IPSW, for the OS, SystemOS and AppOS disk images
  (`../restore/ipsw/`), plus the iPhone X device tree for `/product` (archive/sweet/README.md step 2).
- Google: the sunfish factory image `sunfish-tq3a.230805.001.s2` (`../restore/factory/`), for the vendor image
  (WLAN, ADSP and sensors firmware). Qualcomm modem/rmtfs data comes from your own phone's partition backup (step 1).

## 1. Back up the phone (once)
- Dump every partition except userdata into `../backup/` with SHA256SUMS (per-device data: modemst1/2, fsg, fsc,
  persist). Keep it out of git. How: [recovery.md](recovery.md) "Backups" and archive/flame/RECOVERY.md.

## 2. Build the iOS root image
```sh
tools/mkiosroot.sh OS.dmg SystemOS.dmg AppOS.dmg ../restore/ios16-root.img 16G
sudo fsck.hfsplus -fy ../restore/ios16-root.img
sudo mount -t hfsplus -o loop,force ../restore/ios16-root.img /mnt/iosroot
sudo python3 tools/sweet-root-tweaks.py /mnt/iosroot     # Setup skipped, activation, CommCenter removed, ...
sudo umount /mnt/iosroot
img2simg ../restore/ios16-root.img ../restore/ios16-root.simg
```

## 3. Write it to userdata (destructive: erases Android's data)
- `fastboot -S 512M flash userdata ../restore/ios16-root-tail.simg`
- ABL is slow on the big trailing FILL chunk of a plain img2simg image ([features/boot.md](features/boot.md)): the
  "-tail" image has that trailing FILL turned into a SKIP. The tool that made it is not in git (gap, Oct 22);
  `ios16-root-tailpatch.simg` (2 GB) was a second, partial write of the same tail. To redo: rewrite the last chunk
  of the sparse image as DONT_CARE (simg format) before flashing.

## 4. Build the kernel and boot
```sh
tools/vm-build-xnu.sh NN            # work/kernelNN (applies xnu/patches/ in order)
tools/sunfish-livelog.sh ../logs/usb-live.log &    # live log + USB network (phone 192.168.7.2)
tools/sunfish-boot.sh first work/kernelNN/kernel.development.sweet   # RAM-only, nothing flashed
# the Bluetooth device-tree node comes from tools/mkadt.py (work/boot/sunfish-hi.adt; rebuild it with mkadt.py --highmem)
```
The first boot takes a few minutes; the lock screen appears. Boot args and defaults: [methods.md](methods.md).
Everyday boot once the features are installed (2026-10-30): `HAPTICS=1 CAMERA=1 tools/sunfish-boot.sh NAME work/kernel116/kernel.development.sweet sweetse9=2 sweetcampmic=2` (HAPTICS/CAMERA add the ADT nodes; sweetse9/sweetcampmic power SE9 and the camera PMIC).

## 5. /private/var on its own volume (by hand)
The 16 GB root is 94 % full; /var moves to the rest of userdata (/dev/ufsroot1, xnu 0185+, kernel68+):
Do step 6 first (SSH and the phone tools, incl. `vcopy`), then on the phone:
```sh
newfs_hfs -s -J 8192k /dev/rufsroot1                 # 94 GB case-sensitive HFS+, 8 MB journal (2026-10-27 g62)
mkdir -p /private/var/local/mnt-data && /sbin/mount_hfs /dev/ufsroot1 /private/var/local/mnt-data
vcopy -x /private/var/local/mnt-data /private/var /private/var/local/mnt-data   # keeps ACLs, xattrs, flags
vcopy -c /private/var /private/var/local/mnt-data -x /private/var/local/mnt-data  # compare; then vcopy -m if needed
echo "/dev/ufsroot1 /private/var hfs rw,nosuid,nodev 0 2" > /private/etc/fstab
```
Reboot. Escape if it doesn't boot: boot with `sweetnofstab=1`, then `toybox unlink /private/etc/fstab`.
Not `toybox cp -a`: it drops ACLs and container xattrs (installcoordinationd EACCES). The exact flags of the
Oct 27 run were not all logged (newfs_hfs flags are reconstructed from its output); details in
[features/storage.md](features/storage.md).

## 6. Phone software (scripted)
```sh
tools/build-userland-linux.sh          # dropbear, toybox, dash, the phone tools, wland, adspboot
tools/sunfish-install-ssh.sh           # SSH; reboot afterwards
tools/sunfish-dropbear-early.sh        # SSH earlier at boot
tools/sunfish-install-wifi.sh          # wland + firmware (from ../restore and ../backup)
python3 -I tools/sunfish-netprefs.py   # en0 network service
tools/build-metal2-linux.sh && tools/sunfish-metal2-install.sh work/metal2-linux/out/phone/metal2/out --no-restart
tools/build-cocoatop-linux.sh
tools/sunfish-setup-phone.sh           # launchd job drops, CA_FORCE_TIMER, phone tools, CocoaTop, timezone, flag files
tools/sunfish-install-adsp.sh          # audio (unfinished feature)
tools/sunfish-install-bt.sh            # Bluetooth firmware (crbtfw21.tlv, crnv21.bin from the factory vendor image) + btup
tools/sunfish-install-sensors.sh       # auto-rotate: adspboot sensor feed at boot, job org.iosonpixel.sensors (kernel107+, sweetmotion=0x37 default; after step 8's config push)
tools/sunfish-install-btbridge.sh      # Bluetooth for iOS: btbridge + BlueTool stand-in + bluetoothd transport (kernel105+, boot-arg btbridge=1, default)
tools/sunfish-install-camera.sh        # camera for iOS (unfinished): camd job, camdev.on, StartRecorderServer, MobileGestalt rebuild; after sunfish-install-audio.sh; boot with CAMERA=1 (kernel111+)
tools/sunfish-install-haptics.sh       # haptics for iOS: hapticd + drvbuzz, job org.iosonpixel.hapticd (off: /var/local/haptics.off), MobileGestalt rebuild; after sunfish-install-audio.sh; boot with HAPTICS=1
```
Reboot after these (launchd reads its job list at boot).

## 7. Wi-Fi network (by hand, never in git)
Write `/var/local/wlan.conf` (mode 600) with your network name and password in the format wland expects
([features/wifi.md](features/wifi.md)). The phone joins at every boot.

## 8. Sensors files for the DSP (unfinished feature)
`tools/sunfish-sensors-files.sh` prints the push commands for the sensor configs from the factory vendor image;
`tools/sunfish-soc0-files.sh` writes the fake soc0 files.

## 9. iOS settings (by hand, with fake taps over SSH)
- Auto-Lock Never: Settings > Display & Brightness > Auto-Lock > Never (rows in [features/apps.md](features/apps.md),
  fake taps in [methods.md](methods.md) "Phone control file").
- Location Services on: `echo "L 1" > /var/local/sweet-orient`.
- Files "On My iPhone" (LocalStorage provider needs a plugin data container): `lsreg -i com.apple.FileProvider.LocalStorage`,
  then `lsreg -c com.apple.FileProvider.LocalStorage /System/Library/Frameworks/FileProvider.framework/PlugIns/LocalStorageFileProvider.appex`,
  then `launchctl kickstart -k system/com.apple.pluginkit.pkd` and `launchctl kickstart -k user/501/com.apple.FileProvider`.
