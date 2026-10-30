/*
 * camrx: camera stage c1, the CSID-lite 0 test pattern generator (no PHY, no IFE DMA, no SMMU). Register spec:
 * research/notes/camera-stage-c.md (tags redfin/DT/Linux/guess), sequences from ios-redfin 0921 (csid_reset,
 * csid_config, paths_down).
 *   camrx [--dry-run] status        read-only: PLL2, GDSC, the RCGs/CBCRs below, refgen, CSID-lite (if clocked)
 *   camrx [--dry-run] clocks on|off cphy_rx 384, ife_lite_csid 384, ife_lite 320 MHz (all PLL2) + their branches
 *   camrx [--dry-run] tpg [SECONDS] CSID reset, TPG 640x480 RAW10 colour bars into RDI0, poll IRQ status every 100 ms
 * Needs (checked, read-only): gcc camera_ahb on, TITAN_TOP GDSC on (camid --keep), PLL2 locked (camclk pll2 on).
 * Never enables PLLs/GDSCs/regulators, never writes GCC/RPMh/refgen. Writes only the whitelist in allowed();
 * every write is logged with its read-back. No camcc register is read before the GDSC check passes, no CSID
 * register before its two branches run.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../wifi/pmem.h"

#define GCC_CAM_AHB	0x10b008u
#define CAMCC		0xad00000u
#define PLL2_MODE	(CAMCC + 0x2000)
#define TITAN_GDSC	0xad0c1c4u
#define LOCK_DET	(1u << 31)
#define CBCR_CLK_OFF	(1u << 31)

/* RCG CMD addresses (CFG = +4) and branches (notes section 1, Linux camcc-sm7150.c) */
#define CPHY_RX_RCG	0xad0a064u
#define LITE_CSID_RCG	0xad0c020u
#define LITE_RCG	0xad0c004u
#define PHYTIMER0_RCG	0xad06004u	/* status only (stage c2) */
#define CSIPHY0_CBCR	0xad06020u	/* status only */
#define PHYTIMER0_CBCR	0xad0601cu	/* status only */
#define LITE_CSID_CBCR	0xad0c038u
#define LITE_CPHY_CBCR	0xad0c040u
#define LITE_CBCR	0xad0c01cu

/* refgen (msm-4.14 refgen.c offsets, recall): read only, never written */
#define REFGEN_BIAS_EN	0xff1008u
#define REFGEN_BG_CTRL	0xff1014u

/* CSID-lite 0 (DT csid-lite175, redfin 0921 offsets) */
#define CSID		0xacc8000u
#define CSID_END	(CSID + 0x700)
#define HW_VERSION	0x000
#define RST_STROBES	0x010
#define RX_IRQ_STATUS	0x020
#define RX_IRQ_MASK	0x024
#define RX_IRQ_CLEAR	0x028
#define RDI_IRQ_STATUS(n) (0x030 + 0x10 * (n))
#define RDI_IRQ_MASK(n)	(0x034 + 0x10 * (n))
#define RDI_IRQ_CLEAR(n) (0x038 + 0x10 * (n))
#define TOP_IRQ_STATUS	0x070
#define TOP_IRQ_MASK	0x074
#define TOP_IRQ_CLEAR	0x078
#define IRQ_CMD		0x080
#define RX_CFG0		0x100
#define RX_CFG1		0x104
#define RX_PKTS		0x160
#define RX_ECC		0x164
#define RX_CRC		0x168
#define RDI_CFG0(n)	(0x200 + 0x100 * (n))
#define RDI0_CFG1	0x204
#define RDI0_CTRL	0x208
#define RDI0_FRM_DROP_PAT 0x20c
#define RDI0_FRM_DROP_PER 0x210
#define RDI0_SUB_PAT	0x214
#define RDI0_SUB_PER	0x218
#define RDI0_PIX_DROP_PAT 0x224
#define RDI0_PIX_DROP_PER 0x228
#define RDI0_LINE_DROP_PAT 0x22c
#define RDI0_LINE_DROP_PER 0x230
#define RDI0_STATUS	0x250
#define RDI0_BYTES_PING	0x2e0
#define RDI0_BYTES_PONG	0x2e4
#define TPG_CTRL	0x600
#define TPG_VC_CFG0	0x604
#define TPG_VC_CFG1	0x608
#define TPG_LFSR	0x60c
#define TPG_DT0_CFG0	0x610
#define TPG_DT0_CFG1	0x614
#define TPG_DT0_CFG2	0x618
#define TPG_COLOR_BARS	0x640
#define TPG_COMMON_GEN	0x648

#define RAW10		0x2b
#define TPG_VC		0xa
#define TPG_W		640	/* redfin used 1600x1200; smaller frame, same pattern */
#define TPG_H		480

static int dry;
static volatile sig_atomic_t stop;

static int allowed(uint32_t pa)
{
	static const uint32_t ok[] = {
		CPHY_RX_RCG, CPHY_RX_RCG + 4, LITE_CSID_RCG, LITE_CSID_RCG + 4, LITE_RCG, LITE_RCG + 4,
		LITE_CSID_CBCR, LITE_CPHY_CBCR, LITE_CBCR,
	};
	for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; i++)
		if (pa == ok[i])
			return 1;
	return pa >= CSID && pa < CSID_END && !(pa & 3);
}

static void wr(uint32_t pa, uint32_t v, const char *why)
{
	if (!allowed(pa)) {
		printf("camrx: BLOCKED wr %#x = %#x (%s)\n", pa, v, why);
		exit(1);
	}
	if (dry) {
		printf("[dry] wr %#x = %#010x  [%s]\n", pa, v, why);
		return;
	}
	uint32_t old = rd32(pa);
	wr32(pa, v);
	printf("camrx: wr %#x %#010x -> %#010x, reads %#010x  [%s]\n", pa, old, v, rd32(pa), why);
}

/* CSID write; quiet = no log line (the 100 ms poll's clears) */
static void cw(uint32_t off, uint32_t v, const char *why)
{
	wr(CSID + off, v, why);
}

static void cw_quiet(uint32_t off, uint32_t v)
{
	if (!dry)
		wr32(CSID + off, v);
}

static uint32_t cr(uint32_t off)
{
	return dry ? 0 : rd32(CSID + off);
}

static int poll(uint32_t pa, uint32_t mask, uint32_t want, int tries, unsigned us_step)
{
	if (dry)
		return 0;
	for (int i = 0; i < tries; i++) {
		if ((rd32(pa) & mask) == want)
			return 0;
		usleep(us_step);
	}
	return (rd32(pa) & mask) == want ? 0 : -1;
}

static void stage(const char *s)	/* redfin cstep: a reset leaves the last line on the console */
{
	printf("camrx: == %s\n", s);
	fflush(stdout);
	usleep(300000);
}

static int branch_on(uint32_t cbcr)
{
	return (rd32(cbcr) & 1) && !(rd32(cbcr) & CBCR_CLK_OFF);
}

static int csid_clocked(void)
{
	return branch_on(LITE_CSID_CBCR) && branch_on(LITE_CPHY_CBCR);
}

/* read-only preconditions; 0 = ok. Order: GCC, GDSC, then camcc */
static int precheck(void)
{
	uint32_t a = rd32(GCC_CAM_AHB);
	if (!(a & 1) || (a & CBCR_CLK_OFF)) {
		printf("camrx: gcc camera_ahb %#x not on, abort\n", a);
		return 1;
	}
	uint32_t g = rd32(TITAN_GDSC);
	if (!(g & (1u << 31))) {
		printf("camrx: titan gdsc %#x off: run 'camid --keep' first (no camcc read before that)\n", g);
		return 1;
	}
	uint32_t m = rd32(PLL2_MODE);
	if (!(m & LOCK_DET) || !(m & 1)) {
		printf("camrx: pll2 MODE %#x not locked/out: run 'camclk pll2 on' first\n", m);
		return 1;
	}
	return 0;
}

static void show_rcg(const char *n, uint32_t b)
{
	uint32_t cfg = rd32(b + 4);
	printf("%-14s rcg %#x CMD %#010x CFG %#010x (src %u, div field %u)\n", n, b, rd32(b), cfg, (cfg >> 8) & 7,
	       cfg & 0x1f);
}

static void show_cbcr(const char *n, uint32_t pa)
{
	uint32_t v = rd32(pa);
	printf("%-14s cbcr %#x = %#010x%s\n", n, pa, v, (v & CBCR_CLK_OFF) ? " (off)" : " (on)");
}

static int status(void)
{
	printf("gcc camera_ahb %#x, titan gdsc %#x\n", rd32(GCC_CAM_AHB), rd32(TITAN_GDSC));
	if (!(rd32(TITAN_GDSC) & (1u << 31))) {
		printf("camrx: gdsc off, camcc not read\n");
		return 1;
	}
	uint32_t m = rd32(PLL2_MODE);
	printf("pll2 MODE %#010x: lock %u outctrl %u\n", m, m >> 31, m & 1);
	show_rcg("cphy_rx", CPHY_RX_RCG);
	show_rcg("ife_lite_csid", LITE_CSID_RCG);
	show_rcg("ife_lite", LITE_RCG);
	show_rcg("csi0phytimer", PHYTIMER0_RCG);
	show_cbcr("csiphy0", CSIPHY0_CBCR);
	show_cbcr("csi0phytimer", PHYTIMER0_CBCR);
	show_cbcr("lite_cphy_rx", LITE_CPHY_CBCR);
	show_cbcr("lite_csid", LITE_CSID_CBCR);
	show_cbcr("ife_lite", LITE_CBCR);
	/* guess: offsets from msm-4.14 refgen.c recalled, not verified */
	printf("refgen BIAS_EN %#x = %#x (on 0x7), BG_CTRL %#x = %#x (on 0x6)  [offsets: guess]\n", REFGEN_BIAS_EN,
	       rd32(REFGEN_BIAS_EN), REFGEN_BG_CTRL, rd32(REFGEN_BG_CTRL));
	if (!csid_clocked()) {
		printf("csid-lite: lite_csid/lite_cphy_rx branches off, not read\n");
		return 0;
	}
	printf("csid-lite HW_VERSION %#x; irq top %#x rx %#x rdi0 %#x; RX_CFG0 %#x RDI0_CFG0 %#x STATUS %#x "
	       "pkts %u ecc %#x crc %u; TPG_CTRL %#x\n", rd32(CSID + HW_VERSION), rd32(CSID + TOP_IRQ_STATUS),
	       rd32(CSID + RX_IRQ_STATUS), rd32(CSID + RDI_IRQ_STATUS(0)), rd32(CSID + RX_CFG0), rd32(CSID + RDI_CFG0(0)),
	       rd32(CSID + RDI0_STATUS), rd32(CSID + RX_PKTS), rd32(CSID + RX_ECC), rd32(CSID + RX_CRC),
	       rd32(CSID + TPG_CTRL));
	return 0;
}

static const struct { const char *n; uint32_t rcg, cfg; } rcgs[] = {
	{ "cphy_rx 384 MHz (PLL2 early 960 / 2.5)", CPHY_RX_RCG, 0x504 },
	{ "ife_lite_csid 384 MHz (PLL2 early 960 / 2.5)", LITE_CSID_RCG, 0x504 },
	{ "ife_lite 320 MHz (PLL2 main 480 / 1.5)", LITE_RCG, 0x502 },
};
/* order on; off goes in reverse */
static const struct { const char *n; uint32_t cbcr; } branches[] = {
	{ "ife_lite_cphy_rx", LITE_CPHY_CBCR }, { "ife_lite_csid", LITE_CSID_CBCR }, { "ife_lite", LITE_CBCR },
};
#define N(a) (sizeof(a) / sizeof((a)[0]))

static int clocks_on(void)
{
	int bad = 0;
	stage("rcgs (CFG, then CMD UPDATE)");
	for (unsigned i = 0; i < N(rcgs); i++) {
		uint32_t b = rcgs[i].rcg;
		/* redfin masks CFG with 0x371f: div [4:0], src [10:8], mode [13:12] */
		wr(b + 4, (rd32(b + 4) & ~0x371fu) | rcgs[i].cfg, rcgs[i].n);
		wr(b, rd32(b) | 1, "CMD UPDATE");
		if (poll(b, 1, 0, 500, 1000)) {	/* redfin: 500 ms */
			printf("camrx: %s: UPDATE stuck (CMD %#x), branches not enabled\n", rcgs[i].n, rd32(b));
			return 1;
		}
	}
	stage("branches (enable, wait CLK_OFF clear)");
	for (unsigned i = 0; i < N(branches); i++) {
		wr(branches[i].cbcr, rd32(branches[i].cbcr) | 1, branches[i].n);
		if (poll(branches[i].cbcr, CBCR_CLK_OFF, 0, 200, 1000)) {
			printf("camrx: %s stuck off (CBCR %#x)\n", branches[i].n, rd32(branches[i].cbcr));
			bad = 1;
		}
	}
	return bad;
}

static int clocks_off(void)
{
	stage("branches off");
	for (int i = N(branches) - 1; i >= 0; i--) {
		wr(branches[i].cbcr, rd32(branches[i].cbcr) & ~1u, branches[i].n);
		if (poll(branches[i].cbcr, CBCR_CLK_OFF, CBCR_CLK_OFF, 200, 1000))
			printf("camrx: %s CLK_OFF not set (CBCR %#x)\n", branches[i].n, rd32(branches[i].cbcr));
	}
	return 0;
}

/* redfin csid_reset: register reset (strobe 0x1), then hw reset (0x1e), each waits TOP_IRQ bit0 (100 ms) */
static int csid_reset(void)
{
	cw(RX_IRQ_MASK, 0, "RX_IRQ_MASK 0");
	for (unsigned r = 0; r < 4; r++)
		cw(RDI_IRQ_MASK(r), 0, "RDIn_IRQ_MASK 0");
	cw(TOP_IRQ_CLEAR, 1, "TOP_CLEAR");
	cw(RX_IRQ_CLEAR, 0x0fffffff, "RX_CLEAR");
	for (unsigned r = 0; r < 4; r++)
		cw(RDI_IRQ_CLEAR(r), 0x7fff, "RDIn_CLEAR");
	cw(IRQ_CMD, 1, "IRQ_CMD");
	cw(RX_CFG1, 0x80, "RX_CFG1 0x80");
	for (unsigned r = 0; r < 4; r++)
		cw(RDI_CFG0(r), 2, "RDIn_CFG0 2");
	for (unsigned hw = 0; hw < 2; hw++) {
		stage(hw ? "csid hw reset (strobes 0x1e)" : "csid register reset (strobes 0x1)");
		cw(TOP_IRQ_CLEAR, 1, "TOP_CLEAR");
		cw(IRQ_CMD, 1, "IRQ_CMD");
		if (hw) {
			cw(TOP_IRQ_MASK, 1, "TOP_MASK 1");
			cw(IRQ_CMD, 1, "IRQ_CMD");
		}
		cw(RST_STROBES, hw ? 0x1e : 0x1, "RST_STROBES");
		if (!hw) {
			cw(TOP_IRQ_MASK, 1, "TOP_MASK 1");
			cw(IRQ_CMD, 1, "IRQ_CMD");
		}
		if (poll(CSID + TOP_IRQ_STATUS, 1, 1, 100, 1000)) {
			printf("camrx: csid %s reset did not finish (top irq %#x)\n", hw ? "hw" : "register",
			       cr(TOP_IRQ_STATUS));
			return 1;
		}
		cw(TOP_IRQ_CLEAR, 1, "TOP_CLEAR");
		cw(IRQ_CMD, 1, "IRQ_CMD");
	}
	return 0;
}

static void paths_down(void)
{
	stage("down: TPG off, RDI0 halt + disable, RX_CFG0 0, masks 0");
	cw(TPG_CTRL, 0, "TPG_CTRL off");
	cw(RDI_IRQ_MASK(0), 0, "RDI0_IRQ_MASK 0");
	cw(RDI0_CTRL, (cr(RDI0_CTRL) & ~3u) | 2u, "RDI0_CTRL halt");
	cw(RDI_CFG0(0), cr(RDI_CFG0(0)) & ~(1u << 31), "RDI0_CFG0 disable");
	cw(RX_CFG0, 0, "RX_CFG0 0");
	cw(TOP_IRQ_MASK, 0, "TOP_MASK 0");
	cw(TOP_IRQ_CLEAR, 1, "TOP_CLEAR");
	cw(RDI_IRQ_CLEAR(0), 0x7fff, "RDI0_CLEAR");
	cw(RX_IRQ_CLEAR, 0x0fffffff, "RX_CLEAR");
	cw(IRQ_CMD, 1, "IRQ_CMD");
}

static void on_sig(int s)
{
	(void)s;
	stop = 1;
}

static const char *rdi_bit[15] = {	/* msm-4.14 cam_ife_csid_core.h, recall (guess) */
	[1] = "RST_DONE", [2] = "FIFO_OVERFLOW", [3] = "SUBSAMPLED_EOF", [4] = "SUBSAMPLED_SOF",
	[9] = "INPUT_EOF", [11] = "INPUT_SOL", [12] = "INPUT_SOF", [13] = "PIX_COUNT_ERR", [14] = "LINE_COUNT_ERR",
};

static int tpg(int secs)
{
	if (!dry && !csid_clocked()) {
		printf("camrx: lite_csid/lite_cphy_rx branches not on: run 'camrx clocks on' first\n");
		return 1;
	}
	printf("camrx: csid-lite HW_VERSION %#x\n", cr(HW_VERSION));
	printf("camrx: note: TPG_CTRL/VC_CFG1/DT0_CFG2 field meanings, RDI bit names and status latching without a "
	       "mask bit are guesses (values copied from redfin 0921)\n");
	stage("csid reset");
	if (csid_reset())
		return 1;
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	stage("csid rx + tpg config (640x480 RAW10 colour bars, vc 0xa)");
	cw(RX_CFG0, 3 | 0x3210u << 4 | 0u << 20, "RX_CFG0 4 lanes 0x3210 phy 0");
	cw(RX_CFG1, 1u << 6 | 1u, "RX_CFG1 0x41");
	cw(TPG_VC_CFG0, 1u << 10 | TPG_VC, "TPG_VC_CFG0 1 DT, vc 0xa");
	cw(TPG_VC_CFG1, 0x3ffu << 12 | 0x740, "TPG_VC_CFG1 (guess: blanking)");	/* guess: field meaning */
	cw(TPG_LFSR, 0x12345678, "TPG_LFSR_SEED");
	cw(TPG_DT0_CFG0, TPG_W << 16 | TPG_H, "TPG_DT0_CFG0 640x480");
	cw(TPG_DT0_CFG1, RAW10, "TPG_DT0_CFG1 RAW10");
	cw(TPG_DT0_CFG2, (3u - 1) << 16 | 0x8, "TPG_DT0_CFG2 (guess: payload mode/encode)");	/* guess */
	cw(TPG_COLOR_BARS, 1u << 5, "TPG_COLOR_BARS");
	cw(TPG_COMMON_GEN, 0, "TPG_COMMON_GEN");
	stage("rdi0 config (no write master)");
	uint32_t cfg0 = TPG_VC << 22 | RAW10 << 16 | 0xfu << 12 | 1u << 2 | 3u;
	cw(RDI_CFG0(0), cfg0, "RDI0_CFG0");
	cw(RDI0_CFG1, 2, "RDI0_CFG1");
	cw(RDI0_FRM_DROP_PER, 1, "FRM_DROP_PER");
	cw(RDI0_FRM_DROP_PAT, 0, "FRM_DROP_PAT");
	cw(RDI0_SUB_PER, 1, "IRQ_SUB_PER");
	cw(RDI0_SUB_PAT, 0, "IRQ_SUB_PAT");
	cw(RDI0_PIX_DROP_PAT, 0, "PIX_DROP_PAT");
	cw(RDI0_PIX_DROP_PER, 1, "PIX_DROP_PER");
	cw(RDI0_LINE_DROP_PAT, 0, "LINE_DROP_PAT");
	cw(RDI0_LINE_DROP_PER, 1, "LINE_DROP_PER");
	cw(RDI0_CTRL, 0, "RDI0_CTRL 0");
	cw(RDI_CFG0(0), cfg0 | 1u << 31, "RDI0_CFG0 enable");
	cw(RDI0_CTRL, 1, "RDI0_CTRL resume at frame boundary");
	cw(RDI_IRQ_MASK(0), 0x7e, "RDI0_IRQ_MASK bits 1-6");
	stage("tpg start");
	uint32_t pkts0 = cr(RX_PKTS);
	cw(TPG_CTRL, 4u << 20 | 0x80u << 8 | 3u << 4 | 7u, "TPG_CTRL start (guess: fields)");	/* guess: fields */
	unsigned rdi_cnt[32] = { 0 }, rx_cnt[32] = { 0 }, top_cnt[32] = { 0 }, polls = 0;
	for (int t = 0; !dry && !stop && t < secs * 10; t++) {
		usleep(100000);
		uint32_t top = cr(TOP_IRQ_STATUS), rx = cr(RX_IRQ_STATUS), rdi = cr(RDI_IRQ_STATUS(0));
		cw_quiet(TOP_IRQ_CLEAR, top);
		cw_quiet(RX_IRQ_CLEAR, rx);
		cw_quiet(RDI_IRQ_CLEAR(0), rdi);
		cw_quiet(IRQ_CMD, 1);
		for (int b = 0; b < 32; b++) {
			top_cnt[b] += top >> b & 1;
			rx_cnt[b] += rx >> b & 1;
			rdi_cnt[b] += rdi >> b & 1;
		}
		polls++;
		if (t < 3)
			printf("camrx: poll %d: top %#x rx %#x rdi0 %#x status %#x pkts %u\n", t, top, rx, rdi,
			       cr(RDI0_STATUS), cr(RX_PKTS));
	}
	uint32_t pkts1 = cr(RX_PKTS), ecc = cr(RX_ECC), crc = cr(RX_CRC), st = cr(RDI0_STATUS);
	uint32_t ping = cr(RDI0_BYTES_PING), pong = cr(RDI0_BYTES_PONG);
	paths_down();
	printf("camrx: totals over %u polls of 100 ms (counts = polls a bit was latched in, max 1 per poll):\n", polls);
	printf("camrx:   SOF %u (INPUT_SOF %u, SUBSAMPLED_SOF %u), EOF %u (INPUT_EOF %u, SUBSAMPLED_EOF %u)\n",
	       rdi_cnt[12] + rdi_cnt[4], rdi_cnt[12], rdi_cnt[4], rdi_cnt[9] + rdi_cnt[3], rdi_cnt[9], rdi_cnt[3]);
	for (int b = 0; b < 32; b++) {
		if (rdi_cnt[b])
			printf("camrx:   rdi0 bit %d %s: %u\n", b, b < 15 && rdi_bit[b] ? rdi_bit[b] : "?", rdi_cnt[b]);
		if (rx_cnt[b])
			printf("camrx:   rx bit %d: %u\n", b, rx_cnt[b]);
		if (top_cnt[b])
			printf("camrx:   top bit %d: %u\n", b, top_cnt[b]);
	}
	printf("camrx:   RX_PKTS %u -> %u, ECC %#x, CRC %u, RDI0_STATUS %#x, bytes ping/pong %u/%u, HW_VERSION %#x\n",
	       pkts0, pkts1, ecc, crc, st, ping, pong, cr(HW_VERSION));
	return 0;
}

int main(int argc, char **argv)
{
	int a = 1;
	if (a < argc && !strcmp(argv[a], "--dry-run")) {
		dry = 1;
		a++;
	}
	if (a >= argc)
		goto usage;
	pm_init();
	if (!strcmp(argv[a], "status"))
		return status();
	if (precheck())
		return 1;
	if (!strcmp(argv[a], "clocks") && a + 2 == argc) {
		if (!strcmp(argv[a + 1], "on"))
			return clocks_on();
		if (!strcmp(argv[a + 1], "off"))
			return clocks_off();
	}
	if (!strcmp(argv[a], "tpg") && a + 2 >= argc) {
		int s = a + 1 < argc ? atoi(argv[a + 1]) : 1;
		if (s < 1 || s > 60)
			goto usage;
		return tpg(s);
	}
usage:
	fprintf(stderr, "usage: camrx [--dry-run] status | clocks on|off | tpg [SECONDS 1-60]\n");
	return 2;
}
