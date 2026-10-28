/*
 * CS35L41 smart amp over SE9 I2C (0x40 left/bottom speaker, 0x41 right): bring-up for a short test tone, no
 * DSP firmware, no speaker protection. Register map from memory of Linux sound/soc/codecs/cs35l41*.c; the
 * confidence of each register is in cs35l41.c. Hard limits (enforced here, not by the caller): digital volume
 * at least 20 dB down, boost peak current at most the DT's 3700 mA, the class-D gain (AMP_GAIN_CTRL) is
 * only read, any IRQ1 error bit shuts the amp down.
 */
#ifndef CS35L41_H
#define CS35L41_H
#include <stdint.h>

struct cs_amp {
	uint8_t addr;
	int right;		/* takes ASP RX slot 1 */
	uint32_t pwr2_saved;	/* PWR_CTRL2 before us */
	int up;
};

void cs_set_dry(int dry);
int cs_probe(struct cs_amp *a);				/* IDs + state, read-only; 0 = it answers */
int cs_config(struct cs_amp *a, int vol_db_down, int boost);	/* ASP I2S 48k/16, clocks, volume, boost */
int cs_power_up(struct cs_amp *a);			/* 0 = PUP_DONE, no error bits */
int cs_check(struct cs_amp *a);				/* 0 = no IRQ1 error bit */
void cs_power_down(struct cs_amp *a);

#endif
