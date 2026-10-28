# CPU: archived log (entries before 2026-10-25)
Current file: [features/cpu.md](../../features/cpu.md). Same format, newest first.

## Log

### 2026-10-24 21:30 xnu 0166 CPU sampler works (kernel42, clean CI run 40150300651)
- See [smoothness](smoothness.md) for this entry; it also touches this feature.

### 2026-10-23 12:35 0147 big-core preference — **backboardd ~2x faster; panel path is now the cap**
- See [smoothness](smoothness.md) for this entry; it also touches this feature.

### 2026-10-23 experiments.md lines
| date | patch / switch | hypothesis | predicted ms saved | measured fps and GPU ms | verdict | commit |
|---|---|---|---|---|---|---|
| 2026-10-23 | xnu 0147 UI threads prefer A76 (`sweetbig=0` off) | render thread lands on A55s | CPU | no bench recorded; 0166 later saw 55-75% of render CPU on A76s | inconclusive | 9b6dfee |
