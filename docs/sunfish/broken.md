# Sunfish: disabled services and known-broken things

What has been switched off on the phone, and what is known to be broken but not yet looked at. Numbers come from
the 30 min app soaks in `docs/workday.md` (pp1, dd1, dd2 on 2026-10-27). Add a line when you find something; move
it to "Fixed" (with the date and patch) instead of deleting it.

## 1. Disabled launchd jobs
Removed from `/System/Library/xpc/launchd.plist` with `tools/sunfish-launchd-drop.sh LABEL...` (applies on reboot).
Backups on the phone: the original is `/System/Library/xpc/launchd.plist.pre-drop`; the copy from before step 6
is `/var/local/launchd.plist.pre-step6` (also `work/userland/` on the PC). To undo one job, copy its dict back
from the backup with plistlib.

| Label | Disabled | Why | Known side effect |
|---|---|---|---|
| com.apple.voicemail.vmd | 2026-10-23 | spun one CPU core without cellular | no visual voicemail (no cellular anyway) |
| com.apple.ReportCrash | 2026-10-23 | busy with TTS synthesizer crashes | no crash reports on the phone; "Failed to send exception EXC_CORPSE_NOTIFY" lines (~400/h) |
| com.apple.accessibility.axassetsd | 2026-10-23 | crash loop / CPU | accessibility asset downloads |
| com.apple.assetsd | 2026-10-27 | crash loop (88 per 30 min, SIGABRT) | Photos opens to its first-run sheet (it rendered black before); photo library likely not usable |
| com.apple.assetsd.nebulad | 2026-10-27 | assetsd helper (camera bursts) | none seen (no camera) |
| com.apple.driverkitd | 2026-10-27 | crash loop (58 in the long log) | no DriverKit drivers (none used) |
| com.apple.spotlight.IndexAgent | 2026-10-27 | thought to launch the Spotlight index extensions (it did not) | none seen |
| com.apple.corespotlightservice | 2026-10-27 | same | none seen |
| com.apple.searchd | 2026-10-27 | launched ~0.4 s before every Spotlight index extension (21 times in 30 min) | Spotlight screen still opens (Siri Suggestions + search field); search results not checked. dd2: 0 Spotlight extension launches, 0 of their crashes |

## 2. Broken services to look at later
| Process | Symptom | Count | Notes |
|---|---|---|---|
| assetsd | SIGABRT at start (`reason ns 2 code 0x6`) | 88 / 30 min | disabled; Photos needs it for a real library. Backtrace in the live log (`CRASH assetsd bt ...`) |
| *SpotlightIndexExtension (Tips, PassKit, Contacts, MobileSMS, Music, Maps, iBooks, TV, Reminders) | SIGABRT ~40 ms after exec | ~85 / 30 min before searchd was dropped, 0 after (dd2) | launched via searchd (now disabled); why they abort is unknown |
| AegirPoster | crash loop | 487 in the long log | lock-screen poster (wallpaper) extension; lock screen still draws, left enabled |
| FitnessWidget, com.apple.VoiceMemos, com.apple.podcasts (widget/extension) | crash loop | 9-10 / 30 min each in dd1, 0 in dd2 (15 min, searchd off), 160 in the long log | probably Spotlight-launched too (gone with searchd) |
| PhotosFileProvider, fileproviderd | occasional crash | 1 / 30 min each | |
| wifid | `apple80211: wifid gets 103 (CURRENT_NETWORK) ... error 6` | ~270 / h | ioctl not implemented in our Wi-Fi shim |
| wifid | `apple80211: wifid sets 383 ... REFUSED` | ~19 / 30 min | |
| SpringBoard / CA | `CATransformLayer ... allowsGroupOpacity in transform-only layer` | ~150 / h | log noise, probably harmless |

## 3. Other known-broken things on the phone
| Area | State | Where to start |
|---|---|---|
| Charging | battery drains ~0.5 A under load even on the charger; the charger is never configured | `docs/sunfish/charger-regs.md`; PMIC writes are risky, ask the user first |
| Battery % | IR-compensated estimate (0128) never checked | status.md |
| USB link | port resets, replug and boot all fine since xnu 0184 (kernel60); ck1-ck3: 0 drops in 3x5 min | - |
| GPU hang recovery | works on injected hangs (5/5, ~26 ms) but never met a real hang since 0181/0182 | plan.md step 2 |
| UI watchdog | catches a frozen UI only; a kernel hang with swaps still running is not caught | plan.md step 4 |
| Speed | ~21 screen updates/s ceiling while swiping; present blit waits behind backboardd | display-perf.md |
| Reduce Transparency | works but slow (~6 swaps/s) | workday.md rtj/rtj2 |
| Audio, Bluetooth, cellular, camera, sensors | not started | status.md |
| Wi-Fi | scan works, iOS never joins the saved network (2026-10-27 ck1: no route, nettest -1009) | plan.md step 1 |

## Fixed
(move rows here with the date and the patch or change that fixed them)
