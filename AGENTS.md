# Working on the iOS port to the Pixel 4a (sunfish)

- Read `README.md`, then `docs/README.md` (the feature index), `docs/plan.md` (the current plan) and
  `docs/methods.md` (tools and test loop). Each feature has its facts and dated log in `docs/features/<name>.md`;
  the daily journal is `docs/journal/`. The earlier ports are archived: flame (Pixel 4) in `docs/archive/flame/`
  (HANDOFF.md, NOTES.md with the ABL boot image rules, DEVICE.md, METAL-NEXT.md, RECOVERY.md, QEMU_TESTING.md) and
  ios-sweet in `docs/archive/sweet/`.
- Record each milestone in the feature's Log and its row in `docs/README.md`, with the kernel number, the boot args,
  exact results and what is still open. Keep observations apart from guesses. A new kernel patch gets a line in the
  feature Log too.
- Kernel changes: the kernel tree is `work/src/xnu` (a git repo, HEAD = baseline + applied patches). Commit there,
  then save the diff as the next `xnu/patches/NNNN-sunfish-*.patch`. Build it in the local macOS VM:
  `tools/vm-build-xnu.sh NN --stop` (about 1 min incremental, 6 min full) -> `work/kernelNN`. The VM takes 8 GB of
  the PC's 16 GB, so `--stop` shuts it down afterwards. CI fallback: `gh workflow run xnu.yml --ref main`,
  `gh run watch ID`, `gh run download ID -n xnu-kernel -D work/kernelNN`.
- Git: push only to branch `main` (the `sunfish` branch is frozen), never force-push.
- Device bring-up uses temporary `fastboot boot` images (`tools/sunfish-boot.sh NAME KERNEL [bootargs]`). Never
  flash ABL, XBL, TZ, modem or other firmware partitions, never relock the bootloader on modified software, never
  select "Factory data reset" in recovery, and keep the partition backups intact. Writes to userdata are
  incremental only, with a backup first. Confirm each destructive step with the user before running it.
  `docs/recovery.md` covers getting back to stock Android on the 4a.
- Keep out of git: partition dumps, device trees and logs from a phone (serials, IMEI, network names),
  Apple's files (IPSW parts, the root image, shared cache, kernelcache, anything translated from Apple's
  shaders; `research/` is gitignored), Google and Qualcomm images and firmware, and Wi-Fi credentials
  (`/var/local/wlan.conf` lives on the phone only).
