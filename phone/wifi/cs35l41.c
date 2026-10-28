/*
 * CS35L41 bring-up (see cs35l41.h). 32-bit big-endian register addresses and values. Confidence per register
 * (from memory of Linux cs35l41.h / cs35l41.c, not checked against a datasheet) is in the table comments;
 * a wrong guess there gives silence or an IRQ error (shutdown), not more power: the boost peak current is
 * only ever lowered, the gain is never written, and the default is boost bypass (VBST = VP).
 */
#include <stdio.h>
#include <unistd.h>
#include "geni_i2c.h"
#include "cs35l41.h"

#define DEVID		0x00000000	/* 0x35a40 (85%) */
#define REVID		0x00000004	/* 0xb2 = B2 (70%) */
#define OTPID		0x00000010	/* logged only; no OTP unpack (60%) */
#define PWR_CTRL1	0x00002014	/* bit0 GLOBAL_EN (80%) */
#define PWR_CTRL2	0x00002018	/* bit0 AMP_EN, [5:4] BST_EN 0 off/1 bypass FET on/2 boost (60%) */
#define PLL_CLK_CTRL	0x00002c04	/* [2:0] REFCLK_SEL 0 = BCLK, bit4 REFCLK_EN, [10:5] FREQ, bit11 OPEN_LOOP (50%) */
#define GLOBAL_CLK_CTRL	0x00002c0c	/* [5:0] GLOBAL_FS, 48 kHz = 0x03 (55%) */
#define BST_PEAK_CUR	0x00003808	/* [6:0] IPK code = (mA - 1600) / 50 + 0x10 (50% address, 70% formula) */
#define BST_COEFF	0x00003810	/* K1 [15:8], K2 [7:0] (45%) */
#define BST_SLOPE_LBST	0x00003814	/* SLOPE [15:8], LBST [1:0] (45%) */
#define SP_ENABLES	0x00004800	/* bit16 ASP_RX1_EN (60%) */
#define SP_RATE_CTRL	0x00004804	/* [5:0] ASP_BCLK_FREQ code (35%) */
#define SP_FORMAT	0x00004808	/* TX width [31:24], RX width [23:16], FMT [10:8] 2 = I2S (55%) */
#define SP_HIZ_CTRL	0x0000480c	/* [1:0] = DT cirrus,asp-sdout-hiz (55%) */
#define SP_RX_SLOT	0x00004820	/* RX1 slot [5:0] (55%) */
#define SP_RX_WL	0x00004840	/* [5:0] word length (55%) */
#define DAC_PCM1_SRC	0x00004c00	/* 0x08 = ASPRX1 (55%) */
#define AMP_DIG_VOL	0x00006000	/* [13:3] signed, 0.125 dB, 0 = 0 dB (55%) */
#define AMP_GAIN_CTRL	0x00006c04	/* read only here (60%) */
#define IRQ1_STATUS1	0x00010010	/* W1C (70%) */
#define IRQ_PUP_DONE	(1u << 24)
#define IRQ_PDN_DONE	(1u << 23)
/* AMP_SHORT 31, TEMP_ERR 17, TEMP_WARN 15, BST_SHORT 8, BST_DCM_UVP 7, BST_OVP 6 (65%) */
#define IRQ_ERRORS	((1u << 31) | (1u << 17) | (1u << 15) | (1u << 8) | (1u << 7) | (1u << 6))

#define DT_BST_PEAK_MA	3700		/* recon/live.dts:9724 */
#define BCLK_CODE	0x1b		/* 1.536 MHz in the PLL/BCLK tables (30%) */

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
	return 0;
}

static int wr(struct cs_amp *a, uint32_t reg, uint32_t v)
{
	printf("cs35l41 %#x: %swrite %08x = %08x\n", a->addr, dry ? "[dry] " : "", reg, v);
	if (dry)
		return 0;
	uint8_t p[8] = { reg >> 24, reg >> 16, reg >> 8, reg, v >> 24, v >> 16, v >> 8, v };
	int err = gi2c_msg(a->addr, p, 8, NULL, 0, 0);
	if (err)
		printf("cs35l41 %#x: write %08x: err %d\n", a->addr, reg, err);
	return err;
}

static int upd(struct cs_amp *a, uint32_t reg, uint32_t mask, uint32_t val)
{
	uint32_t v;
	int err = rd(a, reg, &v);
	return err ? err : wr(a, reg, (v & ~mask) | (val & mask));
}

int cs_probe(struct cs_amp *a)
{
	static const struct { const char *n; uint32_t r; } regs[] = {
		{ "devid", DEVID }, { "revid", REVID }, { "otpid", OTPID }, { "pwr_ctrl1", PWR_CTRL1 },
		{ "pwr_ctrl2", PWR_CTRL2 }, { "bst_peak_cur", BST_PEAK_CUR }, { "amp_dig_vol", AMP_DIG_VOL },
		{ "amp_gain_ctrl", AMP_GAIN_CTRL }, { "irq1_status1", IRQ1_STATUS1 },
	};
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
		if (!i && !dry && (v & 0xffffff) != 0x35a40) {
			printf("cs35l41 %#x: unexpected devid: stop\n", a->addr);
			return -1;
		}
		if (regs[i].r == PWR_CTRL2)
			a->pwr2_saved = v;
	}
	return 0;
}

int cs_config(struct cs_amp *a, int vol_db_down, int boost)
{
	if (vol_db_down < 20)
		vol_db_down = 20;	/* hard limit */
	uint32_t ipk = (DT_BST_PEAK_MA - 1600) / 50 + 0x10, cur;
	int err = 0;
	/* boost peak current: only lower it, never above the DT's 3700 mA */
	if (!rd(a, BST_PEAK_CUR, &cur) && (dry || (cur & 0x7f) > ipk))
		err |= upd(a, BST_PEAK_CUR, 0x7f, ipk);
	if (boost) {
		/* 1000 nH (LBST code 0), 15 uF (cap range 0): cs35l41_bst_k1/k2/slope tables (45%) */
		err |= upd(a, BST_COEFF, 0xffff, 0x24 << 8 | 0x24);
		err |= upd(a, BST_SLOPE_LBST, 0xff03, 0x75 << 8 | 0);
	}
	/* clocks from BCLK 1.536 MHz (48 kHz x 16 bit x 2) */
	err |= upd(a, PLL_CLK_CTRL, 1u << 11, 1u << 11);		/* open loop */
	err |= upd(a, PLL_CLK_CTRL, 0x7e7, BCLK_CODE << 5 | 0);	/* freq, ref = BCLK */
	err |= upd(a, PLL_CLK_CTRL, 1u << 4, 0);
	err |= upd(a, PLL_CLK_CTRL, 1u << 4, 1u << 4);
	err |= upd(a, PLL_CLK_CTRL, 1u << 11, 0);			/* closed loop */
	err |= upd(a, GLOBAL_CLK_CTRL, 0x3f, 0x03);
	err |= upd(a, SP_RATE_CTRL, 0x3f, BCLK_CODE);
	/* ASP: I2S, 16-bit slots, RX1 from slot 0 (left) or 1 (right), 16-bit words, SDOUT hi-z 3 (DT) */
	err |= upd(a, SP_FORMAT, 0xffff0700, 16u << 24 | 16u << 16 | 2u << 8);
	err |= upd(a, SP_RX_SLOT, 0x3f, a->right ? 1 : 0);
	err |= upd(a, SP_RX_WL, 0x3f, 16);
	err |= upd(a, SP_HIZ_CTRL, 0x3, 3);
	err |= upd(a, SP_ENABLES, 1u << 16, 1u << 16);
	err |= upd(a, DAC_PCM1_SRC, 0x7f, 0x08);
	/* digital volume: -vol_db_down dB, 0.125 dB steps, 11-bit two's complement */
	uint32_t vol = (uint32_t)(0x800 - vol_db_down * 8) & 0x7ff;
	err |= upd(a, AMP_DIG_VOL, 0x7ff << 3, vol << 3);
	/* boost on, or bypass (VBST = VP through the boost FET); AMP_EN */
	err |= upd(a, PWR_CTRL2, 0x31, (boost ? 2u : 1u) << 4 | 1u);
	return err ? -1 : 0;
}

int cs_check(struct cs_amp *a)
{
	uint32_t s;
	if (rd(a, IRQ1_STATUS1, &s))
		return -1;
	if (s & IRQ_ERRORS) {
		printf("cs35l41 %#x: IRQ1 error bits %08x (status %08x)\n", a->addr, s & IRQ_ERRORS, s);
		return -1;
	}
	return 0;
}

int cs_power_up(struct cs_amp *a)
{
	wr(a, IRQ1_STATUS1, 0xffffffffu);
	if (upd(a, PWR_CTRL1, 1, 1))
		return -1;
	a->up = 1;
	for (int i = 0; i < 20 && !dry; i++) {
		uint32_t s;
		usleep(1000);
		if (rd(a, IRQ1_STATUS1, &s))
			return -1;
		if (s & IRQ_ERRORS) {
			printf("cs35l41 %#x: IRQ1 error bits %08x at power-up\n", a->addr, s & IRQ_ERRORS);
			return -1;
		}
		if (s & IRQ_PUP_DONE) {
			printf("cs35l41 %#x: PUP_DONE after %d ms\n", a->addr, i + 1);
			return 0;
		}
	}
	if (!dry)
		printf("cs35l41 %#x: no PUP_DONE in 20 ms\n", a->addr);
	return dry ? 0 : -1;
}

void cs_power_down(struct cs_amp *a)
{
	upd(a, PWR_CTRL1, 1, 0);
	for (int i = 0; i < 100 && !dry; i++) {
		uint32_t s;
		usleep(1000);
		if (rd(a, IRQ1_STATUS1, &s) || (s & IRQ_PDN_DONE))
			break;
	}
	wr(a, IRQ1_STATUS1, 0xffffffffu);
	upd(a, PWR_CTRL2, 0x31, a->pwr2_saved & 0x31);
	a->up = 0;
	printf("cs35l41 %#x: powered down\n", a->addr);
}
