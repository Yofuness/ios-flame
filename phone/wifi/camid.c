/*
 * Camera M1: power the rear IMX363 (SLG51000 LDOs on SE9 @0x75, GCC/CAMCC clocks, TLMM pins, reset gpio43) and
 * read its chip ID (sensor reg 0x0016, expect 03 63) over CCI0 master 0. Values from mainline Linux.
 *   camid [--dry-run] [--addr 0xNN] [--keep] [--front] [--mclk-asis] [--stream FILE [--hold S|--ctl] [--exposure L] [--gain C]]
 * --stream FILE (camera stage b, rear only): after the ID read, write FILE's registers (`0xADDR 0xVAL DELAY_US` a
 * line, `#` comments; from tools/sunfish-camtab.py; at most 2000), read back 0x0136/0x0137/0x0112/0x0114, stream
 * on (0x0100 = 1), sample the frame counter 0x0005 every 100 ms for 1 s, read 0x0100, stream off. Any CCI error
 * aborts (stream off first if on) with the failing line and address.
 * --hold SECONDS (camera e1, with --stream; default 1 = the 100 ms samples above): stream SECONDS (1-900), the
 * frame counter sampled once a second. --exposure LINES / --gain CODE (camera e2): after the table, before stream
 * on, in group hold (0x0104 = 1 .. 0): coarse integration 0x0202/0x0203 (clamped to 4 .. frame_length 0x0340/0x0341
 * - 40, redfin 0923 EXPOSURE_MARGIN), analog gain code 0x0204/0x0205 (clamped to 448 = 8x, Sony 512 - 512/g, redfin
 * gain max 8x for the IMX363); printed with their read-back.
 * --ctl (camera f1, camd; with --stream): stream until stdin ends, "Q" or SIGINT/SIGTERM (then stream off), instead of
 * --hold; stdin lines "E LINES CODE" (-1 = keep) apply exposure/gain as --exposure/--gain do, between frames in
 * group hold; replies on fd 3: "S FL LINES CODE" at stream on, "E FL LINES CODE" (read back) or "X" per command.
 * --mclk-asis: leave the MCLK RCG as it is (e.g. 24 MHz from `camclk mclk 0 24`) instead of setting it to XO.
 * --front: the front sensor instead (LDO7/2/5, MCLK2 gpio15, reset gpio68, CCI0 master 1 gpio19/20); tries 0x1a,
 * 0x10, 0x36, 0x2d (CCI reset after each NACK) and prints the raw bytes at 0x0016 and 0x0000 of the first that ACKs.
 * --dry-run: reads only the SLG51000, GCC, TLMM and the TITAN_TOP GDSC; every write and every other read is
 * printed as planned. Without --keep everything is powered down at exit (error and SIGINT/SIGTERM too).
 * Hazard rule: no CAMCC register but GDSC/GDSC_CLK before the GDSC is on, no CCI register before the GDSC and
 * the cci_0 branch are on (enforced in mrd/mwr). Needs SE9 clocks + pins (boot with sweetse9=2).
 */
#include <poll.h>
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
#define GCC_CAM_SF_AXI	0x10b06c	/* cam20: Linux gcc-sm7150.c gcc_camera_sf_axi_clk; redfin 0921 enables it */

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
#define MCLK2_RCG	0xad05044
#define MCLK2_CBCR	0xad0505c

#define CCI		0xac4a000
#define CCI_END		0xac4b000
#define CCI_IRQ_STATUS	(CCI + 0xc0c)
#define CCI_ERRORS	0x18000ee6u

#define TLMM_S		0x3d00000
#define TLMM_N		0x3900000

static int dry, keep, front, mclk_asis;
static const char *stream_file;
static int hold = 1, exp_lines = -1, gain_code = -1;	/* e1/e2: --hold, --exposure, --gain */
static int gdsc_on, cci_clk_on, in_pd;
static int ctl;	/* f1: --ctl */
static volatile sig_atomic_t ctl_quit;
static int rb_fl = -1, rb_lines = -1, rb_code = -1;	/* f1: last read back */

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
static const struct ldo ldos_rear[5] = {
	{ 7, 0x3100, 0x31c1, 0x3c, "1.8 V" },
	{ 3, 0x2300, 0x23c1, 0xa5, "2.85 V" },
	{ 1, 0x2000, 0x20c1, 0x5a, "2.85 V" },
	{ 4, 0x2500, 0x25c1, 0x3c, "1.8 V" },
	{ 6, 0x2900, 0x29c1, 0x82, "1.05 V" },
};
/* --front: vio, vana (LDO2 needs the 2.4 V base), vdig (0.4 V + 160 x 5 mV) */
static const struct ldo ldos_front[3] = {
	{ 7, 0x3100, 0x31c1, 0x3c, "1.8 V" },
	{ 2, 0x2200, 0x22c1, 0x50, "2.8 V" },
	{ 5, 0x2700, 0x27c1, 0xa0, "1.2 V" },
};
static const struct ldo *ldos = ldos_rear;
static int nldo = 5;
static unsigned ldo_on;		/* bits we set in MATRIX_CONF_A */

/* --- TLMM --- */
struct pin {
	int gpio;
	uint64_t ctl;
	uint32_t newctl, old;
	int saved;
};
static struct pin pins_rear[4] = {
	{ 17, TLMM_S + 0x1000 * 17, 1 << 2 | 3 | (2 / 2 - 1) << 6, 0, 0 },
	{ 18, TLMM_S + 0x1000 * 18, 1 << 2 | 3 | (2 / 2 - 1) << 6, 0, 0 },
	{ 13, TLMM_S + 0x1000 * 13, 1 << 2 | 0 | (4 / 2 - 1) << 6, 0, 0 },
	{ 43, TLMM_N + 0x1000 * 43, 0 << 2 | 0 | (2 / 2 - 1) << 6 | 1 << 9, 0, 0 },
};
static struct pin pins_front[4] = {
	{ 19, TLMM_S + 0x1000 * 19, 1 << 2 | 3 | (2 / 2 - 1) << 6, 0, 0 },
	{ 20, TLMM_S + 0x1000 * 20, 1 << 2 | 3 | (2 / 2 - 1) << 6, 0, 0 },
	{ 15, TLMM_S + 0x1000 * 15, 1 << 2 | 0 | (4 / 2 - 1) << 6, 0, 0 },
	{ 68, TLMM_N + 0x1000 * 68, 0 << 2 | 0 | (2 / 2 - 1) << 6 | 1 << 9, 0, 0 },
};
static struct pin *pins = pins_rear;
static int reset_gpio = 43;
static uint64_t reset_io = TLMM_N + 0x1000 * 43 + 4;
#define RESET_IO	reset_io
/* --front swaps these: MCLK2 and CCI0 master 1 (offset +0x100, queue regs +0x200, status bits) */
static uint64_t mclk_rcg = MCLK0_RCG, mclk_cbcr = MCLK0_CBCR;
static unsigned cci_m, cci_mo, cci_qo;
static uint32_t cci_rd_done = 1u << 0, cci_q_report = 1u << 4, cci_err = CCI_ERRORS;

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
		mwr(mclk_cbcr, mrd(mclk_cbcr) & ~1u), printf("camid: pd mclk off\n");
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
	for (int i = nldo - 1; i >= 0; i--)	/* rear 6, 4, 1, 3, 7; front 5, 2, 7 */
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
	if (ctl) {	/* f1: the stream loop ends and turns the stream off */
		ctl_quit = 1;
		return;
	}
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

/* --- CCI M0 Q0 (M1 Q0 with --front) --- */
static int cci_run(const uint32_t *w, int n, uint32_t done)
{
	for (int i = 0; i < n; i++)
		mwr(CCI + 0x310 + cci_qo, w[i]);
	mwr(CCI + 0x300 + cci_qo, n);
	mwr(CCI + 0x008, 1u << (cci_m * 2));
	if (dry)
		return 0;
	uint32_t s = 0;
	for (int i = 0; i <= 10; i++) {
		s = mrd(CCI_IRQ_STATUS);
		if (s & (done | cci_err))
			break;
		delay_ms(1);
	}
	mwr(CCI + 0xc08, s);
	mwr(CCI + 0xc00, 1);
	if (s & cci_err) {
		printf("camid: cci status %#x (error)\n", s);
		return -1;
	}
	if (!(s & done)) {
		printf("camid: cci status %#x (timeout)\n", s);
		return -1;
	}
	return 0;
}

static int read_reg(unsigned a, unsigned reg)
{
	uint32_t w1[3] = { 1 | a << 4, 9 | 2 << 4 | (reg >> 8) << 8 | (reg & 0xff) << 16, 0x108 };
	uint32_t w2[3] = { 1 | a << 4, 10 | 2 << 4, 0x108 };
	if (cci_run(w1, 3, cci_q_report)) {
		printf("camid: 0x%02x: address write failed\n", a);
		return -1;
	}
	if (cci_run(w2, 3, cci_rd_done)) {
		printf("camid: 0x%02x: read failed\n", a);
		return -1;
	}
	if (dry)
		return 0;
	uint32_t lvl = mrd(CCI + 0x11c + cci_mo), b[8 * 4];
	unsigned nb = 0;
	if (lvl > 8)
		lvl = 8;
	for (unsigned i = 0; i < lvl; i++) {
		uint32_t v = mrd(CCI + 0x118 + cci_mo);
		for (int k = 0; k < 4; k++)
			b[nb++] = (v >> (8 * k)) & 0xff;
	}
	if (front)
		printf("camid: front sensor answers at 0x%02x: reg %#06x level %u bytes:", a, reg, lvl);
	else
		printf("camid: 0x%02x: level %u bytes:", a, lvl);
	for (unsigned i = 0; i < nb; i++)
		printf(" %02x", b[i]);
	printf("\n");
	if (!front && nb >= 3 && b[1] == 0x03 && b[2] == 0x63)
		printf("camid: IMX363 ID OK\n");
	return 0;
}

static int read_id(unsigned a)
{
	if (read_reg(a, 0x0016))
		return -1;
	if (front)
		(void)read_reg(a, 0x0000);
	return 0;
}

/* --- camera stage b: 16-bit address, 8-bit data writes on the same queue --- */
static int write_reg(unsigned a, unsigned reg, unsigned v)
{
	uint32_t w[3] = { 1 | a << 4, 9 | 3 << 4 | (reg >> 8) << 8 | (reg & 0xff) << 16 | (v & 0xff) << 24, 0x108 };
	if (dry) {
		printf("[dry] cci wr 0x%02x %#06x = %#04x\n", a, reg, v);
		return 0;
	}
	if (cci_run(w, 3, cci_q_report)) {
		printf("camid: 0x%02x: write %#06x = %#04x failed\n", a, reg, v);
		return -1;
	}
	return 0;
}

/* one byte at reg (the byte after the slave echo); -1 on error */
static int read_u8(unsigned a, unsigned reg)
{
	uint32_t w1[3] = { 1 | a << 4, 9 | 2 << 4 | (reg >> 8) << 8 | (reg & 0xff) << 16, 0x108 };
	uint32_t w2[3] = { 1 | a << 4, 10 | 1 << 4, 0x108 };
	if (dry) {
		printf("[dry] cci rd 0x%02x %#06x\n", a, reg);
		return 0;
	}
	if (cci_run(w1, 3, cci_q_report) || cci_run(w2, 3, cci_rd_done)) {
		printf("camid: 0x%02x: read %#06x failed\n", a, reg);
		return -1;
	}
	uint32_t lvl = mrd(CCI + 0x11c + cci_mo), v = 0, b[8];
	unsigned nb = 0;
	for (unsigned i = 0; i < lvl && i < 2; i++) {
		v = mrd(CCI + 0x118 + cci_mo);
		for (int k = 0; k < 4; k++)
			b[nb++] = (v >> (8 * k)) & 0xff;
	}
	if (nb < 2) {
		printf("camid: 0x%02x: read %#06x: level %u\n", a, reg, lvl);
		return -1;
	}
	return b[1];	/* b[0] is the slave echo (cam2: 35 03 63 63) */
}

/* e2: big-endian 16-bit sensor register (two byte reads); -1 on error */
static int read_u16(unsigned a, unsigned reg)
{
	int h = read_u8(a, reg), l = h < 0 ? -1 : read_u8(a, reg + 1);
	return l < 0 ? -1 : h << 8 | l;
}

#define EXP_MARGIN	40	/* redfin 0923 EXPOSURE_MARGIN */
#define GAIN_CODE_MAX	448	/* IMX363 8x (redfin ios_gain_max), Sony code 512 - 512/8 */

/* e2: exposure and/or analog gain in one group hold; the stream is still off */
static int exposure(unsigned a)
{
	int fl = read_u16(a, 0x0340);
	if (fl < 0)
		return -1;
	if (dry)
		fl = 0xffff;
	printf("camid: frame_length 0x0340 = %d lines\n", fl);
	rb_fl = fl;
	unsigned lines = 0, code = 0;
	if (exp_lines >= 0) {
		int mx = fl - EXP_MARGIN < 4 ? 4 : fl - EXP_MARGIN;
		lines = exp_lines < 4 ? 4 : exp_lines > mx ? (unsigned)mx : (unsigned)exp_lines;
		printf("camid: exposure %d lines asked, %u written (clamp 4..%d)\n", exp_lines, lines, mx);
	}
	if (gain_code >= 0) {
		code = gain_code > GAIN_CODE_MAX ? GAIN_CODE_MAX : (unsigned)gain_code;
		printf("camid: analog gain code %d asked, %u written (max %d = 8x), = %.2fx\n", gain_code, code,
		       GAIN_CODE_MAX, 512.0 / (512 - code));
	}
	int ok = !write_reg(a, 0x0104, 1);
	if (ok && exp_lines >= 0)
		ok = !write_reg(a, 0x0202, lines >> 8) && !write_reg(a, 0x0203, lines & 0xff);
	if (ok && gain_code >= 0)
		ok = !write_reg(a, 0x0204, code >> 8) && !write_reg(a, 0x0205, code & 0xff);
	if (write_reg(a, 0x0104, 0))	/* the hold is released also after a failed write */
		ok = 0;
	if (!ok) {
		printf("camid: exposure/gain group hold write failed\n");
		return -1;
	}
	int rl = read_u16(a, 0x0202), rg = read_u16(a, 0x0204);
	if (rl < 0 || rg < 0)
		return -1;
	printf("camid: readback coarse_integration 0x0202 = %d, analog_gain 0x0204 = %d\n", rl, rg);
	rb_lines = rl, rb_code = rg;
	return 0;
}

#define MAX_WRITES 2000
static struct sw { unsigned addr, val, us, line; } sws[MAX_WRITES];

/* f1 (--ctl): commands from camd on stdin while streaming, replies on fd 3; 0 = ended cleanly */
static int ctl_loop(unsigned a)
{
	char buf[256];
	size_t n = 0;
	rb_fl = read_u16(a, 0x0340), rb_lines = read_u16(a, 0x0202), rb_code = read_u16(a, 0x0204);
	dprintf(3, "S %d %d %d\n", rb_fl, rb_lines, rb_code);
	for (int t = 0; !ctl_quit; t++) {
		struct pollfd p = { .fd = 0, .events = POLLIN };
		int r = poll(&p, 1, 100);
		if (r > 0) {
			ssize_t k = read(0, buf + n, sizeof buf - 1 - n);
			if (k <= 0) {
				printf("camid: ctl: stdin closed\n");
				break;
			}
			n += (size_t)k;
			buf[n] = 0;
			char *nl;
			while ((nl = strchr(buf, '\n'))) {
				*nl = 0;
				int l, c;
				if (buf[0] == 'Q') {
					printf("camid: ctl: Q\n");
					return 0;
				}
				if (sscanf(buf, "E %d %d", &l, &c) == 2 && l <= 0xffff && c <= 0xffff) {
					exp_lines = l, gain_code = c;
					if (exposure(a))
						dprintf(3, "X\n");
					else
						dprintf(3, "E %d %d %d\n", rb_fl, rb_lines, rb_code);
				} else {
					printf("camid: ctl: bad line \"%s\"\n", buf);
					dprintf(3, "X\n");
				}
				n -= (size_t)(nl + 1 - buf);
				memmove(buf, nl + 1, n + 1);
			}
			if (n >= sizeof buf - 1)
				n = 0, buf[0] = 0;
		}
		if (t % 100 == 99) {	/* every 10 s */
			int v = read_u8(a, 0x0005);
			if (v < 0)
				return -1;
			printf("camid: ctl t=%d s frame_count 0x0005 = %d\n", (t + 1) / 10, v);
		}
	}
	return 0;
}

static int stream(unsigned a)
{
	FILE *f = fopen(stream_file, "r");
	char buf[256];
	int n = 0, ln = 0, on = 0;
	if (!f) {
		printf("camid: --stream %s: cannot open\n", stream_file);
		return -1;
	}
	while (fgets(buf, sizeof buf, f)) {
		char *c = strchr(buf, '#'), *e;
		unsigned long ad, v, us;
		ln++;
		if (c)
			*c = 0;
		c = buf + strspn(buf, " \t\r\n");
		if (!*c)
			continue;
		ad = strtoul(c, &e, 0);
		if (e == c || (v = strtoul(c = e, &e, 0), e == c) || (us = strtoul(c = e, &e, 0), e == c) ||
		    *(e + strspn(e, " \t\r\n")) || ad > 0xffff || v > 0xff || us > 1000000 || n >= MAX_WRITES) {
			printf("camid: --stream %s line %d: bad or too many (max %d)\n", stream_file, ln, MAX_WRITES);
			fclose(f);
			return -1;
		}
		sws[n++] = (struct sw){ ad, v, us, ln };
	}
	fclose(f);
	printf("camid: --stream %s: %d writes\n", stream_file, n);
	for (int i = 0; i < n; i++) {
		if (write_reg(a, sws[i].addr, sws[i].val)) {
			printf("camid: --stream: line %u addr %#06x failed\n", sws[i].line, sws[i].addr);
			return -1;
		}
		if (sws[i].us)
			usleep(sws[i].us);
	}
	static const unsigned rb[4] = { 0x0136, 0x0137, 0x0112, 0x0114 };
	for (int i = 0; i < 4; i++) {
		int v = read_u8(a, rb[i]);
		if (v < 0)
			return -1;
		printf("camid: readback %#06x = %#04x\n", rb[i], v);
	}
	if ((exp_lines >= 0 || gain_code >= 0) && exposure(a))
		return -1;
	int rc = -1;
	if (write_reg(a, 0x0100, 1)) {
		printf("camid: --stream: stream on (0x0100) failed\n");
		goto off;
	}
	on = 1;
	printf("camid: stream on\n");
	if (ctl) {
		rc = ctl_loop(a);
		goto off;
	}
	for (int t = 1; hold > 1 && t <= hold; t++) {	/* e1 */
		sleep(1);
		int v = read_u8(a, 0x0005);
		if (v < 0)
			goto off;
		printf("camid: t=%d s frame_count 0x0005 = %d\n", t, v);
	}
	for (int t = 0; hold == 1 && t <= 10; t++) {
		int v = read_u8(a, 0x0005);
		if (v < 0)
			goto off;
		printf("camid: t=%d ms frame_count 0x0005 = %d\n", t * 100, v);
		if (t < 10)
			usleep(100000);
	}
	int mode = read_u8(a, 0x0100);
	if (mode < 0)
		goto off;
	printf("camid: mode_select 0x0100 = %#04x\n", mode);
	rc = 0;
off:
	if (on) {
		if (write_reg(a, 0x0100, 0))
			rc = -1;
		else
			printf("camid: stream off\n");
	}
	return rc;
}

/* CCI reset (all masters) + clear IRQ status; also run after a front NACK so the next address starts clean */
static void cci_reset(void)
{
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
}

int main(int argc, char **argv)
{
	int addr = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dry-run"))
			dry = 1;
		else if (!strcmp(argv[i], "--keep"))
			keep = 1;
		else if (!strcmp(argv[i], "--front"))
			front = 1;
		else if (!strcmp(argv[i], "--mclk-asis"))
			mclk_asis = 1;
		else if (!strcmp(argv[i], "--stream") && i + 1 < argc)
			stream_file = argv[++i];
		else if (!strcmp(argv[i], "--ctl"))
			ctl = 1;
		else if (!strcmp(argv[i], "--hold") && i + 1 < argc)
			hold = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--exposure") && i + 1 < argc)
			exp_lines = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--gain") && i + 1 < argc)
			gain_code = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--addr") && i + 1 < argc)
			addr = strtoul(argv[++i], NULL, 0);
		else {
			fprintf(stderr, "usage: camid [--dry-run] [--addr 0xNN] [--keep] [--front] [--mclk-asis] [--stream FILE [--hold S|--ctl] [--exposure LINES] [--gain CODE]]\n");
			return 2;
		}
	}
	if (hold < 1 || hold > 900 || exp_lines > 0xffff || gain_code > 0xffff ||
	    (!stream_file && (hold != 1 || ctl || exp_lines >= 0 || gain_code >= 0))) {
		fprintf(stderr, "camid: --hold 1-900, --exposure, --gain need --stream\n");
		return 2;
	}
	if (stream_file && front) {
		fprintf(stderr, "camid: --stream is rear only\n");
		return 2;
	}
	if (addr && addr != 0x10 && addr != 0x1a && !(front && (addr == 0x36 || addr == 0x2d)))
		return 2;
	if (front) {
		ldos = ldos_front, nldo = 3;
		pins = pins_front, reset_gpio = 68, reset_io = TLMM_N + 0x1000 * 68 + 4;
		mclk_rcg = MCLK2_RCG, mclk_cbcr = MCLK2_CBCR;
		cci_m = 1, cci_mo = 0x100, cci_qo = 0x200;
		cci_rd_done = 1u << 12, cci_q_report = 1u << 16, cci_err = 0x60ee6000u;
	}
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
	int an = front ? 2 : 1, dn = front ? 5 : 6;	/* analog LDO (needs the 2.4 V base), digital LDO (maybe bypass) */
	if (srd(front ? 0x2264 : 0x2064, &misc))
		die("slg read");
	printf("camid: LDO%d MISC1 %02x (%s base)\n", an, misc, misc & 1 ? "2.4 V" : "1.2 V");
	if (!(misc & 1))
		die("analog LDO has the 1.2 V base: cannot reach 2.8 V");
	if (srd(front ? 0x2763 : 0x2963, &trim))
		die("slg read");
	int ldo6_bypass = trim & 1;
	printf("camid: LDO%d TRIM2 %02x%s\n", dn, trim, ldo6_bypass ? " (bypass switch: VSEL not written)" : "");
	if (srd(MATRIX_CONF_A, &m))
		die("slg read");
	printf("camid: MATRIX_CONF_A %02x\n", m);
	for (int i = 0; i < nldo; i++) {
		const struct ldo *l = &ldos[i];
		uint8_t old, rb;
		if (srd(l->vsel_reg, &old))
			die("slg read");
		if (l->n == dn && ldo6_bypass) {
			printf("camid: LDO%d VSEL %02x kept (bypass)\n", dn, old);
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
	for (int i = 0; i < nldo; i++) {
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
		delay_ms(i == nldo - 1 ? 5 : 1);
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
	/* cam2: hf_axi (and xo) stay CLK_OFF until the camera GDSC is up: enable now, check after the GDSC. */
	uint32_t hf = mrd(GCC_CAM_HF_AXI);
	mwr(GCC_CAM_HF_AXI, hf | 1);
	printf("camid: gcc camera_hf_axi was %#x (enabled; checked after the GDSC)\n", hf);
	/* cam20: the IFE's SID 0xcc0 half reset the phone when passed (cam17/18); redfin also runs sf_axi */
	uint32_t sf = mrd(GCC_CAM_SF_AXI);
	mwr(GCC_CAM_SF_AXI, sf | 1);
	printf("camid: gcc camera_sf_axi was %#x (enabled; checked after the GDSC)\n", sf);

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
	if (!dry)
		(void)wait_bit(GCC_CAM_HF_AXI, 1u << 31, 0, 1);
	if (!dry)
		(void)wait_bit(GCC_CAM_SF_AXI, 1u << 31, 0, 1);
	printf("camid: after gdsc: gcc camera_hf_axi %#x, camera_sf_axi %#x, camera_xo %#x\n", mrd(GCC_CAM_HF_AXI),
	       mrd(GCC_CAM_SF_AXI), mrd(GCC_CAM_XO));
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
		if (pins[i].gpio == reset_gpio)
			mwr(RESET_IO, 0);
		mwr(pins[i].ctl, pins[i].newctl);
		printf("camid: gpio%d ctl %#x -> %#x\n", pins[i].gpio, pins[i].old, pins[i].newctl);
	}
	st_pins = 1;
	st_reset = !dry;

	if (mclk_asis)
		printf("camid: %s rcg left as is: cfg %#x m %#x n %#x\n", front ? "mclk2" : "mclk0", mrd(mclk_rcg + 4),
		       mrd(mclk_rcg + 8), mrd(mclk_rcg + 12));
	else
		rcg_xo(front ? "mclk2" : "mclk0", mclk_rcg, 1);
	branch_on(front ? "mclk2" : "mclk0", mclk_cbcr, 0, &st_mclk);

	/* E. reset */
	delay_ms(1);
	mwr(RESET_IO, 2);
	delay_ms(10);
	printf("camid: reset high\n");

	/* F. CCI0 */
	printf("camid: cci hw_version %#x\n", mrd(CCI));
	cci_reset();
	mwr(CCI + 0x100 + cci_mo, 0x004E0072);
	mwr(CCI + 0x104 + cci_mo, 0x001C001C);
	mwr(CCI + 0x108 + cci_mo, 0x000A004D);
	mwr(CCI + 0x10c + cci_mo, 0x76);
	mwr(CCI + 0x110 + cci_mo, 0x161);
	int rc = 1;
	if (front && !addr) {
		static const unsigned fa[4] = { 0x1a, 0x10, 0x36, 0x2d };
		for (int i = 0; i < 4 && rc; i++) {
			if (read_id(fa[i]) == 0) {
				rc = 0;
				break;
			}
			cci_reset();	/* a NACK poisons the next try (rear run): reset + reprogram master 1 */
			mwr(CCI + 0x200, 0x004E0072);
			mwr(CCI + 0x204, 0x001C001C);
			mwr(CCI + 0x208, 0x000A004D);
			mwr(CCI + 0x20c, 0x76);
			mwr(CCI + 0x210, 0x161);
		}
	} else if (addr)
		rc = read_id(addr) ? 1 : 0;
	else if (read_id(0x10) == 0 || read_id(0x1a) == 0)
		rc = 0;
	if (!rc && stream_file && stream(addr ? addr : 0x1a))
		rc = 1;
	power_down();
	return rc;
}
