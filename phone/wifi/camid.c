/*
 * Camera M1: power the rear IMX363 (SLG51000 LDOs on SE9 @0x75, GCC/CAMCC clocks, TLMM pins, reset gpio43) and
 * read its chip ID (sensor reg 0x0016, expect 03 63) over CCI0 master 0. Values from mainline Linux.
 *   camid [--dry-run] [--addr 0x10|0x1a] [--keep]
 * --dry-run: reads only the SLG51000, GCC, TLMM and the TITAN_TOP GDSC; every write and every other read is
 * printed as planned. Without --keep everything is powered down at exit (error and SIGINT/SIGTERM too).
 * Hazard rule: no CAMCC register but GDSC/GDSC_CLK before the GDSC is on, no CCI register before the GDSC and
 * the cci_0 branch are on (enforced in mrd/mwr). Needs SE9 clocks + pins (boot with sweetse9=2).
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "geni_i2c.h"
#include "pmem.h"

#define SLG		0x75
#define MATRIX_CONF_A	0x110d

#define GCC_CAM_AHB	0x10b008
#define GCC_CAM_XO	0x10b02c
#define GCC_CAM_HF_AXI	0x10b020

#define CAMCC		0xad00000
#define CAMCC_END	0xad20000
#define GDSC_CLK	0xad0c1a0
#define TITAN_GDSC	0xad0c1c4
#define CPAS_AHB	0xad0c124
#define CORE_AHB	0xad0c184
#define CAMNOC_AXI	0xad0c148
#define SLOW_AHB_RCG	0xad07058
#define CCI0_RCG	0xad0c0c4
#define CCI0_CBCR	0xad0c0dc
#define MCLK0_RCG	0xad05004
#define MCLK0_CBCR	0xad0501c

#define CCI		0xac4a000
#define CCI_END		0xac4b000
#define CCI_IRQ_STATUS	(CCI + 0xc0c)
#define CCI_ERRORS	0x18000ee6u

#define TLMM_S		0x3d00000
#define TLMM_N		0x3900000

static int dry, keep;
static int gdsc_on, cci_clk_on, in_pd;

/* --- MMIO with the hazard rule --- */
static int allowed(uint64_t pa)
{
	if (pa == 0xae44000)
		return 0;
	if (pa >= CCI && pa < CCI_END)
		return gdsc_on && cci_clk_on;
	if (pa >= CAMCC && pa < CAMCC_END && pa != GDSC_CLK && pa != TITAN_GDSC)
		return gdsc_on;
	return 1;
}

static uint32_t mrd(uint64_t pa)
{
	if (!allowed(pa) || (dry && !(pa == TITAN_GDSC || pa == GDSC_CLK || (pa >> 20) == 0x1 ||
				      (pa >> 20) == 0x3d || (pa >> 20) == 0x39))) {
		printf("[dry] rd %#llx skipped\n", (unsigned long long)pa);
		return 0;
	}
	return rd32(pa);
}

static void mwr(uint64_t pa, uint32_t v)
{
	if (dry || !allowed(pa)) {
		printf("[%s] wr %#llx = %#x\n", dry ? "dry" : "blocked", (unsigned long long)pa, v);
		return;
	}
	wr32(pa, v);
}

static int mpoll(uint64_t pa, uint32_t mask, int set, unsigned ms)
{
	if (dry || !allowed(pa))
		return 0;
	return wait_bit(pa, mask, set, ms);
}

/* --- SLG51000 --- */
static int srd(unsigned reg, uint8_t *v)
{
	uint8_t a[2] = { reg >> 8, reg };
	int e = gi2c_msg(SLG, a, 2, NULL, 0, 1);
	if (!e)
		e = gi2c_msg(SLG, NULL, 0, v, 1, 0);
	if (e)
		printf("camid: slg rd %#06x err %d\n", reg, e);
	return e;
}

static int swr(unsigned reg, uint8_t v)
{
	if (dry) {
		printf("[dry] wr slg %#06x = %#04x\n", reg, v);
		return 0;
	}
	uint8_t a[3] = { reg >> 8, reg, v };
	int e = gi2c_msg(SLG, a, 3, NULL, 0, 0);
	if (e)
		printf("camid: slg wr %#06x err %d\n", reg, e);
	return e;
}

struct ldo {
	int n;
	unsigned vsel_reg, status;
	uint8_t vsel;
	const char *what;
};
/* power-up order */
static const struct ldo ldos[5] = {
	{ 7, 0x3100, 0x31c1, 0x3c, "1.8 V" },
	{ 3, 0x2300, 0x23c1, 0xa5, "2.85 V" },
	{ 1, 0x2000, 0x20c1, 0x5a, "2.85 V" },
	{ 4, 0x2500, 0x25c1, 0x3c, "1.8 V" },
	{ 6, 0x2900, 0x29c1, 0x82, "1.05 V" },
};
static unsigned ldo_on;		/* bits we set in MATRIX_CONF_A */

/* --- TLMM --- */
struct pin {
	int gpio;
	uint64_t ctl;
	uint32_t newctl, old;
	int saved;
};
static struct pin pins[4] = {
	{ 17, TLMM_S + 0x1000 * 17, 1 << 2 | 3 | (2 / 2 - 1) << 6, 0, 0 },
	{ 18, TLMM_S + 0x1000 * 18, 1 << 2 | 3 | (2 / 2 - 1) << 6, 0, 0 },
	{ 13, TLMM_S + 0x1000 * 13, 1 << 2 | 0 | (4 / 2 - 1) << 6, 0, 0 },
	{ 43, TLMM_N + 0x1000 * 43, 0 << 2 | 0 | (2 / 2 - 1) << 6 | 1 << 9, 0, 0 },
};
#define RESET_IO	(TLMM_N + 0x1000 * 43 + 4)

static int st_pins, st_reset, st_mclk, st_cci, st_ahb[3], st_gdsc;

static void ldo_off(int n)
{
	uint8_t m;
	if (srd(MATRIX_CONF_A, &m) == 0 && swr(MATRIX_CONF_A, m & ~(1u << (n - 1))) == 0)
		printf("camid: pd LDO%d off (matrix %02x)\n", n, m & ~(1u << (n - 1)));
}

static void power_down(void)
{
	if (in_pd++)
		return;
	if (keep) {
		printf("camid: --keep: left powered\n");
		return;
	}
	if (st_reset)
		mwr(RESET_IO, 0), printf("camid: pd reset low\n");
	if (st_mclk)
		mwr(MCLK0_CBCR, mrd(MCLK0_CBCR) & ~1u), printf("camid: pd mclk0 off\n");
	for (int i = 3; i >= 0; i--)
		if (pins[i].saved)
			mwr(pins[i].ctl, pins[i].old), printf("camid: pd gpio%d ctl restored %#x\n", pins[i].gpio, pins[i].old);
	if (st_cci)
		mwr(CCI0_CBCR, mrd(CCI0_CBCR) & ~1u), cci_clk_on = 0, printf("camid: pd cci_0 off\n");
	static const uint64_t br[3] = { CPAS_AHB, CORE_AHB, CAMNOC_AXI };
	for (int i = 2; i >= 0; i--)	/* camnoc, core_ahb, cpas_ahb */
		if (st_ahb[i])
			mwr(br[i], mrd(br[i]) & ~1u), printf("camid: pd branch %#llx off\n", (unsigned long long)br[i]);
	if (st_gdsc) {
		mwr(TITAN_GDSC, mrd(TITAN_GDSC) | 1u);
		gdsc_on = 0;
		printf("camid: pd gdsc collapse\n");
	}
	for (int i = 4; i >= 0; i--)	/* 6, 4, 1, 3, 7 */
		if (ldo_on & (1u << (ldos[i].n - 1))) {
			ldo_off(ldos[i].n);
			delay_ms(1);
		}
	(void)st_pins;
}

static void die(const char *msg)
{
	printf("camid: ABORT: %s\n", msg);
	power_down();
	exit(1);
}

static void on_sig(int s)
{
	printf("camid: signal %d\n", s);
	power_down();
	_exit(1);
}

/* --- clock helpers --- */
static void branch_on(const char *name, uint64_t pa, int halt_delay, int *st)
{
	uint32_t v = mrd(pa);
	mwr(pa, v | 1);
	*st = 1;
	if (halt_delay) {
		usleep(10);
		printf("camid: %s %#llx was %#x, on (halt delay 10 us)\n", name, (unsigned long long)pa, v);
		return;
	}
	if (mpoll(pa, 1u << 31, 0, 1))
		die(name);
	printf("camid: %s %#llx was %#x, now %#x\n", name, (unsigned long long)pa, v, mrd(pa));
}

static void rcg_xo(const char *name, uint64_t base, int mnd)
{
	mwr(base + 4, 0);
	if (mnd) {
		mwr(base + 8, 0);
		mwr(base + 0xc, 0);
		mwr(base + 0x10, 0);
	}
	mwr(base, mrd(base) | 1);
	if (mpoll(base, 1, 0, 1))
		die(name);
	printf("camid: %s rcg %#llx = XO, cmd %#x\n", name, (unsigned long long)base, mrd(base));
}

/* --- CCI M0 Q0 --- */
static int cci_run(const uint32_t *w, int n, uint32_t done)
{
	for (int i = 0; i < n; i++)
		mwr(CCI + 0x310, w[i]);
	mwr(CCI + 0x300, n);
	mwr(CCI + 0x008, 1);
	if (dry)
		return 0;
	uint32_t s = 0;
	for (int i = 0; i <= 10; i++) {
		s = mrd(CCI_IRQ_STATUS);
		if (s & (done | CCI_ERRORS))
			break;
		delay_ms(1);
	}
	mwr(CCI + 0xc08, s);
	mwr(CCI + 0xc00, 1);
	if (s & CCI_ERRORS) {
		printf("camid: cci status %#x (error)\n", s);
		return -1;
	}
	if (!(s & done)) {
		printf("camid: cci status %#x (timeout)\n", s);
		return -1;
	}
	return 0;
}

static int read_id(unsigned a)
{
	uint32_t w1[3] = { 1 | a << 4, 9 | 2 << 4 | 0x00 << 8 | 0x16 << 16, 0x108 };
	uint32_t w2[3] = { 1 | a << 4, 10 | 2 << 4, 0x108 };
	if (cci_run(w1, 3, 1u << 4)) {
		printf("camid: 0x%02x: address write failed\n", a);
		return -1;
	}
	if (cci_run(w2, 3, 1u << 0)) {
		printf("camid: 0x%02x: read failed\n", a);
		return -1;
	}
	if (dry)
		return 0;
	uint32_t lvl = mrd(CCI + 0x11c), b[8 * 4];
	unsigned nb = 0;
	if (lvl > 8)
		lvl = 8;
	for (unsigned i = 0; i < lvl; i++) {
		uint32_t v = mrd(CCI + 0x118);
		for (int k = 0; k < 4; k++)
			b[nb++] = (v >> (8 * k)) & 0xff;
	}
	printf("camid: 0x%02x: level %u bytes:", a, lvl);
	for (unsigned i = 0; i < nb; i++)
		printf(" %02x", b[i]);
	printf("\n");
	if (nb >= 3 && b[1] == 0x03 && b[2] == 0x63)
		printf("camid: IMX363 ID OK\n");
	return 0;
}

int main(int argc, char **argv)
{
	int addr = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dry-run"))
			dry = 1;
		else if (!strcmp(argv[i], "--keep"))
			keep = 1;
		else if (!strcmp(argv[i], "--addr") && i + 1 < argc)
			addr = strtoul(argv[++i], NULL, 0);
		else {
			fprintf(stderr, "usage: camid [--dry-run] [--addr 0x10|0x1a] [--keep]\n");
			return 2;
		}
	}
	if (addr && addr != 0x10 && addr != 0x1a)
		return 2;
	pm_init();
	if (gi2c_init())
		return 1;
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	/* A. SLG51000 */
	uint8_t id[3], m, misc, trim;
	for (int i = 0; i < 3; i++)
		if (srd(0x1105 + i, &id[i]))
			die("slg read");
	printf("camid: slg PATN_ID %02x %02x %02x\n", id[0], id[1], id[2]);
	if (id[0] != 0x05 || id[1] != 0x35 || id[2] != 0xa9)
		die("not an SLG51000");
	if (srd(0x2064, &misc))
		die("slg read");
	printf("camid: LDO1 MISC1 %02x (%s base)\n", misc, misc & 1 ? "2.4 V" : "1.2 V");
	if (!(misc & 1))
		die("LDO1 has the 1.2 V base: cannot reach 2.85 V");
	if (srd(0x2963, &trim))
		die("slg read");
	int ldo6_bypass = trim & 1;
	printf("camid: LDO6 TRIM2 %02x%s\n", trim, ldo6_bypass ? " (bypass switch: VSEL not written)" : "");
	if (srd(MATRIX_CONF_A, &m))
		die("slg read");
	printf("camid: MATRIX_CONF_A %02x\n", m);
	for (int i = 0; i < 5; i++) {
		const struct ldo *l = &ldos[i];
		uint8_t old, rb;
		if (srd(l->vsel_reg, &old))
			die("slg read");
		if (l->n == 6 && ldo6_bypass) {
			printf("camid: LDO6 VSEL %02x kept (bypass)\n", old);
			continue;
		}
		if (m & (1u << (l->n - 1))) {
			printf("camid: LDO%d already on, VSEL %02x kept\n", l->n, old);
			continue;
		}
		if (swr(l->vsel_reg, l->vsel))
			die("slg write");
		rb = l->vsel;
		if (!dry && (srd(l->vsel_reg, &rb) || rb != l->vsel))
			die("VSEL readback mismatch");
		printf("camid: LDO%d VSEL %02x -> %02x (%s)\n", l->n, old, rb, l->what);
	}
	for (int i = 0; i < 5; i++) {
		const struct ldo *l = &ldos[i];
		uint8_t st = 0;
		if (srd(MATRIX_CONF_A, &m) || swr(MATRIX_CONF_A, m | 1u << (l->n - 1)))
			die("slg matrix");
		if (!(m & 1u << (l->n - 1)) && !dry)
			ldo_on |= 1u << (l->n - 1);
		int ok = dry;
		for (int t = 0; t <= 5 && !ok; t++) {
			if (srd(l->status, &st))
				break;
			if ((st & 2) && !(st & 1))
				ok = 1;
			else if (t < 5)
				delay_ms(1);
		}
		if (!dry)
			printf("camid: LDO%d on, status %02x\n", l->n, st);
		if (!ok)
			die("LDO VOUT_OK / ILIM");
		delay_ms(i == 4 ? 5 : 1);
	}

	/* B. GCC */
	uint32_t g;
	g = mrd(GCC_CAM_AHB);
	if (!(g & 1))
		mwr(GCC_CAM_AHB, g | 1);
	printf("camid: gcc camera_ahb %#x\n", g);
	g = mrd(GCC_CAM_XO);
	if (!(g & 1))
		mwr(GCC_CAM_XO, g | 1);
	printf("camid: gcc camera_xo %#x\n", g);
	g = mrd(GCC_CAM_HF_AXI);
	mwr(GCC_CAM_HF_AXI, g | 1);
	if (mpoll(GCC_CAM_HF_AXI, 1u << 31, 0, 1))
		die("gcc camera_hf_axi stuck off");
	printf("camid: gcc camera_hf_axi was %#x, now %#x\n", g, mrd(GCC_CAM_HF_AXI));

	/* C. CAMCC */
	g = mrd(GDSC_CLK);
	mwr(GDSC_CLK, g | 1);
	printf("camid: gdsc_clk was %#x\n", g);
	g = mrd(TITAN_GDSC);
	mwr(TITAN_GDSC, g & ~1u);
	st_gdsc = !dry;
	if (!dry && wait_bit(TITAN_GDSC, 1u << 31, 1, 1))
		die("TITAN_TOP GDSC not on");
	gdsc_on = !dry;
	printf("camid: titan gdsc was %#x, now %#x\n", g, dry ? 0 : rd32(TITAN_GDSC));
	branch_on("cpas_ahb", CPAS_AHB, 0, &st_ahb[0]);
	branch_on("core_ahb", CORE_AHB, 1, &st_ahb[1]);
	branch_on("camnoc_axi", CAMNOC_AXI, 0, &st_ahb[2]);
	rcg_xo("slow_ahb", SLOW_AHB_RCG, 0);
	rcg_xo("cci_0", CCI0_RCG, 0);
	branch_on("cci_0", CCI0_CBCR, 0, &st_cci);
	cci_clk_on = !dry;

	/* D. TLMM */
	for (int i = 0; i < 4; i++) {
		pins[i].old = mrd(pins[i].ctl);
		pins[i].saved = !dry;
		if (pins[i].gpio == 43)
			mwr(RESET_IO, 0);
		mwr(pins[i].ctl, pins[i].newctl);
		printf("camid: gpio%d ctl %#x -> %#x\n", pins[i].gpio, pins[i].old, pins[i].newctl);
	}
	st_pins = 1;
	st_reset = !dry;

	rcg_xo("mclk0", MCLK0_RCG, 1);
	branch_on("mclk0", MCLK0_CBCR, 0, &st_mclk);

	/* E. reset */
	delay_ms(1);
	mwr(RESET_IO, 2);
	delay_ms(10);
	printf("camid: reset high\n");

	/* F. CCI0 */
	printf("camid: cci hw_version %#x\n", mrd(CCI));
	mwr(CCI + 0xc04, 0);
	mwr(CCI + 0x004, 0x0f73f3f7);
	if (!dry) {
		if (wait_bit(CCI_IRQ_STATUS, 1u << 24, 1, 10))
			die("CCI reset ack timeout");
	}
	uint32_t s = mrd(CCI_IRQ_STATUS);
	mwr(CCI + 0xc08, s);
	mwr(CCI + 0xc00, 1);
	printf("camid: cci reset done, status %#x\n", s);
	mwr(CCI + 0x100, 0x004E0072);
	mwr(CCI + 0x104, 0x001C001C);
	mwr(CCI + 0x108, 0x000A004D);
	mwr(CCI + 0x10c, 0x76);
	mwr(CCI + 0x110, 0x161);
	int rc = 1;
	if (addr)
		rc = read_id(addr) ? 1 : 0;
	else if (read_id(0x10) == 0 || read_id(0x1a) == 0)
		rc = 0;
	power_down();
	return rc;
}
