/*
 * CS35L41 bring-up (see cs35l41.h). 32-bit big-endian register addresses and values. Sequence and registers from
 * research/notes/cs35l41-plan.md (Linux cs35l41.c / cs35l41-lib.c / cs35l41.h, master). sp1: boost bypass only
 * (VBST = VP), no boost code; class-D gain 0 (0.5 dB) via the B2 errata, digital volume never above -20 dB; every
 * configured field is read back before GLOBAL_EN can be set; only amp 0x40 is ever written.
 * sp8: AMP_OUT_MUTE handling, SP_RATE_CTRL / TST_FS_MON0 and the optional internal boost follow the sequence of
 * ios-redfin (DenialOfGravity/ios-redfin, MIT; xnu patch 0953 configure() / sweet_amp_start()), checked against Linux.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "geni_i2c.h"
#include "cs35l41.h"

#define DEVID		0x00000000	/* 0x35a40 */
#define REVID		0x00000004	/* [7:0] 0xb2 = B2 */
#define OTPID		0x00000010	/* sp6: selects the OTP map for the trim unpack */
#define OTP_MEM0	0x00000400	/* sp6: 32 words of packed trims */
#define OTP_WORDS	32
#define TEST_KEY_CTL	0x00000040	/* 0x55, 0xaa unlock; 0xcc, 0x33 lock */
#define PWR_CTRL1	0x00002014	/* bit0 GLOBAL_EN */
#define PWR_CTRL2	0x00002018	/* bit0 AMP_EN, [5:4] BST_EN 0 off / 1 bypass (FET on, VBST = VP) / 2 boost */
#define PWR_CTRL3	0x0000201c	/* sp6: was 0x2028 (wrong) */
#define AMP_OUT_MUTE	0x00002024	/* sp8: bit4 OUT_MUTE */
#define PROTECT_REL	0x00002034	/* error release bits */
#define PUP_PDN_PATCH	0x00002084	/* 0x002f1aa0 before GLOBAL_EN, 0x002f1aa3 after power-down (unlocked) */
#define PLL_CLK_CTRL	0x00002c04	/* [2:0] REFCLK_SEL 0 = BCLK, bit4 REFCLK_EN, [10:5] FREQ, bit11 OPEN_LOOP */
#define GLOBAL_CLK_CTRL	0x00002c0c	/* [4:0] GLOBAL_FS, 48 kHz = 0x03 */
#define TST_FS_MON0	0x00002d10	/* sp8: FS1 window [10:0], FS2 [23:12]; test register (unlocked) */
#define BST_PEAK_CUR	0x00003808	/* sp8: [6:0] IPK */
#define BST_COEFF	0x00003810	/* sp8: K1 [7:0], K2 [15:8] */
#define BST_SLOPE_LBST	0x00003814	/* sp8: slope [15:8], LBST [1:0] */
#define SP_RATE_CTRL	0x00004804	/* sp8: [5:0] ASP_BCLK_FREQ */
#define SP_ENABLES	0x00004800	/* bit16 ASP_RX1_EN */
#define SP_FORMAT	0x00004808	/* RX width [31:24], TX width [23:16], FMT [10:8] 2 = I2S, masters bits 0/4, inv 2/6 */
#define SP_HIZ_CTRL	0x0000480c	/* [1:0] = DT cirrus,asp-sdout-hiz */
#define SP_RX_SLOT	0x00004820	/* RX1 slot [5:0] */
#define SP_RX_WL	0x00004840	/* [5:0] word length */
#define DAC_PCM1_SRC	0x00004c00	/* [6:0] 0x08 = ASPRX1 (DSP bypass) */
#define AMP_DIG_VOL	0x00006000	/* [13:3] signed, 0.125 dB, 0 = 0 dB; bit15 kept (reset value) */
#define AMP_GAIN_CTRL	0x00006c04	/* PCM gain [9:5]; 0 = 0.5 dB (B2 errata), reset value restored at the end */
#define AMP_GAIN_MAX	10		/* sp3: 10.5 dB ceiling for --amp-gain */
#define IRQ1_STATUS1	0x00010010	/* W1C */
#define IRQ1_STATUS3	0x00010018	/* sp2: bit1 PLL_LOCK */
#define IRQ1_STATUS4	0x0001001c
#define IRQ3_PLL_LOCK	(1u << 1)
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

int cs_read(struct cs_amp *a, uint32_t reg, uint32_t *v)
{
	return rd(a, reg, v);
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

/* sp7: --rec ASP TX routing writes from adspboot; logged, 0x40 only like every write */
int cs_write(struct cs_amp *a, uint32_t reg, uint32_t v)
{
	return wr(a, reg, v);
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

/*
 * sp6: OTP trim unpack (facts from Linux cs35l41-lib.c otp_map_1/otp_map_2 and cs35l41_otp_unpack): the fuses at
 * OTP_MEM0 hold bit fields packed LSB-first from word 2 bit 16; each field goes into {reg, shift, size}; reg 0 =
 * padding (skipped). Map 2 = map 1 with VMON_POL (0x4000 bit 11) in place of the padding bit. OTPID 1/8 -> map 1,
 * 2/3/6 -> map 2; other ids: skipped. Runs unlocked, right after the B2 errata. CS35L41_NO_OTP=1 turns it off.
 */
struct otp_field {
	uint32_t reg;
	uint8_t shift, size;
};

static const struct otp_field otp_fields[] = {
	{ 0x2030, 0, 4 }, { 0x2030, 7, 1 }, { 0x208c, 24, 6 }, { 0x2090, 14, 4 }, { 0x2090, 10, 4 },
	{ 0x300c, 11, 4 }, { 0x394c, 23, 2 },
	{ 0x3950, 0, 7 }, { 0x3950, 8, 7 }, { 0x3950, 16, 8 }, { 0x3950, 24, 8 },
	{ 0x3954, 0, 7 }, { 0x3954, 8, 7 }, { 0x3954, 16, 8 }, { 0x3954, 24, 8 },
	{ 0x3958, 0, 7 }, { 0x3958, 8, 7 }, { 0x3958, 16, 8 }, { 0x3958, 24, 8 },
	{ 0x395c, 0, 7 }, { 0x395c, 8, 7 }, { 0x395c, 16, 8 }, { 0x395c, 24, 8 },
	{ 0x416c, 0, 8 }, { 0x4160, 0, 7 }, { 0x416c, 8, 8 }, { 0x4160, 16, 10 }, { 0x416c, 16, 12 },
	{ 0x416c, 28, 1 },
	{ 0x4170, 0, 6 }, { 0x4170, 6, 1 }, { 0x4170, 8, 6 }, { 0x4170, 14, 1 }, { 0x4170, 16, 9 },
	{ 0x4360, 0, 5 }, { 0x4360, 6, 9 },
	{ 0x4448, 0, 8 }, { 0x4448, 8, 8 }, { 0x4448, 16, 8 }, { 0x4448, 24, 8 }, { 0x444c, 0, 3 },
	{ 0x6e30, 0, 5 }, { 0x6e30, 8, 5 }, { 0x6e30, 16, 5 }, { 0x6e30, 24, 5 },
	{ 0x6e34, 0, 5 }, { 0x6e34, 8, 5 }, { 0x6e34, 16, 5 }, { 0x6e34, 24, 5 },
	{ 0x6e38, 0, 5 }, { 0x6e38, 8, 5 }, { 0x6e38, 16, 5 }, { 0x6e38, 24, 5 },
	{ 0x6e3c, 0, 5 }, { 0x6e3c, 8, 5 }, { 0x6e3c, 16, 5 }, { 0x6e3c, 24, 5 },
	{ 0x6e40, 0, 5 }, { 0x6e40, 8, 5 }, { 0x6e40, 16, 5 }, { 0x6e40, 24, 5 }, { 0x6e44, 0, 5 },
	{ 0x6e48, 0, 10 }, { 0x6e48, 10, 10 }, { 0x6e48, 20, 10 }, { 0x6e4c, 0, 10 }, { 0x6e4c, 10, 10 },
	{ 0x6e4c, 20, 10 }, { 0x6e50, 0, 10 }, { 0x6e50, 10, 10 }, { 0x6e50, 20, 10 }, { 0x6e54, 0, 10 },
	{ 0x6e54, 10, 10 }, { 0x6e54, 20, 10 }, { 0x6e58, 0, 10 }, { 0x6e58, 10, 10 }, { 0x6e58, 20, 10 },
	{ 0x6e5c, 0, 10 }, { 0x6e5c, 10, 10 }, { 0x6e5c, 20, 10 }, { 0x6e60, 0, 10 }, { 0x6e60, 10, 10 },
	{ 0x6e60, 20, 10 }, { 0x6e64, 0, 10 },
	{ 0x7418, 7, 5 }, { 0x741c, 0, 5 }, { 0x741c, 11, 4 }, { 0x741c, 19, 4 }, { 0x7434, 17, 1 },
	{ 0x7434, 18, 7 }, { 0x7068, 0, 9 }, { 0x410c, 7, 1 }, { 0x400c, 0, 7 },
	{ 0x0000, 0, 1 },	/* OTP_PAD: map 1 padding bit, map 2 VMON_POL */
	{ 0x17040, 0, 8 }, { 0x17040, 8, 8 }, { 0x17040, 16, 8 }, { 0x17040, 24, 8 }, { 0x17044, 0, 24 },
};
#define OTP_PAD		93	/* index of the map 1 / map 2 difference */

/* call unlocked; 0 = done or skipped (unknown id, CS35L41_NO_OTP), -1 = I2C error */
static int otp_unpack(struct cs_amp *a)
{
	uint32_t id, mem[OTP_WORDS];
	if (getenv("CS35L41_NO_OTP")) {
		printf("cs35l41 %#x: otp: skipped (CS35L41_NO_OTP)\n", a->addr);
		return 0;
	}
	if (rd(a, OTPID, &id))
		return -1;
	int map = id == 1 || id == 8 ? 1 : id == 2 || id == 3 || id == 6 ? 2 : 0;
	if (!map) {
		printf("cs35l41 %#x: otp: no map for OTPID %#x: skipped (trims stay default)\n", a->addr, id);
		return 0;
	}
	for (int i = 0; i < OTP_WORDS; i++)
		if (rd(a, OTP_MEM0 + 4 * i, &mem[i]))
			return -1;
	int n = sizeof(otp_fields) / sizeof(otp_fields[0]), nw = 0, err = 0;
	printf("cs35l41 %#x: otp: OTPID %#x -> map %d (Linux otp_map_%d), %d fields\n", a->addr, id, map, map, n);
	unsigned pos = 2 * 32 + 16;	/* word 2, bit 16 */
	for (int i = 0; i < n; i++) {
		struct otp_field f = otp_fields[i];
		if (i == OTP_PAD && map == 2)
			f = (struct otp_field){ 0x4000, 11, 1 };	/* VMON_POL */
		unsigned w = pos / 32, b = pos % 32;
		pos += f.size;
		if (w >= OTP_WORDS || (b + f.size > 32 && w + 1 >= OTP_WORDS)) {
			printf("cs35l41 %#x: otp: field %d past the end: stop\n", a->addr, i);
			return -1;
		}
		uint64_t win = mem[w] | (b + f.size > 32 ? (uint64_t)mem[w + 1] << 32 : 0);
		uint32_t fm = (uint32_t)((1ull << f.size) - 1);
		uint32_t val = (uint32_t)(win >> b) & fm, mask = fm << f.shift, v;
		if (!f.reg)
			continue;	/* padding */
		if (rd(a, f.reg, &v))
			return -1;
		printf("cs35l41 %#x: otp: trim %08x mask %08x val %08x\n", a->addr, f.reg, mask, val << f.shift);
		err |= wr(a, f.reg, (v & ~mask) | (val << f.shift));
		nw++;
	}
	printf("cs35l41 %#x: otp: %d trim fields written%s\n", a->addr, nw, err ? " (write errors)" : "");
	return err ? -1 : 0;
}

/* sp8: AMP_OUT_MUTE read and logged */
void cs_out_mute(struct cs_amp *a, const char *tag)
{
	uint32_t v;
	if (!rd(a, AMP_OUT_MUTE, &v))
		printf("cs35l41 %#x: AMP_OUT_MUTE %s %08x (bit4 %s)\n", a->addr, tag, v, v & 0x10 ? "muted" : "clear");
}

int cs_config(struct cs_amp *a, int vol_db, int boost)
{
	if (!a->probed || a->addr != AMP_ADDR) {
		printf("cs35l41 %#x: config refused (%s)\n", a->addr, a->probed ? "only 0x40" : "not probed");
		return -1;
	}
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
	err |= otp_unpack(a);	/* sp6: still unlocked, as Linux */
	err |= lock(a);
	/* sp3: analog PCM gain AMP_GAIN_CTRL[9:5] (Linux cs35l41.h AMP_GAIN_PCM_MASK 0x3e0), 0.5 dB + 1 dB per step,
	 * hard clamp 0..10 (<= 10.5 dB; stock 19 = 19.5 dB) */
	int g = a->amp_gain < 0 ? 0 : a->amp_gain > AMP_GAIN_MAX ? AMP_GAIN_MAX : a->amp_gain;
	printf("cs35l41 %#x: analog gain %d (%d.5 dB)\n", a->addr, g, g);
	err |= upd(a, AMP_GAIN_CTRL, 0x3e0, (uint32_t)g << 5);
	if (boost) {
		/*
		 * sp8 (--amp-boost): internal boost, Linux cs35l41-lib.c cs35l41_boost_config() for the stock DT
		 * (live.dts: boost-ind-nanohenry 1000 -> LBST 0, boost-cap-microfarad 15 -> range 0, boost-peak-milliamp
		 * 3700): k1_table[0][0] 0x24, k2_table[0][0] 0x24, slope_table[0] 0x75, IPK (3700 - 1600) / 50 + 0x10 =
		 * 0x3a; then BST_EN 2. Same values as ios-redfin 0953 configure().
		 */
		printf("cs35l41 %#x: boost: K1 0x24 K2 0x24 slope 0x75 LBST 0 IPK 0x3a (3700 mA), BST_EN 2\n", a->addr);
		err |= upd(a, BST_COEFF, 0xffff, 0x24 | 0x24 << 8);
		err |= upd(a, BST_SLOPE_LBST, 0xff03, 0x75 << 8 | 0);
		err |= upd(a, BST_PEAK_CUR, 0x7f, (3700 - 1600) / 50 + 0x10);
		err |= upd(a, PWR_CTRL2, 0x30, 2u << 4);
	} else	/* boost bypass: FET on, VBST = VP */
		err |= upd(a, PWR_CTRL2, 0x30, 1u << 4);
	if (a->clk) {	/* sp8: SP_RATE_CTRL before the PLL, as Linux cs35l41_component_set_sysclk / ios-redfin 0953 */
		uint32_t v;
		err |= upd(a, SP_RATE_CTRL, 0x3f, BCLK_CODE);	/* our 1.536 MHz BCLK code (redfin's 0x28 is 6.144 MHz) */
		if (!rd(a, SP_RATE_CTRL, &v))
			printf("cs35l41 %#x: clk: SP_RATE_CTRL read back %08x\n", a->addr, v);
	}
	/* PLL from BCLK 1.536 MHz (48 kHz x 16 bit x 2), cs35l41.c:841-856 */
	err |= upd(a, PLL_CLK_CTRL, 1u << 11, 1u << 11);		/* open loop */
	err |= upd(a, PLL_CLK_CTRL, 0x7e0, BCLK_CODE << 5);		/* freq */
	err |= upd(a, PLL_CLK_CTRL, 1u << 4, 0);
	err |= upd(a, PLL_CLK_CTRL, 0x7, 0);				/* ref = BCLK */
	err |= upd(a, PLL_CLK_CTRL, 1u << 4, 1u << 4);
	err |= upd(a, PLL_CLK_CTRL, 1u << 11, 0);			/* closed loop */
	err |= upd(a, GLOBAL_CLK_CTRL, 0x1f, 0x03);			/* 48 kHz; SP_RATE_CTRL: sp8 --amp-clk */
	if (a->clk > 1) {	/* sp8: FS monitor (--amp-fsmon only: 16|36<<12 is Linux's >6 MHz value, ours is 1.536 MHz), test keys around it (Linux cs35l41_dai_set_sysclk, ios-redfin 0953) */
		uint32_t v;
		err |= unlock(a);
		err |= set(a, TST_FS_MON0, 16 | 36u << 12);
		if (!rd(a, TST_FS_MON0, &v))
			printf("cs35l41 %#x: clk: TST_FS_MON0 read back %08x\n", a->addr, v);
		err |= lock(a);
	}
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
	if (a->out_mute) {	/* sp8: output muted until 2 ms after GLOBAL_EN (ios-redfin 0953 configure()) */
		cs_out_mute(a, "before set");
		err |= upd(a, AMP_OUT_MUTE, 0x10, 0x10);
		cs_out_mute(a, "after set");
	}
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
	int t0 = 0;
	if (a->out_mute) {	/* sp8: unmute 2 ms after GLOBAL_EN, as ios-redfin 0953 sweet_amp_start() */
		if (!dry)
			usleep(2000);
		if (upd(a, AMP_OUT_MUTE, 0x10, 0))
			return -1;
		cs_out_mute(a, "after clear");
		t0 = 2;
	}
	int pll_ms = 0;	/* sp2: STATUS3 bit1 PLL_LOCK polled alongside PUP_DONE (lx/cs35l41.h:707) */
	uint32_t s = 0, s3 = 0;
	for (int i = t0; i < 100 && !dry; i++) {
		usleep(1000);
		if (rd(a, IRQ1_STATUS1, &s) || rd(a, IRQ1_STATUS3, &s3))
			return -1;
		if ((s3 & IRQ3_PLL_LOCK) && !pll_ms)
			printf("cs35l41 %#x: PLL_LOCK first after %d ms\n", a->addr, pll_ms = i + 1);
		if (s & IRQ_ERRORS) {
			printf("cs35l41 %#x: IRQ1 error bits %08x at power-up\n", a->addr, s & IRQ_ERRORS);
			a->err_bits |= s & IRQ_ERRORS;
			return -1;
		}
		if (s & IRQ_PUP_DONE) {
			printf("cs35l41 %#x: PUP_DONE after %d ms (STATUS1 %08x STATUS3 %08x, PLL_LOCK %s)\n", a->addr, i + 1,
			       s, s3, pll_ms ? "seen" : "never");
			return wr(a, IRQ1_STATUS1, IRQ_PUP_DONE) ? -1 : 0;
		}
	}
	if (!dry)
		printf("cs35l41 %#x: no PUP_DONE in 100 ms (STATUS1 %08x STATUS3 %08x, PLL_LOCK %s)\n", a->addr, s, s3,
		       pll_ms ? "seen" : "never");
	return dry ? 0 : -1;
}

/* sp6: on-window snapshot, read-only. RAW bit names from Linux cs35l41.h (it names no LRCLK/BCLK/FS error bits) */
void cs_diag(struct cs_amp *a, const char *tag)
{
	static const struct { const char *n; uint32_t r; } regs[] = {
		{ "raw1", 0x10090 }, { "raw2", 0x10094 }, { "raw3", 0x10098 }, { "raw4", 0x1009c },
		{ "clock_detect_1", 0x14000 }, { "ng_cfg", 0x6808 }, { "mixer_ngate_ch1", 0x12004 },
		{ "sp_enables", SP_ENABLES }, { "sp_format", SP_FORMAT }, { "sp_rx_wl", SP_RX_WL }, { "pwr_ctrl3", PWR_CTRL3 },
	};
	uint32_t v[sizeof(regs) / sizeof(regs[0])] = { 0 };
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
		if (!rd(a, regs[i].r, &v[i]))
			printf("cs35l41 %#x: diag %s %s %08x = %08x\n", a->addr, tag, regs[i].n, regs[i].r, v[i]);
	printf("cs35l41 %#x: diag %s decode:%s%s%s%s%s%s%s%s%s%s%s\n", a->addr, tag,
	       v[0] & IRQ_AMP_SHORT ? " AMP_SHORT" : "", v[0] & IRQ_TEMP_ERR ? " TEMP_ERR" : "",
	       v[0] & IRQ_TEMP_WARN ? " TEMP_WARN" : "", v[0] & IRQ_BST_SHORT ? " BST_SHORT" : "",
	       v[0] & IRQ_BST_UVP ? " BST_DCM_UVP" : "", v[0] & IRQ_BST_OVP ? " BST_OVP" : "",
	       v[0] & IRQ_PUP_DONE ? " PUP_DONE" : "", v[0] & IRQ_PDN_DONE ? " PDN_DONE" : "",
	       v[2] & IRQ3_PLL_LOCK ? " PLL_LOCK" : "", v[2] & 0x10 ? " PLL_UNLOCK" : "",
	       v[3] & 0x80000000u ? " OTP_BOOT_ERR" : v[3] & 0x2 ? " OTP_BOOT_DONE" : "");
}

void cs_power_down(struct cs_amp *a)
{
	if (a->addr != AMP_ADDR || !a->configured)
		return;
	if (a->up) {
		if (a->out_mute)	/* sp8: mute before GLOBAL_EN 0, as ios-redfin 0953 */
			upd(a, AMP_OUT_MUTE, 0x10, 0x10);
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
