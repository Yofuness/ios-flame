/*
 * Read-only I2C probe on QUP SE9 (geni_i2c.c): writes the register address, then reads N bytes. Never writes
 * register data. For the camera PMIC slg51000 @0x75 (16-bit registers) and the CS35L41 amps (32-bit registers).
 *   i2cprobe ADDR REGBYTES REG [N]     e.g. i2cprobe 0x75 2 0x0000 8
 *   i2cprobe scan                      ACK scan of 0x08-0x77 (a 1-byte read of register 0 at each address)
 * Needs SE9 clocks + pins (boot with sweetse9=2). Don't run while another SE9 user is active.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "geni_i2c.h"
#include "pmem.h"

static int rd(unsigned addr, unsigned rb, unsigned reg, uint8_t *out, unsigned n)
{
	uint8_t a[4];
	for (unsigned i = 0; i < rb; i++)
		a[i] = (uint8_t)(reg >> (8 * (rb - 1 - i)));
	gi2c_lock();	/* write-addr + read as one locked sequence (SE9 lock) */
	int e = gi2c_msg((uint8_t)addr, a, rb, NULL, 0, 1);
	if (!e)
		e = gi2c_msg((uint8_t)addr, NULL, 0, out, n, 0);
	gi2c_unlock();
	return e;
}

int main(int argc, char **argv)
{
	if (argc < 2 || (strcmp(argv[1], "scan") && argc < 4)) {
		fprintf(stderr, "usage: i2cprobe ADDR REGBYTES REG [N] | i2cprobe scan\n");
		return 2;
	}
	pm_init();
	if (gi2c_init())
		return 1;
	uint8_t b[64];
	if (!strcmp(argv[1], "scan")) {
		for (unsigned a = 0x08; a < 0x78; a++) {
			uint8_t z = 0;
			gi2c_lock();
			int ok = gi2c_msg((uint8_t)a, &z, 1, NULL, 0, 1) == 0 && gi2c_msg((uint8_t)a, NULL, 0, b, 1, 0) == 0;
			gi2c_unlock();
			if (ok)
				printf("i2cprobe: 0x%02x answers (reg 0 = %02x)\n", a, b[0]);
		}
		return 0;
	}
	unsigned addr = strtoul(argv[1], NULL, 0), rb = strtoul(argv[2], NULL, 0), reg = strtoul(argv[3], NULL, 0);
	unsigned n = argc > 4 ? strtoul(argv[4], NULL, 0) : 1;
	if (addr > 0x7f || rb < 1 || rb > 4 || n < 1 || n > sizeof(b))
		return 2;
	int e = rd(addr, rb, reg, b, n);
	if (e) {
		printf("i2cprobe: 0x%02x reg %#x: err %d (no ACK?)\n", addr, reg, e);
		return 1;
	}
	printf("i2cprobe: 0x%02x reg %#x:", addr, reg);
	for (unsigned i = 0; i < n; i++)
		printf(" %02x", b[i]);
	printf("\n");
	return 0;
}
