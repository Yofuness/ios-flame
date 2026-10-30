/*
 * TI DRV2624 LRA driver on SE9 @0x5a, reset = TLMM north gpio11 (haptics.md): the code drvbuzz and hapticd share
 * (gpio11 setup/restore, Android's init writes, RTP). Hard caps (in drv_wr): OD_CLAMP <= 0xA1, CONTROL1 bit7 (LRA)
 * never cleared, 0xFD-0xFF never touched, RTP <= drv_rtp_max. Lines are printed as "TAG: ..." (drv_tag); reads and
 * good writes only when drv_verbose. SE9 is shared: one user at a time (cs35l41, camid, i2cprobe, kern.sweet_camprobe).
 */
#ifndef DRV2624_H
#define DRV2624_H
#include <stdint.h>

#define DRV_R_ID	0x00
#define DRV_R_STATUS	0x01
#define DRV_R_MODE	0x07
#define DRV_R_CONTROL1	0x08
#define DRV_R_GO	0x0c
#define DRV_R_CONTROL2	0x0d
#define DRV_R_RTP	0x0e
#define DRV_R_OD_CLAMP	0x20
#define DRV_OD_CLAMP_MAX 0xa1
#define DRV_LEVEL_MAX	0x40	/* drvbuzz's cap (half scale); hapticd raises drv_rtp_max for taps */
#define DRV_RTP_HARD	0x60	/* no caller may set drv_rtp_max above this */

extern const char *drv_tag;	/* default "drvbuzz" */
extern int drv_verbose;		/* default 1 */
extern int drv_rtp_max;		/* default DRV_LEVEL_MAX, clamped to DRV_RTP_HARD */

int drv_pick_tile(void);	/* read-only: find gpio11 (TLMM north); 0 = usable */
int drv_pin_setup(void);	/* power-on CFG -> the DT output CFG (remembered for drv_cfg_restore) */
void drv_reset_low(void);	/* gpio11 low: chip in reset (no-op if not released) */
void drv_cfg_restore(void);	/* gpio11 CFG back to what it was */
int drv_power_up(void);		/* low 2 ms, high, 2 ms, ID check */
int drv_rd(unsigned reg, uint8_t *v);
int drv_wr(unsigned reg, uint8_t v);
int drv_wr_check(unsigned reg, uint8_t v, uint8_t mask);
void drv_dump(void);		/* 0x00-0x30 but STATUS */
int drv_init_chip(void);	/* Android's init writes, each read back */
int drv_buzz(int ms, int level);	/* RTP level for ms, stop, STATUS check */

#endif
