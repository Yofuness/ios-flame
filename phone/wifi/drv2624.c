/*
 * The DRV2624 code of drvbuzz (haptics hp2), shared with hapticd (haptics step 3): see drv2624.h and drvbuzz.c's
 * header (tile, CFG values, init table). Behaviour and caps as drvbuzz had them; drv_rtp_max may be raised by the
 * caller up to DRV_RTP_HARD (0x60, hapticd's taps), never beyond.
 */
#include <stdio.h>
#include <unistd.h>
#include "drv2624.h"
#include "geni_i2c.h"
#include "pmem.h"

const char *drv_tag = "drvbuzz";
int drv_verbose = 1;
int drv_rtp_max = DRV_LEVEL_MAX;

#define DRV		0x5a
#define TLMM_S		0x3d00000
#define TLMM_N		0x3900000
#define GPIO		11


static uint64_t io_pa;		/* gpio11 IN_OUT word of the chosen tile, 0 = none */
static int pin_high;
static uint64_t cfg_pa;		/* gpio11 CFG word, set when we changed it (restored at exit) */
static uint32_t cfg_old;

#define CFG_DEFAULT	0x001	/* power-on: gpio, 2 mA, input, pull-down */
#define CFG_DT		0x200	/* drv2624_rst: gpio, 2 mA, output, no pull */

void drv_reset_low(void)
{
	if (!io_pa || !pin_high)
		return;
	wr32(io_pa, 0);
	pin_high = 0;
	if (drv_verbose)
		printf("%s: wr %#llx = 0 (gpio11 low, chip in reset)\n", drv_tag, (unsigned long long)io_pa);
}

/* gpio11 back to the CFG it had (the pull-down keeps NRST low) */
void drv_cfg_restore(void)
{
	if (!cfg_pa)
		return;
	wr32(cfg_pa, cfg_old);
	if (drv_verbose)
		printf("%s: wr %#llx = %#x (gpio11 cfg restored)\n", drv_tag, (unsigned long long)cfg_pa, cfg_old);
	cfg_pa = 0;
}

/* --- gpio11: pick the tile --- */
int drv_pick_tile(void)
{
	static const uint64_t tiles[2] = { TLMM_S, TLMM_N };
	static const char *names[2] = { "TLMM_S", "TLMM_N" };
	int n = 0, pick = -1;
	for (int i = 0; i < 2; i++) {
		uint64_t cfg = tiles[i] + 0x1000 * GPIO;
		uint32_t c = rd32(cfg), io = rd32(cfg + 4);
		/* DT drv2624_rst: FUNC [5:2] = 0 (gpio), DRV [8:6] = 0 (2 mA), OE bit9 = 1; pull bits ignored.
		   Only the north tile holds gpio11; south is printed for reference. */
		int dt = (c & 0x3fc) == CFG_DT, def = c == CFG_DEFAULT;
		printf("%s: %s gpio11 cfg %#llx = %#010x io %#llx = %#010x (in %u out %u)%s\n", drv_tag, names[i],
		       (unsigned long long)cfg, c, (unsigned long long)(cfg + 4), io, io & 1, (io >> 1) & 1,
		       i != 1 ? " (not gpio11's tile)" : dt ? " <- DT pinctrl" : def ? " <- power-on default" : " <- unexpected");
		if (i == 1 && (dt || def))
			n++, pick = i, cfg_old = c;
	}
	if (n != 1) {
		printf("%s: TLMM_N gpio11 cfg neither the DT pinctrl nor the power-on default, not driving gpio11\n", drv_tag);
		return -1;
	}
	io_pa = tiles[pick] + 0x1000 * GPIO + 4;
	printf("%s: using %s, gpio11 io %#llx\n", drv_tag, names[pick], (unsigned long long)io_pa);
	return 0;
}

/* the DT pinctrl if the pin is still at its power-on CFG: IN_OUT low first, then output */
int drv_pin_setup(void)
{
	uint64_t cfg = io_pa - 4;
	if ((cfg_old & 0x3fc) == CFG_DT)
		return 0;
	wr32(io_pa, 0);
	wr32(cfg, CFG_DT);
	cfg_pa = cfg;
	uint32_t r = rd32(cfg);
	if (drv_verbose)
		printf("%s: wr %#llx = %#x (gpio11 output low), reads %#x\n", drv_tag, (unsigned long long)cfg, CFG_DT, r);
	if ((r & 0x3ff) != CFG_DT)
		return -1;
	usleep(2000);
	return 0;
}

/* --- DRV2624 --- */
int drv_rd(unsigned reg, uint8_t *v)
{
	uint8_t a = reg;
	gi2c_lock();	/* write-addr + read as one locked sequence (SE9 lock) */
	int e = gi2c_msg(DRV, &a, 1, NULL, 0, 1);
	if (!e)
		e = gi2c_msg(DRV, NULL, 0, v, 1, 0);
	gi2c_unlock();
	if (e)
		printf("%s: rd %#04x err %d\n", drv_tag, reg, e);
	else
		if (drv_verbose)
			printf("%s: rd %#04x = %#04x\n", drv_tag, reg, *v);
	return e;
}

int drv_wr(unsigned reg, uint8_t v)
{
	if (reg >= 0xfd || (reg == DRV_R_OD_CLAMP && v > DRV_OD_CLAMP_MAX) || (reg == DRV_R_CONTROL1 && !(v & 0x80)) ||
	    (reg == DRV_R_RTP && (int8_t)v > (drv_rtp_max < DRV_RTP_HARD ? drv_rtp_max : DRV_RTP_HARD))) {
		printf("%s: wr %#04x = %#04x refused (hard cap)\n", drv_tag, reg, v);
		return -1;
	}
	uint8_t a[2] = { reg, v };
	int e = gi2c_msg(DRV, a, 2, NULL, 0, 0);
	if (drv_verbose || e)
		printf("%s: wr %#04x = %#04x%s\n", drv_tag, reg, v, e ? " err" : "");
	return e;
}

/* write, then read back under mask; mismatch = error */
int drv_wr_check(unsigned reg, uint8_t v, uint8_t mask)
{
	uint8_t r;
	if (drv_wr(reg, v) || drv_rd(reg, &r))
		return -1;
	if ((r & mask) != (v & mask)) {
		printf("%s: readback %#04x = %#04x, wanted %#04x (mask %#04x)\n", drv_tag, reg, r, v, mask);
		return -1;
	}
	return 0;
}

/* read-modify-write of the bits in mask */
static int drv_upd(unsigned reg, uint8_t mask, uint8_t v)
{
	uint8_t r;
	if (drv_rd(reg, &r))
		return -1;
	return drv_wr_check(reg, (r & ~mask) | (v & mask), 0xff);
}

int drv_power_up(void)
{
	wr32(io_pa, 0);
	if (drv_verbose)
		printf("%s: wr %#llx = 0 (gpio11 low)\n", drv_tag, (unsigned long long)io_pa);
	delay_ms(2);
	pin_high = 1;
	wr32(io_pa, 2);
	if (drv_verbose)
		printf("%s: wr %#llx = 2 (gpio11 high)\n", drv_tag, (unsigned long long)io_pa);
	delay_ms(2);
	uint8_t id, st;
	if (drv_rd(DRV_R_ID, &id) || drv_rd(DRV_R_STATUS, &st))
		return -1;
	printf("%s: ID %#04x (chip %u rev %u) STATUS %#04x\n", drv_tag, id, id >> 4, id & 15, st);
	if (id & 0xf0) {
		printf("%s: not a DRV2624 ((ID & 0xf0) != 0)\n", drv_tag);
		return -1;
	}
	return 0;
}

void drv_dump(void)
{
	uint8_t v;
	for (unsigned r = 0; r <= 0x30; r++)
		if (r != DRV_R_STATUS)	/* clear-on-read; already read */
			drv_rd(r, &v);
}

/* Android dev_init_platform_data (drv2624.c:591-707) with the sunfish DT values, in that order */
int drv_init_chip(void)
{
	if (drv_wr_check(DRV_R_MODE, 0x48, 0xff) ||		/* TRIG internal (GO), RTP */
	    drv_upd(DRV_R_CONTROL1, 0xd0, 0xd0) ||		/* LRA, open loop, AUTO_BRK_OL */
	    drv_upd(DRV_R_CONTROL2, 0x23, 0x20) ||		/* playback interval 1 ms, bit7 left alone */
	    drv_wr_check(0x1f, 0x6d, 0xff) ||		/* RATED_VOLTAGE */
	    drv_wr_check(DRV_R_OD_CLAMP, 0xa1, 0xff) ||	/* OD_CLAMP 3.42 V peak */
	    drv_wr_check(0x21, 0x12, 0xff) ||		/* A_CAL_COMP */
	    drv_wr_check(0x22, 0x96, 0xff) ||		/* A_CAL_BEMF */
	    drv_wr_check(0x23, 0x04, 0xff) ||		/* LOOP_CONTROL: bemf gain 0, fb brake 0 */
	    drv_wr_check(0x27, 0x18, 0xff) ||		/* DRIVE_TIME 24 */
	    drv_wr_check(0x28, 0x22, 0xff) ||		/* BLANKING 2, IDISS 2 */
	    drv_wr_check(0x29, 0x0c, 0xff) ||		/* ZC_DET_TIME */
	    drv_wr_check(0x2c, 0x01, 0xff) ||		/* sine */
	    drv_wr_check(0x2e, 0x01, 0xff) ||		/* OL_LRA_PERIOD 0x106 = 155 Hz */
	    drv_wr_check(0x2f, 0x06, 0xff))
		return -1;
	return 0;
}

int drv_buzz(int ms, int level)
{
	uint8_t st, go = 1;
	if (drv_wr_check(DRV_R_MODE, 0x48, 0xff) || drv_wr_check(DRV_R_RTP, (uint8_t)level, 0xff) || drv_wr(DRV_R_GO, 1))
		return -1;
	printf("%s: playing RTP %#04x for %d ms\n", drv_tag, level, ms);
	delay_ms(ms);
	int e = drv_wr(DRV_R_GO, 0);
	delay_ms(12);	/* interval 1 ms x 10 + 1: leave pseudo-standby with a read (D 7.3.10) */
	if (drv_rd(DRV_R_STATUS, &st))
		return -1;
	printf("%s: STATUS %#04x: UVLO %u OT %u OC %u PROCESS_DONE %u\n", drv_tag, st, (st >> 2) & 1, (st >> 1) & 1,
	       st & 1, (st >> 3) & 1);
	for (int t = 0; t <= 100 && go; t += 5) {
		if (drv_rd(DRV_R_GO, &go))
			return -1;
		go &= 1;
		if (go)
			delay_ms(5);
	}
	if (go)
		printf("%s: GO still 1 after 100 ms\n", drv_tag);
	uint8_t p5, p6;
	if (!drv_rd(0x05, &p5) && !drv_rd(0x06, &p6))
		printf("%s: LRA_PERIOD %#05x\n", drv_tag, (p5 & 3) << 8 | p6);
	return e || go || (st & 7) ? -1 : 0;
}

