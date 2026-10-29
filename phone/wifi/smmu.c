/*
 * A stream on the apps SMMU (0x15000000) through a translation-off context bank, so the device's DMA addresses are
 * physical (radio.c for WLAN, adspboot for the audio DSP). Takes the lowest free SMR and the lowest bank that nothing
 * points at and nobody set up; never changes an existing SMR (an overlapping one is reported, not touched).
 */
#include <stdio.h>
#include <stdlib.h>
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

/*
 * SIDS through one new translating bank: AArch64 stage 1, 4 KB pages, a 32-bit input space (T0SZ 32, walk from
 * level 1), [IOVA, IOVA+LEN) -> [PA, PA+LEN), normal non-cacheable, read/write, faults terminate (no stall). The
 * three tables come from pm_alloc. For DMA masters that only take 32-bit addresses while all XNU RAM is above 4 GB
 * (the ADSP FastRPC streams). A SID that already has an SMR (e.g. smmu_identity's) is repointed; else a free SMR.
 * This SMMU has no address translation ops (ID0.ATOSNS), so the check is a read-back of the tables and registers.
 * 0 = ok.
 */
#define T_SCTLR		0x00000061u	/* M | CFRE | CFIE; CFCFG 0: terminate on fault */
#define T_TCR		(32u | (1u << 23))	/* T0SZ 32, EPD1; walks non-cacheable, 4 KB granule */
/*
 * audio step 6: the DSP puts the stream into address bits 33:32 (IOVA | 3<<32 = 0x380000000), which is outside a
 * 32-bit input space: CB13 took a translation fault at the first access. So a 39-bit space (T0SZ 25, still walking
 * from level 1, as the SM7150 reference's TCR 0x00803619) with the 1 GB L1 slot mapped at both IOVA and IOVA|3<<32.
 * SMMU_T0SZ32=1 in the environment goes back to the 32-bit space.
 */
#define T_TCR39		(25u | (1u << 23))
#define T_ALIAS		(3ull << 32)
#define T_TCR2		0x00000002u	/* PASize 40 bits */
#define T_MAIR0		0x00000044u	/* attr 0: normal non-cacheable */
#define T_PAGE		((1ull << 10) | (1ull << 6) | 3)	/* AF | AP 01 (EL0/EL1 RW) | page */
/*
 * audio step 7: SMMU_REF=1 in the environment gives the translating bank exactly the SM7150 reference's values
 * (sound_sm7150.c space_on): walks inner/outer write-back, inner shareable (pm_wr cleans the tables to PoC),
 * TRE|AFE|ASIDPNE in SCTLR, 16-bit ASIDs, BPSHCFG 3 in CBAR, and its leaf (SH 2, nG). Same table layout.
 */
#define R_SCTLR		0x00001127u	/* M | TRE | AFE | CFRE | HUPCF | ASIDPNE */
#define R_TCR		(25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)	/* 0x00803519 */
#define R_TCR2		(1u | 1u << 4 | 7u << 15)	/* PASize 36 bits, AS (16-bit ASID), SEP 7 */
#define R_MAIR0		0xf404ff44u
#define R_CBAR		(1u << 16 | 3u << 8 | 0xfu << 12)	/* 0x0001f300 */
#define R_PAGE		(3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)	/* 0xe43: attr 0, AP 01, SH 2, AF, nG */

int smmu_translate(const char *name, const uint16_t *sids, unsigned n, uint64_t iova, uint64_t pa, uint64_t len)
{
	uint64_t s = SMMU_PA;
	uint32_t id0 = rd32(s + 0x20), id1 = rd32(s + 0x24);
	uint32_t nsmr = id0 & 0xff, ncb = id1 & 0xff;
	uint32_t psize = (id1 >> 31) ? 0x10000 : 0x1000, npage = 1u << (((id1 >> 28) & 7) + 1);
	uint64_t used[4] = { 0 };
	int cb = -1, slot[16];

	if (n > 16 || !len || (iova | pa | len) & 0xfff || iova + len > (1ull << 32) || ((iova ^ (iova + len - 1)) >> 21)) {
		printf("smmu: %s: bad range %llx+%llx (one 2 MB block, 4 KB aligned, below 4 GB)\n", name,
		       (unsigned long long)iova, (unsigned long long)len);
		return -1;
	}
	for (uint32_t i = 0; i < nsmr && i < 128; i++) {
		uint32_t smr = rd32(s + 0x800 + 4 * i), s2cr = rd32(s + 0xc00 + 4 * i);
		if ((smr >> 31) && ((s2cr >> 16) & 3) == 0)
			used[(s2cr & 0xff) / 64] |= 1ull << ((s2cr & 0xff) % 64);
	}
	for (uint32_t c = 0; c < ncb && c < 256; c++) {
		uint64_t cbp = s + (uint64_t)npage * psize + (uint64_t)c * psize;
		if (!(used[c / 64] & (1ull << (c % 64))) && !(rd32(cbp) & 1) && rd32(s + psize + 4 * c) == 0) {
			cb = (int)c;
			break;
		}
	}
	for (unsigned k = 0; k < n; k++) {
		slot[k] = -1;
		for (uint32_t i = 0; i < nsmr && i < 128 && slot[k] < 0; i++)
			if ((rd32(s + 0x800 + 4 * i) & 0x80007fffu) == (0x80000000u | sids[k]))
				slot[k] = (int)i;	/* an exact SMR for this SID already */
		for (uint32_t i = 0; i < nsmr && i < 128 && slot[k] < 0; i++) {
			int taken = 0;
			for (unsigned j = 0; j < k; j++)
				taken |= slot[j] == (int)i;
			if (!(rd32(s + 0x800 + 4 * i) >> 31) && !taken)
				slot[k] = (int)i;
		}
		if (slot[k] < 0) {
			printf("smmu: %s: no SMR for %#x\n", name, sids[k]);
			return -1;
		}
	}
	if (cb < 0 || (uint64_t)(npage + cb + 1) * psize > SMMU_SIZE) {
		printf("smmu: %s: no free bank\n", name);
		return -1;
	}
	if (smmu_dry) {
		printf("smmu: %s: would map %llx+%llx -> %llx through CB%d (SCTLR %08x TCR %08x TCR2 %08x MAIR0 %08x),"
		       " %u SIDs:", name, (unsigned long long)iova, (unsigned long long)len, (unsigned long long)pa, cb,
		       T_SCTLR, T_TCR, T_TCR2, T_MAIR0, n);
		for (unsigned k = 0; k < n; k++)
			printf(" %#x->SMR%d", sids[k], slot[k]);
		printf(" (dry run: nothing written)\n");
		/* audio step 2: the writes and table entries the real run makes (T = the 3 table pages from pm_alloc) */
		printf("smmu: %s dry: SCTLR=0 FSR=ffffffff TTBR0=T|ASID %d<<48 TTBR1=0 TCR=%08x TCR2=%08x MAIR0=%08x"
		       " CBA2R=%08x CBAR=%08x TLBIASID=%d TLBIALL TLBSYNC SCTLR=%08x, then S2CR=%d + SMR per SID\n", name, cb,
		       T_TCR, T_TCR2, T_MAIR0, CB_CBA2R, CB_CBAR, cb, T_SCTLR, cb);
		printf("smmu: %s dry: L1[%llu]=T+0x1000|3 L2[%llu]=T+0x2000|3 L3[%llu..%llu]=%llx..%llx (AF %s)\n", name,
		       (unsigned long long)((iova >> 30) & 3), (unsigned long long)((iova >> 21) & 0x1ff),
		       (unsigned long long)((iova >> 12) & 0x1ff), (unsigned long long)(((iova + len - 1) >> 12) & 0x1ff),
		       (unsigned long long)(pa | T_PAGE), (unsigned long long)((pa + len - 0x1000) | T_PAGE),
		       (T_PAGE >> 10) & 1 ? "set" : "MISSING");
		return 0;
	}
	int ref = getenv("SMMU_REF") != NULL;	/* audio step 7 */
	int t39 = ref || !getenv("SMMU_T0SZ32");	/* audio step 6 */
	uint32_t tcr = ref ? R_TCR : t39 ? T_TCR39 : T_TCR, sctlr = ref ? R_SCTLR : T_SCTLR;
	uint32_t tcr2 = ref ? R_TCR2 : T_TCR2, mair0 = ref ? R_MAIR0 : T_MAIR0, cbar = ref ? R_CBAR : CB_CBAR;
	uint64_t page = ref ? R_PAGE : T_PAGE;
	unsigned l1i = (unsigned)((iova >> 30) & (t39 ? 0x1ff : 3)), l1a = (unsigned)(((iova | T_ALIAS) >> 30) & 0x1ff);
	uint64_t asid = (uint64_t)cb << 48;	/* audio step 2: ASID = bank number, as the SM7150 reference */
	uint64_t tbl = pm_alloc(3 * 0x1000, 0x1000);
	if (!tbl) {
		printf("smmu: %s: no memory for the tables\n", name);
		return -1;
	}
	uint64_t l1 = tbl, l2 = tbl + 0x1000, l3 = tbl + 0x2000, d;
	d = l2 | 3;
	pm_wr(l1 + 8 * l1i, &d, 8);
	if (t39)
		pm_wr(l1 + 8 * l1a, &d, 8);		/* audio step 6: the IOVA|3<<32 alias, same L2 */
	d = l3 | 3;
	pm_wr(l2 + 8 * ((iova >> 21) & 0x1ff), &d, 8);
	for (uint64_t o = 0; o < len; o += 0x1000) {
		d = (pa + o) | page;
		pm_wr(l3 + 8 * (((iova + o) >> 12) & 0x1ff), &d, 8);
	}
	uint64_t gr1 = s + psize, cbp = s + (uint64_t)npage * psize + (uint64_t)cb * psize;
	wr32(cbp, 0);
	wr32(cbp + 0x58, 0xffffffff);			/* FSR: clear */
	wr64(cbp + 0x20, l1 | asid);			/* TTBR0 */
	wr64(cbp + 0x28, 0);				/* TTBR1 */
	wr32(cbp + 0x30, tcr);
	wr32(cbp + 0x10, tcr2);
	wr32(cbp + 0x38, mair0);
	wr32(cbp + 0x3c, 0);
	wr32(gr1 + 0x800 + 4 * cb, CB_CBA2R);
	wr32(gr1 + 4 * cb, cbar);
	wr32(cbp + 0x610, (uint32_t)cb);		/* TLBIASID (audio step 2) */
	wr32(cbp + 0x618, 0);				/* TLBIALL */
	wr32(cbp + 0x7f0, 0);				/* TLBSYNC */
	for (int i = 0; i < 1000 && (rd32(cbp + 0x7f4) & 1); i++)
		delay_ms(1);
	wr32(cbp, sctlr);				/* SCTLR last */
	uint64_t r1 = 0, r2 = 0, r3a = 0, r3b = 0;
	pm_rd(l1 + 8 * l1i, &r1, 8);
	if (t39) {
		uint64_t ra = 0;
		pm_rd(l1 + 8 * l1a, &ra, 8);
		r1 = ra == r1 ? r1 : 0;
	}
	pm_rd(l2 + 8 * ((iova >> 21) & 0x1ff), &r2, 8);
	pm_rd(l3 + 8 * ((iova >> 12) & 0x1ff), &r3a, 8);
	pm_rd(l3 + 8 * (((iova + len - 1) >> 12) & 0x1ff), &r3b, 8);
	uint64_t ttbr = ((uint64_t)rd32(cbp + 0x24) << 32) | rd32(cbp + 0x20);
	int ok = r1 == (l2 | 3) && r2 == (l3 | 3) && r3a == (pa | page) && r3b == ((pa + len - 0x1000) | page) &&
		 ttbr == (l1 | asid) && rd32(cbp + 0x30) == tcr && (rd32(cbp + 0x10) & 7) == (tcr2 & 7) &&	/* TCR2 reads back 0x62: bits 5-6 set by the hardware */
		 (rd32(cbp) & ~0x1000u) == (sctlr & ~0x1000u) &&	/* SCTLR.HUPCF (bit 12) reads 0 here (au8) */
		 rd32(gr1 + 4 * cb) == cbar;
	printf("smmu: %s: CB%d tables %llx: TTBR0 %llx TCR %08x TCR2 %08x SCTLR %08x CBAR %08x; L3 first %llx last %llx"
	       " -> %s\n", name, cb, (unsigned long long)tbl, (unsigned long long)ttbr, rd32(cbp + 0x30),
	       rd32(cbp + 0x10), rd32(cbp), rd32(gr1 + 4 * cb), (unsigned long long)r3a, (unsigned long long)r3b,
	       ok ? "ok" : "NOT as written");
	if (!ok) {
		printf("smmu: %s: not routing any SID to CB%d\n", name, cb);
		return -1;
	}
	for (unsigned k = 0; k < n; k++) {
		wr32(s + 0xc00 + 4 * slot[k], (uint32_t)cb);	/* S2CR: translate through the new bank */
		wr32(s + 0x800 + 4 * slot[k], 0x80000000u | sids[k]);
	}
	wr32(s + 0x70, 0);				/* sTLBGSYNC */
	for (int i = 0; i < 1000 && (rd32(s + 0x74) & 1); i++)
		delay_ms(1);
	for (unsigned k = 0; k < n; k++)
		printf("smmu: %s %#x -> CB%d via SMR%d: SMR %08x S2CR %08x\n", name, sids[k], cb, slot[k],
		       rd32(s + 0x800 + 4 * slot[k]), rd32(s + 0xc00 + 4 * slot[k]));
	return 0;
}

void smmu_faults(const char *name, uint32_t sid)
{
	uint64_t s = SMMU_PA;
	uint32_t id0 = rd32(s + 0x20), id1 = rd32(s + 0x24), nsmr = id0 & 0xff;
	uint32_t psize = (id1 >> 31) ? 0x10000 : 0x1000, npage = 1u << (((id1 >> 28) & 7) + 1);
	for (uint32_t i = 0; i < nsmr && i < 128; i++) {
		if ((rd32(s + 0x800 + 4 * i) & 0x80007fffu) != (0x80000000u | sid))
			continue;
		uint32_t cb = rd32(s + 0xc00 + 4 * i) & 0xff;
		uint64_t cbp = s + (uint64_t)npage * psize + (uint64_t)cb * psize;
		/* FSR bits 10:9 are FORMAT (0x400 = AArch64 record), not a fault: only 0xc00001fe counts. */
		uint32_t fsr = rd32(cbp + 0x58);
		/* audio step 7: FSYNR0 decoded; PTWF = the fault came from the table walk itself */
		uint32_t sy = rd32(cbp + 0x68);
		printf("smmu: %s %#x CB%u: FSR %08x (%s) FORMAT %u FAR %08x%08x FSYNR0 %08x (PLVL %u WNR %u PNU %u IND %u"
		       " NSATTR %u PTWF %u AFR %u S1CBNDX %u)\n", name, sid, cb, fsr,
		       (fsr & 0xc00001feu) ? "FAULT" : "no fault", (fsr >> 9) & 3, rd32(cbp + 0x64), rd32(cbp + 0x60),
		       sy, sy & 3, (sy >> 4) & 1, (sy >> 5) & 1, (sy >> 6) & 1, (sy >> 7) & 1, (sy >> 8) & 1, (sy >> 9) & 1,
		       (sy >> 16) & 0xff);
		return;
	}
	printf("smmu: %s %#x: no SMR\n", name, sid);
}

/* audio step 3: read-only fault state: the global sGFSR/sGFSYNR0-1/sGFAR and the bank SID goes through. */
void smmu_status(const char *name, uint32_t sid)
{
	uint64_t s = SMMU_PA;
	printf("smmu: sGFSR %08x sGFSYNR0 %08x sGFSYNR1 %08x sGFAR %08x%08x sCR0 %08x\n", rd32(s + 0x48), rd32(s + 0x50),
	       rd32(s + 0x54), rd32(s + 0x44), rd32(s + 0x40), rd32(s));
	smmu_faults(name, sid);
}

/* audio step 6: read-only: the registers of SID's bank and the table entries walked for IOVA (TTBR0 read back). */
void smmu_dump(const char *name, uint32_t sid, uint64_t iova)
{
	uint64_t s = SMMU_PA;
	uint32_t id0 = rd32(s + 0x20), id1 = rd32(s + 0x24), nsmr = id0 & 0xff;
	uint32_t psize = (id1 >> 31) ? 0x10000 : 0x1000, npage = 1u << (((id1 >> 28) & 7) + 1);
	for (uint32_t i = 0; i < nsmr && i < 128; i++) {
		if ((rd32(s + 0x800 + 4 * i) & 0x80007fffu) != (0x80000000u | sid))
			continue;
		uint32_t cb = rd32(s + 0xc00 + 4 * i) & 0xff, tcr;
		uint64_t cbp = s + (uint64_t)npage * psize + (uint64_t)cb * psize, gr1 = s + psize;
		uint64_t ttbr = ((uint64_t)rd32(cbp + 0x24) << 32) | rd32(cbp + 0x20);
		tcr = rd32(cbp + 0x30);
		printf("smmu: %s %#x CB%u: SCTLR %08x TCR %08x TCR2 %08x MAIR0 %08x TTBR0 %llx CBAR %08x CBA2R %08x\n",
		       name, sid, cb, rd32(cbp), tcr, rd32(cbp + 0x10), rd32(cbp + 0x38), (unsigned long long)ttbr,
		       rd32(gr1 + 4 * cb), rd32(gr1 + 0x800 + 4 * cb));
		uint32_t t0sz = tcr & 0x3f;
		if (!(rd32(cbp) & 1) || t0sz < 25 || t0sz > 33) {
			printf("smmu: %s: no walk (translation off or T0SZ %u)\n", name, t0sz);
			return;
		}
		/* 4 KB granule, start level 1 for T0SZ 25..33 */
		uint64_t t = ttbr & 0xfffffffff000ull, d = 0;
		for (int lv = 1; lv <= 3; lv++) {
			unsigned idx = (unsigned)((iova >> (39 - 9 * lv)) & 0x1ff);
			if (lv == 1)
				idx = (unsigned)((iova >> 30) & ((1u << (34 - t0sz)) - 1));	/* 64 - T0SZ - 30 index bits */
			d = 0;
			pm_rd(t + 8 * idx, &d, 8);
			printf("smmu: %s walk %llx: L%d[%u] @%llx = %016llx\n", name, (unsigned long long)iova, lv, idx,
			       (unsigned long long)(t + 8 * idx), (unsigned long long)d);
			if ((d & 3) != 3)
				return;
			t = d & 0xfffffffff000ull;
		}
		return;
	}
	printf("smmu: %s %#x: no SMR\n", name, sid);
}
