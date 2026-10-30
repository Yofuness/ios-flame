/*
 * Userland side of /dev/pmem (kernel: work/dxb/xnu/bsd/dev/sweet_pmem.c).
 * Offsets are physical addresses. MMIO and carve-outs: 32-bit aligned
 * accesses only. Managed DRAM (pm_alloc buffers): any size, kept coherent
 * with devices by the kernel (cache clean+invalidate around each access).
 */
#ifndef PMEM_H
#define PMEM_H
#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>

struct pmem_alloc {
	uint64_t size;
	uint64_t align;
	uint64_t pa;
};
struct pmem_smc {
	uint64_t x[8];
};
#define PMEM_LIST_MAX	64
struct pmem_list {
	uint32_t count;
	uint32_t pad;
	struct {
		uint64_t pa, size;
	} ent[PMEM_LIST_MAX];
};
#define PMEM_ALLOC	_IOWR('P', 1, struct pmem_alloc)
#define PMEM_FREE	_IOW('P', 2, uint64_t)
#define PMEM_SMC	_IOWR('P', 3, struct pmem_smc)
#define PMEM_LIST	_IOR('P', 4, struct pmem_list)
struct pmem_rpmh {
	char res[16];
	uint32_t n;
	uint32_t offset[4];
	uint32_t value[4];
};
#define PMEM_RPMH	_IOW('P', 5, struct pmem_rpmh)
#define PMEM_ALLOC32	_IOWR('P', 6, struct pmem_alloc)	/* below 4 GB (kernel109+, xnu 0232) */

void pm_init(void);			/* exits if /dev/pmem is missing */
uint32_t rd32(uint64_t pa);
void wr32(uint64_t pa, uint32_t v);
void wr64(uint64_t pa, uint64_t v);	/* two 32-bit stores, low first */
int pm_rd(uint64_t pa, void *buf, size_t len);
int pm_wr(uint64_t pa, const void *buf, size_t len);
uint64_t pm_alloc(uint64_t size, uint64_t align);	/* 0 on failure */
uint64_t pm_alloc32(uint64_t size, uint64_t align);	/* the same, below 4 GB (32-bit DMA) */
int pm_free(uint64_t pa);
void pm_smc(uint64_t x[8]);		/* x0..x7 in, x0..x3 out */
int pm_list(struct pmem_list *l);
int pm_rpmh(const char *res, unsigned n, const uint32_t *off, const uint32_t *val);	/* errno or 0 */
void delay_ms(unsigned ms);
int wait_bit(uint64_t pa, uint32_t mask, int set, unsigned ms);	/* 0 = ok */

#endif
