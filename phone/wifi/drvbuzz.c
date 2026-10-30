/*
 * Haptics b (B39): TI DRV2624 LRA driver on SE9 @0x5a, reset = TLMM gpio11 active-high (held low after boot).
 * Register table, power-up order and init values: research/notes/haptics.md (Google sunfish drv2624.c, TI SLOS893D).
 *   drvbuzz --gpio            read-only: gpio11 CFG + IN_OUT in the TLMM_S and TLMM_N tiles
 *   drvbuzz --probe           gpio11 low 2 ms, high, 2 ms; read ID/STATUS, dump 0x00-0x30; back to reset. No chip writes.
 *   drvbuzz --buzz MS LEVEL   as --probe, then Android's init writes (each read back), RTP LEVEL for MS ms, reset.
 * The tile: gpio11 is in TLMM north (pinctrl-sm7150.c / sdmmagpie: CFG 0x390b000, IN_OUT +4; research/notes/haptics.md
 * "gpio11 tile"). Under iOS nobody applied the Android pinctrl, so CFG reads the power-on 0x1 (gpio, input, pull-down,
 * NRST held low): then it is set to the DT drv2624_rst value 0x200 (gpio, 2 mA, output, no pull) with IN_OUT low first,
 * and put back to its old value at exit. Any other CFG value: not touched. (gpio11 is also the TE pin of a Qualcomm
 * reference panel and the secondary display in the DT; sunfish's panel uses gpio10.)
 * Hard caps: LEVEL 1..0x40 (half scale), MS 1..500, OD_CLAMP <= 0xA1, CONTROL1 bit7 (LRA) never cleared, 0xFD-0xFF
 * never touched (enforced in dwr). Every exit path leaves gpio11 low (chip in reset).
 * SE9 is shared: don't run with cs35l41, camid, i2cprobe or kern.sweet_camprobe. Needs sweetse9=2.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "drv2624.h"
#include "geni_i2c.h"
#include "pmem.h"

/* the chip code lives in drv2624.c (shared with hapticd since haptics step 3) */
static void on_sig(int s)
{
	drv_reset_low();
	drv_cfg_restore();
	_exit(128 + s);
}

static int fail(void)
{
	drv_reset_low();
	drv_cfg_restore();
	return 1;
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
		if (ms < 1 || ms > 500 || level < 1 || level > DRV_LEVEL_MAX) {
			fprintf(stderr, "drvbuzz: MS 1..500, LEVEL 1..%#x\n", DRV_LEVEL_MAX);
			return 2;
		}
	}
	if (!mode) {
		fprintf(stderr, "usage: drvbuzz --gpio | --probe | --buzz MS LEVEL\n");
		return 2;
	}
	pm_init();
	if (drv_pick_tile() || mode == 1)
		return mode == 1 ? 0 : 1;
	if (gi2c_init())
		return 1;
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	if (drv_pin_setup() || drv_power_up())
		return fail();
	if (mode == 2) {
		drv_dump();
		drv_reset_low();
		drv_cfg_restore();
		return 0;
	}
	if (drv_init_chip() || drv_buzz(ms, level))
		return fail();
	drv_reset_low();
	drv_cfg_restore();
	printf("drvbuzz: done\n");
	return 0;
}
