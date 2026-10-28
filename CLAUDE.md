@AGENTS.md
@docs/plan.md
@docs/README.md

# Sunfish (Pixel 4a) quick rules
- **Boot and partitions:**
  - Work from `port/`. RAM-only boots: `tools/sunfish-boot.sh NAME KERNEL [bootargs]`.
  - Never flash partitions, never relock, never select "Factory data reset" in recovery.
  - Writes to userdata are incremental only, with a backup first.
- **Battery and hangs:**
  - Stop booting below 3600 mV (`fastboot getvar battery-voltage`).
  - After a hard hang, stop and write it up in the feature log (`docs/features/<name>.md`) and today's journal.
- **Git:** push only to branch `main` (the `sunfish` branch is frozen), never force-push.
- **Out of git:** Apple's files (`research/` is gitignored) and Wi-Fi credentials.
- **Docs layout:** `docs/README.md` is the index (one row per feature), `docs/features/<name>.md` has each feature's
  facts and its dated, append-only Log, `docs/journal/YYYY-MM-DD.md` is the daily journal, `docs/methods.md` the tools
  and methods, `docs/archive/` the old entries.
- **Logs:** each work session adds a dated entry (`### YYYY-MM-DD HH:MM <title>`, newest first, with run tags, kernel
  number and exact numbers) to the Log of every feature it touched, plus a line in today's journal file
  (`docs/journal/YYYY-MM-DD.md`; never rewrite a past day's file).
- **Experiments:** every experiment goes in the feature Log before the session ends, including dead ends. Never
  delete entries; add a new one to correct an old one.
- **Milestones:** update the feature's row (state, summary, date) in the `docs/README.md` index and the State line of
  the feature file.
- **Log size:** when a feature Log passes about 150 lines, move its oldest entries to `docs/archive/features/<name>.md`.
- **The plan:** `docs/plan.md` is the current plan. Keep it current as you work:
  - Mark each step done or failed, with its numbers, in the same commit as the work.
  - Add new findings or next steps as you find them.
- **New plan:** move the old one to `docs/plans/YYYY-MM-DD-<topic>.md`. Put a short header on it saying
  what was done, what was not, and what replaced it, then write the new `plan.md`.
- **Metal bundle (Turnip and bridge patches):** build locally, CI only as a fallback.
  1. `tools/build-metal2-linux.sh`
  2. `tools/sunfish-metal2-install.sh work/metal2-linux/out/phone/metal2/out --no-restart`
- **Phone tools:** build locally, CI only as a fallback.
  1. `tools/build-userland-linux.sh`
  2. `python3 -I tools/flame-files.py put work/userland-linux/out/NAME /var/local/bin/NAME 755`
- **CI fallback:** pushing `phone/turnip/patches` or `phone/metal2` to `main` starts the metal2 CI, and `phone/**` starts
  the userland CI (details in `docs/methods.md`). The kernel still builds only in CI.
- **Before booting:** start the live-log listener if it isn't running (`pgrep -af sunfish-livelog`). It also
  provides the USB network link.

# Delegation (save tokens: the main session orchestrates, subagents do the bulk)
- **Main session (Opus):**
  - picks the next step from `docs/plan.md`
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
