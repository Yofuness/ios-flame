# Archived plan (2026-10-23): GPU stability + smooth display
Superseded by docs/sunfish/plan.md on 2026-10-24.

**Done:**
- Phase 0 measurements.
- 0157 (no wait-for-idle per submit; no gain).
- Present experiments (0149 thread, 0158 non-blocking, 0161 on-swap; all worse).
- IOSurface shadow copies (turnip 0104, bridge 0102, xnu 0159).
- CA vsync (CA_FORCE_TIMER).

**Not done:**
- System-client pool cap and reclaim (the leak is still open).
- Async completion in the frontend.
- Interrupt-driven fence waits.
- DCVS.

The original phase list is in git history: the session plan file, and the early `docs/workday.md` sections.
