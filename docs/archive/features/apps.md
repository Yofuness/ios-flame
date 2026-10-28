# Apps: archived log (entries before 2026-10-25)
Current file: [features/apps.md](../../features/apps.md). Same format, newest first.

## Log

### 2026-10-23 22:35 GPU stability — what actually freezes/blacks the screen
- See [gpu-memory](gpu-memory.md) for this entry; it also touches this feature.

### 2026-10-23 09:15 Extra: TTS synthesizer crash loop — **fixed**
- SiriTTSSynthesizerAU / MacinTalkAUSP / KonaSynthesizer aborted ~once a minute (12× each per 6 min).
  Requester found with `oslog`: **axassetsd** (accessibility assets → TTS voice queries). Dropped
  `com.apple.accessibility.axassetsd`. After: 0 TTS crashes in 2 min; swipe bench 16.9 fps (noise vs 17.2).
- Soak so far: 0 kernel panics today; battery 35–36% (fastboot charging between boots keeps it up).
- (Recovered from `git show 1808cf9^:docs/sunfish/workday.md`; that commit deleted it.)

### 2026-10-23 08:50 Fallback: CPU render path — **pass: 4.3 → 17.2 fps** (4×)
- See [smoothness](smoothness.md) for this entry; it also touches this feature.

### 2026-10-23 experiments.md lines
| date | patch / switch | hypothesis | predicted ms saved | measured fps and GPU ms | verdict | commit |
|---|---|---|---|---|---|---|
| 2026-10-23 | drop launchd job vmd (Visual Voicemail) | vmd spins a core without cellular | CPU | 4.9 -> 16.7 fps | kept | df05f86 |
| 2026-10-23 | drop launchd job ReportCrash | busy with TTS synthesizer crashes | CPU | 16.7 -> 17.2 fps | kept (within noise) | df05f86 |

### 2026-10-23 10:00 status.md update
- UI 17 fps while swiping (was 4.3): dropped launchd jobs vmd, ReportCrash, axassetsd; sweetfb_hz=60.
