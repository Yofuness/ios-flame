/*
 * GENI I2C master on QUP SE9 (0xa8c000) from userland over /dev/pmem: the register sequence of the kernel's
 * sweet_touch.c / sweet_camprobe.c (0211). It never touches GCC: it refuses to run unless ABL left the
 * wrapper1/SE9 clocks voted and the I2C firmware loaded. Processes share SE9 through a flock on
 * GI2C_LOCK_PATH (every gi2c_msg and gi2c_init takes it; SUNFISH_SE9_NOLOCK=1 = off). The kernel's
 * kern.sweet_camprobe does not take it: don't run it meanwhile.
 */
#ifndef GENI_I2C_H
#define GENI_I2C_H
#include <stdint.h>

#define GI2C_LOCK_PATH		"/var/run/sunfish-se9.lock"
#define GI2C_LOCK_PATH_ALT	"/var/local/sunfish-se9.lock"	/* if /var/run isn't writable */

int gi2c_init(void);	/* 0 = SE9 ready (100 kHz) */
/* One message: nt bytes out, or nr bytes in (not both); stop_stretch = repeated start after it. 0 or errno. */
int gi2c_msg(uint8_t addr, const uint8_t *tx, unsigned nt, uint8_t *rx, unsigned nr, int stop_stretch);
/* Hold the SE9 lock across a multi-message sequence (recursive within a process). */
void gi2c_lock(void);
void gi2c_unlock(void);

#endif
