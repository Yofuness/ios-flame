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
 * Stage d (one frame through IFE-lite RDI0 WM0 into RAM; spec research/notes/camera-stage-d.md):
 *   camrx [--dry-run] capture OUTFILE [--stream-cmd "CMD"] [--alloc32]  one process for the whole DMA: buffer =
 *       display carveout slot1 PA 0x9cb00000 + 0xB00000 (zero-copy slot 1, unused without sweetfb_zerocopy; XNU has
 *       no RAM below 4 GB, cam10), zeroed through /dev/pmem first; --alloc32: 8 MB PMEM_ALLOC32 instead. SMMU (cam13): --trap = the 3 DT IFE SMRs
 *       (0x900/0x460 0x880/0x460 0x820/0x440) into one free bank with M=1 + EPD0|EPD1 (every access faults, the
 *       bank reports the SID), --bypass --sid X = one mask-0 SMR into an identity bank + (cam16) the other DT IFE
 *       SIDs (non-overlapping SMRs) into a trap bank; refused if any valid SMR overlaps (saved + restored on
 *       every exit), CSID rx arm, IFE reset + WM0 frame
 *       mode, runs CMD (default: camid mode 8 stream), WM0 off at the first buf done, readbacks, guard check
 *       (+ where), frame -> OUTFILE (fsync'd), cleanup: WM0 off, CSID down, IFE reset, identity SMR -> trap bank,
 *       2 s, late-write check, SMMU restore, PMEM_FREE (--alloc32 only). Needs clocks on + phy on (+ camid's cpas_ahb/camnoc_axi).
 * Needs (checked, read-only): gcc camera_ahb on, TITAN_TOP GDSC on (camid --keep), PLL2 locked (camclk pll2 on).
 *   camrx smmu [SID...]   read-only: sCR0, valid SMRs/S2CRs, their banks, faults; which SMRs each SID (default: the
 *                         14 DT IFE SIDs) matches
 * Never enables PLLs/GDSCs/regulators, never writes GCC/RPMh/refgen. Writes only the whitelist in allowed();
 * every write is logged with its read-back. No camcc register is read before the GDSC check passes, no CSID
 * register before its two branches run.
 */
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#include "../wifi/pmem.h"
#include "../wifi/smmu.h"

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
#define CPAS_AHB_CBCR	0xad0c124u	/* stage d: read only (camid turns it on) */
#define CAMNOC_CBCR	0xad0c148u	/* stage d: read only (camid turns it on) */
#define CAMNOC_RCG	0xad0c12cu	/* stage d: set to 150 MHz if still at XO */
#define CAMNOC_CFG_150	0x207u		/* ftbl F(150 MHz, PLL0 even 600, /4) (camcc-sm7150.c:421): src 2, div 7 (redfin) */

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

/* IFE-lite 0 (DT vfe-lite0 reg 0xacc4000 0x4000; offsets redfin 0921 / Linux cam_vfe_lite17x, notes section 1) */
#define IFE		0xacc4000u
#define IFE_END		(IFE + 0x4000)
#define IFE_HW_VERSION	0x000
#define IFE_GLOBAL_RESET 0x018
#define IFE_IRQ_GCLEAR	0x058
#define IFE_IRQ_MASK0	0x05c
#define IFE_IRQ_CLEAR0	0x064
#define IFE_IRQ_CLEAR1	0x068
#define IFE_IRQ_STATUS0	0x06c
#define IFE_IRQ_STATUS1	0x070
#define IFE_VIOLATION	0x07c
#define IFE_REG_UPDATE	0x4ac
#define BUS_HW_VERSION	0x2000
#define BUS_CGC_OVD	0x200c
#define BUS_IRQ_MASK1	0x2048
#define BUS_IRQ_CLEAR1	0x2054
#define BUS_IRQ_STATUS(n) (0x205c + 4 * (n))
#define BUS_IRQ_GCLEAR	0x2068
#define BUS_COMP_ERROR	0x206c
#define BUS_SYNC_FRAME_HDR 0x2080
#define BUS_SYNC_NO_SYNC 0x2084
#define BUS_TEST_CTRL	0x211c
#define WM0_STATUS0	0x2200
#define WM0_STATUS1	0x2204
#define WM0_CFG		0x2208
#define WM0_IMAGE_ADDR	0x2214
#define WM0_WIDTH	0x221c
#define WM0_HEIGHT	0x2220
#define WM0_PACKER	0x2224
#define WM0_STRIDE	0x2228
#define WM0_SUB_PERIOD	0x2248
#define WM0_SUB_PATTERN	0x224c
#define WM0_DROP_PERIOD	0x2250
#define WM0_DROP_PATTERN 0x2254
#define WM0_FRAME_INC	0x2258
#define WM0_BURST_LIMIT	0x225c

#define FRAME_BYTES	3810240u	/* 2016 x 1512 RAW10 packed = CSID bytes ping/pong in cam8 */
#define CAP_BUF		0x800000u	/* --alloc32: frame + guard (must stay zero) */
#define CAP_CO_PA	0x9cb00000ull	/* display carveout zero-copy slot 1 (SweetSurface SWEET_ZC_SLOT_PA(1)) */
#define CAP_CO_LEN	0xB00000u	/* frame + guard */
#define STREAM_CMD	"/var/local/bin/camid --addr 0x1a --keep --mclk-asis --stream /var/local/cam/imx363-mode8.txt"

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
		CAMNOC_RCG, CAMNOC_RCG + 4,	/* stage d */
	};
	for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; i++)
		if (pa == ok[i])
			return 1;
	if (pa >= CSIPHY && pa < CSIPHY_END && !(pa & 3))	/* stage c2 */
		return 1;
	if (pa >= IFE && pa < IFE_END && !(pa & 3))	/* stage d */
		return 1;
	return pa >= CSID && pa < CSID_END && !(pa & 3);
}

static void wr(uint32_t pa, uint32_t v, const char *why)
{
	if (!allowed(pa)) {
		printf("camrx: BLOCKED wr %#x = %#x (%s)\n", pa, v, why);
		exit(1);	/* capture: its atexit handler stops the WM and frees */
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

/* ---- stage d: the camera's own SMMU mapping (cam12: SMR5 0x800/0x440 already matched 0x800, nothing was written,
 * the IFE wrote somewhere else and the phone reset). Apps SMMU v2 at 0x15000000; layout as phone/wifi/smmu.c. ---- */

#define SMMU		0x15000000ull
#define SMMU_SIZE	0x00100000u
#define SM_SCR0		0x0
#define SM_GFSR		0x48
#define SM_TLBGSYNC	0x70
#define SM_TLBGSTATUS	0x74
#define SM_SMR(n)	(0x800 + 4 * (n))
#define SM_S2CR(n)	(0xc00 + 4 * (n))
#define CB_SCTLR_	0x00
#define CB_RESUME	0x08
#define CB_TCR2		0x10
#define CB_TTBR0	0x20
#define CB_TTBR1	0x28
#define CB_TCR		0x30
#define CB_MAIR0	0x38
#define CB_MAIR1	0x3c
#define CB_FSR		0x58
#define CB_FAR		0x60
#define CB_FSYNR0	0x68
#define CB_TLBIALL	0x618
#define CB_TLBSYNC	0x7f0
#define CB_TLBSTATUS	0x7f4
#define TRAP_SCTLR	0x00000021u	/* M | CFRE; CFIE 0 (no IRQ), CFCFG 0 (terminate, no stall) */
#define TRAP_TCR	(25u | 1u << 7 | 1u << 23)	/* T0SZ 25, EPD0 | EPD1: no walks, every access a translation fault */
#define BYP_SCTLR	0x000000e0u	/* as smmu.c CB_SCTLR: CFCFG | CFIE | CFRE, M = 0 (identity) */
#define CAM_CBAR	0x0001f000u	/* as smmu.c CB_CBAR: S1 translation, S2 bypass */
#define CAM_CBA2R	0x00000001u	/* AArch64 */

/* DT msm_cam_smmu_ife (../recon/live.dts:13150): the SIDs and the three SMRs that cover them */
static const uint16_t ife_sids[14] = { 0x900, 0xd00, 0x880, 0xc80, 0x820, 0xc20, 0x920, 0xd20, 0x8a0, 0xca0, 0x940,
				       0xd40, 0x8c0, 0xcc0 };
static const uint16_t ife_smr[3][2] = { { 0x900, 0x460 }, { 0x880, 0x460 }, { 0x820, 0x440 } };

static struct {
	uint32_t nsmr, ncb, psize, npage;
} sg;

static void smmu_geom(void)
{
	uint32_t id0 = rd32(SMMU + 0x20), id1 = rd32(SMMU + 0x24);
	sg.nsmr = id0 & 0xff;
	sg.ncb = id1 & 0xff;
	sg.psize = (id1 >> 31) ? 0x10000 : 0x1000;
	sg.npage = 1u << (((id1 >> 28) & 7) + 1);
	if (sg.nsmr > 128)
		sg.nsmr = 128;
}

static uint64_t gr1(void)
{
	return SMMU + sg.psize;
}

static uint64_t cbpage(uint32_t cb)
{
	return SMMU + (uint64_t)sg.npage * sg.psize + (uint64_t)cb * sg.psize;
}

/* two SMRs (or an SMR and a SID with mask 0) match a common SID */
static int smr_overlap(uint32_t id1, uint32_t m1, uint32_t id2, uint32_t m2)
{
	return ((id1 ^ id2) & ~(m1 | m2) & 0x7fff) == 0;
}

static void show_scr0(uint32_t v)
{
	printf("smmu: sCR0 %08x: CLIENTPD %u GFRE %u GFIE %u EXIDENABLE %u GCFGFRE %u GCFGFIE %u STALLD %u GSE %u "
	       "USFCFG %u VMIDPNE %u PTM %u FB %u SMCFCFG %u NSCFG %u\n", v, v & 1, (v >> 1) & 1, (v >> 2) & 1,
	       (v >> 3) & 1, (v >> 4) & 1, (v >> 5) & 1, (v >> 8) & 1, (v >> 9) & 1, (v >> 10) & 1, (v >> 11) & 1,
	       (v >> 12) & 1, (v >> 13) & 1, (v >> 21) & 1, (v >> 28) & 3);
	printf("smmu: unmatched streams: %s\n", v & 1 ? "CLIENTPD=1, the whole SMMU is bypassed" :
	       (v >> 10) & 1 ? "USFCFG=1: fault (terminated, sGFSR.USF)" : "USFCFG=0: bypass (untranslated, PA)");
	if ((v >> 3) & 1)
		printf("smmu: EXIDENABLE=1: SMR valid bit is S2CR.EXIDVALID (bit 10), not SMR bit 31 (this dump assumes "
		       "bit 31; guess)\n");
}

static const char *s2cr_type[4] = { "translate", "bypass", "fault", "res" };

static void show_cb(uint32_t cb)
{
	uint64_t p = cbpage(cb), g = gr1();
	uint32_t sc = rd32(p), cbar = rd32(g + 4 * cb), fsr = rd32(p + CB_FSR);
	printf("smmu:  CB%u SCTLR %08x (M %u TRE %u AFE %u CFRE %u CFIE %u CFCFG %u) CBAR %08x (TYPE %u VMID %#x "
	       "IRPTNDX %u) CBA2R %08x TCR %08x TCR2 %08x TTBR0 %08x%08x TTBR1 %08x%08x\n", cb, sc, sc & 1,
	       (sc >> 1) & 1, (sc >> 2) & 1, (sc >> 5) & 1, (sc >> 6) & 1, (sc >> 7) & 1, cbar, (cbar >> 16) & 3,
	       cbar & 0xff, cbar >> 24, rd32(g + 0x800 + 4 * cb), rd32(p + CB_TCR), rd32(p + CB_TCR2),
	       rd32(p + CB_TTBR0 + 4), rd32(p + CB_TTBR0), rd32(p + CB_TTBR1 + 4), rd32(p + CB_TTBR1));
	printf("smmu:  CB%u FSR %08x (%s) FAR %08x%08x FSYNR0 %08x CBFRSYNRA %08x\n", cb, fsr,
	       fsr & 0xc00001feu ? "FAULT" : "no fault", rd32(p + CB_FAR + 4), rd32(p + CB_FAR), rd32(p + CB_FSYNR0),
	       rd32(g + 0x400 + 4 * cb));
}

/* camrx smmu [SID...]: read-only, no writes at all */
static int smmu_report(int n, char **sids)
{
	smmu_geom();
	uint32_t scr0 = rd32(SMMU + SM_SCR0);
	printf("smmu: %u SMRs, %u context banks, page %#x x %u; sGFSR %08x sGFSYNR0 %08x sGFSYNR1 %08x\n", sg.nsmr,
	       sg.ncb, sg.psize, sg.npage, rd32(SMMU + SM_GFSR), rd32(SMMU + 0x50), rd32(SMMU + 0x54));
	show_scr0(scr0);
	uint64_t shown[4] = { 0 };
	for (uint32_t i = 0; i < sg.nsmr; i++) {
		uint32_t smr = rd32(SMMU + SM_SMR(i)), s2 = rd32(SMMU + SM_S2CR(i));
		if (!(smr >> 31))
			continue;
		uint32_t t = (s2 >> 16) & 3, cb = s2 & 0xff;
		printf("smmu: SMR%u %08x id %#x mask %#x -> S2CR %08x TYPE %s CBNDX %u PRIVCFG %u INSTCFG %u SHCFG %u "
		       "NSCFG %u\n", i, smr, smr & 0x7fff, (smr >> 16) & 0x7fff, s2, s2cr_type[t], cb, (s2 >> 24) & 3,
		       (s2 >> 26) & 3, (s2 >> 8) & 3, (s2 >> 18) & 3);
		if (t == 0 && cb < sg.ncb && !(shown[cb / 64] & (1ull << (cb % 64)))) {
			shown[cb / 64] |= 1ull << (cb % 64);
			show_cb(cb);
		}
	}
	uint16_t list[64];
	unsigned nl = 0;
	for (int k = 0; k < n && nl < 64; k++)
		list[nl++] = (uint16_t)(strtoul(sids[k], NULL, 0) & 0x7fff);
	if (!nl)
		for (; nl < 14; nl++)
			list[nl] = ife_sids[nl];
	for (unsigned k = 0; k < nl; k++) {
		int hits = 0, first = -1;
		uint32_t s2 = 0;
		for (uint32_t i = 0; i < sg.nsmr; i++) {
			uint32_t smr = rd32(SMMU + SM_SMR(i));
			if ((smr >> 31) && smr_overlap(smr & 0x7fff, (smr >> 16) & 0x7fff, list[k], 0)) {
				if (first < 0) {
					first = (int)i;
					s2 = rd32(SMMU + SM_S2CR(i));
				}
				hits++;
			}
		}
		if (!hits)
			printf("smmu: SID %#x: no SMR -> %s\n", list[k], scr0 & 1 ? "bypass (CLIENTPD)" :
			       (scr0 >> 10) & 1 ? "unmatched: fault" : "unmatched: bypass");
		else if (hits == 1)
			printf("smmu: SID %#x: SMR%d -> %s CB%u\n", list[k], first, s2cr_type[(s2 >> 16) & 3], s2 & 0xff);
		else
			printf("smmu: SID %#x: %d SMRs match (first SMR%d): CONFLICT (multi-match fault, sGFSR.SMCF if "
			       "SMCFCFG)\n", list[k], hits, first);
	}
	return 0;
}

/* what capture installed; cam_smmu_down puts it back. cam16: up to 2 banks (bank[0] = the SMR's own bank: trap
 * in --trap mode, identity in --bypass; trap = the faulting bank, == bank[0] in --trap mode) */
#define CS_MAXSMR	16
struct cbsave {
	uint32_t sctlr, cbar, cba2r, tcr, tcr2, ttbr0[2], ttbr1[2], mair[2];
};
static struct {
	int on, nb, bank[2], trap, nslot, slot[CS_MAXSMR], bk[CS_MAXSMR];
	uint32_t smr[CS_MAXSMR], s2cr[CS_MAXSMR];
	struct cbsave sv[2];
} cs = { .bank = { -1, -1 }, .trap = -1 };

/* SMMU whitelist: only the SMR/S2CR slots and the banks capture allocated, plus sTLBGSYNC */
static int smmu_allowed(uint64_t pa)
{
	if (pa == SMMU + SM_TLBGSYNC)
		return 1;
	for (int k = 0; k < cs.nslot; k++)
		if (pa == SMMU + SM_SMR(cs.slot[k]) || pa == SMMU + SM_S2CR(cs.slot[k]))
			return 1;
	for (int b = 0; b < 2; b++) {
		if (cs.bank[b] < 0)
			continue;
		if (pa == gr1() + 4 * cs.bank[b] || pa == gr1() + 0x800 + 4 * cs.bank[b])
			return 1;
		uint64_t p = cbpage((uint32_t)cs.bank[b]);
		if (pa >= p && pa < p + 0x1000 && !(pa & 3))
			return 1;
	}
	return 0;
}

static void sw(uint64_t pa, uint32_t v, const char *why)
{
	if (!smmu_allowed(pa)) {
		printf("camrx: BLOCKED smmu wr %#llx = %#x (%s)\n", (unsigned long long)pa, v, why);
		exit(1);
	}
	if (dry) {
		printf("[dry] smmu wr %#llx = %#010x  [%s]\n", (unsigned long long)pa, v, why);
		return;
	}
	uint32_t old = rd32(pa);
	wr32(pa, v);
	printf("camrx: smmu wr %#llx %#010x -> %#010x, reads %#010x  [%s]\n", (unsigned long long)pa, old, v, rd32(pa),
	       why);
}

static void smmu_sync(void)
{
	sw(SMMU + SM_TLBGSYNC, 0, "sTLBGSYNC");
	for (int i = 0; !dry && i < 1000 && (rd32(SMMU + SM_TLBGSTATUS) & 1); i++)
		usleep(1000);
}

/* one bank: save, then TRAP (M=1, both walks disabled: every access faults, nothing written) or M=0 identity */
static void cb_setup(int b, int trap)
{
	int cb = cs.bank[b];
	uint64_t p = cbpage((uint32_t)cb), g = gr1();
	struct cbsave *s = &cs.sv[b];
	s->sctlr = rd32(p);
	s->cbar = rd32(g + 4 * cb);
	s->cba2r = rd32(g + 0x800 + 4 * cb);
	s->tcr = rd32(p + CB_TCR);
	s->tcr2 = rd32(p + CB_TCR2);
	s->ttbr0[0] = rd32(p + CB_TTBR0), s->ttbr0[1] = rd32(p + CB_TTBR0 + 4);
	s->ttbr1[0] = rd32(p + CB_TTBR1), s->ttbr1[1] = rd32(p + CB_TTBR1 + 4);
	s->mair[0] = rd32(p + CB_MAIR0), s->mair[1] = rd32(p + CB_MAIR1);
	sw(p + CB_SCTLR_, 0, "CB SCTLR 0 while set up");
	sw(p + CB_FSR, 0xffffffff, "CB FSR clear");
	sw(p + CB_TTBR0, 0, "TTBR0");
	sw(p + CB_TTBR0 + 4, 0, "TTBR0 hi");
	sw(p + CB_TTBR1, 0, "TTBR1");
	sw(p + CB_TTBR1 + 4, 0, "TTBR1 hi");
	sw(p + CB_TCR, trap ? TRAP_TCR : 0, trap ? "TCR EPD0|EPD1" : "TCR");
	sw(p + CB_TCR2, trap ? 0x2 : 0, "TCR2");
	sw(p + CB_MAIR0, 0, "MAIR0");
	sw(p + CB_MAIR1, 0, "MAIR1");
	sw(g + 0x800 + 4 * cb, CAM_CBA2R, "CBA2R AArch64");
	sw(g + 4 * cb, CAM_CBAR, "CBAR S1, S2 bypass");
	sw(p + CB_TLBIALL, 0, "TLBIALL");
	sw(p + CB_TLBSYNC, 0, "TLBSYNC");
	for (int i = 0; !dry && i < 1000 && (rd32(p + CB_TLBSTATUS) & 1); i++)
		usleep(1000);
	sw(p + CB_SCTLR_, trap ? TRAP_SCTLR : BYP_SCTLR, trap ? "SCTLR M|CFRE (trap)" : "SCTLR M=0 (identity)");
}

static void cb_restore(int b)
{
	int cb = cs.bank[b];
	uint64_t p = cbpage((uint32_t)cb), g = gr1();
	struct cbsave *s = &cs.sv[b];
	sw(p + CB_SCTLR_, s->sctlr, "SCTLR restore");
	sw(p + CB_TCR, s->tcr, "TCR restore");
	sw(p + CB_TCR2, s->tcr2, "TCR2 restore");
	sw(p + CB_TTBR0, s->ttbr0[0], "TTBR0 restore");
	sw(p + CB_TTBR0 + 4, s->ttbr0[1], "TTBR0 hi restore");
	sw(p + CB_TTBR1, s->ttbr1[0], "TTBR1 restore");
	sw(p + CB_TTBR1 + 4, s->ttbr1[1], "TTBR1 hi restore");
	sw(p + CB_MAIR0, s->mair[0], "MAIR0 restore");
	sw(p + CB_MAIR1, s->mair[1], "MAIR1 restore");
	sw(g + 4 * cb, s->cbar, "CBAR restore");
	sw(g + 0x800 + 4 * cb, s->cba2r, "CBA2R restore");
	sw(p + CB_FSR, 0xffffffff, "FSR clear");
	sw(p + CB_TLBIALL, 0, "TLBIALL");
	sw(p + CB_TLBSYNC, 0, "TLBSYNC");
}

/*
 * NSMR SMRs (ids/masks), only if no valid SMR overlaps any of them; the first NREQ are needed, the rest are
 * dropped when free SMR slots run out. TRAP[k]: SMR k -> the trap bank, else -> the identity bank (cam16:
 * --bypass puts the other IFE SIDs on a trap bank, so they fault in a bank, not as unmatched streams).
 */
static int cam_smmu_up(int nsmr, int nreq, const uint16_t (*m)[2], const int *trap)
{
	smmu_geom();
	uint64_t used[4] = { 0 };
	int bad = 0, ntrap = 0, nbyp = 0;
	cs.nslot = 0;
	for (uint32_t i = 0; i < sg.nsmr; i++) {
		uint32_t smr = rd32(SMMU + SM_SMR(i)), s2 = rd32(SMMU + SM_S2CR(i));
		if (!(smr >> 31)) {
			if (cs.nslot < nsmr)
				cs.slot[cs.nslot++] = (int)i;
			continue;
		}
		if (((s2 >> 16) & 3) == 0)
			used[(s2 & 0xff) / 64] |= 1ull << ((s2 & 0xff) % 64);
		for (int k = 0; k < nsmr; k++)
			if (smr_overlap(smr & 0x7fff, (smr >> 16) & 0x7fff, m[k][0], m[k][1])) {
				printf("camrx: smmu CONFLICT: SMR%u %08x (id %#x mask %#x, S2CR %08x) overlaps %#x/%#x\n", i,
				       smr, smr & 0x7fff, (smr >> 16) & 0x7fff, s2, m[k][0], m[k][1]);
				bad = 1;
			}
	}
	if (cs.nslot >= nreq && cs.nslot < nsmr) {
		printf("camrx: smmu: only %d free SMRs: %d optional trap SMRs dropped\n", cs.nslot, nsmr - cs.nslot);
		nsmr = cs.nslot;
	}
	for (int k = 0; k < nsmr; k++)
		trap[k] ? ntrap++ : nbyp++;
	int nb = (ntrap > 0) + (nbyp > 0);
	cs.nb = 0;
	for (uint32_t c = 0; c < sg.ncb && c < 256 && cs.nb < nb; c++)
		if (!(used[c / 64] & (1ull << (c % 64))) && !(rd32(cbpage(c)) & 1) && rd32(gr1() + 4 * c) == 0 &&
		    (uint64_t)(sg.npage + c + 1) * sg.psize <= SMMU_SIZE)
			cs.bank[cs.nb++] = (int)c;
	if (bad || cs.nslot < nreq || cs.nb < nb) {
		printf("camrx: smmu: no dedicated mapping (conflict %d, free SMRs %d of %d, banks %d of %d): abort before "
		       "any IFE write\n", bad, cs.nslot, nreq, cs.nb, nb);
		cs.nslot = 0;
		cs.nb = 0;
		cs.bank[0] = cs.bank[1] = -1;
		return -1;
	}
	cs.nslot = nsmr;
	/* bank[0] = what SMR 0 needs; the other one (if any) the rest */
	int b0trap = trap[0];
	cs.trap = ntrap ? (b0trap ? 0 : 1) : -1;
	for (int k = 0; k < nsmr; k++) {
		cs.bk[k] = trap[k] == b0trap ? 0 : 1;
		cs.smr[k] = rd32(SMMU + SM_SMR(cs.slot[k]));
		cs.s2cr[k] = rd32(SMMU + SM_S2CR(cs.slot[k]));
		printf("camrx: smmu: %#x/%#x -> SMR%d, CB%d (%s)\n", m[k][0], m[k][1], cs.slot[k], cs.bank[cs.bk[k]],
		       trap[k] ? "trap" : "bypass");
	}
	cs.on = 1;
	for (int b = 0; b < cs.nb; b++)
		cb_setup(b, b == cs.trap);
	for (int k = 0; k < nsmr; k++)
		sw(SMMU + SM_S2CR(cs.slot[k]), (uint32_t)cs.bank[cs.bk[k]], "S2CR translate -> bank");
	for (int k = 0; k < nsmr; k++)
		sw(SMMU + SM_SMR(cs.slot[k]), 1u << 31 | (uint32_t)m[k][1] << 16 | m[k][0], "SMR valid (last)");
	smmu_sync();
	return 0;
}

/* every exit path: SMRs back (invalid) first, then S2CRs, then the bank registers, TLB sync */
static void cam_smmu_down(void)
{
	if (!cs.on)
		return;
	cs.on = 0;
	for (int k = 0; k < cs.nslot; k++)
		sw(SMMU + SM_SMR(cs.slot[k]), cs.smr[k], "SMR restore");
	smmu_sync();
	for (int k = 0; k < cs.nslot; k++)
		sw(SMMU + SM_S2CR(cs.slot[k]), cs.s2cr[k], "S2CR restore");
	for (int b = 0; b < cs.nb; b++)
		cb_restore(b);
	smmu_sync();
	printf("camrx: smmu restored (%d SMRs, CB%d%s%d)\n", cs.nslot, cs.bank[0], cs.nb > 1 ? " + CB" : "",
	       cs.nb > 1 ? cs.bank[1] : 0);
}

/* the trap bank: what it caught; then clear FSR (write back) and, if stalled, terminate (RESUME TnR=1). 1 = a fault */
static int cam_smmu_trap_report(void)
{
	if (dry || !cs.on || cs.trap < 0)
		return 0;
	int cb = cs.bank[cs.trap];
	uint64_t p = cbpage((uint32_t)cb);
	uint32_t fsr = rd32(p + CB_FSR), sy = rd32(p + CB_FSYNR0), fr = rd32(gr1() + 0x400 + 4 * cb);
	printf("camrx: trap CB%d: FSR %08x (TF %u AFF %u PF %u EF %u TLBMCF %u MULTI %u SS %u FORMAT %u) FAR %08x%08x "
	       "FSYNR0 %08x (WNR %u PLVL %u) CBFRSYNRA %08x (SID %#x, guess: bits 14:0)\n", cb, fsr, (fsr >> 1) & 1,
	       (fsr >> 2) & 1, (fsr >> 3) & 1, (fsr >> 4) & 1, (fsr >> 5) & 1, fsr >> 31, (fsr >> 30) & 1,
	       (fsr >> 9) & 3, rd32(p + CB_FAR + 4), rd32(p + CB_FAR), sy, (sy >> 4) & 1, sy & 3, fr, fr & 0x7fff);
	printf("camrx: trap: sGFSR %08x (USF %u SMCF %u)\n", rd32(SMMU + SM_GFSR), (rd32(SMMU + SM_GFSR) >> 1) & 1,
	       (rd32(SMMU + SM_GFSR) >> 2) & 1);
	if (fsr & 0xc00001feu) {
		sw(p + CB_FSR, fsr, "FSR clear (write back)");
		if ((fsr >> 30) & 1)
			sw(p + CB_RESUME, 1, "RESUME TnR=1 (terminate)");
	}
	return (fsr & 0xc00001feu) != 0;
}

/* cam16 (cam15 reset ~10 s after cleanup): before the SMRs go, point the identity SMRs at the trap bank, so
 * late IFE writes become context faults in our bank, not unmatched-stream faults; wait, report */
static void cam_smmu_park(void)
{
	if (!cs.on || cs.trap < 0)
		return;
	for (int k = 0; k < cs.nslot; k++)
		if (cs.bk[k] != cs.trap) {
			cs.bk[k] = cs.trap;
			sw(SMMU + SM_S2CR(cs.slot[k]), (uint32_t)cs.bank[cs.trap], "S2CR -> trap bank (park)");
		}
	smmu_sync();
	msleep_(2000);
	if (!dry)
		printf("camrx: late writes after stop: %s\n", cam_smmu_trap_report() ? "Y (trap bank above)" : "N");
}

/* ---- stage d: one frame through IFE-lite RDI0 WM0 ---- */

static struct {
	int armed, ife, smmu;	/* what cap_down must undo */
	int ife_used;		/* ife_config ran: cap_down resets the IFE-lite (cam16) */
	uint64_t pa;		/* buffer, 0 = none */
	uint32_t len;		/* frame + guard */
	int owned;		/* PMEM_ALLOC32'd: PMEM_FREE it */
} cap;

static void iw(uint32_t off, uint32_t v, const char *why)
{
	wr(IFE + off, v, why);
}

static uint32_t ir(uint32_t off)
{
	return dry ? 0 : rd32(IFE + off);
}

static void wm_off(const char *why)
{
	iw(WM0_CFG, 0, why);
	iw(IFE_REG_UPDATE, 0x2, "REG_UPDATE RDI0");
}

/* cam16: IFE-lite GLOBAL_RESET as in ife_config (bounded poll), so the bus master is idle before the SMMU goes */
static void ife_reset(const char *why)
{
	iw(IFE_IRQ_MASK0, 1u << 31, "IRQ_MASK0 reset done");
	iw(IFE_IRQ_CLEAR0, 0xffffffff, "IRQ_CLEAR0");
	iw(IFE_IRQ_GCLEAR, 1, "IRQ_GLOBAL_CLEAR");
	iw(IFE_GLOBAL_RESET, 0x3f9f, why);
	if (poll(IFE + IFE_IRQ_STATUS0, 1u << 31, 1u << 31, 100, 1000))
		printf("camrx: ife-lite reset (cleanup) not done after 100 ms (irq0 %#x)\n", ir(IFE_IRQ_STATUS0));
}

/* every exit path (also exit(1) from wr and SIGINT/SIGTERM via the main loop): WM off, CSID down, IFE reset,
 * wait a frame, SMMU parked on the trap bank + 2 s + late-write check, SMMU back, buffer freed. Notes section 4
 * restore order (the sensor is stopped by its own command). */
static void cap_down(void)
{
	if (cap.ife) {
		cap.ife = 0;
		wm_off("WM0_CFG 0 (cleanup)");
	}
	if (cap.armed) {
		cap.armed = 0;
		paths_down();
	}
	if (cap.ife_used) {
		cap.ife_used = 0;
		stage("cleanup: ife-lite reset");
		ife_reset("GLOBAL_RESET (cleanup, cam16)");
	}
	msleep_(100);	/* > 1 frame at ~30 fps: a write in flight finishes */
	if (cap.smmu) {
		cap.smmu = 0;
		stage("cleanup: smmu park on the trap bank, 2 s");
		cam_smmu_park();	/* cam16 */
		cam_smmu_down();	/* cam13: own SMRs, not smmu_identity/smmu_release */
	}
	/* cam11: a camnoc RCG left on PLL0 even outlives `camclk off` and then camid can't start camnoc_axi
	 * (its source is off); put it back on XO while PLL0 still runs (the WM is stopped by now) */
	if (!dry && (rd32(CAMNOC_RCG + 4) & 0x71f) == CAMNOC_CFG_150) {
		wr(CAMNOC_RCG + 4, rd32(CAMNOC_RCG + 4) & ~0x371fu, "camnoc_axi CFG back to XO (cleanup)");
		wr(CAMNOC_RCG, rd32(CAMNOC_RCG) | 1, "CMD UPDATE");
		if (poll(CAMNOC_RCG, 1, 0, 500, 1000))
			printf("camrx: camnoc_axi UPDATE to XO stuck (CMD %#x)\n", rd32(CAMNOC_RCG));
	}
	if (cap.pa && cap.owned && !dry) {
		printf("camrx: PMEM_FREE %#llx -> %d\n", (unsigned long long)cap.pa, pm_free(cap.pa));
		cap.pa = 0;
	}
	fflush(stdout);
}

static int branch_req(const char *n, uint32_t cbcr)
{
	if (branch_on(cbcr))
		return 0;
	printf("camrx: %s branch %#x = %#x not on, abort\n", n, cbcr, rd32(cbcr));
	return 1;
}

static int cap_precheck(void)
{
	uint32_t m = rd32(PLL0_MODE), u = rd32(PLL0_USER);
	if (!(m & LOCK_DET) || !(m & 1) || !(u & 2)) {
		printf("camrx: pll0 MODE %#x USER_CTL %#x not locked / even out off, abort\n", m, u);
		return 1;
	}
	if (branch_req("lite_csid", LITE_CSID_CBCR) || branch_req("lite_cphy_rx", LITE_CPHY_CBCR) ||
	    branch_req("ife_lite", LITE_CBCR) || branch_req("csiphy0", CSIPHY0_CBCR) ||
	    branch_req("csi0phytimer", PHYTIMER0_CBCR) || branch_req("cpas_ahb (camid --keep)", CPAS_AHB_CBCR) ||
	    branch_req("camnoc_axi (camid --keep)", CAMNOC_CBCR))
		return 1;
	show_rcg("camnoc_axi", CAMNOC_RCG);
	uint32_t cfg = rd32(CAMNOC_RCG + 4);
	if ((cfg & 0x71f) == CAMNOC_CFG_150)
		return 0;
	if ((cfg >> 8 & 7) != 0) {	/* someone set another rate: leave it */
		printf("camrx: camnoc_axi not at XO and not 150 MHz (CFG %#x): left as is\n", cfg);
		return 0;
	}
	/* the IFE's path to DDR: redfin ran it at 150 MHz (0921:171) = the DT's lowest non-XO level */
	stage("camnoc_axi rcg 150 MHz (PLL0 even 600 / 4), was XO");
	wr(CAMNOC_RCG + 4, (cfg & ~0x371fu) | CAMNOC_CFG_150, "camnoc_axi CFG src 2 div 7");
	wr(CAMNOC_RCG, rd32(CAMNOC_RCG) | 1, "CMD UPDATE");
	if (poll(CAMNOC_RCG, 1, 0, 500, 1000)) {
		printf("camrx: camnoc_axi UPDATE stuck (CMD %#x), abort\n", rd32(CAMNOC_RCG));
		return 1;
	}
	return 0;
}

/* redfin ife_config (0921:515-541): reset, bus init, WM0 frame mode at pa, FRAME_INC 0, REG_UPDATE */
static int ife_config(uint32_t pa)
{
	stage("ife-lite reset");
	printf("camrx: ife-lite HW_VERSION %#x BUS_HW_VERSION %#x\n", ir(IFE_HW_VERSION), ir(BUS_HW_VERSION));
	iw(IFE_IRQ_MASK0, 1u << 31, "IRQ_MASK0 reset done");
	cap.ife = 1;
	cap.ife_used = 1;
	iw(IFE_GLOBAL_RESET, 0x3f9f, "GLOBAL_RESET");
	if (poll(IFE + IFE_IRQ_STATUS0, 1u << 31, 1u << 31, 100, 1000)) {
		printf("camrx: ife-lite does not reset (irq0 %#x)\n", ir(IFE_IRQ_STATUS0));
		return 1;
	}
	iw(IFE_IRQ_CLEAR0, 0xffffffff, "IRQ_CLEAR0");
	iw(IFE_IRQ_CLEAR1, 0xffffffff, "IRQ_CLEAR1");
	iw(IFE_IRQ_GCLEAR, 1, "IRQ_GLOBAL_CLEAR");
	stage("ife-lite bus + wm0 (frame mode)");
	iw(BUS_SYNC_FRAME_HDR, 0, "ADDR_SYNC_FRAME_HDR 0");
	iw(BUS_CGC_OVD, 0xfffff, "BUS_CGC_OVD");
	iw(BUS_TEST_CTRL, 0, "BUS_TEST_CTRL 0");
	iw(BUS_SYNC_NO_SYNC, 0xffffff, "ADDR_SYNC_NO_SYNC");
	iw(WM0_BURST_LIMIT, 0xf, "WM0 BURST_LIMIT");
	/* frame based: WIDTH/STRIDE 0xff01 + HEIGHT 0 (Linux RDI defaults, redfin); PACKER 0 = MIPI bytes as they come */
	iw(WM0_WIDTH, 0xff01, "WM0 WIDTH (frame mode)");
	iw(WM0_HEIGHT, 0, "WM0 HEIGHT 0");
	iw(WM0_PACKER, 0, "WM0 PACKER plain");
	iw(WM0_STRIDE, 0xff01, "WM0 STRIDE (frame mode)");
	iw(WM0_DROP_PERIOD, 0, "WM0 FRAMEDROP_PERIOD");
	iw(WM0_DROP_PATTERN, 1, "WM0 FRAMEDROP_PATTERN");
	iw(WM0_SUB_PERIOD, 0, "WM0 IRQ_SUB_PERIOD");
	iw(WM0_SUB_PATTERN, 1, "WM0 IRQ_SUB_PATTERN");
	iw(BUS_IRQ_CLEAR1, 0xffffffff, "BUS_IRQ_CLEAR1");
	iw(BUS_IRQ_GCLEAR, 1, "BUS_IRQ_GLOBAL_CLEAR");
	iw(BUS_IRQ_MASK1, 1, "BUS_IRQ_MASK1 WM0 buf done");
	iw(WM0_IMAGE_ADDR, pa, "WM0 IMAGE_ADDR");
	iw(WM0_FRAME_INC, 0, "WM0 FRAME_INC 0 (every frame same place)");
	iw(WM0_CFG, 3, "WM0_CFG en | frame based");
	iw(IFE_REG_UPDATE, 0x2, "REG_UPDATE RDI0");
	return 0;
}

static int run_cmd(const char *cmd, pid_t *pid)
{
	static char buf[512];
	char *av[32];
	int n = 0;
	snprintf(buf, sizeof buf, "%s", cmd);
	for (char *t = strtok(buf, " \t"); t && n < 31; t = strtok(NULL, " \t"))	/* no shell: split on blanks */
		av[n++] = t;
	av[n] = NULL;
	if (!n)
		return 1;
	fflush(stdout);
	*pid = fork();
	if (*pid < 0)
		return 1;
	if (*pid == 0) {
		signal(SIGINT, SIG_IGN);	/* the sensor command finishes (stream off) even on ^C */
		execv(av[0], av);
		perror(av[0]);
		_exit(127);
	}
	return 0;
}

static uint64_t nonzero(uint64_t pa, uint64_t len)
{
	static uint8_t b[65536];
	uint64_t n = 0;
	for (uint64_t o = 0; o < len; o += sizeof b) {
		size_t k = len - o < sizeof b ? (size_t)(len - o) : sizeof b;
		if (pm_rd(pa + o, b, k))
			return ~0ull;
		for (size_t i = 0; i < k; i++)
			n += b[i] != 0;
	}
	return n;
}

static int save_frame(uint64_t pa, const char *out)
{
	static uint8_t b[65536];
	FILE *f = fopen(out, "wb");
	if (!f) {
		perror(out);
		return 1;
	}
	for (uint64_t o = 0; o < FRAME_BYTES; o += sizeof b) {
		size_t k = FRAME_BYTES - o < sizeof b ? (size_t)(FRAME_BYTES - o) : sizeof b;
		if (pm_rd(pa + o, b, k) || fwrite(b, 1, k, f) != k) {
			fclose(f);
			return 1;
		}
	}
	/* cam16: cam15's file never reached disk before the reset: fsync it and its directory */
	if (fflush(f) || fsync(fileno(f))) {
		perror("camrx: fsync");
		fclose(f);
		return 1;
	}
	if (fclose(f))
		return 1;
	char dir[512];
	snprintf(dir, sizeof dir, "%s", out);
	char *sl = strrchr(dir, '/');
	if (sl == dir)
		sl[1] = 0;
	else if (sl)
		*sl = 0;
	else
		snprintf(dir, sizeof dir, ".");
	int d = open(dir, O_RDONLY);
	if (d < 0 || fsync(d)) {
		perror(dir);
		if (d >= 0)
			close(d);
		return 1;
	}
	close(d);
	return 0;
}

/* cam16 (cam15: 5896 guard bytes): where the guard writes are: taller frame (contiguous after the end) or scattered */
static void guard_where(uint64_t pa, uint32_t len)
{
	static uint8_t b[4096];
	long first = -1, last = -1;
	unsigned pages = 0;
	for (uint32_t o = FRAME_BYTES & ~4095u; o < len; o += sizeof b) {
		size_t k = len - o < sizeof b ? len - o : sizeof b;
		if (pm_rd(pa + o, b, k))
			return;
		int hit = 0;
		for (size_t i = 0; i < k; i++)
			if (b[i] && o + i >= FRAME_BYTES) {
				if (first < 0)
					first = (long)(o + i);
				last = (long)(o + i);
				hit = 1;
			}
		pages += hit;
	}
	if (first < 0)
		return;
	printf("camrx: guard writes: first at +%ld, last at +%ld past the frame end (offsets %ld..%ld), in %u 4 KB "
	       "pages; span = %.1f lines of 2520 B\n", first - (long)FRAME_BYTES, last - (long)FRAME_BYTES, first, last,
	       pages, (last + 1 - (long)FRAME_BYTES) / 2520.0);
}

/* the carveout slot1 is ours only while the framebuffer's zero-copy mode is off */
static int zerocopy_on(void)
{
	char ba[1024] = "";
	size_t n = sizeof ba - 1;
	if (sysctlbyname("kern.bootargs", ba, &n, NULL, 0)) {
		perror("camrx: sysctl kern.bootargs");
		return 1;
	}
	ba[n < sizeof ba ? n : sizeof ba - 1] = 0;
	for (char *p = ba; (p = strstr(p, "sweetfb_zerocopy=")); p++) {
		if (p != ba && p[-1] != ' ')
			continue;
		if (strtoul(p + 17, NULL, 0)) {
			printf("camrx: boot-arg sweetfb_zerocopy set (\"%s\"): carveout slot1 is the display's, abort "
			       "(use --alloc32)\n", ba);
			return 1;
		}
	}
	return 0;
}

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* sweet_pmem takes multi-byte pwrite/pread on unmanaged PAs (page by page, 32-bit stores): 64 KB chunks */
static int cap_zero(uint64_t pa, uint32_t len)
{
	static uint8_t z[65536], b[4];
	double t0 = now_s();
	for (uint32_t o = 0; o < len; o += sizeof z)
		if (pm_wr(pa + o, z, len - o < sizeof z ? len - o : sizeof z))
			return 1;
	double t1 = now_s();
	uint32_t bad = 0;
	for (uint32_t o = 0; o < len; o += 4096) {	/* sample: first word of every 4 KB page */
		if (pm_rd(pa + o, b, 4))
			return 1;
		bad += (b[0] | b[1] | b[2] | b[3]) != 0;
	}
	printf("camrx: zeroed %#x B at %#llx in %.3f s; readback of %u pages: %u nonzero\n", len,
	       (unsigned long long)pa, t1 - t0, len / 4096, bad);
	return bad != 0;
}

static int capture(const char *out, const char *cmd, int alloc32, int trap, int sid)
{
	/* cam13: no IFE DMA without a dedicated SMMU mapping; --trap first, --bypass only for the SID it found */
	if (trap == (sid >= 0)) {
		printf("camrx: capture needs --trap, or --bypass --sid 0xNNN (the SID --trap reported)\n");
		return 2;
	}
	/* --bypass (cam16): the SID (mask 0) -> identity bank; the DT SMRs that don't cover it and (optional, if free
	 * slots) the other DT SIDs under the one that does -> a trap bank. No two of them overlap (SMCFCFG). */
	uint16_t m[CS_MAXSMR][2];
	int tk[CS_MAXSMR], nm = 0, nreq = 0;
	if (sid >= 0) {
		int in = 0;
		m[nm][0] = (uint16_t)(sid & 0x7fff), m[nm][1] = 0, tk[nm++] = 0;
		for (int k = 0; k < 3; k++)
			if (smr_overlap(ife_smr[k][0], ife_smr[k][1], m[0][0], 0))
				in = 1;
			else
				m[nm][0] = ife_smr[k][0], m[nm][1] = ife_smr[k][1], tk[nm++] = 1;
		if (!in) {
			printf("camrx: --sid %#x is not covered by the DT IFE SMRs: refused\n", sid);
			return 2;
		}
		nreq = nm;
		for (int i = 0; i < 14 && nm < CS_MAXSMR; i++) {
			int ov = 0;
			for (int k = 0; k < nm; k++)
				ov |= smr_overlap(m[k][0], m[k][1], ife_sids[i], 0);
			if (!ov)
				m[nm][0] = ife_sids[i], m[nm][1] = 0, tk[nm++] = 1;
		}
	} else {
		for (int k = 0; k < 3; k++)
			m[nm][0] = ife_smr[k][0], m[nm][1] = ife_smr[k][1], tk[nm++] = 1;
		nreq = nm;
	}
	for (int a = 0; a < nm; a++)	/* by construction; checked anyway */
		for (int b = a + 1; b < nm; b++)
			if (smr_overlap(m[a][0], m[a][1], m[b][0], m[b][1])) {
				printf("camrx: own SMRs %#x/%#x and %#x/%#x overlap: refused\n", m[a][0], m[a][1], m[b][0],
				       m[b][1]);
				return 2;
			}
	if (!dry && cap_precheck())
		return 1;
	printf("camrx: note: SMMU SIDs/masks from the DT msm_cam_smmu_ife (guess: IFE-lite is one of them), WM frame mode values (redfin/Linux) and that one frame = "
	       "%u B (cam8 CSID bytes) are taken over, not verified here\n", FRAME_BYTES);
	atexit(cap_down);
	if (alloc32) {
		stage("pmem alloc 8 MB");
		cap.len = CAP_BUF;
		cap.pa = dry ? 0x80000000ull : pm_alloc32(CAP_BUF, 0x10000);	/* needs kernel109 (xnu 0232) */
		if (!cap.pa)
			return 1;
		cap.owned = 1;
		printf("camrx: buffer PA %#llx + %#x\n", (unsigned long long)cap.pa, cap.len);
		if (cap.pa + cap.len > 0x100000000ull) {	/* IMAGE_ADDR is 32 bit */
			printf("camrx: buffer above 4 GB, WM can't reach it: abort\n");
			return 1;
		}
	} else {
		/* cam10: no XNU RAM below 4 GB; the splash carveout's zero-copy slot 1 is free without sweetfb_zerocopy */
		stage("buffer: carveout slot1, zero it");
		if (!dry && zerocopy_on())
			return 1;
		cap.len = CAP_CO_LEN;
		printf("camrx: buffer PA %#llx + %#x (carveout slot1)\n", CAP_CO_PA, cap.len);
		if (!dry && cap_zero(CAP_CO_PA, cap.len))
			return 1;
		cap.pa = CAP_CO_PA;
	}
	stage(trap ? "smmu: 3 DT SMRs -> one trap bank (M=1, EPD0|EPD1)" :
	      "smmu: SID SMR (mask 0) -> identity bank, other IFE SIDs -> trap bank");
	uint32_t gfsr0 = rd32(SMMU + SM_GFSR);
	cap.smmu = 1;	/* cam_smmu_down undoes whatever part was written */
	if (cam_smmu_up(nm, nreq, (const uint16_t (*)[2])m, tk))
		return 1;
	uint64_t cbp = cbpage((uint32_t)cs.bank[0]);
	printf("camrx: smmu before: GFSR %#x, CB%d FSR %#x\n", gfsr0, cs.bank[0], dry ? 0 : rd32(cbp + CB_FSR));
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	if (rx_arm())
		return 1;
	cap.armed = 1;
	if (ife_config((uint32_t)cap.pa))
		return 1;
	stage("sensor stream");
	printf("camrx: stream command: %s\n", cmd);
	int done_at = -1, status = 0, t = 0;
	if (!dry) {
		pid_t pid;
		if (run_cmd(cmd, &pid)) {
			printf("camrx: could not start the stream command\n");
			return 1;
		}
		for (;; t++) {	/* 10 ms polls; WM0 off at the first buf done (or on a signal) */
			if (done_at < 0 && (ir(BUS_IRQ_STATUS(1)) & 1)) {
				done_at = t;
				wm_off("WM0_CFG 0 (first buf done)");
			}
			if (stop && done_at < 0) {
				done_at = -2;
				wm_off("WM0_CFG 0 (signal)");
			}
			pid_t r = waitpid(pid, &status, WNOHANG);
			if (r == pid || r < 0)
				break;
			if (t == 1500) {	/* 15 s: the command should take ~1 s */
				printf("camrx: stream command still running after 15 s, SIGTERM\n");
				kill(pid, SIGTERM);
			}
			usleep(10000);
		}
		printf("camrx: stream command exit status %d after ~%d ms; first buf done at ~%d ms%s\n",
		       WEXITSTATUS(status), t * 10, done_at * 10, done_at < 0 ? " (none)" : "");
	}
	stage("stop: wm0 off, rx stop");
	wm_off("WM0_CFG 0 (after stream)");
	cap.ife = 0;
	msleep_(100);
	paths_down();
	cap.armed = 0;
	printf("camrx: readback: CSID bytes ping/pong %u/%u, RDI0_STATUS %#x\n", cr(RDI0_BYTES_PING), cr(RDI0_BYTES_PONG),
	       cr(RDI0_STATUS));
	printf("camrx: readback: IFE irq0 %#x irq1 %#x VIOLATION %#x; BUS irq %#x %#x %#x COMP_ERROR %#x; WM0 status "
	       "%#x %#x\n", ir(IFE_IRQ_STATUS0), ir(IFE_IRQ_STATUS1), ir(IFE_VIOLATION), ir(BUS_IRQ_STATUS(0)),
	       ir(BUS_IRQ_STATUS(1)), ir(BUS_IRQ_STATUS(2)), ir(BUS_COMP_ERROR), ir(WM0_STATUS0), ir(WM0_STATUS1));
	int bad = 0;
	if (!dry) {
		uint32_t fsr = rd32(cbp + CB_FSR);
		/* FSR 0x400 is the FORMAT field, not a fault (redfin 0934:42) */
		printf("camrx: smmu after: GFSR %#x GFSYNR1 %#x; CB%d FSR %#x (fault bits %#x) FAR %#x%08x FSYNR0 %#x\n",
		       rd32(SMMU + SM_GFSR), rd32(SMMU + 0x54), cs.bank[0], fsr, fsr & 0x800001feu,
		       rd32(cbp + CB_FAR + 4), rd32(cbp + CB_FAR), rd32(cbp + CB_FSYNR0));
		int tf = cam_smmu_trap_report();	/* --bypass: the other IFE SIDs' trap bank */
		uint64_t nf = nonzero(cap.pa, FRAME_BYTES), ng = nonzero(cap.pa + FRAME_BYTES, cap.len - FRAME_BYTES);
		printf("camrx: nonzero bytes: frame %llu of %u, guard %llu of %u%s\n", (unsigned long long)nf, FRAME_BYTES,
		       (unsigned long long)ng, cap.len - FRAME_BYTES, ng ? "  GUARD WRITTEN: frame bigger than expected" : "");
		if (ng)
			guard_where(cap.pa, cap.len);
		/* no WM0 byte-count/last-address register in the notes (camera-stage-d.md): raw values only */
		printf("camrx: wm0: STATUS0 %#x STATUS1 %#x IMAGE_ADDR %#x WIDTH %#x HEIGHT %#x STRIDE %#x FRAME_INC %#x\n",
		       ir(WM0_STATUS0), ir(WM0_STATUS1), ir(WM0_IMAGE_ADDR), ir(WM0_WIDTH), ir(WM0_HEIGHT), ir(WM0_STRIDE),
		       ir(WM0_FRAME_INC));
		if (trap)
			printf("camrx: trap verdict: %s; buffer/guard nonzero %llu/%llu (expected 0/0)\n",
			       tf ? "fault caught (SID above)" : "NO fault in the trap bank: the IFE stream is not one of the 3 SMRs",
			       (unsigned long long)nf, (unsigned long long)ng);
		bad = trap ? (!tf || nf || ng) : ((fsr & 0x800001feu) || ng);
		if (save_frame(cap.pa, out)) {
			printf("camrx: writing %s failed\n", out);
			bad = 1;
		} else {
			printf("camrx: frame (%u B) -> %s, saved, fsync ok\n", FRAME_BYTES, out);
		}
	}
	cap_down();
	return bad || done_at < 0;
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
	if (!strcmp(argv[a], "smmu"))	/* cam13: read-only */
		return smmu_report(argc - a - 1, argv + a + 1);
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
	if (!strcmp(argv[a], "capture") && a + 2 <= argc) {
		const char *cmd = STREAM_CMD;
		int a32 = 0, trap = 0, byp = 0, sid = -1;
		for (int i = a + 2; i < argc; i++) {
			if (!strcmp(argv[i], "--alloc32"))
				a32 = 1;
			else if (!strcmp(argv[i], "--trap"))
				trap = 1;
			else if (!strcmp(argv[i], "--bypass"))
				byp = 1;
			else if (!strcmp(argv[i], "--sid") && i + 1 < argc)
				sid = (int)(strtoul(argv[++i], NULL, 0) & 0x7fff);
			else if (!strcmp(argv[i], "--stream-cmd") && i + 1 < argc)
				cmd = argv[++i];
			else
				goto usage;
		}
		if (trap == byp || (byp && sid < 0) || (trap && sid >= 0))
			goto usage;
		return capture(argv[a + 1], cmd, a32, trap, sid);
	}
	if (!strcmp(argv[a], "tpg") && a + 2 >= argc) {
		int s = a + 1 < argc ? atoi(argv[a + 1]) : 1;
		if (s < 1 || s > 60)
			goto usage;
		return tpg(s);
	}
usage:
	fprintf(stderr, "usage: camrx [--dry-run] status | clocks on|off | tpg [SECONDS 1-60] | phy on [SETTLE 0-255] | "
			"phy off | rx arm|report|stop | smmu [SID...] | capture OUTFILE --trap|--bypass --sid 0xNNN "
			"[--stream-cmd \"CMD\"] [--alloc32]\n");
	return 2;
}
