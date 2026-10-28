# Methods and tools

How the work on the Pixel 4a (sunfish) is done: booting, building, measuring, profiling and reverse engineering.
Results live in the feature logs (`docs/features/`); the plan is `docs/plan.md`. Most of this file came from the old
`docs/sunfish/display-perf.md` ("Methods and tools", "Per-frame timeline") and `docs/sunfish/status.md` ("How to boot").

## Booting (from fastboot)
```
cd port && tools/sunfish-boot.sh NAME work/kernel37/kernel.development.sweet sweetpresentwait=1
```
- RAM-only boots: `tools/sunfish-boot.sh NAME KERNEL [bootargs]`. Defaults and escape hatches: [features/boot.md](features/boot.md).
- Stop booting below 3600 mV (`fastboot getvar battery-voltage`). Start the live-log listener first (`pgrep -af sunfish-livelog`).
- Tools (status.md): `flame-cmd.py reboot|swipe|tap` (and `key N [MS]`), `flame-files.py get|put`, `flame-shot OUT.png`,
  `sunfish-install-ssh.sh`, `sunfish-install-wifi.sh`, `sunfish-livelog.sh` (log → logs/usb-live.log).

## Building
- Kernel: edit `work/src/xnu`, commit there, save the diff as `xnu/patches/NNNN-sunfish-*.patch`. Build it with tools/vm-build-xnu.sh (below); CI fallback: `gh workflow run xnu.yml --ref main`, `gh run watch ID`, `gh run download ID -n xnu-kernel -D work/kernelNN`.
- Incremental xnu CI skips new files (b5cb57f); `git add -A` in work/src/xnu once committed the whole untracked tree into a patch (2026-10-24): add paths only.
- Local builds on this Linux PC (since 2026-10-28; no runner queue). They need the iOS SDK in research/sdk/theos-sdks/iPhoneOS16.5.sdk (theos/sdks, gitignored), clang + ld64.lld, ldid, and per-user Rust 1.98.0 with aarch64-apple-ios in ~/.cargo/bin. tools/linux-ios-cc is the clang wrapper and tools/linux-ios-sign the signer (ldid; codesign on macOS).
  - Metal bundle: `tools/build-metal2-linux.sh` (full 5 min 11 s, no-change 14 s) -> work/metal2-linux/out/phone/metal2/out; then `tools/sunfish-metal2-install.sh work/metal2-linux/out/phone/metal2/out --no-restart`. Matches CI run 40676463855 in size, dylibs, exports and install names (SDK version 16.5 vs CI 17.5; a few extra weak libc++ imports).
  - Kernel (since 2026-10-28): `tools/vm-build-xnu.sh NN [--clean] [--stop]` builds in the macOS 14 VM (~/VMs/macos, Xcode 15.0.1, same steps as xnu.yml) -> work/kernelNN (kernel + .link + SHA256SUMS). The VM keeps ~/xnubuild/dxb; after a good build the next run is incremental (re-patch + rsync -c). Full 337 s, no-change 54-68 s. kernel71 = kernel70's patch set: same size (17895944), load commands and segment sizes; only the version string's "runner:" becomes "builder:". Build log in the VM: ~/xnubuild/build.log. `VM_JOBS=N` lowers build.sh's make -j8.
  - The VM: ~/VMs/macos (outside git): mac-vm.sh (QEMU/KVM, OSX-KVM OpenCore, 8 vCPUs as 8 cores x 1 thread pinned to the host E-cores 4-11 by vm-pin.py, 8 GB, SSH localhost:2222, VNC :5901 with a password), vm-start.sh (it also picks Macintosh HD in the OpenCore picker), vm-stop.sh, vssh, vmctl.py (QMP screenshots/keys/clicks). Credentials in ~/VMs/macos/credentials (mode 600). macOS 14.8.9 + Xcode 15.0.1 (the same as the CI runner).
  - Phone tools: `tools/build-userland-linux.sh` (35 s, 18 binaries) -> work/userland-linux/out; then `python3 -I tools/flame-files.py put work/userland-linux/out/NAME /var/local/bin/NAME 755`.
- CI is the fallback and still builds on push: phone/turnip/patches or phone/metal2 -> metal2 CI (`gh run download ID -D DIR`); phone/** -> userland CI (`gh run download ID -n ios-userland -D DIR`). macOS arm64 runners often fail with "job not acquired" (3 of 4 metal2 runs on 2026-10-28).

## RAM-log rescue (the previous boot's kernel log)
- The shim rescues the previous kernel's RAM log on every boot into the last 1153 blocks of userdata (rescue block 28728186). Read it before rebooting, or it is lost.
- Read: `toybox cmp -l /dev/rufsroot0 /dev/zero $((28728186*4096)) 0 | toybox head -n 262144` (cmp seeks and reads large aligned chunks; sub-block raw reads are broken). The overnight run also used `toybox xxd -s 117670649856 -l 262144 -p /dev/rufsroot0` (64 blocks at nblocks-1153). Details: [features/storage.md](features/storage.md).

## App soak
- `tools/sunfish-appsoak.sh TAG SECONDS` opens 6 stock apps in cycles. Stopping an agent does not stop the background scripts it started: check `ps` for scratchpad scripts after every TaskStop.

Subagents that run these: `.claude/agents/` (phone-runner, ci-builder, implementer, investigator); see CLAUDE.md "Delegation".
### Where the data comes from
- **Live kernel log:**
  - `tools/sunfish-livelog.sh ../logs/usb-live.log` runs on the PC in the background and appends every kernel log
    line from the phone. It also brings up the USB network link (phone 192.168.7.2).
  - If SSH says "No route to host", this listener is probably not running.
  - Every boot or bench script writes a `=== NAME ...` marker line into it. Slice the log between markers with
    `grep -n '=== NAME'`, or `sed -n '/=== bench X start/,/=== bench X end/p'`.
- **Kernel stats lines, every 5 s:**
  - `SWEET FB: 5s: N swaps (fps), M panel copies, avg copy U us`: what CA sent versus what reached the panel,
    and the present's wall time.
  - `SWEET GPU: 5s: N submits, GPU busy X ms (P%), longest, mean; <proc> n/ms ...`: GPU time per process.
    For backboardd, ms divided by n is the GPU time per frame.
  - `SWEET GPU: present: N frames, avg cache clean, GPU blit, put off` (every 300 frames).
  - `SWEET GPU: pool: N MB in use` and the census lines track GPU memory.
  - `SWEET BATTERY: mV mA ... shown N%` gives the battery level; negative mA means draining.
- **Benchmark:** `tools/sunfish-fpsbench.sh TAG SECONDS` (BENCH_TIGHT=1)
  - It swipes between home pages with `flame-cmd swipe`.
  - It then averages the `SWEET FB: 5s` lines between its markers into "swaps/s, panel updates/s".
  - `sunfish-cfgbench.sh` and `sunfish-m2env-bench.sh` both call it.
- **Soak logger:** `tools/sunfish-soak.sh ../logs/soak.log` logs up / unreachable / REBOOTED, battery and new
  crash lines every 60 s. Use it to catch reboots and hangs while away.
- **Battery in fastboot:** `fastboot getvar battery-voltage`.
- **Screen (webcam):**
  - `ffmpeg -f v4l2 -i /dev/video0 -frames:v 1 snap.jpg`. The Iriun device number can change; check with
    `v4l2-ctl --list-devices`.
  - Check every visual change on the webcam.
- **GPU trace totals:**
  - Copy the `MESA_GPU_TRACES` log to the PC, then run `tools/gputrace-sum.py` on it.
  - It prints GPU time per operation, e.g. `copy_buffer_to_image` against `render_pass`.
- **Bridge telemetry:**
  - `/var/local/mtl2.log` `K` lines give per commit: `rp` passes, `dr` draws, `kpx` target size, and
    `ctc` = commit-to-callback (about the GPU time).
  - Commit timestamps show the frame pacing.
- **CPU sampling:**
  - On the phone: `pstack PID >> file` 80× every 0.25 s during a bench.
  - On the PC: symbolize the addresses with `a2s`, then count leaf and inclusive frames.
  - Threads with pc 0 or in libsystem_kernel are idle.

### Metal client experiments without a rebuild
- **`tools/sunfish-m2env-bench.sh TAG KEY=VAL...`** writes `/var/local/mtl2-env`, restarts backboardd and benches.
  `TUDBG=opts` sets TU_DEBUG.
- **Clients run as mobile:** pre-create log files with mode 666, e.g. `/var/local/turnip-backboardd.log` or
  `/var/local/m2v-msg.log`.
- **GPU time per operation:** `MESA_GPU_TRACES=print MESA_GPU_TRACEFILE=/var/local/turnip-backboardd.log`.
  Sum `start_X`/`end_X` pairs.
- **Bridge degradations:** `SWEET_M2V_MESSAGES=/var/local/m2v-msg.log`. These are shadowed textures and similar,
  and each kind is logged only once.
- **Frontend telemetry:** `touch /var/local/mtl2-telemetry` makes `/var/local/mtl2.log` show per-commit
  passes, draws and commit-to-callback time every 10 s.

### Boot matrix
- **`tools/sunfish-cfgbench.sh NAME KERNEL args`** reboots, boots the kernel, and runs two 20 s swipe benches.
  Set `DIM=N` to dim the screen.
- **Bench noise is about ±3 updates/s.** Always run twice, and compare configs from fresh boots only.

### CPU profiling
- Run `pstack PID` in a loop on the phone, then symbolize shared-cache addresses with `/var/local/bin/a2s`
  (`phone/tools/a2s.c`).
- Shared-cache local symbols are redacted on the phone. Use the PC copy for real names.

### Reverse engineering iOS frameworks
- **`research/dsc/`** is the full shared cache pulled from the phone. It is gitignored because it contains Apple's
  files; keep it out of git.
- **Disassemble one image:**
  `systemd-run --user --scope -p MemoryMax=8G ipsw dyld disass research/dsc/dyld_shared_cache_arm64 --image QuartzCore > research/re/QuartzCore.s`
  Then grep for `_stub` calls and the symbols of the `__ZN…` functions that contain them.
- **Never run `ipsw dyld xref --all`:** it ran the 16 GB PC out of memory.
- **Copying a big file from the phone:** boot with `flamereboot=0`, and give `ssh` `</dev/null` inside
  `while read` loops (otherwise it reads the rest of the loop's input).

### Brightness
- **`tools/sunfish-dim.sh N`** sets the LM_0 blend alpha. It resets on reboot; leave it at 100 unless asked.
- **Reading 0x0ae44000 hangs the bus.** The MDP registers are at the catalog offset + 0x1000.

## Per-frame timeline (xnu 0163)
- Boot with `sweettrace=1` (works with `sweetpresentwait=1`). The kernel logs `SWEET TL:` lines (events: swap, vsync tick, notified, present begin/end, every submit, each landed submit's CP start/end, a mach/CP clock pair every 30 ticks).
- `tools/sunfish-timeline.py ../logs/usb-live.log "bench TAG start" "bench TAG end"` joins them: swap-to-swap histogram (60 Hz metric: share <= 17 ms), GPU time per submit by pid (0 = kernel present), and the per-stage gaps. Mach time and the CP always-on counter both run at 24 MHz here.
- Tracing overhead is not visible in the bench (22.3 / 21.7 updates/s vs 21-24 without).
