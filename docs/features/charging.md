# Charging
**State:** 🟡 charges (bootloader-configured: 2 A charge, 1.5 A input); a weak USB source can't cover the load. **Updated:** 2026-10-28.

## How to use
- PMIC writes are risky: ask the user first. Register dump: [reference/charger-regs.md](../reference/charger-regs.md).

## Facts
- On the laptop's TB4 port with a 240 W cable the phone charges fine (+968 mA idle); replugging kills the USB link (2026-10-24).

## Open
- Configure the charger (plan "Later").

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

### 2026-10-28 16:00 analysis: charging is configured; the limit is the source
- From the read-only dump (docs/reference/charger-regs.md, SID 0) decoded against smb5-reg.h (from memory): CHARGER_STATUS_1 0x1006 = 0x03 (FAST_CHARGE), FAST_CHARGE_CURRENT_CFG 0x1061 = 0x28 (2.0 A), FLOAT_VOLTAGE_CFG 0x1070 = 0x55 (4.45 V = the DT's google,fv-max-uv), USBIN_CURRENT_LIMIT_CFG 0x1370 = 0x1e (1.5 A). XBL/ABL set this up. The archive's TB4 + 240 W cable test gave +968 mA idle / +948 mA under a bench with the same registers; the usual port 0 to -250 mA. So ~7.5 W in from a 1.5 A source is below the load; no write is needed. Idle 0 mA at 100% is charge termination + the 97% auto-recharge hysteresis.
- Optional read-only follow-up: add SID 0 peripherals 0x14/0x15/0x16 (DCIN, TYPEC, MISC) to the read whitelist to see the source's Rp level / APSD type. A higher ICL (0x1370 = 0x28) only with a 3 A/DCP source, and only with the user's approval; never touch 0x1070 or the JEITA registers.

Older entries (before 2026-10-25): [archive/features/charging.md](../archive/features/charging.md).
