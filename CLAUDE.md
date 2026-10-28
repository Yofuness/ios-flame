@AGENTS.md
@docs/sunfish/display-perf.md
@docs/sunfish/plan.md
@docs/sunfish/status.md

# Sunfish (Pixel 4a) quick rules
- **Boot and partitions:**
  - Work from `port/`. RAM-only boots: `tools/sunfish-boot.sh NAME KERNEL [bootargs]`.
  - Never flash partitions, never relock, never select "Factory data reset" in recovery.
  - Writes to userdata are incremental only, with a backup first.
- **Battery and hangs:**
  - Stop booting below 3600 mV (`fastboot getvar battery-voltage`).
  - After a hard hang, stop and write it up in `docs/workday.md`.
- **Git:** push only to branch `sunfish`, never force-push.
- **Out of git:** Apple's files (`research/` is gitignored) and Wi-Fi credentials.
- **Logs:** the newest dated log is `docs/workday.md`. Add a section for each work session with exact numbers.
- **Experiments:** every experiment gets a `docs/sunfish/experiments.md` line before the session ends, including dead
  ends. Never delete lines.
- **The plan:** `docs/sunfish/plan.md` is the current plan. Keep it current as you work:
  - Mark each step done or failed, with its numbers, in the same commit as the work.
  - Add new findings or next steps as you find them.
- **New plan:** move the old one to `docs/sunfish/plans/YYYY-MM-DD-<topic>.md`. Put a short header on it saying
  what was done, what was not, and what replaced it, then write the new `plan.md`.
- **Sunfish differs from AGENTS.md (which describes flame):**
  - The kernel tree is `work/src/xnu`. Commit there, then save it as `xnu/patches/NNNN-sunfish-*.patch`.
  - Build in CI:
    1. `gh workflow run xnu.yml --ref sunfish`
    2. `gh run watch ID`
    3. `gh run download ID -n xnu-kernel -D work/kernelNN`
  - Milestones go in `docs/sunfish/status.md` and `docs/workday.md`, not in HANDOFF.md.
- **Metal bundle (Turnip and bridge patches):** pushing `phone/turnip/patches` or `phone/metal2` starts the
  metal2 CI.
  1. `gh run download ID -D DIR`
  2. `tools/sunfish-metal2-install.sh <dir containing SweetMetal2.bundle> --no-restart`
- **Phone tools:** pushing `phone/tools` starts the userland CI.
  1. `gh run download ID -n ios-userland -D DIR`
  2. `python3 -I tools/flame-files.py put DIR/NAME /var/local/bin/NAME 755`
- **Before booting:** start the live-log listener if it isn't running (`pgrep -af sunfish-livelog`). It also
  provides the USB network link.

# Delegation (save tokens: the main session orchestrates, subagents do the bulk)
- **Main session (Opus):**
  - picks the next step from `docs/sunfish/plan.md`
  - writes precise specs
  - judges results
  - updates the docs and plan
- **Hand the rest to the agents in `.claude/agents/`:**
  - `phone-runner` (sonnet, low effort): boots, benches, webcam, log slicing; checks itself and Reads a webcam frame after every boot and visible change. It is the only agent that touches the phone.
  - `ci-builder` (haiku): commit/export patches, push, CI, download, install.
  - `implementer` (opus, low effort): writes one patch from a spec.
  - `investigator` (opus, low effort): read-only source, binary and log analysis.
  - `net-prober` (haiku): read-only router and Wi-Fi scan probing (never changes router settings).
- **Specs:**
  - Every spec says exactly what to return: numbers and tables, never raw logs.
  - Run waits (CI, benches) in the background and don't poll them.
- **Phone access:**
  - Only one session or agent uses the phone at a time.
  - If the user says another session is on the phone, don't run `phone-runner`.
- **When usage runs low (Max plan):** run the main session on Sonnet for long, routine plan steps. Switch back to
  Opus for design decisions or when stuck after two failed attempts.
