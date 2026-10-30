/*
 * camrx: camera stage c1, the CSID-lite 0 test pattern generator (no PHY, no IFE DMA, no SMMU). Register spec:
 * research/notes/camera-stage-c.md (tags redfin/DT/Linux/guess), sequences from ios-redfin 0921 (csid_reset,
 * csid_config, paths_down).
 *   camrx [--dry-run] status        read-only: PLL2, GDSC, the RCGs/CBCRs below, refgen, CSID-lite (if clocked)
 *   camrx [--dry-run] clocks on|off cphy_rx 384, ife_lite_csid 384, ife_lite 320 MHz (all PLL2) + their branches
 *   camrx [--dry-run] tpg [SECONDS] CSID reset, TPG 640x480 RAW10 colour bars into RDI0, poll IRQ status every 100 ms
 * Stage c2 (sensor data through CSIPHY0; Linux excerpts: research/notes/camera-stage-c2-linux.md):
 *   camrx [--dry-run] phy on [SETTLE]  csi0phytimer 300 MHz (PLL0 even /2; needs camclk pll0 on) + csiphy0/timer
 *                                      branches, CSIPHY0 v1.2 4-lane D-PHY init (redfin 0922 csiphy_up)
 *   camrx [--dry-run] phy off          CSIPHY0 IRQ masks 0 + reset (redfin csiphy_down), then the two branches off
 *   camrx [--dry-run] rx arm           CSID reset, RX from CSIPHY0 (4 lanes), RDI0 VC 0 DT 0x2b; left running
 *   camrx rx report                    read-only: RX/RDI0 counters + IRQ status (not cleared), CSIPHY0 status
 *   camrx [--dry-run] rx stop          RDI0 off, RX off, masks 0
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
#define PLL0_MODE	(CAMCC + 0x0)	/* Fabia (camclk.c); read only here */
#define PLL0_USER	(CAMCC + 0xc)
#define PLL0_OPMODE	(CAMCC + 0x2c)
#define TITAN_GDSC	0xad0c1c4u
#define LOCK_DET	(1u << 31)
#define CBCR_CLK_OFF	(1u << 31)

/* RCG CMD addresses (CFG = +4) and branches (notes section 1, Linux camcc-sm7150.c) */
#define CPHY_RX_RCG	0xad0a064u
#define LITE_CSID_RCG	0xad0c020u
#define LITE_RCG	0xad0c004u
#define PHYTIMER0_RCG	0xad06004u	/* stage c2: written by phy on */
#define CSIPHY0_CBCR	0xad06020u	/* camcc-sm7150.c:1189 (parent cphy_rx) */
#define PHYTIMER0_CBCR	0xad0601cu	/* camcc-sm7150.c:1117 */
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

/* CSIPHY0 (DT "qcom,csiphy-v1.2" @0xace0000; redfin maps 0x1000, the common block is at +0x800) */
#define CSIPHY		0xace0000u
#define CSIPHY_END	(CSIPHY + 0x1000)
#define PHY_LANES	4
#define PHY_LANE_MAP	0x3210u	/* guess: redfin's fallback; sunfish laneAssign not yet read from metric_imx363.bin */
#define LINK_MBPS	304	/* IMX363 mode 8, per lane (camera.md cam6) */
#define TIMER_MHZ	300

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
		PHYTIMER0_RCG, PHYTIMER0_RCG + 4, CSIPHY0_CBCR, PHYTIMER0_CBCR,	/* stage c2 */
	};
	for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; i++)
		if (pa == ok[i])
			return 1;
	if (pa >= CSIPHY && pa < CSIPHY_END && !(pa & 3))	/* stage c2 */
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

static int phy_clocked(void)
{
	return branch_on(CSIPHY0_CBCR) && branch_on(PHYTIMER0_CBCR);
}

static void pw(uint32_t off, uint32_t v, const char *why)	/* CSIPHY0 write */
{
	wr(CSIPHY + off, v, why);
}

static void msleep_(unsigned ms)
{
	if (!dry)
		usleep(ms * 1000);
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
	m = rd32(PLL0_MODE);
	printf("pll0 MODE %#010x: lock %u outctrl %u, OPMODE %#x, USER_CTL %#x\n", m, m >> 31, m & 1, rd32(PLL0_OPMODE),
	       rd32(PLL0_USER));
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
	if (phy_clocked())
		printf("csiphy0 common ctrl5 (lanes) %#x ctrl6 %#x; status 0x8b0.. %02x %02x %02x %02x %02x %02x %02x %02x "
		       "%02x %02x %02x\n", rd32(CSIPHY + 0x814), rd32(CSIPHY + 0x818), rd32(CSIPHY + 0x8b0) & 0xff,
		       rd32(CSIPHY + 0x8b4) & 0xff, rd32(CSIPHY + 0x8b8) & 0xff, rd32(CSIPHY + 0x8bc) & 0xff,
		       rd32(CSIPHY + 0x8c0) & 0xff, rd32(CSIPHY + 0x8c4) & 0xff, rd32(CSIPHY + 0x8c8) & 0xff,
		       rd32(CSIPHY + 0x8cc) & 0xff, rd32(CSIPHY + 0x8d0) & 0xff, rd32(CSIPHY + 0x8d4) & 0xff,
		       rd32(CSIPHY + 0x8d8) & 0xff);
	else
		printf("csiphy0: csiphy0/csi0phytimer branches off, not read\n");
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

/* RDI0 for RAW10 on VC vc, no write master (redfin 0921 csid_config) */
static void rdi0_config(uint32_t vc)
{
	stage("rdi0 config (no write master)");
	uint32_t cfg0 = vc << 22 | RAW10 << 16 | 0xfu << 12 | 1u << 2 | 3u;
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
}

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
	rdi0_config(TPG_VC);
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

/* ---- stage c2: CSIPHY0 + CSID RX from the sensor ---- */

/* csiphy_settle_cnt_calc (Linux v6.15 camss-csiphy-3ph-1-0.c:500-520): link_freq = DDR clock = bit rate / 2;
 * ui = 1e12 / link_freq / 2 ps; t_hs_settle = 85 ns + 6 UI; cnt = t / timer period - 6 (u8) */
static unsigned settle_calc(unsigned mbps, unsigned timer_mhz)
{
	uint64_t link = (uint64_t)mbps * 500000u;
	uint32_t ui = (uint32_t)(1000000000000ull / link) / 2;
	uint32_t t = 85000 + 6 * ui, period = (uint32_t)(1000000000000ull / ((uint64_t)timer_mhz * 1000000u));
	printf("camrx: settle: %u Mb/s/lane -> link %llu Hz, ui %u ps, t_hs_settle %u ps; timer %u MHz period %u ps; "
	       "cnt = %u/%u - 6 = %u\n", mbps, (unsigned long long)link, ui, t, timer_mhz, period, t, period,
	       (uint8_t)(t / period - 6));
	return (uint8_t)(t / period - 6);
}

/* redfin csiphy_reset (0922) */
static void csiphy_reset(void)
{
	pw(0x814, 0x00, "CTRL5 lanes off");
	msleep_(5);
	pw(0x818, 0x00, "CTRL6 0");
	pw(0x81c, 0x00, "CTRL7 0");
	pw(0x800, 0x01, "CTRL0 reset");
	msleep_(1);
	pw(0x800, 0x00, "CTRL0 reset release");
}

static void show_csiphy(void)
{
	printf("camrx: csiphy0 status 0x8b0..0x8d8:");
	for (unsigned i = 0; i < 11; i++)
		printf(" %02x", dry ? 0 : rd32(CSIPHY + 0x8b0 + 4 * i) & 0xff);
	printf("  (redfin show_csiphy; bit meanings unknown)\n");
}

static int phy_on(int settle_arg)
{
	uint32_t m = rd32(PLL0_MODE), u = rd32(PLL0_USER);
	if (!dry && (!(m & LOCK_DET) || !(m & 1) || !(u & 2))) {
		printf("camrx: pll0 MODE %#x USER_CTL %#x not locked / even out off: run 'camclk pll0 on' first\n", m, u);
		return 1;
	}
	if (!dry && !csid_clocked()) {	/* csiphy0's parent is cphy_rx, set by 'clocks on' */
		printf("camrx: run 'camrx clocks on' first (cphy_rx is the csiphy0 branch's parent)\n");
		return 1;
	}
	/* guess: refgen offsets recalled (notes 2); read only, not a precondition */
	printf("camrx: refgen BIAS_EN %#x BG_CTRL %#x (read only; offsets guess, never written)\n", rd32(REFGEN_BIAS_EN),
	       rd32(REFGEN_BG_CTRL));
	unsigned settle = settle_arg >= 0 ? (unsigned)settle_arg : settle_calc(LINK_MBPS, TIMER_MHZ);
	printf("camrx: settle count used: %u (%#x)%s\n", settle, settle, settle_arg >= 0 ? " (from the command line)" : "");
	printf("camrx: note: lane map %#x is a guess (redfin fallback); CSIPHY values are redfin 0922's v1.2 table\n",
	       PHY_LANE_MAP);
	stage("csi0phytimer rcg 300 MHz (PLL0 even 600 / 2)");
	/* ftbl F(300000000, P_CAMCC_PLL0_OUT_EVEN, 2, 0, 0) (camcc-sm7150.c:501): div field 2*2-1 = 3, src 2 (map_0) */
	wr(PHYTIMER0_RCG + 4, (rd32(PHYTIMER0_RCG + 4) & ~0x371fu) | 0x203, "csi0phytimer CFG src 2 div 3");
	wr(PHYTIMER0_RCG, rd32(PHYTIMER0_RCG) | 1, "CMD UPDATE");
	if (poll(PHYTIMER0_RCG, 1, 0, 500, 1000)) {
		printf("camrx: csi0phytimer UPDATE stuck (CMD %#x), branches not enabled\n", rd32(PHYTIMER0_RCG));
		return 1;
	}
	stage("csiphy0 + csi0phytimer branches");
	static const struct { const char *n; uint32_t cbcr; } pb[] = {
		{ "csi0phytimer", PHYTIMER0_CBCR }, { "csiphy0", CSIPHY0_CBCR },
	};
	for (unsigned i = 0; i < N(pb); i++) {
		wr(pb[i].cbcr, rd32(pb[i].cbcr) | 1, pb[i].n);
		if (poll(pb[i].cbcr, CBCR_CLK_OFF, 0, 200, 1000)) {
			printf("camrx: %s stuck off (CBCR %#x), no CSIPHY access\n", pb[i].n, rd32(pb[i].cbcr));
			return 1;
		}
	}
	/* redfin csiphy_up: positions data 0, clock, data 1, 2, 3; bases below; enable bit 2n per data lane + 0x80 */
	static const uint8_t pos_of_lane[4] = { 0, 2, 3, 4 };
	static const uint16_t pos_base[5] = { 0x000, 0x700, 0x200, 0x400, 0x600 };
	static const uint8_t irq_mask[11] = { 0xff, 0xff, 0xfb, 0xff, 0x7f, 0xff, 0xff, 0xef, 0xff, 0xff, 0xff };
	uint32_t pos_mask = 1u << 1, lane_enable = 0x80, la = PHY_LANE_MAP;
	for (unsigned l = 0; l < PHY_LANES; l++, la >>= 4) {
		pos_mask |= 1u << pos_of_lane[la & 3];
		lane_enable |= 1u << (2 * (la & 3));
	}
	printf("camrx: positions %#x, lane enable %#x\n", pos_mask, lane_enable);
	stage("csiphy0 reset");
	csiphy_reset();
	stage("csiphy0 common (csiphy_common_reg_1_2)");
	pw(0x814, lane_enable, "CTRL5 lane enable");
	pw(0x818, 0x01, "CTRL6 PWRDN_B");
	pw(0x81c, 0x5a, "CTRL7 0x5a (Linux camss writes 0x02)");
	pw(0x800, 0x03, "CTRL0 0x3");
	msleep_(1);
	pw(0x800, 0x02, "CTRL0 0x2");
	pw(0x884, 0x01, "CTRL33 1");
	pw(0x824, 0x72, "CTRL9 0x72");
	stage("csiphy0 lanes (csiphy_2ph_v1_2_reg)");
	for (unsigned pos = 0; pos < 5; pos++) {
		if (!(pos_mask & (1u << pos)))
			continue;
		uint32_t b = pos_base[pos];
		int clk = pos == 1;
		printf("camrx: lane base %#x (%s)\n", b, clk ? "clock" : "data");
		pw(b + 0x30, 0x00, "CFG7");
		pw(b + 0x2c, 0x01, "CFG6");
		pw(b + 0x34, 0x0f, "CFG8");
		pw(b + 0x10, 0x52, "CFG5");
		pw(b + 0x1c, 0x0a, "TEST_IMP");
		pw(b + 0x14, 0x60, "0x14");
		if (clk)
			pw(b + 0x28, 0x04, "MISC1 IS_CLKLANE");
		pw(b + 0x3c, 0xb8, "LANE_CTRL15 SOT symbol");
		pw(b + 0x00, clk ? 0x80 : 0x91, "CFG1");
		pw(b + 0x04, 0x0c, "CFG2");
		pw(b + 0x20, 0x00, "0x20");
		pw(b + 0x24, 0x00, "0x24");
		pw(b + 0x08, settle & 0xff, "CFG3 settle count");
		if (clk) {
			pw(b + 0x0c, 0xff, "CFG4");
			pw(b + 0x38, 0x1f, "CFG9");
		} else {
			pw(b + 0x38, 0xfe, "CFG9");
			pw(b + 0x5c, 0xc0, "0x5c");
			pw(b + 0x60, 0x0d, "0x60");
		}
	}
	stage("csiphy0 irq masks + clear (csiphy_irq_reg_1_2)");
	for (unsigned i = 0; i < 11; i++) {
		pw(0x82c + 4 * i, irq_mask[i], "IRQ mask");
		pw(0x858 + 4 * i, 0xff, "IRQ clear");
	}
	pw(0x828, 1, "CTRL10 latch clear");
	pw(0x828, 0, "CTRL10 0");
	show_csiphy();
	return 0;
}

static int phy_off(void)
{
	if (dry || phy_clocked()) {
		stage("csiphy0 down: irq masks 0, reset");
		for (unsigned i = 0; i < 11; i++)
			pw(0x82c + 4 * i, 0, "IRQ mask 0");
		csiphy_reset();
	} else {
		printf("camrx: csiphy0 branches already off, CSIPHY not touched\n");
	}
	stage("csiphy0 + csi0phytimer branches off");
	static const uint32_t cb[] = { CSIPHY0_CBCR, PHYTIMER0_CBCR };
	for (unsigned i = 0; i < N(cb); i++) {
		wr(cb[i], rd32(cb[i]) & ~1u, i ? "csi0phytimer off" : "csiphy0 off");
		if (poll(cb[i], CBCR_CLK_OFF, CBCR_CLK_OFF, 200, 1000))
			printf("camrx: CBCR %#x CLK_OFF not set (%#x)\n", cb[i], rd32(cb[i]));
	}
	return 0;
}

static int rx_arm(void)
{
	if (!dry && (!csid_clocked() || !phy_clocked())) {
		printf("camrx: run 'camrx clocks on' and 'camrx phy on' first\n");
		return 1;
	}
	printf("camrx: csid-lite HW_VERSION %#x; lane map %#x is a guess\n", cr(HW_VERSION), PHY_LANE_MAP);
	stage("csid reset");
	if (csid_reset())
		return 1;
	stage("csid rx from csiphy0 (4 lanes)");
	cw(RX_CFG0, (PHY_LANES - 1) | PHY_LANE_MAP << 4 | 0u << 20, "RX_CFG0 4 lanes, lane map, phy 0");
	cw(RX_CFG1, 1u << 6 | 1u, "RX_CFG1 ECC correction + MISR (0x41)");
	rdi0_config(0);
	printf("camrx: armed; RX_PKTS %u. Start the sensor stream now, then 'camrx rx report'\n", cr(RX_PKTS));
	return 0;
}

static const char *rx_bit[25] = {	/* msm-4.14 cam_ife_csid_core.h, recall (guess) */
	[0] = "DL0_EOT", [1] = "DL1_EOT", [2] = "DL2_EOT", [3] = "DL3_EOT", [4] = "DL0_SOT", [5] = "DL1_SOT",
	[6] = "DL2_SOT", [7] = "DL3_SOT", [8] = "LONG_PKT", [9] = "SHORT_PKT", [15] = "DL0_FIFO_OVF",
	[16] = "DL1_FIFO_OVF", [17] = "DL2_FIFO_OVF", [18] = "DL3_FIFO_OVF", [19] = "CRC_ERR", [20] = "ECC_ERR",
	[21] = "MMAPPED_VC_DT", [22] = "UNMAPPED_VC_DT", [23] = "STREAM_UNDERFLOW", [24] = "UNBOUNDED_FRAME",
};

static void decode(const char *what, uint32_t v, const char *const *names, unsigned n)
{
	printf("camrx:   %s %#010x:", what, v);
	for (unsigned b = 0; b < 32; b++)
		if (v >> b & 1)
			printf(" %u%s%s", b, b < n && names[b] ? "=" : "", b < n && names[b] ? names[b] : "");
	printf("\n");
}

/* read only: nothing is cleared, so the counts are since 'rx arm' */
static int rx_report(void)
{
	if (!csid_clocked()) {
		printf("camrx: csid branches off, not read\n");
		return 1;
	}
	printf("camrx: RX_PKTS %u, ECC %#x, CRC %u, RDI0_STATUS %#x, bytes ping/pong %u/%u, RX_CFG0 %#x RDI0_CFG0 %#x\n",
	       rd32(CSID + RX_PKTS), rd32(CSID + RX_ECC), rd32(CSID + RX_CRC), rd32(CSID + RDI0_STATUS),
	       rd32(CSID + RDI0_BYTES_PING), rd32(CSID + RDI0_BYTES_PONG), rd32(CSID + RX_CFG0), rd32(CSID + RDI_CFG0(0)));
	printf("camrx: irq status (latched since arm; bit names are recall/guess):\n");
	decode("top", rd32(CSID + TOP_IRQ_STATUS), NULL, 0);
	decode("rx ", rd32(CSID + RX_IRQ_STATUS), rx_bit, N(rx_bit));
	decode("rdi0", rd32(CSID + RDI_IRQ_STATUS(0)), rdi_bit, N(rdi_bit));
	if (phy_clocked())
		show_csiphy();
	else
		printf("camrx: csiphy0 branches off, PHY status not read\n");
	return 0;
}

static int rx_stop(void)
{
	if (!dry && !csid_clocked()) {
		printf("camrx: csid branches off, nothing to stop\n");
		return 1;
	}
	paths_down();
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
	if (!strcmp(argv[a], "phy") && a + 2 <= argc && a + 3 >= argc) {
		if (!strcmp(argv[a + 1], "on")) {
			int st = a + 2 < argc ? atoi(argv[a + 2]) : -1;
			if (a + 2 < argc && (st < 0 || st > 255))
				goto usage;
			return phy_on(st);
		}
		if (!strcmp(argv[a + 1], "off") && a + 2 == argc)
			return phy_off();
	}
	if (!strcmp(argv[a], "rx") && a + 2 == argc) {
		if (!strcmp(argv[a + 1], "arm"))
			return rx_arm();
		if (!strcmp(argv[a + 1], "report"))
			return rx_report();
		if (!strcmp(argv[a + 1], "stop"))
			return rx_stop();
	}
	if (!strcmp(argv[a], "tpg") && a + 2 >= argc) {
		int s = a + 1 < argc ? atoi(argv[a + 1]) : 1;
		if (s < 1 || s > 60)
			goto usage;
		return tpg(s);
	}
usage:
	fprintf(stderr, "usage: camrx [--dry-run] status | clocks on|off | tpg [SECONDS 1-60] | phy on [SETTLE 0-255] | "
			"phy off | rx arm|report|stop\n");
	return 2;
}
