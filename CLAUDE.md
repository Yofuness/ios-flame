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
- **All builds are local** (the user's rule, 2026-10-28). The GitHub workflows have no push trigger; don't run them
  (`gh workflow run`) unless the user asks.
- **Metal bundle (Turnip and bridge patches):**
  1. `tools/build-metal2-linux.sh`
  2. `tools/sunfish-metal2-install.sh work/metal2-linux/out/phone/metal2/out --no-restart`
- **Phone tools:**
  1. `tools/build-userland-linux.sh`
  2. `python3 -I tools/flame-files.py put work/userland-linux/out/NAME /var/local/bin/NAME 755`
- **Kernel:** `tools/vm-build-xnu.sh NN --stop` builds in the local macOS VM (`~/VMs/macos`). Never start a second VM instance, and never `pkill -f` a pattern that matches your own shell: the VM
  scripts use `~/VMs/macos/qemu.pid`.
- **Before booting:** start the live-log listener if it isn't running (`pgrep -af sunfish-livelog`). It also
  provides the USB network link.

# How to work (the user's rules, 2026-10-29; keep these across compactions)
- **Goal:** keep iterating until everything on iOS works (except iCloud). Getting as many systems working as
  possible comes first.
- **Focus:** a couple of systems at a time, from the Priority list in `docs/plan.md` (at most 5, the active one
  marked). Easy things go first: the user loves seeing progress.
- **After each finished priority item:** scan the Broken list for low-hanging fruit and move the easiest items up
  into the Priority list.
- **Wins:** push-notify the user every time something starts working.
- **Broken list:** `docs/plan.md`'s Broken list. Anything newly found broken goes on it; then carry on.
- **While waiting** (tests, builds, boots): work on other Broken items instead of idling.
- **Need the user's input:** take the recommended path, push-notify the user, and add the question (with the
  choice made) to the "Questions for you" list in `docs/plan.md`, so they can answer later. Don't wait.
- **Reliability:** if checking a fix needs repeated tests of an hour or more, move it to the "Later" list and move on.
- **Dev-speed optimizations:** anything slowing development down becomes priority. Give it at most about an hour;
  past that it's likely not worth it: note it and move on.
- **Risky steps** (the user, 2026-10-29): where hardware damage is possible (e.g. PMIC charger writes), proceed with
  caution: research how the system works first, then lots of baby steps. Steps that only risk a reset (hypervisor
  calls, SMMU, DSP memory) are fine with the same care; a reset is logged (feature Log + journal), then work goes
  on with a smaller step. Flash/relock/battery/factory-reset rules above stay absolute.
- **Notifications:** anytime, also at night.
- **Other optimization:** make systems usable, not fast. Once something works and could be faster, add it to the
  "Optimize (last)" list and move on.

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
