# Charging
**State:** ❌ the battery drains ~0.5 A under load even on the charger; the charger is never configured. **Updated:** 2026-10-24.

## How to use
- PMIC writes are risky: ask the user first. Register dump: [reference/charger-regs.md](../reference/charger-regs.md).

## Facts
- On the laptop's TB4 port with a 240 W cable the phone charges fine (+968 mA idle); replugging kills the USB link (2026-10-24).

## Open
- Configure the charger (plan "Later").

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

Older entries (before 2026-10-25): [archive/features/charging.md](../archive/features/charging.md).
