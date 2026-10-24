@AGENTS.md
@docs/sunfish/display-perf.md
@docs/sunfish/plan.md

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
- **The plan:** `docs/sunfish/plan.md` is the current plan. Keep it current as you work:
  - Mark each step done or failed, with its numbers, in the same commit as the work.
  - Add new findings or next steps as you find them.
- **New plan:** move the old one to `docs/sunfish/plans/YYYY-MM-DD-<topic>.md`. Put a short header on it saying
  what was done, what was not, and what replaced it, then write the new `plan.md`.
