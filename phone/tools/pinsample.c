/*
 * pinsample: read a 32-bit register through /dev/pmem many times as fast as possible and count how often each of the
 * low 4 bits is set and how often bit 0 changed (sp15: does the LPI SD0 / BCLK / WS pin toggle during a tone?).
 *   pinsample PA [COUNT]     (COUNT default 200000; read-only)
 * Prints: reads, seconds, ones per bit 0-3, bit-0 transitions, distinct values seen (up to 8).
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: pinsample PA [COUNT]\n");
		return 2;
	}
	uint64_t pa = strtoull(argv[1], NULL, 0);
	long n = argc > 2 ? strtol(argv[2], NULL, 0) : 200000;
	int fd = open("/dev/pmem", O_RDONLY);
	if (fd < 0) {
		perror("/dev/pmem");
		return 1;
	}
	long ones[4] = { 0 }, trans = 0;
	uint32_t vals[8], prev = 0;
	int nv = 0;
	struct timeval t0, t1;
	gettimeofday(&t0, NULL);
	for (long i = 0; i < n; i++) {
		uint32_t v;
		if (pread(fd, &v, 4, (off_t)pa) != 4) {
			perror("pread");
			return 1;
		}
		for (int b = 0; b < 4; b++)
			ones[b] += (v >> b) & 1;
		if (i && ((v ^ prev) & 1))
			trans++;
		prev = v;
		int seen = 0;
		for (int k = 0; k < nv; k++)
			seen |= vals[k] == v;
		if (!seen && nv < 8)
			vals[nv++] = v;
	}
	gettimeofday(&t1, NULL);
	double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
	printf("pinsample %#llx: %ld reads in %.3f s; ones bit0 %ld bit1 %ld bit2 %ld bit3 %ld; bit0 transitions %ld; values",
	       (unsigned long long)pa, n, s, ones[0], ones[1], ones[2], ones[3], trans);
	for (int k = 0; k < nv; k++)
		printf(" %08x", vals[k]);
	printf("\n");
	return 0;
}
