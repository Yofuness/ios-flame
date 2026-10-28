# Clock
**State:** ✅ PM6150 RTC + `sweetrtcoff`; timezone America/Toronto since 2026-10-28. **Updated:** 2026-10-28.

## How to use
- RTC offset: `sweetrtcoff` (value in `port/work/rtc-offset`).
- Timezone link: /var/db/timezone/localtime -> zoneinfo/America/Toronto (set with toybox ln -sfn).

## Facts
- PM6150 RTC + `sweetrtcoff`; the kernel UTC was already right; the 3 h offset was the US/Pacific timezone (status.md, broken.md).

## Open
- None open.

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

### 2026-10-28 00:00-02:10 overnight run, part 2 (clock part)
- Clock: kernel UTC was right; the phone's timezone was US/Pacific (3 h behind). /var/db/timezone/localtime -> zoneinfo/America/Toronto; lock screen 1:58 Wed Oct 28 = PC time.

### 2026-10-28 experiments.md lines
- 2026-10-28 k68r (kernel68, defaults): boots fine after k68t; timezone: /var/db/timezone/localtime pointed at US/Pacific (lock screen 3 h behind EDT, not 15 h); relinked to America/Toronto with toybox ln -sfn (old target .../zoneinfo/US/Pacific); `date` now EDT. Lock screen not yet checked.

### 2026-10-28 broken.md "Fixed" row
| Area | Fixed | How |
|---|---|---|
| Clock 3 h behind (timezone US/Pacific) | 2026-10-28 | /var/db/timezone/localtime -> zoneinfo/America/Toronto (toybox ln -sfn); kernel UTC was already right |

No entries before 2026-10-25.
