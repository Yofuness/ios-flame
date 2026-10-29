/*
 * A stream on the apps SMMU (0x15000000) through a translation-off context bank, so the device's DMA addresses are
 * physical (radio.c for WLAN, adspboot for the audio DSP). Takes the lowest free SMR and the lowest bank that nothing
 * points at and nobody set up; never changes an existing SMR (an overlapping one is reported, not touched).
 */
#include <stdio.h>
#include "pmem.h"
#include "smmu.h"

int smmu_dry;

#define SMMU_PA		0x15000000ull
#define SMMU_SIZE	0x00100000u
#define CB_CBAR		0x0001f000u	/* S1 translation, S2 bypass, as ABL's banks */
#define CB_CBA2R	0x00000001u	/* AArch64 */
#define CB_SCTLR	0x000000e0u	/* CFCFG | CFIE | CFRE, M = 0: no translation */

static int sid_match(uint32_t smr, uint32_t sid, uint32_t mask)
{
	uint32_t id = smr & 0x7fff, m = (smr >> 16) & 0x7fff;
	return ((id ^ sid) & ~(m | mask) & 0x7fff) == 0;
}

int smmu_identity(const char *name, uint32_t sid, uint32_t mask)
{
	uint64_t s = SMMU_PA;
	uint32_t id0 = rd32(s + 0x20), id1 = rd32(s + 0x24);
	uint32_t nsmr = id0 & 0xff, ncb = id1 & 0xff;
	uint32_t psize = (id1 >> 31) ? 0x10000 : 0x1000, npage = 1u << (((id1 >> 28) & 7) + 1);
	uint64_t used[4] = { 0 };
	int slot = -1, cb = -1;

	printf("smmu: sCR0 %08x ID0 %08x ID1 %08x: %u SMRs, %u context banks, page %#x x %u\n", rd32(s), id0, id1, nsmr,
	       ncb, psize, npage);
	for (uint32_t i = 0; i < nsmr && i < 128; i++) {
		uint32_t smr = rd32(s + 0x800 + 4 * i), s2cr = rd32(s + 0xc00 + 4 * i);
		if (!(smr >> 31)) {
			if (slot < 0)
				slot = (int)i;
			continue;
		}
		if (((s2cr >> 16) & 3) == 0)
			used[(s2cr & 0xff) / 64] |= 1ull << ((s2cr & 0xff) % 64);
		if (sid_match(smr, sid, mask)) {
			uint32_t c = s2cr & 0xff;
			uint64_t cbp = s + (uint64_t)npage * psize + (uint64_t)c * psize;
			printf("smmu: SMR%u %08x already matches %#x: S2CR %08x, CB%u SCTLR %08x CBAR %08x\n", i, smr,
			       sid, s2cr, c, rd32(cbp), rd32(s + psize + 4 * c));
			return ((s2cr >> 16) & 3) == 0 && !(rd32(cbp) & 1) ? 0 : -1;
		}
	}
	/* lowest free bank that nothing points at and nobody set up (the top ones may be secure) */
	for (uint32_t c = 0; c < ncb && c < 256; c++) {
		uint64_t cbp = s + (uint64_t)npage * psize + (uint64_t)c * psize;
		if (!(used[c / 64] & (1ull << (c % 64))) && !(rd32(cbp) & 1) && rd32(s + psize + 4 * c) == 0) {
			cb = (int)c;
			break;
		}
	}
	if (slot < 0 || cb < 0 || (uint64_t)(npage + cb + 1) * psize > SMMU_SIZE) {
		printf("smmu: no free SMR/bank (SMR %d, bank %d)\n", slot, cb);
		return -1;
	}
	if (smmu_dry) {
		printf("smmu: %s %#x/%#x would take SMR%d and CB%d (dry run: nothing written)\n", name, sid, mask, slot, cb);
		return 0;
	}
	uint64_t gr1 = s + psize, cbp = s + (uint64_t)npage * psize + (uint64_t)cb * psize;
	wr32(cbp + 0x58, 0xffffffff);			/* FSR: clear */
	wr32(cbp + 0x20, 0);				/* TTBR0 */
	wr32(cbp + 0x24, 0);
	wr32(cbp + 0x30, 0);				/* TCR */
	wr32(cbp + 0x10, 0);				/* TCR2 */
	wr32(cbp + 0x38, 0);				/* MAIR */
	wr32(cbp + 0x3c, 0);
	wr32(gr1 + 0x800 + 4 * cb, CB_CBA2R);
	wr32(gr1 + 4 * cb, CB_CBAR);
	wr32(cbp, CB_SCTLR);				/* SCTLR last */
	wr32(s + 0xc00 + 4 * slot, (uint32_t)cb);	/* S2CR: translate (type 0) through this bank */
	wr32(s + 0x800 + 4 * slot, (1u << 31) | (mask << 16) | sid);	/* SMR valid last */
	wr32(s + 0x70, 0);				/* sTLBGSYNC */
	for (int i = 0; i < 1000 && (rd32(s + 0x74) & 1); i++)
		delay_ms(1);
	uint32_t smr = rd32(s + 0x800 + 4 * slot), s2cr = rd32(s + 0xc00 + 4 * slot);
	int ok = (smr >> 31) && sid_match(smr, sid, mask) && ((s2cr >> 16) & 3) == 0 &&
		    (s2cr & 0xff) == (uint32_t)cb;
	printf("smmu: %s %#x/%#x -> CB%d via SMR%d: SMR %08x S2CR %08x SCTLR %08x CBAR %08x -> %s\n", name, sid,
	       mask, cb, slot, smr, s2cr, rd32(cbp), rd32(gr1 + 4 * cb), ok ? "ok" : "NOT as written");
	return ok ? 0 : -1;
}
