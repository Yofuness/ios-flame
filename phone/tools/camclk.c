/*
 * camclk: camera stage a2, a 24 MHz MCLK from camcc PLL2 (values and sequences from Linux 6.15
 * drivers/clk/qcom/camcc-sm7150.c, clk-alpha-pll.c, clk-rcg2.c, clk-branch.c; line numbers below).
 *   camclk status          read-only: PLL2 registers, MCLK0-3 RCG + CBCR
 *   camclk pll2 on         clk_agera_pll_configure + clk_alpha_pll_enable (PLL2 is Agera: camcc-sm7150.c:156,2037)
 *   camclk pll0 on         stage c2: clk_fabia_pll_configure + alpha_pll_fabia_enable, 1200 MHz (camcc-sm7150.c:46-69,
 *                          2035), even 600 / odd 400; for csi0phytimer 300 MHz (camrx phy on)
 *   camclk mclk N 24       MCLK N (0..3) RCG = pll2_out_aux (480 MHz), pre-div 1, M/N 1/20; then the branch on
 *   camclk off             MCLK0-3 branches off, then PLL0 off (alpha_pll_fabia_disable), then PLL2 off
 * Writes only the named camcc registers (PLL0, PLL2, MCLK0-3); reads GCC camera_ahb (no GCC/PMIC/RPMh writes).
 * Every write is logged with its read-back. Needs camera_ahb on (it is after boot, cam1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../wifi/pmem.h"

#define CAMCC		0xad00000u
#define GCC_CAM_AHB	0x10b008u

/* PLL2: .offset 0x2000, Fabia register layout (camcc-sm7150.c:147-148, clk-alpha-pll.c:120-131) */
#define PLL2		(CAMCC + 0x2000)
#define PLL_MODE	(PLL2 + 0x00)
#define PLL_L		(PLL2 + 0x04)
#define PLL_USER	(PLL2 + 0x0c)
#define PLL_USER_U	(PLL2 + 0x10)
#define PLL_CONFIG	(PLL2 + 0x14)
#define PLL_CONFIG_U	(PLL2 + 0x18)
#define PLL_TEST	(PLL2 + 0x1c)
#define PLL_TEST_U	(PLL2 + 0x20)
#define PLL_STATUS	(PLL2 + 0x24)
#define PLL_OPMODE	(PLL2 + 0x2c)
#define PLL_FRAC	(PLL2 + 0x38)
/* MODE bits (clk-alpha-pll.c:17-34) */
#define OUTCTRL		(1u << 0)
#define BYPASSNL	(1u << 1)
#define RESET_N		(1u << 2)
#define VOTE_FSM_ENA	(1u << 20)
#define UPDATE_BYPASS	(1u << 23)
#define LOCK_DET	(1u << 31)

/* PLL0: .offset 0x0, Fabia (camcc-sm7150.c:55-58); same layout as above. OPMODE STANDBY 0 / RUN 1, outputs
 * main/even/odd = USER_CTL [2:0] (clk-alpha-pll.c:333-335). Excerpts: research/notes/camera-stage-c2-linux.md */
#define PLL0		(CAMCC + 0x0)
#define P0_MODE		(PLL0 + 0x00)
#define P0_L		(PLL0 + 0x04)
#define P0_USER		(PLL0 + 0x0c)
#define P0_TEST		(PLL0 + 0x1c)
#define P0_OPMODE	(PLL0 + 0x2c)
#define P0_FRAC		(PLL0 + 0x38)
#define PLL_RUN		1u
#define PLL_OUT_MASK	7u

/* MCLK N: cmd_rcgr 0x5004 + 0x20 N (camcc-sm7150.c:823,837,851,865), CBCR 0x501c + 0x20 N (:1747,1765,1783,1801) */
#define MCLK_RCG(n)	(CAMCC + 0x5004 + 0x20 * (n))
#define MCLK_CBCR(n)	(CAMCC + 0x501c + 0x20 * (n))
#define CBCR_CLK_OFF	(1u << 31)

static int allowed(uint32_t pa)
{
	if (pa >= PLL2 && pa <= PLL_FRAC)
		return 1;
	if (pa <= P0_FRAC && pa >= PLL0)	/* stage c2: the PLL0 registers only */
		return 1;
	for (int n = 0; n < 4; n++)
		if ((pa >= MCLK_RCG(n) && pa <= MCLK_RCG(n) + 0x10) || pa == MCLK_CBCR(n))
			return 1;
	return 0;
}

static void wr(uint32_t pa, uint32_t v, const char *why)
{
	if (!allowed(pa)) {
		printf("camclk: BLOCKED wr %#x = %#x (%s)\n", pa, v, why);
		exit(1);
	}
	uint32_t old = rd32(pa);
	wr32(pa, v);
	printf("camclk: wr %#x %#010x -> %#010x, reads %#010x  [%s]\n", pa, old, v, rd32(pa), why);
}

static void upd(uint32_t pa, uint32_t mask, uint32_t val, const char *why)	/* regmap_update_bits */
{
	wr(pa, (rd32(pa) & ~mask) | (val & mask), why);
}

/* poll until (reg & mask) == want; tries x us_step; 0 = ok */
static int poll(uint32_t pa, uint32_t mask, uint32_t want, int tries, unsigned us_step)
{
	for (int i = 0; i < tries; i++) {
		if ((rd32(pa) & mask) == want)
			return 0;
		usleep(us_step);
	}
	return (rd32(pa) & mask) == want ? 0 : -1;
}

static void status(void)
{
	static const struct { const char *n; uint32_t off; } r[] = {	/* Fabia/Agera offsets, PLL0 and PLL2 alike */
		{ "MODE", 0x00 }, { "L", 0x04 }, { "USER_CTL", 0x0c }, { "USER_CTL_U", 0x10 },
		{ "CONFIG_CTL", 0x14 }, { "CONFIG_CTL_U", 0x18 }, { "TEST_CTL", 0x1c },
		{ "TEST_CTL_U", 0x20 }, { "STATUS", 0x24 }, { "OPMODE", 0x2c }, { "FRAC(alpha)", 0x38 },
	};
	printf("gcc camera_ahb %#x\n", rd32(GCC_CAM_AHB));
	for (unsigned i = 0; i < sizeof r / sizeof r[0]; i++)
		printf("pll0 %-12s %#x = %#010x\n", r[i].n, PLL0 + r[i].off, rd32(PLL0 + r[i].off));
	uint32_t m0 = rd32(P0_MODE), u0 = rd32(P0_USER);
	double vco0 = 19.2 * ((rd32(P0_L) & 0xffff) + (rd32(P0_FRAC) & 0xffff) / 65536.0);
	printf("pll0: lock %u outctrl %u reset_n %u update_bypass %u fsm %u opmode %u; VCO %.1f MHz, outs main/even/odd "
	       "%u%u%u, postdiv even %u odd %u (Linux uses fixed /2 /3)\n", m0 >> 31, m0 & 1, (m0 >> 2) & 1, (m0 >> 23) & 1,
	       (m0 >> 20) & 1, rd32(P0_OPMODE), vco0, u0 & 1, (u0 >> 1) & 1, (u0 >> 2) & 1, (u0 >> 8) & 0xf,
	       (u0 >> 12) & 0xf);
	for (unsigned i = 0; i < sizeof r / sizeof r[0]; i++)
		printf("pll2 %-12s %#x = %#010x\n", r[i].n, PLL2 + r[i].off, rd32(PLL2 + r[i].off));
	uint32_t m = rd32(PLL_MODE), l = rd32(PLL_L), u = rd32(PLL_USER);
	/* rate = 19.2 MHz * (L + FRAC/2^16) (Fabia alpha width 16); aux = VCO >> fls(postdiv) (clk-alpha-pll postdiv_ops) */
	double vco = 19.2 * ((l & 0xffff) + (rd32(PLL_FRAC) & 0xffff) / 65536.0);
	unsigned pd = (u >> 8) & 3, sh = pd ? 32 - __builtin_clz(pd) : 0;
	printf("pll2: lock %u outctrl %u bypassnl %u reset_n %u fsm %u; VCO %.1f MHz, outs main/aux/early %u%u%u, "
	       "postdiv %u -> aux %.1f MHz\n", m >> 31, m & 1, (m >> 1) & 1, (m >> 2) & 1, (m >> 20) & 1, vco,
	       u & 1, (u >> 1) & 1, (u >> 3) & 1, pd, vco / (1u << sh));
	for (int n = 0; n < 4; n++) {
		uint32_t b = MCLK_RCG(n), cfg = rd32(b + 4), mm = rd32(b + 8), nn = rd32(b + 0xc);
		printf("mclk%d rcg %#x CMD %#010x CFG %#010x M %#x N %#x D %#x (src %u, div %u, mode %u) CBCR %#010x%s\n",
		       n, b, rd32(b), cfg, mm, nn, rd32(b + 0x10), (cfg >> 8) & 7, cfg & 0x1f, (cfg >> 12) & 3,
		       rd32(MCLK_CBCR(n)), (rd32(MCLK_CBCR(n)) & CBCR_CLK_OFF) ? " (off)" : " (on)");
	}
}

static int pll2_on(void)
{
	uint32_t m = rd32(PLL_MODE);
	if (m & VOTE_FSM_ENA) {	/* clk-alpha-pll.c:588: FSM vote mode; PLL2 has no enable_reg, so don't touch it */
		printf("camclk: pll2 MODE %#x in FSM vote mode, abort\n", m);
		return 1;
	}
	if ((m & (OUTCTRL | RESET_N | BYPASSNL)) == (OUTCTRL | RESET_N | BYPASSNL)) {	/* :596 skip if enabled */
		printf("camclk: pll2 already enabled (MODE %#x)\n", m);
		return 0;
	}
	/* clk_agera_pll_configure (clk-alpha-pll.c:1851-1866) with camcc_pll2_config (camcc-sm7150.c:139-144);
	 * clk_alpha_pll_write_config (:434) skips zero values: alpha, test_ctl, test_ctl_hi are 0, not written */
	wr(PLL_L, 0x64, "L 0x64 = 1920 MHz; camcc-sm7150.c:140, clk-alpha-pll.c:1854");
	wr(PLL_USER, 0x30b, "USER_CTL 0x30b; camcc-sm7150.c:143, clk-alpha-pll.c:1856");
	wr(PLL_CONFIG, 0x20000954, "CONFIG_CTL; camcc-sm7150.c:142, clk-alpha-pll.c:1858");
	wr(PLL_CONFIG_U, 0x400003d6, "CONFIG_CTL_U; camcc-sm7150.c:141, clk-alpha-pll.c:1860");
	/* clk_alpha_pll_enable (clk-alpha-pll.c:578-628), the agera .enable (:1894) */
	upd(PLL_MODE, BYPASSNL, BYPASSNL, "BYPASSNL; clk-alpha-pll.c:600");
	usleep(5);	/* :605-610 5 us between bypass and reset */
	upd(PLL_MODE, RESET_N, RESET_N, "RESET_N; clk-alpha-pll.c:612");
	/* wait_for_pll_enable_lock (:413, :381; Linux 1500 x 1 us); here 100 x 1 ms */
	if (poll(PLL_MODE, LOCK_DET, LOCK_DET, 100, 1000)) {
		printf("camclk: pll2 no lock after 100 ms (MODE %#x), undo RESET_N/BYPASSNL, OUTCTRL not set\n",
		       rd32(PLL_MODE));
		upd(PLL_MODE, RESET_N | BYPASSNL, 0, "undo; as clk_alpha_pll_disable clk-alpha-pll.c:650");
		return 1;
	}
	printf("camclk: pll2 locked, MODE %#x\n", rd32(PLL_MODE));
	upd(PLL_MODE, OUTCTRL, OUTCTRL, "OUTCTRL; clk-alpha-pll.c:621");
	return 0;
}

static void pll2_off(void)
{
	/* clk_alpha_pll_disable (clk-alpha-pll.c:630-652) */
	if (rd32(PLL_MODE) & VOTE_FSM_ENA) {
		printf("camclk: pll2 in FSM vote mode, left alone\n");
		return;
	}
	upd(PLL_MODE, OUTCTRL, 0, "OUTCTRL off; clk-alpha-pll.c:644");
	usleep(1);	/* :647-648 */
	upd(PLL_MODE, RESET_N | BYPASSNL, 0, "RESET_N|BYPASSNL off; clk-alpha-pll.c:650-651");
}

/* stage c2: PLL0 = Fabia. clk_fabia_pll_configure (clk-alpha-pll.c:1297-1332) with camcc_pll0_config
 * (camcc-sm7150.c:47-53), then alpha_pll_fabia_enable (:1334-1389). Linux configures at probe (:2035); here
 * both run once per 'pll0 on'. */
static int pll0_on(void)
{
	uint32_t m = rd32(P0_MODE), op = rd32(P0_OPMODE);
	if (m & VOTE_FSM_ENA) {	/* :1346 FSM vote mode: PLL0 has no enable_reg here, leave it */
		printf("camclk: pll0 MODE %#x in FSM vote mode, abort\n", m);
		return 1;
	}
	if ((op & PLL_RUN) && (m & OUTCTRL)) {	/* :1358 skip if running */
		printf("camclk: pll0 already running (MODE %#x OPMODE %#x)\n", m, op);
		return 0;
	}
	/* precheck: reset state MODE 0 (cam1), or our own 'off' state (RESET_N|UPDATE_BYPASS, standby, no OUTCTRL) */
	if (m & ~(RESET_N | UPDATE_BYPASS)) {
		printf("camclk: pll0 MODE %#x not 0 (or our off state), abort\n", m);
		return 1;
	}
	printf("camclk: pll0 MODE %#x OPMODE %#x before\n", m, op);
	/* configure; zero fields (config_ctl, config_ctl_hi, user_ctl, user_ctl_hi, test_ctl_hi) skipped (:434) */
	wr(P0_L, 0x3e, "L 0x3e; camcc-sm7150.c:48, clk-alpha-pll.c:1302");
	wr(P0_FRAC, 0x8000, "FRAC 0x8000 -> VCO 19.2*62.5 = 1200 MHz; camcc-sm7150.c:49, clk-alpha-pll.c:1303");
	wr(P0_TEST, 0x40000000, "TEST_CTL; camcc-sm7150.c:52, clk-alpha-pll.c:1313");
	upd(P0_USER, 0xffu << 8, 0x31u << 8, "USER_CTL postdiv 0x31; camcc-sm7150.c:50-51, clk-alpha-pll.c:1317-1321");
	upd(P0_MODE, UPDATE_BYPASS, UPDATE_BYPASS, "UPDATE_BYPASS; clk-alpha-pll.c:1327");
	upd(P0_MODE, RESET_N, RESET_N, "RESET_N; clk-alpha-pll.c:1330");
	/* enable */
	upd(P0_MODE, OUTCTRL, 0, "OUTCTRL off; clk-alpha-pll.c:1361");
	wr(P0_OPMODE, 0, "OPMODE STANDBY; clk-alpha-pll.c:1365");
	upd(P0_MODE, RESET_N, RESET_N, "RESET_N; clk-alpha-pll.c:1369");
	wr(P0_OPMODE, PLL_RUN, "OPMODE RUN; clk-alpha-pll.c:1374");
	/* wait_for_pll_enable_lock (:380-405, 1500 x 1 us); here 100 x 1 ms */
	if (poll(P0_MODE, LOCK_DET, LOCK_DET, 100, 1000)) {
		printf("camclk: pll0 no lock after 100 ms (MODE %#x), back to STANDBY, outputs not enabled\n",
		       rd32(P0_MODE));
		wr(P0_OPMODE, 0, "OPMODE STANDBY; as alpha_pll_fabia_disable clk-alpha-pll.c:1418");
		return 1;
	}
	printf("camclk: pll0 locked, MODE %#x\n", rd32(P0_MODE));
	upd(P0_USER, PLL_OUT_MASK, PLL_OUT_MASK, "USER_CTL main/even/odd out; clk-alpha-pll.c:1383");
	upd(P0_MODE, OUTCTRL, OUTCTRL, "OUTCTRL; clk-alpha-pll.c:1387");
	return 0;
}

/* alpha_pll_fabia_disable (clk-alpha-pll.c:1391-1418): RESET_N stays set in Linux too */
static void pll0_off(void)
{
	uint32_t m = rd32(P0_MODE);
	if (m & VOTE_FSM_ENA) {
		printf("camclk: pll0 in FSM vote mode, left alone\n");
		return;
	}
	if (!(m & OUTCTRL) && !(rd32(P0_OPMODE) & PLL_RUN)) {
		printf("camclk: pll0 already off (MODE %#x)\n", m);
		return;
	}
	upd(P0_MODE, OUTCTRL, 0, "OUTCTRL off; clk-alpha-pll.c:1408");
	upd(P0_USER, PLL_OUT_MASK, 0, "outputs off; clk-alpha-pll.c:1413");
	wr(P0_OPMODE, 0, "OPMODE STANDBY; clk-alpha-pll.c:1418");
}

static int mclk_24(int n)
{
	uint32_t b = MCLK_RCG(n);
	if (!(rd32(PLL_MODE) & LOCK_DET) || !(rd32(PLL_MODE) & OUTCTRL)) {
		printf("camclk: pll2 not running (MODE %#x): run 'camclk pll2 on' first\n", rd32(PLL_MODE));
		return 1;
	}
	/* F(24000000, P_CAMCC_PLL2_OUT_AUX, 1, 1, 20) (camcc-sm7150.c:817): pre_div = 2*1-1 = 1 (clk-rcg.h:10);
	 * mnd_width 8, hid_width 5 (:824-825); src 5 from camcc_parent_map_2 (:311-314).
	 * __clk_rcg2_configure_mnd (clk-rcg2.c:517-561): M = 1, N = ~(20-1) & 0xff = 0xec,
	 * D = ~clamp(20, 1, 38) & 0xff = 0xeb, CFG: div 1 | dual edge (2<<12) | src 5<<8 */
	upd(b + 8, 0xff, 1, "M = 1; clk-rcg2.c:526");
	upd(b + 0xc, 0xff, ~(20u - 1), "N = ~(n-m); clk-rcg2.c:531");
	upd(b + 0x10, 0xff, ~20u, "D = ~2d; clk-rcg2.c:544-547");
	/* clk_rcg2_configure (clk-rcg2.c:580-597): read CFG, set src/div/mode (hw_clk_ctrl 0), write, update_config */
	uint32_t cfg = rd32(b + 4);
	cfg &= ~(0x7u << 8);
	cfg |= 5u << 8;					/* :511-512 */
	cfg &= ~(0x1fu | (3u << 12) | (1u << 20));	/* :551-559 */
	cfg |= 1 | (2u << 12);
	wr(b + 4, cfg, "CFG src 5 div 1 dual-edge; clk-rcg2.c:592");
	upd(b, 1, 1, "CMD UPDATE; clk-rcg2.c:121 update_config");
	if (poll(b, 1, 0, 500, 1)) {	/* :127-134, 500 x 1 us */
		printf("camclk: mclk%d rcg didn't update (CMD %#x), branch not enabled\n", n, rd32(b));
		return 1;
	}
	printf("camclk: mclk%d rcg updated, CMD %#x\n", n, rd32(b));
	/* clk_branch2 enable: enable_reg bit 0 (camcc-sm7150.c:1750), BRANCH_HALT poll CLK_OFF clear (clk-branch.c:80-86) */
	upd(MCLK_CBCR(n), 1, 1, "CBCR enable; clk-branch.c / camcc-sm7150.c:1750");
	if (poll(MCLK_CBCR(n), CBCR_CLK_OFF, 0, 200, 1)) {
		printf("camclk: mclk%d branch stuck off (CBCR %#x)\n", n, rd32(MCLK_CBCR(n)));
		return 1;
	}
	printf("camclk: mclk%d on at 24 MHz, CBCR %#x\n", n, rd32(MCLK_CBCR(n)));
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2)
		goto usage;
	pm_init();
	if (!(rd32(GCC_CAM_AHB) & 1) || (rd32(GCC_CAM_AHB) & CBCR_CLK_OFF)) {
		printf("camclk: gcc camera_ahb %#x not on, abort (no GCC writes here)\n", rd32(GCC_CAM_AHB));
		return 1;
	}
	if (!strcmp(argv[1], "status")) {
		status();
		return 0;
	}
	if (!strcmp(argv[1], "pll2") && argc == 3 && !strcmp(argv[2], "on"))
		return pll2_on();
	if (!strcmp(argv[1], "pll0") && argc == 3 && !strcmp(argv[2], "on"))
		return pll0_on();
	if (!strcmp(argv[1], "mclk") && argc == 4 && !strcmp(argv[3], "24")) {
		int n = atoi(argv[2]);
		if (n < 0 || n > 3 || argv[2][1])
			goto usage;
		return mclk_24(n);
	}
	if (!strcmp(argv[1], "off")) {
		for (int n = 0; n < 4; n++)	/* clk_branch2 disable: clear enable bit, BRANCH_HALT waits for CLK_OFF */
			if (rd32(MCLK_CBCR(n)) & 1) {
				upd(MCLK_CBCR(n), 1, 0, "CBCR disable");
				if (poll(MCLK_CBCR(n), CBCR_CLK_OFF, CBCR_CLK_OFF, 200, 1))
					printf("camclk: mclk%d CLK_OFF not set (CBCR %#x)\n", n, rd32(MCLK_CBCR(n)));
			}
		pll0_off();	/* camrx phy off / clocks off first: its csi0phytimer RCG runs from PLL0 even */
		pll2_off();
		return 0;
	}
usage:
	fprintf(stderr, "usage: camclk status | pll2 on | pll0 on | mclk N 24 | off\n");
	return 2;
}
