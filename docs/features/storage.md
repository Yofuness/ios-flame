# Storage
**State:** ✅ /private/var on /dev/ufsroot1 (94 GB HFSX) via /private/etc/fstab, kernel68+. **Updated:** 2026-10-27.

## How to use
- /private/etc/fstab: `/dev/ufsroot1 /private/var hfs rw,nosuid,nodev 0 2` (kernel68+).
- Escape: boot `sweetnofstab=1`, then `toybox unlink /private/etc/fstab`.
- Copy tool: `phone/tools/vcopy` (copyfile metadata, own data copy). Do not use toybox cp -a for /var (loses ACLs and container xattrs).
- Raw reads: the phone has no `dd`; use `toybox cmp -l /dev/rufsroot0 /dev/zero OFF 0 | toybox head -n 262144` (see methods.md, RAM-log rescue).

## Facts
- userdata is 0x1B65FFB000 bytes (~117 GB) = 0x1B65FFB 4 KB blocks; the iOS root (HFS+ 16 GiB) uses ~16 GB of it.
- xnu 0185: /dev/ufsroot1 + rufsroot1 = blocks 4194304 .. nblocks-1153-256 (24,533,626 blocks). The last 1153 blocks hold the rescue/console logs; the RAM-log rescue is at block 28728186.
- Fixes: 0175 raw reads (copyout/copyin for B_PHYS), 0187 newfs ioctls, 0191 d_psize, 0192 clip at device end, 0194 platform binaries on ufsroot1.
- df: 98134504 KB total / 3.7 GB used (k68v). The old /var stays on the root underneath (not visible while mounted).

## Open
- Settings > Storage screenshot; the root's old /var copy could be deleted later to free 3 GB on the root (plan step 5).
- Block driver direct I/O: copyfile's fcopyfile data path gets EFAULT on /dev/ufsroot* files (vcopy uses its own read/write loop). Where to start: sweet_ufs.c strategy with user-space buffers (broken.md).
- Sub-block raw reads return the block start (rsc1, 2026-10-25); the cmp-based read works around it.
- fsck_hfs still flagged the alternate VHB (-60) on k65 -> 0192.

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

### 2026-10-28 13:45 direct-I/O EFAULT: root cause + xnu 0198 (kernel75, not booted)
- 0175 took B_PHYS alone as "user address". Cluster direct I/O (CL_DIRECT_IO, e.g. fcopyfile with F_NOCACHE) is B_PHYS too but UPL-backed (B_CLUSTER, vfs_bio.c:798); buf_map maps the UPL into the kernel, so copyout/copyin on it gave EFAULT. 0198: user VA = B_PHYS && !B_CLUSTER (physio keeps copyout/copyin, the 0175 PAN fix stays). Boot arg sweetufsdio=0 restores the old test. VM build 168 s.
- Test tool phone/tools/fcopy + tools/sunfish-ufs-dio-test.sh (scratch files only); the uncached `fcopy -n` copy is the real check.

### 2026-10-27 ~21:50- overnight run (user away until 05:00; plan.md "Overnight run") (storage part)
- Storage: root is HFS+ 16 GiB, 94% full; userdata has ~93.6 GiB free after it (rescue/console logs in the last 1153 blocks). xnu 0185 (kernel61) adds /dev/ufsroot1 + rufsroot1 = blocks 4194304 .. nblocks-1153-256 (24,533,626 blocks); offset/end checks pass (block 0 and 1000 identical to the partition blocks, last block reads, one past = 0 bytes).
- newfs_hfs failed with ENOTTY: 0187 answers DKIOCGETMAXBYTECOUNT*/MAXSEGMENTBYTECOUNT* (kernel62). Then "Initialized /dev/rufsroot1 as a 94 GB case-sensitive HFS Plus volume with a 8192k journal" (1.2 s).
- Copy test (g62e): 3.1 GiB /var copied in 84 s; 1977/2000 sampled files identical, same after umount/remount; differences = aps.db* (live writes) and 20 emoji-poster snapshots created after the copy; sockets can't be copied.
- fsck "Volume header needs minor repair" + mount_hfs "GetMasterBlock: Error 22": (1) d_psize returned the size in sectors where specfs wants the sector size -> every read(2)/write(2) on a block node failed EINVAL since 0028 (0191, kernel65); (2) alternate VHB still unreadable: strategy failed transfers that cross the device end instead of clipping like IOMedia (0192, kernel66).
- Also built: 0188 Wi-Fi diag logging, 0190 torch via whitelisted SPMI write (sweettorch=1), kernels 63-66 (CI 40617690407, 40618033335, 40618342567, 40621294989).
- Stage 2 attempts: k66v (toybox copy) died at 35 s after installcoordinationd EACCES x3 (copy lost ACLs + com.apple.containermanager.* xattrs). New phone tool vcopy (copyfile; its fcopyfile data path got EFAULT on 5691 sqlite files -> own read/write data copy; it nests SRC's basename into an existing DST; files got ACLs inherited from their dir -> -m pass clears them). Final copy: 0 metadata differences in 47924 entries.
- k67v died at 63.6 s: RAM-log rescue (64 blocks at nblocks-1153, read via `toybox xxd -s 117670649856 -l 262144 -p /dev/rufsroot0`) had the panic: "userspace panic: unexpected exec of non-platform binary: /var/local/bin/wland". sweet_amfi.c (0035) marks binaries platform only on MNT_ROOTFS -> 0194 adds f_mntfromname == /dev/ufsroot1.
- k68v (kernel68, CI 40626725130): /var on ufsroot1 (98134504 KB, 3.7 GB used), home screen on the webcam, SSH up > 3 min, crashes: FitnessWidget x3 only. DONE.
- Lessons: the escape boot hides fstab from cat/stat too, so stage files elsewhere and copy them in on a normal boot; the live log starts ~27-60 s after boot, the RAM-log rescue has the rest.

### 2026-10-27 experiments.md lines
- 2026-10-27 u61 (kernel61, xnu 0185): ufsroot1 data region: offset checks a-e pass (rufsroot1 block 0/1000 == rufsroot0 block 4194304/4195304; last block 4096 B; one past 0 B). newfs_hfs: ENOTTY (missing segment byte-count ioctls).
- 2026-10-27 g62/g62e (kernel62, 0186+0187): newfs_hfs OK (94 GB HFSX, 8 MB journal); mount + cp + sha256 OK; sweetnofstab=1 hides /etc/fstab, rm fails, toybox unlink works. fsck_hfs -fn: "Volume header needs minor repair" (block-node EINVAL, alternate VHB unreadable).
- 2026-10-27 g62e: full /var copy to ufsroot1, 84 s, 1977/2000 sha256 equal (3 live sqlite, 20 created later), stable across remount.
- 2026-10-27 k65 (kernel65, 0191): block-node read works (xxd on /dev/ufsroot1 gives 4858...); fsck still flags the alternate VHB (-60) -> 0192.
- 2026-10-27 k66v (kernel66, /var on ufsroot1 via fstab, toybox copy): reset at 35 s; installcoordinationd Code=13 x3, mobileactivation sig 5. Cause: copy lost ACLs/xattrs.
- 2026-10-27 vcopy runs on k66e: copyfile data path EFAULT on 5691 files; nesting into DST/var; inherited ACLs on 199 files; final -m pass -> 0 metadata differences.
- 2026-10-27 k67v (kernel67, vcopy copy): launchd panic at 63.6 s "unexpected exec of non-platform binary: /var/local/bin/wland" (from the RAM-log rescue). Fix 0194.
- 2026-10-27 k68v (kernel68): /var on ufsroot1 (94 GB) boots to the home screen, SSH, dropbear/wland up, 3 min stable. PASS.

### 2026-10-27 broken.md "Fixed" row
| Area | Fixed | How |
|---|---|---|
| Storage (16 GB of 128 GB) | 2026-10-27 | /var on the 94 GB ufsroot1 volume, kernel68+ (xnu 0185-0194) |

### 2026-10-25 Plan step 2: hang triggers (raw UFS reads and the RAM-log rescue part)
- **[17:02] rst3: a raw disk read resets the phone, reproducibly.**
  - Boot: kernel50, `sweetpresentwait=1 flamereboot=0`. Booted fine at 4454 mV; snaps OK.
  - The phone has no `dd` (/var/local/bin/dd → toybox without dd).
  - I ran `toybox xxd -s $((0xc80ffb-1153)*4096) -l 64 /dev/rufsroot0`, i.e. a read 64 bytes long at the rescue-block offset computed from NOTES.md's flame userdata size.
  - The phone dropped to fastboot instantly. The live log stops at 159.0 s, with no panic line.
  - That offset is probably past the end of sunfish's userdata (the size is unknown on sunfish). **An out-of-range or raw read on rufsroot0 kills the kernel at once and silently.**
  - The soak resets look the same (instant stop, no panic). A UFS I/O fault while apps load is now a suspect.
  - Next: find sunfish's real userdata size (shim log "ufs: userdata first= blocks=", or the GPT from backup/), and look at how the XNU UFS driver handles an out-of-range LBA or a UFS error (sense data, timeouts).
- **UFS investigation (no phone use):**
  - **Partition size:** sunfish userdata is 0x1B65FFB000 bytes (`fastboot getvar partition-size:userdata`) = 0x1B65FFB 4 KB blocks. So the rst3 read offset was inside the partition. The rescue area starts at block 28728186 of /dev/rufsroot0 (1153 = 1025 XNU log ring + 64 scratch + 64 rescue, shim/ufs.h).
  - **Not out of range:** sweet_ufs_strategy bounds-checks (EINVAL beyond part_bytes). Timeouts return ETIMEDOUT and SCSI errors EIO; there is no panic on either path.
  - **Likely cause of the rst3 reset (~65%):** for raw (physio, B_PHYS) I/O, sweet_ufs_strategy memcpy's from the bounce buffer straight into the user virtual address (sweet_ufs.c:448-449, write at 459). With PAN on, that is a data abort → panic, which never reaches the UDP log. The watchdog then bites into fastboot.
    - Fix: copyout/copyin for B_PHYS.
    - Also: an LBA range check in ufs_read_blocks, log the partition geometry, and more state on timeouts.
  - **The soak resets have no UFS lines before them** (no timeouts or errors), so they are probably a different cause. Still open.
- **[17:55] rsc1 (kernel51, xnu 0175): raw reads no longer reset the phone.**
  - The boot-args line was logged at "2815 s": uptime did not reset across the last reset, but it is a fresh boot. The partition-size line does not reach the live log.
  - `toybox xxd -p -s OFF -l N /dev/rufsroot0` at offset 0 and at the rescue block (28728186) works, and the phone stays up. The first rescue block starts with `SWEETRSC`, total 0x4e093 bytes.
  - **New bug:** xxd -p reads the raw device 30 bytes at a time, and every sub-block read returns the start of its 4 KB block. So the rescue dump is garbage: the header repeats every 30 bytes.
  - The decoded fragments show a panic report around 159.8 s, the rst3 xxd reset (lr 0xfffffe0007863940), so rst3 was a panic. The panic string is not readable.
  - /dev/ufsroot0 (the block device) is busy because it is mounted, and xxd's -c is limited to 256.
  - Next: make the raw read path honour the offset within a block (or reject unaligned raw I/O with EINVAL), or add a phone tool that preads whole 4 KB blocks.
- **[18:10] Rescue log read with kernel51 (no new boot, so it was not overwritten).**
  - The shim rescues the previous kernel's RAM log on every boot. Read the rescue before rebooting, or it is lost.
  - Method: `toybox cmp -l /dev/rufsroot0 /dev/zero OFF 0 | toybox head -n 262144`. cmp seeks and reads large aligned chunks and prints each non-zero byte with its position; rebuild on the PC (../logs/rsc-good.txt).
  - **rst3 was a panic, as suspected.** `panic(cpu 0 ...): Kernel data abort at pc 0xfffffe0007bce8b8, lr 0xfffffe000751b710`, esr 0x9600004f (permission fault), far 0x100767918 = x0 = the user buffer of `toybox xxd`. It happened at 159.79 s, 10 ms after `exec toybox`.
  - That is the PAN fault on memcpy into a user address, fixed by 0175. The rescue-area layout and the decode both work.
  - The soak resets' rescue data was overwritten by later boots, so they are still unexplained. Next soak: on a reset, read the rescue with this method right after the next boot (kernel52 also fixes unaligned raw reads, xnu 0176, built and not booted yet).

### 2026-10-25 experiments.md lines
- 2026-10-25 rst3 (kernel50): raw read of /dev/rufsroot0 at offset (0xc80ffb-1153)*4 KB (flame's userdata size) → instant silent reset to fastboot, no panic. UFS I/O error path kills the kernel; suspect for the soak resets too.
- 2026-10-25 UFS read-path review: sunfish userdata 0x1B65FFB blocks (rst3 offset was in range); raw-device reads memcpy into user VA (PAN abort) = likely rst3 cause; soak resets show no UFS errors, cause still open.
- 2026-10-25 rsc1 (kernel51, xnu 0175): raw rufsroot0 reads no longer crash; SWEETRSC found at block 28728186, but sub-block raw reads return the block start (xxd reads 30 B) → dump garbled. rst3 confirmed as a panic (~159.8 s).
- 2026-10-25 rescue log (read via toybox cmp -l vs /dev/zero): rst3 = Kernel data abort, esr 0x9600004f, far = toybox xxd user buffer → PAN memcpy bug (fixed 0175). Soak-reset rescues were overwritten.

No entries before 2026-10-25.
