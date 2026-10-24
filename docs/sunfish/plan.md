# Next steps: Pixel 4a iOS port, display smoothness and stability (handoff plan)

## Context
- **Goal:** iOS 16 running RAM-booted on a bootloader-unlocked Pixel 4a ("sunfish"), smooth enough to scroll and watch Instagram reels.
- **Working today:** Wi-Fi, Safari, Metal via Turnip + metal2vulkan.
- **Display now:** vsync-paced at about 21 panel updates/s, with no dropped swaps. The user says it's a bit smoother and more consistent; the target is as close to 60 as possible.
- **Repo:** `/home/johann/Projects/Claude Code/ios pixel 4a/port`, branch `sunfish`. Push only there, never force-push.
- **Kernel source:** `port/work/src/xnu`. Patches are exported to `port/xnu/patches/NNNN-*.patch`.
- **Logs:** `docs/workday.md` has the full log. Read its last three sections first.

## Hard rules (must follow)
- **Boots:**
  - RAM-only: `tools/sunfish-boot.sh NAME KERNEL [bootargs]`, run from `port/`.
  - Never flash partitions, never relock, never select "Factory data reset".
  - Writes to userdata are incremental only (single files, with a backup first).
- **Battery and hangs:**
  - Stop booting below 3600 mV (`fastboot getvar battery-voltage`).
  - After a hard hang, stop and write it up in docs/workday.md.
  - GPU test boots use flamereboot=1800 (the default). Use flamereboot=0 only for long file copies.
- **MDP registers:**
  - Never read or write MDP registers other than the ones `tools/sunfish-dim.sh` already uses.
  - Reading 0x0ae44000 hangs the bus.
- **Memory:** the PC has 16 GB RAM.
  - Never run `ipsw dyld xref --all`; it caused an OOM.
  - Use `--image X`, and wrap heavy jobs in `systemd-run --user --scope -p MemoryMax=8G`.
- **Commits:** end each message with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- **Brightness:** leave it at 100%. Dim only if the user asks (`tools/sunfish-dim.sh N`).

## Current state
- **Best kernel:** `work/kernel37/kernel.development.sweet`, boot-arg `sweetpresentwait=1`.
  - It is the kernel36 base plus 0159 (64-byte IOSurface rows), 0160 (logs IOMFB async calls) and 0161 (present-on-swap, off by default).
- **On the phone:**
  - Metal bundle with Turnip patches 0100–0104 and bridge patches 0101–0102.
  - `/System/Library/xpc/launchd.plist` has backboardd `CA_FORCE_TIMER=1`; the original is at `/var/local/launchd.plist.pre-vbl`.
- **Tools:**

  | Tool | Use |
  |---|---|
  | `tools/sunfish-cfgbench.sh NAME KERNEL args` | Boot, then 2× swipe bench |
  | `tools/sunfish-m2env-bench.sh TAG KEY=VAL` | Metal env experiment without a reboot |
  | `MESA_GPU_TRACES=print` (via the m2env bench) | GPU time per operation |
  | `SWEET_M2V_MESSAGES=/var/local/m2v-msg.log` | Bridge degradation messages; pre-create mode 666 files |
  | pstack + `a2s` | CPU samples |

- **Shared cache:** `research/dsc/` holds the full iOS shared cache, with symbols.

## Step 1: Present path off the render ring (biggest remaining win)
- **Problem:**
  - The present blit runs inline on the vsync tick and takes 10–17 ms of wall time, but only ~2.4 ms of GPU work.
  - It waits behind backboardd's next GPU job on the single ring, so each vsync notification goes out late.
  - A present thread and present-on-swap (0161) were both worse (~14/s); don't retry them.
- **Option A: NOT APPLICABLE (checked 2026-10-24).** `vsyncFired` already notifies clients before it presents (`SweetFramebuffer.cpp`, client->notify loop precedes `present(rec)`), and re-arms the timer first. The next tick still slips because the present blocks the same timer thread, which a present thread (worse, ~14.7/s) already tested. No patch 0162.
- ~~Option A (original text):~~ skip the present when the previous one is still in flight, but send the vsync notification *before* presenting.
  - In `SweetFramebuffer.cpp` `vsyncFired`, move the client notify loop above the present.
  - Patch 0162 with a boot-arg to turn it off.
- **Option B (next for step 1, risky GPU bring-up; ring code is `a6_ring`/CP_RB_CNTL in `bsd/dev/sweet_a6xx.c`, only RB0 is set up today):** a second ringbuffer at higher priority for the present.
  - Turnip/msm use RB0..3 with preemption on a6xx.
  - Look at `bsd/dev/sweet_a6xx.c` ring setup and `sweet_gpu.c` `sweet_gpu_present`.
  - Bigger job; do it only if A gives less than +5 updates/s.
- **Measure:** `tools/sunfish-cfgbench.sh` and the `SWEET FB: 5s ... avg copy` lines.

## Step 2: Remaining per-frame copies (FAILED 2026-10-24, parked)
- **Tried:** Turnip 0105 (`phone/turnip/failed/0105-*.patch`): explicit-layout images without color/storage/depth usage accept a 16-byte pitch (`fdl_explicit_layout.relaxed_pitch`).
- **Result:** refusals went to 0 (34 in the 30 s before), but the GPU took an SMMU translation fault within ~2 s of backboardd restarting (fsr 2, far 0x100e634240, fence hang, reset), then every Metal call returned DeviceLost and the screen stayed black. The bench numbers after that (23.9 / 24.8 updates/s vs 23.6 / 23.1 before) are meaningless.
- **Likely cause:** the texture unit needs 64-byte-aligned linear pitch; with 16/32-byte pitch it walks past the aliased mapping. Not investigated further.
- **If retried:** keep the bridge's shadow copy for those textures (it is only ~3 ms/frame), or pad the IOSurface allocation itself instead of relaxing Turnip.
- Removed from `patches/` so CI builds without it.

## Step 3: GPU memory leak (mostly NOT a leak, measured 2026-10-24)
- **Method:** `tools/sunfish-m2env-bench.sh census TUDBG=bos` (census goes to `/var/local/tubos/backboardd-PID.log`; `mkdir` + `chmod 777` that dir first), then `flame-cmd tap 200 450` + `swipe 540 2300 540 1000` (open an app, go home) in a loop. Pull the file with `flame-files.py get`.
- **Result (kernel37, k3 boot):** idle swipe bench is flat at ~630 BOs / 157 MB. The first 6 app switches took it to 1380 BOs / 248 MB peak, which settled at 1318 / 204 MB. 12 more switches: 1335, then 1353 BOs (207, 208 MB), i.e. ~1.5 BOs and ~0.2 MB per switch. GPU pool stays ~250 MB (peak 317), far from the 944 MB that froze it.
- **Where the warm-up growth is:** Turnip command streams (cmd cs +158, draw cs +200, draw sub cs +204, tile store cs +103, small) and a few 8-11 MB heap blocks; likely cmd-pool caches, not a leak.
- **Left:** the ~1.5 BOs/switch drift is small; only chase it if the pool grows over a long soak (`tools/sunfish-soak.sh`). Use Instagram/Safari with long scrolling as the heavier test.

## Step 4: Charger
- **Problem:** the phone drains about 0.5 A while benching, even on the charger. The kernel reads the charger but never configures it.
- **Next:** find the PMIC charger current-limit registers (sm7150, pm6150), read-only first. Write up the findings before poking anything.

## Verification (each step)
- `tools/sunfish-cfgbench.sh` twice per config. Record swaps/s and panel updates/s in a table in docs/workday.md.
- Webcam frame for any visual change.
- Commit and push after each measured step.

## Hand-off notes for a smaller model
- One change per boot. If a result is worse, turn it off with its boot-arg and move on; don't keep iterating on it.
- Read `docs/workday.md` sections [00:40] and [08:40] before starting.
