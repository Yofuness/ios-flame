# GPU memory: archived log (entries before 2026-10-25)
Current file: [features/gpu-memory.md](../../features/gpu-memory.md). Same format, newest first.

## Log

### 2026-10-24 10:20 Step 3: BO census over app switches (kernel37, k3)
- Census with TU_DEBUG=bos on backboardd: idle 630 BOs/157 MB; after 6 app switches 1380/248 MB peak, settled 1318/204 MB; after 12 more switches 1335 then 1353 BOs (207, 208 MB). Drift ~1.5 BOs and ~0.2 MB per switch; GPU pool ~250 MB (peak 317).
- The earlier "340 BOs per switch" was cache warm-up (command-stream BOs), not a steady leak. Step 3 downgraded in plan.md.
- The phone auto-rebooted to fastboot once at the 1800 s flamereboot timer (expected, not a hang); rebooted as k3 at 3814 mV.

### 2026-10-23 23:05 Smoothness measurements + memory work (stopped at 16% battery)
- See [smoothness](smoothness.md) for this entry; it also touches this feature.

### 2026-10-23 22:35 GPU stability — what actually freezes/blacks the screen
- **Black screens during app stress were Photos/Notes/Weather stuck at launch** (Photos waits on assetsd /
  PhotosFileProvider, which crash every launch). A hung frontmost app also blocks openapp switches.
  Not a GPU problem; test with Safari/Settings/Calculator.
- **The "zoom + freeze" and the later respring = GPU pool exhaustion in backboardd.** Pool 940/944 MB →
  backboardd's next Turnip command-stream BO refused → Turnip writes through cs->cur == NULL
  (libvulkan_freedreno+0x57b68, called from the bridge's RenderCommandEncoder::end_encoding) → backboardd
  SIGSEGV → the new backboardd gets OutOfMemory → no acceleration / black.
- **backboardd leaks pool memory on app switches**: 0154/0155 census — small (<256K) BOs only grow
  (≈340 per switch, ~7 MB), plus 10 MB render targets churn (alloc+bzero each time). 5 Safari page loads
  ≈ +280 small; 6 switches between the same two apps ≈ +2000 (linear: a leak, not a cache).
  Frontend telemetry: commits == completions (no command-buffer leak), 100 pipelines (stable).
  Critical memory pressure (tools: memwarn) frees only ~33 MB.
- Other fixes today: 0152 (hang = no CP progress for 2 s; GBIF-halt reset retry; recovery from an
  SMMU fault now works), 0153 (full ring waits in kernel; v2 error log), 0154/0155 census.
- A real GPU hang also happened once at 565 MHz (k70); none at 430 MHz (sweetgpufreq=4) so far.
- Next: find the small-BO owner (bridge runtime vs Turnip) — per-BO alloc call-site tagging; cap the
  render-target churn (reuse instead of alloc+bzero); then smoothness (CP_WAIT_FOR_IDLE, async waits).

### 2026-10-23 18:56 STOP — iOS USB (USB 2) dead on every boot since ~17:15; fastboot (USB 3) fine
- See [usb-network](usb-network.md) for this entry; it also touches this feature.

### 2026-10-23 experiments.md lines
| date | patch / switch | hypothesis | predicted ms saved | measured fps and GPU ms | verdict | commit |
|---|---|---|---|---|---|---|
| 2026-10-23 | xnu 0156 per-client pool reuse | stop 10 MB bzero per app switch | memory | 73 hits in 6 switches; no fps change | kept (memory, not fps) | eeca377 |
