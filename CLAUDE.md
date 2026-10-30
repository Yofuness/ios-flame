@docs/plan.md

This file is the one home for the project's rules (AGENTS.md and the memory notes only point here). Start with
`docs/plan.md` (loaded above); read `README.md`, `docs/README.md` and `docs/methods.md` only when a task needs them.

# Sunfish (Pixel 4a) quick rules
- **Boot and partitions:**
  - Work from `port/`. RAM-only boots: `tools/sunfish-boot.sh NAME KERNEL [bootargs]`.
  - Never flash partitions, never relock, never select "Factory data reset" in recovery.
  - Writes to userdata are incremental only, with a backup first.
- **Battery and hangs:**
  - Stop booting below 3600 mV (`fastboot getvar battery-voltage`).
  - After a hard hang or reset: write it up in the feature log (`docs/features/<name>.md`) and today's journal,
    then go on with a smaller step ("Risky steps" below).
- **Git:** commit and push to branch `main` after each finished change without asking (the `sunfish` branch is
  frozen), never force-push.
- **Out of git:** Apple's files (IPSW parts, the root image, shared cache, kernelcache, anything translated from
  Apple's shaders), Google and Qualcomm images and firmware, partition dumps, device trees and logs from a phone
  (serials, IMEI, network names), Wi-Fi credentials (`/var/local/wlan.conf` lives on the phone only). Research notes
  and source excerpts go in `research/notes/` (`research/` is gitignored), never only in a session scratchpad.
- **Reproducible on-device changes** (the user, 2026-10-30): every change made on the phone (files, plists, launchd
  jobs, iOS settings changed by taps, config in /var/local) must be reproducible from git: an install script under
  `tools/`, or written steps in `docs/setup.md` that a script will replace later. Log each on-device change in
  `docs/device-changes.md` (dated, what/where/how, the script or setup step that reproduces it) in the same commit.
  For an unfinished feature under work, a log line is enough; script it once the feature works.
- **Destructive steps** (partition backups, userdata writes): confirm with the user first. `docs/recovery.md` gets
  back to stock Android.
- **Docs layout:** `README.md` has the status table (one row per feature), `docs/README.md` indexes the docs,
  `docs/features/<name>.md` has each feature's
  facts and its dated, append-only Log, `docs/journal/YYYY-MM-DD.md` is the daily journal, `docs/methods.md` the tools
  and methods, `docs/archive/` the old entries.
- **Logs:** each work session adds a dated entry (`### YYYY-MM-DD HH:MM <title>`, newest first, with run tags, kernel
  number and exact numbers) to the Log of every feature it touched, plus a line in today's journal file
  (`docs/journal/YYYY-MM-DD.md`; never rewrite a past day's file).
- **Experiments:** every experiment goes in the feature Log before the session ends, including dead ends. Never
  delete entries; add a new one to correct an old one.
- **Milestones:** when a feature starts working (or changes state), update its row in the status table of the main
  `README.md` (state, summary, the date in the table heading) and the State line of the feature file, in the same
  commit. Record the kernel number, boot args, exact results and what is still open in the feature Log; keep
  observations apart from guesses. A new kernel patch gets a line in the feature Log too.
- **Log size:** when a feature Log passes about 150 lines, move its oldest entries to `docs/archive/features/<name>.md`.
- **The plan:** `docs/plan.md` is the current plan, loaded into every session and agent, so keep it short (about
  6 KB): current state only, one line per item, details and history in the feature Logs. Keep it current as you
  work:
  - Mark each step done or failed in the same commit as the work; move finished items to the "Done" line.
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
- **Kernel:** the tree is `work/src/xnu` (a git repo, HEAD = baseline + applied patches). Commit there, save the
  diff as the next `xnu/patches/NNNN-sunfish-*.patch`, build with `tools/vm-build-xnu.sh NN` in the local macOS VM
  (`~/VMs/macos`, ~35 s incremental) -> `work/kernelNN`. Never start a second VM instance, and never `pkill -f` a
  pattern that matches your own shell: the VM scripts use `~/VMs/macos/qemu.pid`.
- **USB cable:** keep it in the laptop's TB4 port with the 240 W cable (charges ~+950 mA under load). The link does
  not survive a physical unplug/replug: move the cable only while the phone is off or in fastboot.
- **Before booting:** start the live-log listener if it isn't running (`pgrep -af sunfish-livelog`). It also
  provides the USB network link. The log is split per boot: `../logs/boots/` (one file per boot, named by its
  start time; test markers `=== TAG` are inside); `../logs/usb-live.log` still gets every boot (old ones gzipped in
  `../logs/archive/`). Search the per-boot file, not
  everything.

# How to work (the user's rules, 2026-10-29; keep these across compactions)
- **Goal:** keep iterating until everything on iOS works (except iCloud). Getting as many systems working as
  possible comes first.
- **Focus:** a couple of systems at a time, from the Priority list in `docs/plan.md` (at most 5, the active one
  marked). Easy things go first: the user loves seeing progress.
- **After each finished priority item:** scan the Broken list in `docs/backlog.md` for low-hanging fruit and move the easiest items up
  into the Priority list.
- **Wins:** push-notify the user every time something starts working.
- **Backlog:** the Broken, Later and Optimize lists are in `docs/backlog.md`, not auto-loaded. Read it when waiting
  on a test or build, after finishing a priority item, and when adding something newly found broken (add it, then
  carry on).
- **While waiting** (tests, builds, boots): work on other Broken items instead of idling.
- **Need the user's input:** take the recommended path, push-notify the user, and add the question (with the
  choice made) to the "Questions for you" list in `docs/plan.md`, so they can answer later. Don't wait.
- **Reliability:** if checking a fix needs repeated tests of an hour or more, move it to the "Later" list (`docs/backlog.md`) and move on.
- **Time limit:** if a priority item shows no visible progress in about half a day, write down where it stands in
  its feature Log, park it in `docs/backlog.md` and take the next item.
- **Hard items** (days, uncertain): do their read-only research in the background while phone tests run; keep
  quick wins at the top of the list.
- **Find the user-visible breakage too:** the backlog isn't only hardware. Now and then (after big changes, or when
  the list runs low) survey the apps and core functions and add what fails.
- **Dev-speed optimizations:** anything slowing development down goes to the top of the priority list. Give it at most about an hour;
  past that it's likely not worth it: note it and move on.
- **Risky steps** (the user, 2026-10-29): where hardware damage is possible (e.g. PMIC charger writes), proceed with
  caution: research how the system works first, then lots of baby steps. Steps that only risk a reset (hypervisor
  calls, SMMU, DSP memory) are fine with the same care; a reset is logged (feature Log + journal), then work goes
  on with a smaller step. Flash/relock/battery/factory-reset rules above stay absolute.
- **Autonomy:** decide and act without asking on normal engineering choices; root file edits (launchd.plist, jobs)
  are fine with a backup first and the escape boot available.
- **Notifications:** anytime, also at night.
- **Night** (`work/night` exists: the user sleeps, the phone is in their bedroom): keep the iOS brightness slider at
  its minimum (DCS 32; iOS keeps it across reboots), no brightness raises or tests. Never `tools/sunfish-dim.sh`: it
  stacks on the iOS brightness and the webcam can't see the screen (`sunfish-dim.sh 100` clears a leftover dim).
  Remove the flag when the user is up.
- **Quiet hours** (the user, 2026-10-30): no speaker tests (sound from the phone) between 9:30 pm and 5:00 am EST;
  the USB measurement mic (EM-01) is fine to use outside those hours.
- **Other optimization:** make systems usable, not fast. Once something works and could be faster, add it to the
  "Optimize (last)" list (`docs/backlog.md`) and move on.

# Delegation and sessions (tokens)
- **Every agent starts cold:** it loads this file and the plan (~4K tokens) and re-reads what it needs. So:
  - Do short things directly in the main session: up to ~5 commands, a quick SSH check, a small edit.
  - Delegate the bulky ones: builds, boot + test runs, reverse engineering, long logs, big patches.
- **Main session (Opus):** picks the next step from `docs/plan.md`, writes precise specs, judges results, updates the
  docs and plan.
- **Sessions:** start a fresh session per day or per big milestone instead of one session over several
  compactions; `docs/plan.md` and today's journal are the hand-off.
- **Agents in `.claude/agents/`:**
  - `phone-runner` (sonnet, low effort): boots, benches, webcam, log slicing; checks itself and Reads a webcam frame after every boot and visible change. It is the only agent that touches the phone.
  - `ci-builder` (haiku): commit/export patches, push, CI, download, install.
  - `implementer` (opus, low effort): writes one patch from a spec.
  - `investigator` (opus, low effort): read-only source, binary and log analysis.
  - `net-prober` (haiku): read-only router and Wi-Fi scan probing (never changes router settings).
- **Specs:**
  - Every spec says exactly what to return: numbers and tables, never raw logs.
  - phone-runner specs say "foreground only, no `&` or background runs, don't end the turn until done": it stalls
    on its own background watchers.
  - After stopping a phone agent, kill the scripts it launched (`ps -eo pid,ppid,args | grep scratchpad`, parent
    first): they keep driving the phone.
  - Run waits (CI, benches) in the background and don't poll them.
- **Phone access:**
  - Only one session or agent uses the phone at a time.
  - If the user says another session is on the phone, don't run `phone-runner`.
- **When usage runs low (Max plan):** run the main session on Sonnet for long, routine plan steps. Switch back to
  Opus for design decisions or when stuck after two failed attempts.
