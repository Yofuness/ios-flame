/*
 * CS35L41 smart amp over SE9 I2C (0x40 left/bottom speaker, 0x41 right): bring-up for a short test tone, no
 * DSP firmware, no speaker protection. Sequence: research/notes/cs35l41-plan.md. Hard limits (enforced here, not
 * by the caller; sp1): only 0x40 is ever written, boost bypass only (VBST = VP), class-D gain 0.5 dB, digital
 * volume at most -20 dB, GLOBAL_EN only after a clean read-back of every configured field, any IRQ1 error bit
 * shuts the amp down.
 */
#ifndef CS35L41_H
#define CS35L41_H
#include <stdint.h>

#define CS_MAX_EXP	32

struct cs_exp {
	uint32_t reg, mask, val;
};

struct cs_amp {
	uint8_t addr;
	int right;		/* takes ASP RX slot 1 (unused: 0x41 is never written) */
	int amp_gain;		/* sp3: analog PCM gain steps after the errata (0.5 dB + 1 dB/step), clamped 0..10 */
	uint32_t gain_saved;	/* AMP_GAIN_CTRL before us, restored by cs_power_down */
	uint32_t err_bits;	/* IRQ1 error bits seen */
	int probed, configured, verified, up;
	int nexp;
	struct cs_exp exp[CS_MAX_EXP];	/* read-back list */
};

void cs_set_dry(int dry);
int cs_probe(struct cs_amp *a);				/* IDs + state, read-only; 0 = devid/revid B2/GLOBAL_EN 0 ok */
int cs_config(struct cs_amp *a, int vol_db, int boost);	/* errata, OTP trims (sp6), ASP I2S 48k/16, clocks, volume (dB, <= -20), AMP_EN;
							 * boost is ignored (bypass only) */
int cs_verify(struct cs_amp *a);			/* read back every configured field; 0 = all match */
int cs_power_up(struct cs_amp *a);			/* pup patch, GLOBAL_EN, 0 = PUP_DONE; needs cs_verify ok */
int cs_read(struct cs_amp *a, uint32_t reg, uint32_t *v);	/* sp2: read-only, logged (e.g. CLOCK_DETECT_1) */
int cs_write(struct cs_amp *a, uint32_t reg, uint32_t v);	/* sp7: one logged write (0x40 only), no read-back list */
void cs_diag(struct cs_amp *a, const char *tag);		/* sp6: RAW IRQ1 1-4, clocks, ASP, PWR_CTRL3; read-only */
int cs_check(struct cs_amp *a);				/* 0 = no IRQ1 error bit */
void cs_power_down(struct cs_amp *a);			/* GLOBAL_EN 0, PDN_DONE, pdn patch, AMP_EN 0, gain back */

#endif
