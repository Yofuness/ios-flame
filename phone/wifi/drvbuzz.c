/*
 * Haptics b (B39): TI DRV2624 LRA driver on SE9 @0x5a, reset = TLMM gpio11 active-high (held low after boot).
 * Register table, power-up order and init values: research/notes/haptics.md (Google sunfish drv2624.c, TI SLOS893D).
 *   drvbuzz --gpio            read-only: gpio11 CFG + IN_OUT in the TLMM_S and TLMM_N tiles
 *   drvbuzz --probe           gpio11 low 2 ms, high, 2 ms; read ID/STATUS, dump 0x00-0x30; back to reset. No chip writes.
 *   drvbuzz --buzz MS LEVEL   as --probe, then Android's init writes (each read back), RTP LEVEL for MS ms, reset.
 * The tile: gpio11 is driven only if exactly one tile's CFG reads like the DT pinctrl (gpio function, 2 mA, output).
 * Hard caps: LEVEL 1..0x40 (half scale), MS 1..500, OD_CLAMP <= 0xA1, CONTROL1 bit7 (LRA) never cleared, 0xFD-0xFF
 * never touched (enforced in dwr). Every exit path leaves gpio11 low (chip in reset).
 * SE9 is shared: don't run with cs35l41, camid, i2cprobe or kern.sweet_camprobe. Needs sweetse9=2.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "geni_i2c.h"
#include "pmem.h"

#define DRV		0x5a
#define TLMM_S		0x3d00000
#define TLMM_N		0x3900000
#define GPIO		11

#define R_ID		0x00
#define R_STATUS	0x01
#define R_MODE		0x07
#define R_CONTROL1	0x08
#define R_GO		0x0c
#define R_CONTROL2	0x0d
#define R_RTP		0x0e
#define R_OD_CLAMP	0x20
#define OD_CLAMP_MAX	0xa1
#define LEVEL_MAX	0x40

static uint64_t io_pa;		/* gpio11 IN_OUT word of the chosen tile, 0 = none */
static int pin_high;

static void reset_low(void)
{
	if (!io_pa || !pin_high)
		return;
	wr32(io_pa, 0);
	pin_high = 0;
	printf("drvbuzz: wr %#llx = 0 (gpio11 low, chip in reset)\n", (unsigned long long)io_pa);
}

static void on_sig(int s)
{
	reset_low();
	_exit(128 + s);
}

static int fail(void)
{
	reset_low();
	return 1;
}

/* --- gpio11: pick the tile --- */
static int pick_tile(void)
{
	static const uint64_t tiles[2] = { TLMM_S, TLMM_N };
	static const char *names[2] = { "TLMM_S", "TLMM_N" };
	int n = 0, pick = -1;
	for (int i = 0; i < 2; i++) {
		uint64_t cfg = tiles[i] + 0x1000 * GPIO;
		uint32_t c = rd32(cfg), io = rd32(cfg + 4);
		/* DT drv2624_rst: FUNC [5:2] = 0 (gpio), DRV [8:6] = 0 (2 mA), OE bit9 = 1; pull bits ignored */
		int ok = (c & 0x3fc) == 0x200;
		printf("drvbuzz: %s gpio11 cfg %#llx = %#010x io %#llx = %#010x (in %u out %u)%s\n", names[i],
		       (unsigned long long)cfg, c, (unsigned long long)(cfg + 4), io, io & 1, (io >> 1) & 1,
		       ok ? " <- matches DT pinctrl" : "");
		if (ok)
			n++, pick = i;
	}
	if (n != 1) {
		printf("drvbuzz: %d tiles match the DT pinctrl (need exactly 1), not driving gpio11\n", n);
		return -1;
	}
	io_pa = tiles[pick] + 0x1000 * GPIO + 4;
	printf("drvbuzz: using %s, gpio11 io %#llx\n", names[pick], (unsigned long long)io_pa);
	return 0;
}

/* --- DRV2624 --- */
static int drd(unsigned reg, uint8_t *v)
{
	uint8_t a = reg;
	int e = gi2c_msg(DRV, &a, 1, NULL, 0, 1);
	if (!e)
		e = gi2c_msg(DRV, NULL, 0, v, 1, 0);
	if (e)
		printf("drvbuzz: rd %#04x err %d\n", reg, e);
	else
		printf("drvbuzz: rd %#04x = %#04x\n", reg, *v);
	return e;
}

static int dwr(unsigned reg, uint8_t v)
{
	if (reg >= 0xfd || (reg == R_OD_CLAMP && v > OD_CLAMP_MAX) || (reg == R_CONTROL1 && !(v & 0x80)) ||
	    (reg == R_RTP && (int8_t)v > LEVEL_MAX)) {
		printf("drvbuzz: wr %#04x = %#04x refused (hard cap)\n", reg, v);
		return -1;
	}
	uint8_t a[2] = { reg, v };
	int e = gi2c_msg(DRV, a, 2, NULL, 0, 0);
	printf("drvbuzz: wr %#04x = %#04x%s\n", reg, v, e ? " err" : "");
	return e;
}

/* write, then read back under mask; mismatch = error */
static int dwr_check(unsigned reg, uint8_t v, uint8_t mask)
{
	uint8_t r;
	if (dwr(reg, v) || drd(reg, &r))
		return -1;
	if ((r & mask) != (v & mask)) {
		printf("drvbuzz: readback %#04x = %#04x, wanted %#04x (mask %#04x)\n", reg, r, v, mask);
		return -1;
	}
	return 0;
}

/* read-modify-write of the bits in mask */
static int dupd(unsigned reg, uint8_t mask, uint8_t v)
{
	uint8_t r;
	if (drd(reg, &r))
		return -1;
	return dwr_check(reg, (r & ~mask) | (v & mask), 0xff);
}

static int power_up(void)
{
	wr32(io_pa, 0);
	printf("drvbuzz: wr %#llx = 0 (gpio11 low)\n", (unsigned long long)io_pa);
	delay_ms(2);
	pin_high = 1;
	wr32(io_pa, 2);
	printf("drvbuzz: wr %#llx = 2 (gpio11 high)\n", (unsigned long long)io_pa);
	delay_ms(2);
	uint8_t id, st;
	if (drd(R_ID, &id) || drd(R_STATUS, &st))
		return -1;
	printf("drvbuzz: ID %#04x (chip %u rev %u) STATUS %#04x\n", id, id >> 4, id & 15, st);
	if (id & 0xf0) {
		printf("drvbuzz: not a DRV2624 ((ID & 0xf0) != 0)\n");
		return -1;
	}
	return 0;
}

static void dump(void)
{
	uint8_t v;
	for (unsigned r = 0; r <= 0x30; r++)
		if (r != R_STATUS)	/* clear-on-read; already read */
			drd(r, &v);
}

/* Android dev_init_platform_data (drv2624.c:591-707) with the sunfish DT values, in that order */
static int init_chip(void)
{
	if (dwr_check(R_MODE, 0x48, 0xff) ||		/* TRIG internal (GO), RTP */
	    dupd(R_CONTROL1, 0xd0, 0xd0) ||		/* LRA, open loop, AUTO_BRK_OL */
	    dupd(R_CONTROL2, 0x23, 0x20) ||		/* playback interval 1 ms, bit7 left alone */
	    dwr_check(0x1f, 0x6d, 0xff) ||		/* RATED_VOLTAGE */
	    dwr_check(R_OD_CLAMP, 0xa1, 0xff) ||	/* OD_CLAMP 3.42 V peak */
	    dwr_check(0x21, 0x12, 0xff) ||		/* A_CAL_COMP */
	    dwr_check(0x22, 0x96, 0xff) ||		/* A_CAL_BEMF */
	    dwr_check(0x23, 0x04, 0xff) ||		/* LOOP_CONTROL: bemf gain 0, fb brake 0 */
	    dwr_check(0x27, 0x18, 0xff) ||		/* DRIVE_TIME 24 */
	    dwr_check(0x28, 0x22, 0xff) ||		/* BLANKING 2, IDISS 2 */
	    dwr_check(0x29, 0x0c, 0xff) ||		/* ZC_DET_TIME */
	    dwr_check(0x2c, 0x01, 0xff) ||		/* sine */
	    dwr_check(0x2e, 0x01, 0xff) ||		/* OL_LRA_PERIOD 0x106 = 155 Hz */
	    dwr_check(0x2f, 0x06, 0xff))
		return -1;
	return 0;
}

static int buzz(int ms, int level)
{
	uint8_t st, go = 1;
	if (dwr_check(R_MODE, 0x48, 0xff) || dwr_check(R_RTP, (uint8_t)level, 0xff) || dwr(R_GO, 1))
		return -1;
	printf("drvbuzz: playing RTP %#04x for %d ms\n", level, ms);
	delay_ms(ms);
	int e = dwr(R_GO, 0);
	delay_ms(12);	/* interval 1 ms x 10 + 1: leave pseudo-standby with a read (D 7.3.10) */
	if (drd(R_STATUS, &st))
		return -1;
	printf("drvbuzz: STATUS %#04x: UVLO %u OT %u OC %u PROCESS_DONE %u\n", st, (st >> 2) & 1, (st >> 1) & 1,
	       st & 1, (st >> 3) & 1);
	for (int t = 0; t <= 100 && go; t += 5) {
		if (drd(R_GO, &go))
			return -1;
		go &= 1;
		if (go)
			delay_ms(5);
	}
	if (go)
		printf("drvbuzz: GO still 1 after 100 ms\n");
	uint8_t p5, p6;
	if (!drd(0x05, &p5) && !drd(0x06, &p6))
		printf("drvbuzz: LRA_PERIOD %#05x\n", (p5 & 3) << 8 | p6);
	return e || go || (st & 7) ? -1 : 0;
}

int main(int argc, char **argv)
{
	int mode = 0, ms = 0, level = 0;
	if (argc == 2 && !strcmp(argv[1], "--gpio"))
		mode = 1;
	else if (argc == 2 && !strcmp(argv[1], "--probe"))
		mode = 2;
	else if (argc == 4 && !strcmp(argv[1], "--buzz")) {
		mode = 3;
		ms = (int)strtol(argv[2], NULL, 0);
		level = (int)strtol(argv[3], NULL, 0);
		if (ms < 1 || ms > 500 || level < 1 || level > LEVEL_MAX) {
			fprintf(stderr, "drvbuzz: MS 1..500, LEVEL 1..%#x\n", LEVEL_MAX);
			return 2;
		}
	}
	if (!mode) {
		fprintf(stderr, "usage: drvbuzz --gpio | --probe | --buzz MS LEVEL\n");
		return 2;
	}
	pm_init();
	if (pick_tile() || mode == 1)
		return mode == 1 ? 0 : 1;
	if (gi2c_init())
		return 1;
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	if (power_up())
		return fail();
	if (mode == 2) {
		dump();
		reset_low();
		return 0;
	}
	if (init_chip() || buzz(ms, level))
		return fail();
	reset_low();
	printf("drvbuzz: done\n");
	return 0;
}
