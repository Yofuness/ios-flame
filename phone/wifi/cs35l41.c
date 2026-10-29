/*
 * CS35L41 bring-up (see cs35l41.h). 32-bit big-endian register addresses and values. Sequence and registers from
 * research/notes/cs35l41-plan.md (Linux cs35l41.c / cs35l41-lib.c / cs35l41.h, master). sp1: boost bypass only
 * (VBST = VP), no boost code; class-D gain 0 (0.5 dB) via the B2 errata, digital volume never above -20 dB; every
 * configured field is read back before GLOBAL_EN can be set; only amp 0x40 is ever written.
 */
#include <stdio.h>
#include <unistd.h>
#include "geni_i2c.h"
#include "cs35l41.h"

#define DEVID		0x00000000	/* 0x35a40 */
#define REVID		0x00000004	/* [7:0] 0xb2 = B2 */
#define OTPID		0x00000010	/* logged only; no OTP unpack (trims stay default) */
#define TEST_KEY_CTL	0x00000040	/* 0x55, 0xaa unlock; 0xcc, 0x33 lock */
#define PWR_CTRL1	0x00002014	/* bit0 GLOBAL_EN */
#define PWR_CTRL2	0x00002018	/* bit0 AMP_EN, [5:4] BST_EN 0 off / 1 bypass (FET on, VBST = VP) / 2 boost */
#define PWR_CTRL3	0x00002028
#define PROTECT_REL	0x00002034	/* error release bits */
#define PUP_PDN_PATCH	0x00002084	/* 0x002f1aa0 before GLOBAL_EN, 0x002f1aa3 after power-down (unlocked) */
#define PLL_CLK_CTRL	0x00002c04	/* [2:0] REFCLK_SEL 0 = BCLK, bit4 REFCLK_EN, [10:5] FREQ, bit11 OPEN_LOOP */
#define GLOBAL_CLK_CTRL	0x00002c0c	/* [4:0] GLOBAL_FS, 48 kHz = 0x03 */
#define SP_ENABLES	0x00004800	/* bit16 ASP_RX1_EN */
#define SP_FORMAT	0x00004808	/* RX width [31:24], TX width [23:16], FMT [10:8] 2 = I2S, masters bits 0/4, inv 2/6 */
#define SP_HIZ_CTRL	0x0000480c	/* [1:0] = DT cirrus,asp-sdout-hiz */
#define SP_RX_SLOT	0x00004820	/* RX1 slot [5:0] */
#define SP_RX_WL	0x00004840	/* [5:0] word length */
#define DAC_PCM1_SRC	0x00004c00	/* [6:0] 0x08 = ASPRX1 (DSP bypass) */
#define AMP_DIG_VOL	0x00006000	/* [13:3] signed, 0.125 dB, 0 = 0 dB; bit15 kept (reset value) */
#define AMP_GAIN_CTRL	0x00006c04	/* PCM gain [9:5]; 0 = 0.5 dB (B2 errata), reset value restored at the end */
#define IRQ1_STATUS1	0x00010010	/* W1C */
#define IRQ1_STATUS4	0x0001001c
#define IRQ_PUP_DONE	(1u << 24)
#define IRQ_PDN_DONE	(1u << 23)
#define IRQ_AMP_SHORT	(1u << 31)
#define IRQ_TEMP_ERR	(1u << 17)
#define IRQ_TEMP_WARN	(1u << 15)
#define IRQ_BST_SHORT	(1u << 8)
#define IRQ_BST_UVP	(1u << 7)
#define IRQ_BST_OVP	(1u << 6)
#define IRQ_ERRORS	(IRQ_AMP_SHORT | IRQ_TEMP_ERR | IRQ_TEMP_WARN | IRQ_BST_SHORT | IRQ_BST_UVP | IRQ_BST_OVP)

#define BCLK_CODE	0x1b		/* 1.536 MHz (cs35l41.c:66 PLL table) */
#define VOL_CEIL_DB	(-20)		/* sp1: hard ceiling, whatever the caller passes */
#define VOL_FLOOR_DB	(-102)		/* tlv minimum 0x4cf */
#define AMP_ADDR	0x40		/* sp1: the only amp ever written */

static int dry;

void cs_set_dry(int d)
{
	dry = d;
}

static int rd(struct cs_amp *a, uint32_t reg, uint32_t *v)
{
	if (dry) {
		*v = 0;
		return 0;
	}
	uint8_t p[4] = { reg >> 24, reg >> 16, reg >> 8, reg }, b[4];
	int err = gi2c_msg(a->addr, p, 4, NULL, 0, 1);
	if (!err)
		err = gi2c_msg(a->addr, NULL, 0, b, 4, 0);
	if (err) {
		printf("cs35l41 %#x: read %08x: err %d\n", a->addr, reg, err);
		return err;
	}
	*v = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
	printf("cs35l41 %#x: read %08x = %08x\n", a->addr, reg, *v);	/* sp1: every access logged */
	return 0;
}

static int wr(struct cs_amp *a, uint32_t reg, uint32_t v)
{
	if (a->addr != AMP_ADDR) {	/* sp1: never write 0x41 */
		printf("cs35l41 %#x: write %08x refused (only %#x)\n", a->addr, reg, AMP_ADDR);
		return -1;
	}
	printf("cs35l41 %#x: %swrite %08x = %08x\n", a->addr, dry ? "[dry] " : "", reg, v);
	if (dry)
		return 0;
	uint8_t p[8] = { reg >> 24, reg >> 16, reg >> 8, reg, v >> 24, v >> 16, v >> 8, v };
	int err = gi2c_msg(a->addr, p, 8, NULL, 0, 0);
	if (err)
		printf("cs35l41 %#x: write %08x: err %d\n", a->addr, reg, err);
	return err;
}

/* sp1: what a register should read back as (fields merged over all writes to it) */
static void expect(struct cs_amp *a, uint32_t reg, uint32_t mask, uint32_t val)
{
	int i;
	for (i = 0; i < a->nexp && a->exp[i].reg != reg; i++)
		;
	if (i == a->nexp) {
		if (a->nexp == CS_MAX_EXP)
			return;	/* cannot happen: CS_MAX_EXP covers the whole sequence */
		a->exp[a->nexp++] = (struct cs_exp){ reg, 0, 0 };
	}
	a->exp[i].mask |= mask;
	a->exp[i].val = (a->exp[i].val & ~mask) | (val & mask);
}

static int set(struct cs_amp *a, uint32_t reg, uint32_t v)
{
	expect(a, reg, 0xffffffffu, v);
	return wr(a, reg, v);
}

static int upd(struct cs_amp *a, uint32_t reg, uint32_t mask, uint32_t val)
{
	uint32_t v;
	expect(a, reg, mask, val);
	int err = rd(a, reg, &v);
	return err ? err : wr(a, reg, (v & ~mask) | (val & mask));
}

static int unlock(struct cs_amp *a)
{
	return wr(a, TEST_KEY_CTL, 0x55) | wr(a, TEST_KEY_CTL, 0xaa);
}

static int lock(struct cs_amp *a)
{
	return wr(a, TEST_KEY_CTL, 0xcc) | wr(a, TEST_KEY_CTL, 0x33);
}

/* pup (0x002f1aa0) / pdn (0x002f1aa3) patch, test keys around it */
static int patch(struct cs_amp *a, uint32_t v)
{
	int err = unlock(a);
	err |= wr(a, PUP_PDN_PATCH, v);
	return err | lock(a);
}

int cs_probe(struct cs_amp *a)
{
	static const struct { const char *n; uint32_t r; } regs[] = {
		{ "devid", DEVID }, { "revid", REVID }, { "otpid", OTPID }, { "pwr_ctrl1", PWR_CTRL1 },
		{ "pwr_ctrl2", PWR_CTRL2 }, { "pwr_ctrl3", PWR_CTRL3 }, { "amp_dig_vol", AMP_DIG_VOL },
		{ "amp_gain_ctrl", AMP_GAIN_CTRL }, { "irq1_status1", IRQ1_STATUS1 }, { "irq1_status2", IRQ1_STATUS1 + 4 },
		{ "irq1_status3", IRQ1_STATUS1 + 8 }, { "irq1_status4", IRQ1_STATUS4 },
	};
	a->probed = 0;
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		uint32_t v;
		int err = rd(a, regs[i].r, &v);
		if (err) {
			if (!i)
				printf("cs35l41 %#x: no answer (held in reset? PM6150L GPIO %d)\n", a->addr,
				       a->addr == 0x40 ? 8 : 11);
			return err;
		}
		printf("cs35l41 %#x: %s %08x = %08x\n", a->addr, regs[i].n, regs[i].r, v);
		/* sp1: abort unless devid 0x35a40, revid B2 (the errata below are B2's) and GLOBAL_EN still 0 */
		if (!dry && ((regs[i].r == DEVID && (v & 0xffffff) != 0x35a40) ||
			     (regs[i].r == REVID && (v & 0xff) != 0xb2) || (regs[i].r == PWR_CTRL1 && (v & 1)))) {
			printf("cs35l41 %#x: unexpected %s %08x: stop\n", a->addr, regs[i].n, v);
			return -1;
		}
		if (regs[i].r == AMP_GAIN_CTRL)
			a->gain_saved = v;
	}
	a->probed = 1;
	return 0;
}

int cs_config(struct cs_amp *a, int vol_db, int boost)
{
	if (!a->probed || a->addr != AMP_ADDR) {
		printf("cs35l41 %#x: config refused (%s)\n", a->addr, a->probed ? "only 0x40" : "not probed");
		return -1;
	}
	if (boost)
		printf("cs35l41 %#x: boost not available in this version: bypass (VBST = VP)\n", a->addr);
	if (vol_db > VOL_CEIL_DB)
		vol_db = VOL_CEIL_DB;	/* sp1: hard ceiling */
	if (vol_db < VOL_FLOOR_DB)
		vol_db = VOL_FLOOR_DB;
	a->nexp = a->verified = 0;
	a->configured = 1;	/* from here on cs_power_down restores */
	int err = unlock(a);
	/* B2 errata (cs35l41-lib.c:667): amp + boost off, analog gain 0 (0.5 dB) */
	err |= set(a, 0x00004100, 0);
	err |= set(a, 0x00004310, 0);
	err |= set(a, 0x00004400, 0);
	err |= set(a, 0x0000381c, 0x51);
	err |= set(a, 0x02bc20e0, 0);
	err |= set(a, 0x02bc2020, 0);
	err |= set(a, PWR_CTRL2, 0);
	err |= set(a, AMP_GAIN_CTRL, 0);
	err |= set(a, 0x00004c28, 0);
	err |= set(a, 0x00004c2c, 0);
	err |= lock(a);
	/* boost bypass: FET on, VBST = VP; no boost-enable path in this version */
	err |= upd(a, PWR_CTRL2, 0x30, 1u << 4);
	/* PLL from BCLK 1.536 MHz (48 kHz x 16 bit x 2), cs35l41.c:841-856 */
	err |= upd(a, PLL_CLK_CTRL, 1u << 11, 1u << 11);		/* open loop */
	err |= upd(a, PLL_CLK_CTRL, 0x7e0, BCLK_CODE << 5);		/* freq */
	err |= upd(a, PLL_CLK_CTRL, 1u << 4, 0);
	err |= upd(a, PLL_CLK_CTRL, 0x7, 0);				/* ref = BCLK */
	err |= upd(a, PLL_CLK_CTRL, 1u << 4, 1u << 4);
	err |= upd(a, PLL_CLK_CTRL, 1u << 11, 0);			/* closed loop */
	err |= upd(a, GLOBAL_CLK_CTRL, 0x1f, 0x03);			/* 48 kHz; SP_RATE_CTRL left alone (slave) */
	/* ASP: I2S slave, 16-bit slots, RX1 from slot 0 (left), 16-bit words, SDOUT hi-z 3 (DT) */
	err |= upd(a, SP_FORMAT, 0xffff0755, 16u << 24 | 16u << 16 | 2u << 8);
	err |= upd(a, SP_RX_SLOT, 0x3f, 0);
	err |= upd(a, SP_RX_WL, 0x3f, 16);
	err |= upd(a, SP_HIZ_CTRL, 0x3, 3);
	err |= upd(a, SP_ENABLES, 1u << 16, 1u << 16);
	err |= upd(a, DAC_PCM1_SRC, 0x7f, 0x08);
	/* digital volume: vol_db dB, 0.125 dB steps, 11-bit two's complement at [13:3] (-30 dB: 0xb880 with bit15) */
	uint32_t vol = (uint32_t)(-vol_db * 8);
	vol = (0x800 - vol) & 0x7ff;
	printf("cs35l41 %#x: digital volume %d dB (field %#x)\n", a->addr, vol_db, vol);
	err |= upd(a, AMP_DIG_VOL, 0x7ff << 3, vol << 3);
	err |= upd(a, PWR_CTRL2, 1, 1);					/* AMP_EN */
	return err ? -1 : 0;
}

int cs_verify(struct cs_amp *a)
{
	int bad = 0;
	a->verified = 0;
	if (!a->configured)
		return -1;
	if (unlock(a))	/* the errata registers are test registers */
		bad = 1;
	for (int i = 0; i < a->nexp; i++) {
		uint32_t v;
		if (rd(a, a->exp[i].reg, &v)) {
			bad = 1;
			continue;
		}
		if (!dry && (v & a->exp[i].mask) != a->exp[i].val) {
			printf("cs35l41 %#x: verify %08x MISMATCH: read %08x mask %08x want %08x got %08x\n", a->addr,
			       a->exp[i].reg, v, a->exp[i].mask, a->exp[i].val, v & a->exp[i].mask);
			bad = 1;
		} else
			printf("cs35l41 %#x: verify %08x ok (%08x & %08x)\n", a->addr, a->exp[i].reg, v, a->exp[i].mask);
	}
	if (lock(a))
		bad = 1;
	printf("cs35l41 %#x: read-back of %d registers: %s\n", a->addr, a->nexp, bad ? "FAILED (no GLOBAL_EN)" : "ok");
	a->verified = !bad;
	return bad ? -1 : 0;
}

int cs_check(struct cs_amp *a)
{
	uint32_t s;
	if (rd(a, IRQ1_STATUS1, &s)) {
		a->err_bits |= IRQ_ERRORS;	/* unknown state: treat as an error */
		return -1;
	}
	if (s & IRQ_ERRORS) {
		printf("cs35l41 %#x: IRQ1 error bits %08x (status %08x)\n", a->addr, s & IRQ_ERRORS, s);
		a->err_bits |= s & IRQ_ERRORS;
		return -1;
	}
	return 0;
}

int cs_power_up(struct cs_amp *a)
{
	if (!a->verified || a->addr != AMP_ADDR || a->err_bits) {	/* sp1: no GLOBAL_EN without a clean read-back */
		printf("cs35l41 %#x: power-up refused (verified %d, errors %08x)\n", a->addr, a->verified, a->err_bits);
		return -1;
	}
	if (patch(a, 0x002f1aa0) || wr(a, IRQ1_STATUS1, 0xffffffffu))
		return -1;
	a->up = 1;	/* from the GLOBAL_EN attempt on, cs_power_down runs the full power-down */
	if (upd(a, PWR_CTRL1, 1, 1))
		return -1;
	for (int i = 0; i < 100 && !dry; i++) {
		uint32_t s;
		usleep(1000);
		if (rd(a, IRQ1_STATUS1, &s))
			return -1;
		if (s & IRQ_ERRORS) {
			printf("cs35l41 %#x: IRQ1 error bits %08x at power-up\n", a->addr, s & IRQ_ERRORS);
			a->err_bits |= s & IRQ_ERRORS;
			return -1;
		}
		if (s & IRQ_PUP_DONE) {
			printf("cs35l41 %#x: PUP_DONE after %d ms\n", a->addr, i + 1);
			return wr(a, IRQ1_STATUS1, IRQ_PUP_DONE) ? -1 : 0;
		}
	}
	if (!dry)
		printf("cs35l41 %#x: no PUP_DONE in 100 ms\n", a->addr);
	return dry ? 0 : -1;
}

void cs_power_down(struct cs_amp *a)
{
	if (a->addr != AMP_ADDR || !a->configured)
		return;
	if (a->up) {
		upd(a, PWR_CTRL1, 1, 0);
		int i;
		for (i = 0; i < 100 && !dry; i++) {
			uint32_t s;
			usleep(1000);
			if (rd(a, IRQ1_STATUS1, &s) || (s & IRQ_PDN_DONE))
				break;
		}
		if (i == 100)
			printf("cs35l41 %#x: no PDN_DONE in 100 ms\n", a->addr);
		wr(a, IRQ1_STATUS1, 0xffffffffu);
		patch(a, 0x002f1aa3);
		a->up = 0;
	}
	wr(a, PWR_CTRL2, 0);	/* AMP_EN 0, boost FET off */
	if (a->err_bits) {	/* error release (cs35l41.c:379); not re-enabled in this run */
		uint32_t rls = (a->err_bits & IRQ_AMP_SHORT ? 0x02 : 0) | (a->err_bits & IRQ_BST_SHORT ? 0x04 : 0) |
			       (a->err_bits & IRQ_BST_OVP ? 0x08 : 0) | (a->err_bits & IRQ_BST_UVP ? 0x10 : 0) |
			       (a->err_bits & IRQ_TEMP_WARN ? 0x20 : 0) | (a->err_bits & IRQ_TEMP_ERR ? 0x40 : 0);
		wr(a, PROTECT_REL, 0);
		wr(a, PROTECT_REL, rls);
		wr(a, PROTECT_REL, 0);
	}
	wr(a, AMP_GAIN_CTRL, a->gain_saved);	/* reset value back (amp off) */
	a->configured = a->verified = 0;
	printf("cs35l41 %#x: powered down\n", a->addr);
}
