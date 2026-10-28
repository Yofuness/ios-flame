/*
 * GENI I2C master on QUP SE9 (0xa8c000) from userland over /dev/pmem: the register sequence of the kernel's
 * sweet_touch.c / sweet_camprobe.c (0211). It never touches GCC: it refuses to run unless ABL left the
 * wrapper1/SE9 clocks voted and the I2C firmware loaded. Only one user of SE9 at a time (don't run
 * kern.sweet_camprobe meanwhile).
 */
#ifndef GENI_I2C_H
#define GENI_I2C_H
#include <stdint.h>

int gi2c_init(void);	/* 0 = SE9 ready (100 kHz) */
/* One message: nt bytes out, or nr bytes in (not both); stop_stretch = repeated start after it. 0 or errno. */
int gi2c_msg(uint8_t addr, const uint8_t *tx, unsigned nt, uint8_t *rx, unsigned nr, int stop_stretch);

#endif
