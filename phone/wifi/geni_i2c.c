/* Userland GENI I2C on SE9 (see geni_i2c.h); offsets and values as in sweet_camprobe.c. */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include "pmem.h"
#include "geni_i2c.h"

#define GCC_PA	0x00100000u
#define SE	0x00a8c000u

static void sw(uint32_t off, uint32_t v)
{
	wr32(SE + off, v);
}

static uint32_t sr(uint32_t off)
{
	return rd32(SE + off);
}

int gi2c_init(void)
{
	uint32_t vote = rd32(GCC_PA + 0x5200c), cbcr = rd32(GCC_PA + 0x183a4);
	printf("i2c: gcc_vote1 %#x, qupv3_wrap1_s3_cbcr %#x\n", vote, cbcr);
	if ((vote & ((0x0fu << 18) | (1u << 25))) != ((0x0fu << 18) | (1u << 25)) || (cbcr & (1u << 31))) {
		printf("i2c: SE9 clocks off (no GCC writes from here): stop\n");
		return -1;
	}
	uint32_t fw = sr(0x68), tx = sr(0xe24), rx = sr(0xe28), st = sr(0x40);
	printf("i2c: se9 fw %#x status %#x tx %#x rx %#x\n", fw, st, tx, rx);
	if (((fw >> 8) & 0xffu) != 3u || ((tx >> 24) & 0x3fu) != 32u || ((rx >> 24) & 0x3fu) != 32u ||
	    (st & 0x1001u)) {
		printf("i2c: SE9 has no I2C firmware or is busy: stop\n");
		return -1;
	}
	sw(0xe18, 0); sw(0xe1c, 0); sw(0x644, 0);
	sw(0x618, 0xffffffffu); sw(0x648, 0xffffffffu);
	sw(0x258, 0);
	sw(0x28, sr(0x28) | 0x7fu);
	sw(0xe30, sr(0xe30) | 0x0fu);
	sw(0x24, 0x7f); sw(0x20, 1);
	sw(0x7c, 0); sw(0x48, (7u << 4) | 1u);
	sw(0x278, (10u << 20) | (11u << 10) | 26u);	/* 100 kHz */
	sw(0x254, 0);
	sw(0x260, 0x0007f8feu); sw(0x264, 0x000ffefeu);
	sw(0x284, 0x0007f8feu); sw(0x288, 0x000ffefeu);
	sw(0x80c, 0); sw(0x810, 0);
	sw(0x814, ((rx >> 16) & 0x3fu) - 1u);
	return 0;
}

int gi2c_msg(uint8_t addr, const uint8_t *tx, unsigned nt, uint8_t *rx, unsigned nr, int stop_stretch)
{
	unsigned got = 0;
	sw(0x618, 0xffffffffu);
	sw(0x80c, nt ? 1u : 0u);
	sw(nr ? 0x270 : 0x26c, nr ? nr : nt);
	sw(0x600, ((nr ? 2u : 1u) << 27) | ((uint32_t)addr << 9) | (stop_stretch ? 4u : 0u));
	for (unsigned i = 0; i < 2000; i++) {
		uint32_t irq = sr(0x610);
		if (irq & (0x3eu | (3u << 9) | (3u << 12) | (3u << 24) | (3u << 28))) {
			sw(0x604, 2);	/* abort */
			usleep(1000);
			sw(0x618, 0xffffffffu);
			return (irq & (1u << 10)) ? ENXIO : EIO;
		}
		if (nt && (irq & (1u << 30))) {
			for (unsigned j = 0; j < nt; j += 4) {
				uint32_t w = 0;
				for (unsigned k = 0; k < 4 && j + k < nt; k++)
					w |= (uint32_t)tx[j + k] << (8 * k);
				sw(0x700, w);
			}
			sw(0x80c, 0);
			nt = 0;
		}
		for (uint32_t c = sr(0x804) & 0x1ffffffu; c && got < nr; c--) {
			uint32_t w = sr(0x780);
			for (unsigned j = 0; j < 4 && got < nr; j++)
				rx[got++] = (uint8_t)(w >> (8 * j));
		}
		if ((irq & 1u) && got == nr && !nt) {
			sw(0x618, irq);
			return 0;
		}
		sw(0x618, irq & ((3u << 26) | (1u << 30)));
		usleep(100);
	}
	sw(0x604, 2);
	return ETIMEDOUT;
}
