/*
 * sp23: test feeder for adspboot --audio-fifo: writes a 1 kHz sine (s16le stereo, 48 kHz) into the FIFO, paced in real
 * time like the HAL plug-in (cat would pour a file in faster than the ring takes it).
 *   af_feed FIFO SECONDS [--dbfs N] [--dc LEVEL]
 * --dbfs: peak, clamped -40..-6, default -20. --dc: a constant LEVEL instead of the sine (trips the DC guard).
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int dbfs = -20, dc = 0, use_dc = 0;
	if (argc < 3) {
		fprintf(stderr, "usage: af_feed FIFO SECONDS [--dbfs N] [--dc LEVEL]\n");
		return 2;
	}
	for (int i = 3; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "--dbfs"))
			dbfs = atoi(argv[i + 1]);
		else if (!strcmp(argv[i], "--dc")) {
			dc = atoi(argv[i + 1]);
			use_dc = 1;
		}
	}
	dbfs = dbfs > -6 ? -6 : dbfs < -40 ? -40 : dbfs;
	int fd = open(argv[1], O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		printf("af_feed: %s: %s\n", argv[1], strerror(errno));
		return 1;
	}
	int16_t tab[48], buf[480 * 2];
	for (int i = 0; i < 48; i++)
		tab[i] = use_dc ? (int16_t)dc : (int16_t)lrint(32767.0 * pow(10, dbfs / 20.0) * sin(2 * M_PI * i / 48));
	long periods = (long)(atof(argv[2]) * 100), lost = 0;
	unsigned at = 0;
	struct timespec next;
	clock_gettime(CLOCK_MONOTONIC, &next);
	for (long p = 0; p < periods; p++) {
		for (int f = 0; f < 480; f++, at++)
			buf[2 * f] = buf[2 * f + 1] = tab[at % 48];
		for (size_t off = 0; off < sizeof(buf); off += PIPE_BUF) {
			size_t n = sizeof(buf) - off < PIPE_BUF ? sizeof(buf) - off : PIPE_BUF;
			if (write(fd, (char *)buf + off, n) < 0) {
				lost += (long)(sizeof(buf) - off) / 4;	/* full: the rest of the period dropped */
				break;
			}
		}
		next.tv_nsec += 10000000;
		if (next.tv_nsec >= 1000000000) {
			next.tv_nsec -= 1000000000;
			next.tv_sec++;
		}
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		long us = (next.tv_sec - now.tv_sec) * 1000000 + (next.tv_nsec - now.tv_nsec) / 1000;
		if (us > 0)
			usleep((useconds_t)us);
	}
	printf("af_feed: %ld periods, %ld frames not taken\n", periods, lost);
	return 0;
}
