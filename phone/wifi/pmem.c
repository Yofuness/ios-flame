#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pmem.h"

static int pm_fd = -1;

void pm_init(void)
{
	if (pm_fd >= 0)
		return;
	pm_fd = open("/dev/pmem", O_RDWR);
	if (pm_fd < 0) {
		fprintf(stderr, "/dev/pmem: %s (kernel 0094+, boot-arg sweetpmem=1, run as root)\n",
			strerror(errno));
		exit(1);
	}
}

int pm_rd(uint64_t pa, void *buf, size_t len)
{
	ssize_t n = pread(pm_fd, buf, len, (off_t)pa);
	if (n != (ssize_t)len) {
		fprintf(stderr, "pmem: read %zu at %llx: %s\n", len, (unsigned long long)pa,
			n < 0 ? strerror(errno) : "short");
		return -1;
	}
	return 0;
}

int pm_wr(uint64_t pa, const void *buf, size_t len)
{
	ssize_t n = pwrite(pm_fd, buf, len, (off_t)pa);
	if (n != (ssize_t)len) {
		fprintf(stderr, "pmem: write %zu at %llx: %s\n", len, (unsigned long long)pa,
			n < 0 ? strerror(errno) : "short");
		return -1;
	}
	return 0;
}

uint32_t rd32(uint64_t pa)
{
	uint32_t v;
	return pm_rd(pa, &v, 4) ? 0xdeadbeef : v;
}

void wr32(uint64_t pa, uint32_t v)
{
	pm_wr(pa, &v, 4);
}

void wr64(uint64_t pa, uint64_t v)
{
	wr32(pa, (uint32_t)v);
	wr32(pa + 4, (uint32_t)(v >> 32));
}

uint64_t pm_alloc(uint64_t size, uint64_t align)
{
	struct pmem_alloc a = { .size = size, .align = align };
	if (ioctl(pm_fd, PMEM_ALLOC, &a) < 0) {
		fprintf(stderr, "pmem: alloc %llu: %s\n", (unsigned long long)size, strerror(errno));
		return 0;
	}
	return a.pa;
}

uint64_t pm_alloc32(uint64_t size, uint64_t align)
{
	struct pmem_alloc a = { .size = size, .align = align };
	if (ioctl(pm_fd, PMEM_ALLOC32, &a) < 0) {
		fprintf(stderr, "pmem: alloc32 %llu: %s\n", (unsigned long long)size, strerror(errno));
		return 0;
	}
	return a.pa;
}

int pm_free(uint64_t pa)
{
	return ioctl(pm_fd, PMEM_FREE, &pa);
}

void pm_smc(uint64_t x[8])
{
	struct pmem_smc s;
	memcpy(s.x, x, sizeof(s.x));
	if (ioctl(pm_fd, PMEM_SMC, &s) < 0) {
		fprintf(stderr, "pmem: smc: %s\n", strerror(errno));
		x[0] = (uint64_t)-1;
		return;
	}
	memcpy(x, s.x, 4 * sizeof(uint64_t));
}

int pm_list(struct pmem_list *l)
{
	return ioctl(pm_fd, PMEM_LIST, l);
}

void delay_ms(unsigned ms)
{
	usleep(ms * 1000);
}

int wait_bit(uint64_t pa, uint32_t mask, int set, unsigned ms)
{
	for (unsigned i = 0; i <= ms; i++) {
		if (!!(rd32(pa) & mask) == set)
			return 0;
		if (i < ms)
			delay_ms(1);
	}
	return -1;
}

int pm_rpmh(const char *res, unsigned n, const uint32_t *off, const uint32_t *val)
{
	struct pmem_rpmh r;
	memset(&r, 0, sizeof(r));
	snprintf(r.res, sizeof(r.res), "%s", res);
	r.n = n;
	for (unsigned i = 0; i < n && i < 4; i++) {
		r.offset[i] = off[i];
		r.value[i] = val[i];
	}
	return ioctl(pm_fd, PMEM_RPMH, &r) < 0 ? errno : 0;
}
