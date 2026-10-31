/* sp25: host test of cs_standby / cs_resume_check on a fake amp: only GLOBAL_EN moves, the configuration stays. */
#include <stdio.h>
#include <string.h>
#include "cs35l41.c"

#define CHECK(c) do { int ok_ = (c); printf("%s %s\n", ok_ ? "ok  " : "FAIL", #c); fails += !ok_; } while (0)
#define NREG 4096
static struct { uint32_t reg, v; } m[NREG];
static int nm, fails, writes, cfg_writes;
static uint32_t last_reg;

static uint32_t *slot(uint32_t reg)
{
	for (int i = 0; i < nm; i++)
		if (m[i].reg == reg)
			return &m[i].v;
	m[nm].reg = reg;
	m[nm].v = 0;
	return &m[nm++].v;
}

int gi2c_init(void) { return 0; }
void gi2c_lock(void) {}
void gi2c_unlock(void) {}

int gi2c_msg(uint8_t addr, const uint8_t *tx, unsigned nt, uint8_t *rx, unsigned nr, int stop_stretch)
{
	(void)addr; (void)stop_stretch;
	if (nt >= 4)
		last_reg = (uint32_t)tx[0] << 24 | (uint32_t)tx[1] << 16 | (uint32_t)tx[2] << 8 | tx[3];
	if (nt == 8) {
		uint32_t v = (uint32_t)tx[4] << 24 | (uint32_t)tx[5] << 16 | (uint32_t)tx[6] << 8 | tx[7];
		writes++;
		if (last_reg == PWR_CTRL2 || last_reg == AMP_GAIN_CTRL || last_reg == AMP_DIG_VOL)
			cfg_writes++;
		if (last_reg == IRQ1_STATUS1) {	/* W1C */
			*slot(last_reg) &= ~v;
			return 0;
		}
		*slot(last_reg) = v;
		if (last_reg == PWR_CTRL1)
			*slot(IRQ1_STATUS1) |= v & 1 ? IRQ_PUP_DONE : IRQ_PDN_DONE;
	}
	if (nr == 4) {
		uint32_t v = *slot(last_reg);
		rx[0] = v >> 24; rx[1] = v >> 16; rx[2] = v >> 8; rx[3] = v;
	}
	return 0;
}

int main(void)
{
	struct cs_amp a = { .addr = 0x41, .out_mute = 1, .clk = 1 };
	cs_set_quiet(1);
	*slot(DEVID) = 0x35a40;
	*slot(REVID) = 0xb2;
	CHECK(!cs_probe(&a));
	CHECK(!cs_config(&a, -20, 0));
	CHECK(!cs_verify(&a));
	CHECK(!cs_power_up(&a) && a.up);
	uint32_t vol = *slot(AMP_DIG_VOL), pc2 = *slot(PWR_CTRL2);
	CHECK(vol && (pc2 & 1));

	cfg_writes = 0;
	CHECK(!cs_standby(&a));
	CHECK(!a.up && a.configured && a.verified);
	CHECK(!(*slot(PWR_CTRL1) & 1));
	CHECK(*slot(AMP_OUT_MUTE) & 0x10);
	CHECK(!cfg_writes && *slot(AMP_DIG_VOL) == vol && *slot(PWR_CTRL2) == pc2);
	CHECK(cs_standby(&a));	/* not on: refused */

	CHECK(!cs_resume_check(&a));
	writes = 0;
	CHECK(!cs_power_up(&a) && a.up);
	CHECK((*slot(PWR_CTRL1) & 1) && !(*slot(AMP_OUT_MUTE) & 0x10));
	printf("power-up from standby: %d writes, none to config: %d\n", writes, !cfg_writes);
	CHECK(!cfg_writes);
	CHECK(!cs_standby(&a));

	*slot(AMP_DIG_VOL) = vol ^ (1u << 3);	/* a lost volume: no power-up */
	CHECK(cs_resume_check(&a));
	*slot(AMP_DIG_VOL) = vol;

	cs_power_down(&a);	/* the full path after standby */
	CHECK(!a.configured && !a.verified && !a.up);
	CHECK(!(*slot(PWR_CTRL2) & 1));
	printf(fails ? "FAILED: %d\n" : "all ok\n", fails);
	return fails != 0;
}
