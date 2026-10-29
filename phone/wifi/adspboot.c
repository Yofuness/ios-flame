/*
 * adspboot: boot the Pixel 4a's audio DSP (ADSP / LPASS) through TrustZone
 * PAS from userland (/dev/pmem), the way wland boots the modem (audio M0,
 * docs/features/audio.md). No audio: it reports whether the glink "lpass"
 * edge comes up and which channels the ADSP opens.
 *
 *   adspboot [boot]   votes, load adsp.mdt + .bNN, PAS init / mem setup / auth-and-reset, SMP2P ready, glink
 *   adspboot status   read-only: SMP2P state, glink items, crash reason
 *   adspboot shutdown PAS shutdown (boot does it first too); boot takes --watch S (20) and --no-early
 *                     and --no-pdmap (no service locator for the ADSP) and --no-sleepstate (no "sleepstate"
 *                     SMP2P out entry, which the sensors image's remote_proc_state needs)
 *   adspboot boot --rpc [--watch 120]  also serve FastRPC (fastrpc.c): attach to the sensors PD on
 *                     fastrpcglink-apps-dsp, run adsp_default_listener and answer its apps_std file calls
 *                     from /var/local/fw/sensors (+ /var/local/sensors-persist); keeps serving after ready,
 *                     summary at the end. Implies --smmu; the FastRPC streams then go through a translating bank
 *                     (32-bit IOVA 0x80000000 -> a pm_alloc buffer). `adspboot smmu --rpc --dry` shows that plan.
 *   adspboot boot --rpc --root --unsafe  attach to the root PD (pd 0), print the reply and the SMMU fault state
 *                     (audio step 5), then serve its listener (apps_std files, step 6) for --watch seconds
 *                     [--sensors]  then also attach the sensors PD (second half of the buffer, step 6)
 *   adspboot boot --afe [--watch 40]  ap4: --apr, then the SEC_MI2S_RX port: clock, I2S config, start, 2 s, stop, clock off
 *   adspboot boot --apr [--watch 40]  ap3: also open apr_audio_svc in this process and send the APR core
 *                     queries (GET_STATE, FWK_VERSION); combines with --rpc --root --sensors. Channels are
 *                     CLOSEd at exit only with ADSP_CLOSE=1 (apr3: closing the FastRPC channel crashed the ADSP: fastrpc_port_s.c:176)
 *   adspboot smmu --status  read-only: sGFSR/sGFSYNR and CB FSR/FAR of 0x1b23's bank, safe any time
 *                 [--dump]  + the bank's registers and the table walk for 0x80000000 and 0x380000000
 *   adspboot smmu [--dry] [--sid SID MASK]  the audio DSP streams (apps SMMU 0x1b21 audio buffers, 0x1b23-0x1b28
 *                     FastRPC, 0x1b2f USB audio) through translation-off banks, as wland does for WLAN; boot --smmu too
 *
 * Facts (recon/live.dts): qcom,lpass@62400000 pas-id 1, smem-id 423 (crash reason), proxy rails lcx.lvl /
 * lmx.lvl (>= 385); pil_adsp_mem 0x96b00000+0x2200000; smp2p-adsp in 443 / out 429, IPC bit 26; glink_adsp
 * remote-pid 2, IPC bit 24. adsp.mdt is relocatable (linked at 0x93c00000): segments go to
 * region + (paddr - lowest paddr), as Linux qcom_mdt_load.
 * Never touches the modem: SMEM host 2 only, APCS bits 24/26, AOSS bit 0. Exit 0 = edge up.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include "pmem.h"
#include "smem.h"
#include "glink.h"
#include "smmu.h"
#include "fastrpc.h"
#include "pdmapper.h"
#include "qmi.h"

#define PAS_ADSP	1
#define ADSP_PA		0x96b00000ull
#define ADSP_SIZE	0x02200000ull
#define FW_DIR		"/var/local/fw/adsp"
#define SMEM_HOST_ADSP	2
#define CRASH_ITEM	423
/* qcom,smem = <443 429> is <inbound outbound> (Linux smem_items[0] = inbound): the ADSP writes 443. */
#define SMP2P_OUT	429
#define SMP2P_IN	443
#define APCS_IPC	0x17c0000cull
#define IPC_AOSS	0
#define IPC_ADSP_GLINK	24
#define IPC_ADSP_SMP2P	26
#define SCM_PIL(cmd)	(0x42000000ull | (2u << 8) | (cmd))
#define MDT_RELOC	(1u << 27)
/* recon/live.dts: audio-ion 0x1b21, fastrpc cbs 0x1b23-0x1b28, usb-audio 0x1b2f. One SMR each: an SMR with a mask
 * (0x1b20/0xf) reads back 0 (the write is dropped), single SIDs stick. */
static const uint16_t adsp_sids[] = { 0x1b21, 0x1b23, 0x1b24, 0x1b25, 0x1b26, 0x1b27, 0x1b28, 0x1b2f };

/* rpc1/rpc2: the sensor PD takes 32-bit FastRPC addresses and XNU has no RAM below 4 GB, so with --rpc the
 * FastRPC streams go through a translating bank: RPC_IOVA (32-bit, the start of downstream's compute-cb pool)
 * -> a pm_alloc buffer. 0x1b21 and 0x1b2f stay identity. */
#define RPC_IOVA	0x80000000ull
/* audio step 4 (research/notes/audio-sm7150-reference.md): the DSP emits SID 0x1b20 | addr bits 32-35 and uses the
 * low 32 bits as the IOVA, so the address it gets carries stream 3 (0x1b23 -> CB7). */
#define RPC_DSP_ADDR	(RPC_IOVA | 3ull << 32)
/* audio step 2: only 0x1b23 translates; 0x1b24-0x1b28 stay identity as without --rpc */
static const uint16_t rpc_sids[] = { 0x1b23 };
static int rpc_on;

static int adsp_smmu(int one, uint32_t sid, uint32_t mask)
{
	if (one)
		return smmu_identity("ADSP", sid, mask);
	int bad = 0;
	for (unsigned i = 0; i < sizeof(adsp_sids) / sizeof(adsp_sids[0]); i++)
		if (!rpc_on || adsp_sids[i] != rpc_sids[0])
			bad |= smmu_identity("ADSP", adsp_sids[i], 0);
	if (rpc_on && !bad) {
		uint64_t pa = smmu_dry ? 0x100000000ull : pm_alloc(FASTRPC_DMA_SIZE, 0x1000);
		if (!pa)
			return 1;
		bad |= smmu_translate("ADSP rpc", rpc_sids, 1, RPC_IOVA, pa, FASTRPC_DMA_SIZE);
		if (!bad && !smmu_dry)
			fastrpc_buffer(pa, RPC_DSP_ADDR);
	}
	return bad;
}

static void rpc_faults(void)
{
	for (unsigned i = 0; i < sizeof(rpc_sids) / sizeof(rpc_sids[0]); i++)
		smmu_faults("ADSP rpc", rpc_sids[i]);
}

static void kick(unsigned bit)
{
	wr32(APCS_IPC, 1u << bit);
}

static int64_t scm(uint32_t cmd, uint64_t arginfo, uint64_t a, uint64_t b, uint64_t c, uint64_t *r1)
{
	uint64_t x[8] = { SCM_PIL(cmd), arginfo, a, b, c, 0, 0, 0 };
	pm_smc(x);
	if (r1)
		*r1 = x[1];
	return (int64_t)x[0];
}

/* Whole file, zero-padded by 4 so 32-bit device writes may round up. */
static uint8_t *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*len = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = calloc(1, *len + 4);
	if (b && fread(b, 1, *len, f) != *len) {
		free(b);
		b = NULL;
	}
	fclose(f);
	return b;
}

/* ---- power: cmd-db ARC levels (as wland) ---- */

static int cmddb_aux(const char *id, uint64_t *aux, uint32_t *len)
{
	uint64_t db = rd32(0x0c3f000cull);
	if (rd32(db + 4) != 0x0c0330db)
		return -1;
	uint64_t data = db + 4 + 4 + 8 * 16 + 4 + 4;
	for (uint32_t r = 0; r < 8; r++) {
		uint64_t rh = db + 8 + 16 * r;
		uint32_t w0 = rd32(rh), w1 = rd32(rh + 4);
		uint32_t hoff = w0 >> 16, doff = w1 & 0xffff, cnt = w1 >> 16;
		for (uint32_t e = 0; e < cnt; e++) {
			uint64_t ent = data + hoff + 24 * e;
			char name[9];
			for (uint32_t i = 0; i < 8; i++)
				name[i] = (char)(rd32(ent + (i & ~3u)) >> (8 * (i & 3)));
			name[8] = 0;
			if (strncmp(name, id, 8))
				continue;
			uint32_t w5 = rd32(ent + 20);
			*len = w5 & 0xffff;
			*aux = data + doff + (w5 >> 16);
			return 0;
		}
	}
	return -1;
}

/* Vote the first level >= want (downstream vdd_*-uV-uA 385), or the top one. */
static int vote_min(const char *res, uint32_t want)
{
	uint64_t aux;
	uint32_t len, idx = 0, lvl = 0;
	if (cmddb_aux(res, &aux, &len)) {
		fprintf(stderr, "adsp: %s not in cmd-db\n", res);
		return -1;
	}
	for (uint32_t i = 0; i < len / 2; i++) {
		uint32_t v = (rd32(aux + 2 * (i & ~1u)) >> (16 * (i & 1))) & 0xffff;
		if (!v)
			continue;
		idx = i;
		lvl = v;
		if (v >= want)
			break;
	}
	uint32_t off[1] = { 0 }, val[1] = { idx };
	int rc = pm_rpmh(res, 1, off, val);
	printf("adsp: vote %s level [%u]=%u -> %d\n", res, idx, lvl, rc);
	return rc ? -1 : 0;
}

/* ---- AOSS QMP (as wland): "load_state adsp on" before the Q6 runs ---- */

#define QMP_RAM		0x0c300000ull
#define QMP_UP		0x0000ffffu

static int wait_eq(uint64_t pa, uint32_t want, int ms)
{
	for (int i = 0; i < ms; i++) {
		if (rd32(pa) == want)
			return 0;
		usleep(1000);
	}
	return -1;
}

static int qmp_send(const char *msg)
{
	if (rd32(QMP_RAM) != 0x4d41494c || rd32(QMP_RAM + 4) != 1) {
		fprintf(stderr, "adsp: qmp magic %08x\n", rd32(QMP_RAM));
		return -1;
	}
	if (!(rd32(QMP_RAM + 0x24) == QMP_UP && rd32(QMP_RAM + 0x2c) == QMP_UP && rd32(QMP_RAM + 0x30) == QMP_UP)) {
		wr32(QMP_RAM + 0x10, rd32(QMP_RAM + 0x0c));
		wr32(QMP_RAM + 0x24, QMP_UP);
		kick(IPC_AOSS);
		if (wait_eq(QMP_RAM + 0x28, QMP_UP, 1000))
			return -1;
		wr32(QMP_RAM + 0x2c, QMP_UP);
		kick(IPC_AOSS);
		if (wait_eq(QMP_RAM + 0x14, QMP_UP, 1000))
			return -1;
		wr32(QMP_RAM + 0x18, QMP_UP);
		kick(IPC_AOSS);
		if (wait_eq(QMP_RAM + 0x30, QMP_UP, 1000))
			return -1;
	}
	uint32_t off = rd32(QMP_RAM + 0x38), buf[16] = { 0 };
	strncpy((char *)buf, msg, sizeof(buf) - 1);
	for (int i = 0; i < 16; i++)
		wr32(QMP_RAM + off + 4 + 4 * i, buf[i]);
	wr32(QMP_RAM + off, sizeof(buf));
	(void)rd32(QMP_RAM + off);
	kick(IPC_AOSS);
	int rc = wait_eq(QMP_RAM + off, 0, 1000);
	printf("adsp: qmp \"%s\" -> %s\n", msg, rc ? "TIMEOUT" : "acked");
	return rc;
}

/* ---- SMP2P with the ADSP (as wland's, host 2) ---- */

static void wr_name(uint64_t pa, const char *name)
{
	char n[16] = { 0 };
	strncpy(n, name, sizeof(n));
	for (int i = 0; i < 16; i += 4) {
		uint32_t w;
		memcpy(&w, n + i, 4);
		wr32(pa + i, w);
	}
}

/* Outbound item layout ("$SMP" v1): +0 magic, +4 version | features << 8, +8 local pid | remote pid << 16,
 * +12 total entries | valid entries << 16, +16 flags, then 20-byte entries from +20 (16-byte name, u32 value).
 * Entry 0 "master-kernel"; entry 1 "sleepstate", bit 0 = AP awake (Android's smp2p-sleepstate): the ADSP's
 * SEE remote_proc_state driver registers on it and the static sensors image dies at 10 s without it. */
static int sleepstate = 1;	/* --no-sleepstate leaves the entry out (A/B) */

static int smp2p_create(void)
{
	uint64_t pa;
	if (smem_alloc(SMEM_HOST_ADSP, SMP2P_OUT, 20 + 16 * 20, &pa)) {
		fprintf(stderr, "adsp: smp2p: cannot allocate item %u\n", SMP2P_OUT);
		return -1;
	}
	if (rd32(pa) != 0x504d5324) {
		wr32(pa + 8, 0 | (2u << 16));		/* local pid 0, remote pid 2 */
		wr32(pa + 12, 16 | ((sleepstate ? 2u : 1u) << 16));
		wr32(pa + 16, 0);
		wr_name(pa + 20, "master-kernel");
		wr32(pa + 36, 0);
		if (sleepstate) {
			wr_name(pa + 40, "sleepstate");
			wr32(pa + 56, 1);		/* bit 0: awake */
		}
		wr32(pa + 0, 0x504d5324);		/* "$SMP" */
		__asm__ volatile("dsb sy" ::: "memory");
		wr32(pa + 4, 1 | (1u << 8));
	} else if (sleepstate && (rd32(pa + 12) >> 16) == 1) {	/* item from an older run: append */
		wr_name(pa + 40, "sleepstate");
		wr32(pa + 56, 1);
		__asm__ volatile("dsb sy" ::: "memory");
		wr32(pa + 12, 16 | (2u << 16));
	}
	__asm__ volatile("dsb sy" ::: "memory");
	kick(IPC_ADSP_SMP2P);
	uint32_t n = rd32(pa + 12) >> 16;
	printf("adsp: smp2p out item %u at %llx ok\n", SMP2P_OUT, (unsigned long long)pa);
	if (n >= 2)
		printf("adsp: smp2p out item %u: entries %u: \"master-kernel\" = %x, \"sleepstate\" = %x\n",
		       SMP2P_OUT, n, rd32(pa + 36), rd32(pa + 56));
	else
		printf("adsp: smp2p out item %u: entries %u: \"master-kernel\" = %x\n", SMP2P_OUT, n, rd32(pa + 36));
	return 0;
}

/* The ADSP's "slave-kernel" bits (0 err-fatal, 1 ready, 2 handover, 3 stop-ack); -1 = no item yet. */
static int smp2p_in(uint32_t *v)
{
	uint64_t pa;
	uint32_t sz;
	if (smem_get(SMEM_HOST_ADSP, SMP2P_IN, &pa, &sz))
		return -1;
	static int logged;
	uint32_t n = rd32(pa + 12) >> 16;
	if (!logged)
		printf("adsp: smp2p in item %u at %llx: magic %08x pids %08x entries %u\n", SMP2P_IN,
		       (unsigned long long)pa, rd32(pa), rd32(pa + 8), n);
	for (uint32_t i = 0; i < n && i < 16; i++) {
		uint64_t e = pa + 20 + 20ull * i;
		char nm[17];
		for (int k = 0; k < 16; k++)
			nm[k] = (char)(rd32(e + (k & ~3)) >> (8 * (k & 3)));
		nm[16] = 0;
		if (!logged)
			printf("adsp: smp2p in entry %u \"%s\" = %x\n", i, nm, rd32(e + 16));
		if (!strcmp(nm, "slave-kernel")) {
			logged = 1;
			*v = rd32(e + 16);
			return 0;
		}
	}
	return -1;
}

static void crash_reason(void)
{
	uint64_t pa;
	uint32_t sz;
	if (smem_get(SMEM_HOST_ADSP, CRASH_ITEM, &pa, &sz) && smem_get(SMEM_HOST_GLOBAL, CRASH_ITEM, &pa, &sz)) {
		printf("adsp: no crash reason item %u\n", CRASH_ITEM);
		return;
	}
	char s[257] = { 0 };
	for (uint32_t i = 0; i < 256 && i < (sz & ~3u); i += 4) {
		uint32_t w = rd32(pa + i);
		memcpy(s + i, &w, 4);
	}
	printf("adsp: crash reason: \"%s\"\n", s);
}

/* ---- PAS boot (Linux qcom_q6v5_pas + qcom_mdt_loader) ---- */

struct phdr { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; };

static int boot(void)
{
	uint64_t r1 = 0;
	int64_t rc = scm(7, 1, PAS_ADSP, 0, 0, &r1);
	printf("adsp: pas supported(%d) -> %lld/%llx\n", PAS_ADSP, (long long)rc, (unsigned long long)r1);
	/* k93a: TZ answers 0/0 here, and wland boots the modem without asking: only log it; PAS init decides. */
	if (rc || r1 != 1)
		printf("adsp: pas says not supported, trying anyway\n");
	if (vote_min("lcx.lvl", 385) || vote_min("lmx.lvl", 385))
		return -1;

	size_t mlen;
	uint8_t *mdt = slurp(FW_DIR "/adsp.mdt", &mlen);
	if (!mdt) {
		fprintf(stderr, "adsp: no %s/adsp.mdt\n", FW_DIR);
		return -1;
	}
	uint32_t phoff, phnum = 0;
	memcpy(&phoff, mdt + 0x1c, 4);
	memcpy(&phnum, mdt + 0x2c, 2);
	if (memcmp(mdt, "\177ELF", 4) || phoff + 32ull * phnum > mlen) {
		fprintf(stderr, "adsp: adsp.mdt is not an ELF32 image\n");
		return -1;
	}
	struct phdr *ph = (struct phdr *)(mdt + phoff);
	int hash = -1, reloc = 0;
	uint64_t lo = ~0ull, hi = 0;
	for (uint32_t i = 0; i < phnum; i++) {
		if (((ph[i].flags >> 24) & 7) == 2) {
			hash = (int)i;
			continue;
		}
		if (ph[i].type != 1 || !ph[i].memsz)
			continue;
		reloc |= !!(ph[i].flags & MDT_RELOC);
		if (ph[i].paddr < lo)
			lo = ph[i].paddr;
		if (ph[i].paddr + ph[i].memsz > hi)
			hi = ph[i].paddr + ph[i].memsz;
	}
	if (hash < 0 || lo > hi) {
		fprintf(stderr, "adsp: no hash segment or no load segments\n");
		return -1;
	}
	uint64_t base = reloc ? ADSP_PA : lo;	/* segment i goes to base + paddr - lo */
	if (base < ADSP_PA || base + (hi - lo) > ADSP_PA + ADSP_SIZE) {
		fprintf(stderr, "adsp: image %llx-%llx (reloc %d) does not fit %llx+%llx\n", (unsigned long long)lo,
			(unsigned long long)hi, reloc, ADSP_PA, ADSP_SIZE);
		return -1;
	}
	printf("adsp: mdt %u phdrs, hash seg %d, linked %llx-%llx, %s to %llx\n", phnum, hash, (unsigned long long)lo,
	       (unsigned long long)hi, reloc ? "relocated" : "fixed", (unsigned long long)base);

	size_t ehsz = phoff + 32ull * phnum, meta_len = ehsz + ph[hash].filesz;
	uint8_t *meta = calloc(1, meta_len + 4);
	memcpy(meta, mdt, ehsz);
	if (ph[hash].offset + ph[hash].filesz <= mlen) {
		memcpy(meta + ehsz, mdt + ph[hash].offset, ph[hash].filesz);
	} else {
		char path[128];
		size_t hl;
		snprintf(path, sizeof(path), FW_DIR "/adsp.b%02d", hash);
		uint8_t *h = slurp(path, &hl);
		if (!h || hl < ph[hash].filesz) {
			fprintf(stderr, "adsp: hash segment %s missing\n", path);
			return -1;
		}
		memcpy(meta + ehsz, h, ph[hash].filesz);
		free(h);
	}
	uint64_t meta_pa = pm_alloc(meta_len, 0x1000);
	if (!meta_pa)
		return -1;
	pm_wr(meta_pa, meta, meta_len);
	rc = scm(1, 2 | (2u << 6), PAS_ADSP, meta_pa, 0, &r1);
	printf("adsp: pas init (%zu bytes metadata at %llx) -> %lld/%llx\n", meta_len, (unsigned long long)meta_pa,
	       (long long)rc, (unsigned long long)r1);
	pm_free(meta_pa);
	if (rc || r1)
		return -1;

	if (reloc) {
		rc = scm(2, 3, PAS_ADSP, ADSP_PA, ADSP_SIZE, &r1);
		printf("adsp: pas mem setup %llx+%llx -> %lld/%llx\n", ADSP_PA, ADSP_SIZE, (long long)rc,
		       (unsigned long long)r1);
		if (rc || r1)
			return -1;
	}

	static uint8_t zeros[0x10000];
	uint64_t total = 0;
	unsigned nseg = 0;
	for (uint32_t i = 0; i < phnum; i++) {
		if (ph[i].type != 1 || (int)i == hash || !ph[i].memsz)
			continue;
		uint64_t pa = base + (ph[i].paddr - lo);
		if (ph[i].filesz) {
			char path[128];
			size_t sl;
			snprintf(path, sizeof(path), FW_DIR "/adsp.b%02u", i);
			uint8_t *seg = slurp(path, &sl);
			if (!seg || sl < ph[i].filesz) {
				fprintf(stderr, "adsp: %s missing or short\n", path);
				return -1;
			}
			if (pm_wr(pa, seg, (ph[i].filesz + 3) & ~3u))
				return -1;
			free(seg);
			total += ph[i].filesz;
		}
		for (uint64_t o = (ph[i].filesz + 3) & ~3ull; o < ph[i].memsz; o += sizeof(zeros)) {
			uint64_t n = ph[i].memsz - o < sizeof(zeros) ? ph[i].memsz - o : sizeof(zeros);
			if (pm_wr(pa + o, zeros, (n + 3) & ~3ull))
				return -1;
		}
		nseg++;
	}
	printf("adsp: load %u segs, %llu bytes ok\n", nseg, (unsigned long long)total);

	if (smp2p_create())
		return -1;
	if (qmp_send("{class: image, res: load_state, name: adsp, val: on}"))
		return -1;
	rc = scm(5, 1, PAS_ADSP, 0, 0, &r1);
	printf("adsp: pas auth and reset -> %lld/%llx\n", (long long)rc, (unsigned long long)r1);
	return rc || r1 ? -1 : 0;
}

/* ---- glink "lpass" edge: handshake, channels, QRTR on IPCRTR ---- */

/*
 * audio M0b: the edge comes up right after auth-and-reset (not only after ready), so the ADSP's init can reach
 * the AP: VERSION/OPEN/INTENT are answered and QRTR on "IPCRTR" gets HELLO back and empty lookups (no servers).
 * `--no-early` = the old order (edge only after ready).
 */
#define QRTR_NODE_AP	1
#define QRTR_PORT_CTRL	0xfffffffeu
#define NCHAN		8
#define NPEND		4

struct chan {
	char name[32];
	uint16_t rcid, lcid;
	uint32_t r_iid[64], r_isz[64];	/* ap5: 64 (was 16): apr intents were dropped, table full */
	int r_n, ndata, up;
	uint8_t pend[NPEND][512];	/* (pd-mapper replies are ~150 bytes) */
	uint32_t pend_len[NPEND];
	int npend;
};
static struct chan ch[NCHAN];
static struct chan *apr_chan;
static int nch, edge_up, ver_seen, our_iid = 100;
static int rpc_started, ready_seen;
/* au14: --watch was paid twice with --rpc (the early watch, then glink_watch) and silent; one deadline for the
 * whole run, a heartbeat every 10 s, and SIGTERM/SIGINT stop the loop so the summary still prints */
static uint64_t watch_end, run_t0;
static volatile sig_atomic_t got_sig;

static void on_sig(int sig)
{
	got_sig = sig;
}
static struct chan *rpc_chan;
static uint8_t rxbuf[16384];
static size_t rx_have;
static uint32_t rx_iid;

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static struct chan *by_rcid(uint16_t rcid)
{
	for (int i = 0; i < nch; i++)
		if (ch[i].rcid == rcid)
			return &ch[i];
	return NULL;
}

static void hex32(const char *tag, const uint8_t *p, size_t n)
{
	printf("adsp: %s %zu bytes:", tag, n);
	for (size_t i = 0; i < n && i < 64; i++)
		printf(" %02x", p[i]);
	printf("\n");
}

static void give_intents(struct chan *c, int n, uint32_t sz)
{
	uint32_t e[8];
	if (n > 4)
		n = 4;
	for (int i = 0; i < n; i++) {
		e[2 * i] = sz;
		e[2 * i + 1] = (uint32_t)our_iid++;
	}
	glink_send(GLINK_CMD_INTENT, c->lcid, (uint32_t)n, e, 8u * n);
}

/* Send one packet on c using a remote intent; without one, queue it and ask (RX_INTENT_REQ). */
static void chan_send(struct chan *c, const void *pkt, size_t n)
{
	int i;
	for (i = 0; i < c->r_n && c->r_isz[i] < n; i++)
		;
	if (i >= c->r_n) {
		if (c->npend < NPEND && n <= sizeof(c->pend[0])) {
			memcpy(c->pend[c->npend], pkt, n);
			c->pend_len[c->npend++] = (uint32_t)n;
			glink_send(GLINK_CMD_RX_INTENT_REQ, c->lcid, (uint32_t)n, NULL, 0);
		} else {
			printf("adsp: %s: tx %zu bytes dropped (no intent, queue full)\n", c->name, n);
		}
		return;
	}
	uint32_t iid = c->r_iid[i];
	if (c == apr_chan)	/* ap5: which remote intent carried the APR packet */
		printf("adsp: %s: tx %zu bytes on remote intent %u (%u bytes, %d left)\n", c->name, n, iid, c->r_isz[i],
		       c->r_n - 1);
	c->r_iid[i] = c->r_iid[--c->r_n];
	c->r_isz[i] = c->r_isz[c->r_n];
	uint8_t d[8 + sizeof(c->pend[0])];
	if (n > sizeof(c->pend[0])) {
		printf("adsp: %s: tx %zu bytes dropped (too big)\n", c->name, n);
		return;
	}
	uint32_t cl[2] = { (uint32_t)n, 0 };
	memcpy(d, cl, 8);
	memcpy(d + 8, pkt, n);
	glink_send(GLINK_CMD_TX_DATA, c->lcid, iid, d, 8 + n);
}

static void flush_pend(struct chan *c)
{
	int n = c->npend;
	c->npend = 0;
	for (int i = 0; i < n; i++)
		chan_send(c, c->pend[i], c->pend_len[i]);
}

static const char *qrtr_type(uint32_t t)
{
	static const char *n[] = { "?0", "DATA", "HELLO", "BYE", "NEW_SERVER", "DEL_SERVER", "DEL_CLIENT",
				   "RESUME_TX", "EXIT", "PING", "NEW_LOOKUP", "DEL_LOOKUP" };
	return t < 12 ? n[t] : "?";
}

/*
 * audio pd-mapper: pdmapper.c (wland's locator, which also has the msm/adsp PDs at instance 74) answers the ADSP's
 * servreg-locator requests here too: HELLO is followed by NEW_SERVER 64 v1 i1 at 1:PDMAPPER_PORT (as the SM7150
 * reference does), a NEW_LOOKUP for 64 gets it again, DATA to that port goes to pdmapper_handle. Separate edge from
 * wland's (modem), so the two never meet. `--no-pdmap` = the old empty lookups.
 */
static int pdmap = 1;
static struct chan *ipcrtr;
static uint32_t ipcrtr_peer = 5;	/* the ADSP's node; set from its HELLO */

/* pdmapper.c's transport: QRTR v1 from us (node 1) on the IPCRTR channel. */
int qrtr_send(uint32_t type, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *payload, uint32_t len)
{
	uint8_t p[512];
	uint32_t h[8] = { 1, type, QRTR_NODE_AP, src_port, 0, len, dst_node, dst_port };
	if (!ipcrtr || 32 + ((len + 3) & ~3u) > sizeof(p))
		return -1;
	memset(p, 0, sizeof(p));
	memcpy(p, h, 32);
	memcpy(p + 32, payload, len);
	chan_send(ipcrtr, p, 32 + ((len + 3) & ~3u));
	return 0;
}

int qrtr_new_server(uint32_t service, uint32_t version, uint32_t instance, uint32_t port)
{
	uint32_t k[5] = { QRTR_TYPE_NEW_SERVER, service, version | instance << 8, QRTR_NODE_AP, port };
	printf("adsp: qrtr tx NEW_SERVER svc %u at %u:%x\n", service, QRTR_NODE_AP, port);
	return qrtr_send(QRTR_TYPE_NEW_SERVER, QRTR_PORT_CTRL, ipcrtr_peer, QRTR_PORT_CTRL, k, sizeof(k));
}

/*
 * au13: `--stub-svc SVC[:VER[:INST]]` (repeatable, none by default = off): after the ADSP's HELLO, announce SVC at
 * 1:0x20+i and answer every QMI request to it with a bare success result (TLV 2 = {0,0}). Tests whether sar.cc's
 * 10 s fatal waits for a service the AP/modem would normally publish.
 */
#define MAX_STUB 8
static struct { uint32_t svc, ver, inst; } stub[MAX_STUB];
static int nstub;

static void stub_rx(const uint32_t *h, const uint8_t *body, size_t blen)
{
	int i = (int)h[7] - 0x20;
	struct qmi_msg m;
	if (h[4]) {
		uint32_t k[5] = { QRTR_TYPE_RESUME_TX, QRTR_NODE_AP, h[7], 0, 0 };
		qrtr_send(QRTR_TYPE_RESUME_TX, QRTR_PORT_CTRL, h[2], h[3], k, sizeof(k));
	}
	if (qmi_parse(body, (uint32_t)blen, &m)) {
		hex32("stub svc: unparsable", body, blen);
		return;
	}
	printf("adsp: stub svc %u: %s id 0x%x txn %u len %u from %u:%x\n", stub[i].svc,
	       m.type == QMI_REQUEST ? "req" : m.type == QMI_INDICATION ? "ind" : "resp", m.id, m.txn, m.len, h[2], h[3]);
	qmi_dump(&m);
	if (m.type != QMI_REQUEST)
		return;
	struct qmi_buf *q = malloc(sizeof(*q));
	if (!q)
		return;
	qmi_start(q, QMI_RESPONSE, m.txn, m.id);
	qmi_tlv_result(q, 0, 0);
	size_t n = qmi_finish(q);
	qrtr_send(QRTR_TYPE_DATA, h[7], h[2], h[3], q->b, (uint32_t)n);
	free(q);
}

/* QRTR v1 control packet from us (node 1, ctrl port) to dst. */
static void qrtr_ctrl(struct chan *c, uint32_t type, uint32_t dnode, uint32_t dport)
{
	uint32_t p[8 + 5] = { 1, type, QRTR_NODE_AP, QRTR_PORT_CTRL, 0, 20, dnode, dport, type, 0, 0, 0, 0 };
	printf("adsp: qrtr tx %s -> %u:%x\n", qrtr_type(type), dnode, dport);
	chan_send(c, p, sizeof(p));
}

static void qrtr_rx(struct chan *c, const uint8_t *p, size_t n)
{
	uint32_t h[8];
	if (n < sizeof(h)) {
		hex32("qrtr short", p, n);
		return;
	}
	memcpy(h, p, sizeof(h));
	uint32_t ty = h[1], sz = h[5];
	const uint8_t *body = p + 32;
	size_t blen = n - 32 < sz ? n - 32 : sz;
	printf("adsp: qrtr rx v%u %s %u:%x -> %u:%x size %u confirm %u", h[0], qrtr_type(ty), h[2], h[3], h[6], h[7],
	       sz, h[4]);
	if (ty != 1 && blen >= 20) {
		uint32_t k[5];
		memcpy(k, body, 20);
		if (ty == 4 || ty == 5 || ty == 10 || ty == 11)
			printf(" svc %u inst %u node %u port %x", k[1], k[2], k[3], k[4]);
		else if (ty == 6 || ty == 7)
			printf(" client %u:%x", k[1], k[2]);
		else if (ty == 3)
			printf(" node %u", k[1]);
		else
			printf(" raw %x %x %x %x %x", k[0], k[1], k[2], k[3], k[4]);
	}
	printf("\n");
	if (h[0] != 1)
		return;
	ipcrtr = c;
	if (ty == 2) {
		qrtr_ctrl(c, 2, h[2], QRTR_PORT_CTRL);
		ipcrtr_peer = h[2];
		if (pdmap)
			qrtr_new_server(PDMAPPER_SERVICE, 1, 1, PDMAPPER_PORT);
		for (int i = 0; i < nstub; i++)	/* au13 */
			qrtr_new_server(stub[i].svc, stub[i].ver, stub[i].inst, 0x20 + i);
	} else if (ty == 10) {
		uint32_t svc = 0;
		if (blen >= 8)
			memcpy(&svc, body + 4, 4);
		if (pdmap && svc == PDMAPPER_SERVICE) {
			uint32_t k[5] = { QRTR_TYPE_NEW_SERVER, PDMAPPER_SERVICE, 1 | 1 << 8, QRTR_NODE_AP, PDMAPPER_PORT };
			printf("adsp: pd-mapper: lookup for 64 answered with 1:%x\n", PDMAPPER_PORT);
			qrtr_send(QRTR_TYPE_NEW_SERVER, QRTR_PORT_CTRL, h[2], h[3], k, sizeof(k));
		}
		for (int i = 0; i < nstub; i++)	/* au13 */
			if (svc == stub[i].svc) {
				uint32_t k[5] = { QRTR_TYPE_NEW_SERVER, svc, stub[i].ver | stub[i].inst << 8, QRTR_NODE_AP,
						  0x20u + i };
				printf("adsp: stub svc %u: lookup answered with 1:%x\n", svc, 0x20 + i);
				qrtr_send(QRTR_TYPE_NEW_SERVER, QRTR_PORT_CTRL, h[2], h[3], k, sizeof(k));
			}
		qrtr_ctrl(c, 4, h[2], h[3]);	/* all-zero NEW_SERVER = end of lookup */
	} else if (ty == 1 && pdmap && h[7] == PDMAPPER_PORT) {
		struct qrtr_hdr qh = { h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7] };
		if (h[4]) {			/* confirm_rx: RESUME_TX names our port, so the sender goes on */
			uint32_t k[5] = { QRTR_TYPE_RESUME_TX, QRTR_NODE_AP, h[7], 0, 0 };
			qrtr_send(QRTR_TYPE_RESUME_TX, QRTR_PORT_CTRL, h[2], h[3], k, sizeof(k));
		}
		pdmapper_handle(&qh, body, (uint32_t)blen);
	} else if (ty == 1 && h[7] >= 0x20 && h[7] < 0x20u + nstub)
		stub_rx(h, body, blen);
	else if (ty == 1)
		hex32("qrtr data to unserved port, dropped:", body, blen);
}

/*
 * ap3: APR inside adspboot (`--apr`). apr1/apr2: a second process (aprtest) on the edge adspboot left behind made
 * the ADSP assert (glink_core_intent.c:874, glink_channel_migration.c:667). Here the ADSP's own OPEN of
 * "apr_audio_svc" is answered like every other channel; once it is up, the step-1 queries of aprtest.c go out
 * (GET_STATE, GET_FWK_VERSION, fallback GET_VERSIONS; 3 s each) and every APR packet received is logged.
 */
#define APR_HDR_FIELD	0x0250		/* ap5: SEQ_CMD (2) << 8 | 5 words << 4 | version 0 (0x0150 was CMD_RSP) */
#define APR_DOMAIN_ADSP	4
#define APR_DOMAIN_APPS	5
#define APR_SVC_CORE	0x3
#define APR_BASIC_RSP	0x000110e8
#define AVCS_GET_STATE		0x0001290c	/* ap5: was 0x1326c (hakios adsp_sm7150.c, Linux q6core) */
#define AVCS_GET_STATE_RSP	0x0001290d
#define AVCS_GET_FWK_VERSION	0x0001292c
#define AVCS_GET_FWK_VERSION_RSP 0x0001292d
#define AVCS_GET_VERSIONS	0x00012905
#define AVCS_GET_VERSIONS_RSP	0x00012906

struct apr_hdr {
	uint16_t hdr_field, pkt_size;
	uint8_t src_svc, src_domain;
	uint16_t src_port;
	uint8_t dest_svc, dest_domain;
	uint16_t dest_port;
	uint32_t token, opcode;
} __attribute__((packed));

static int apr_on, apr_step, close_acks;	/* apr_step: 0 idle, 1..3 waiting for query 1..3, 4 done */
static uint64_t apr_due;
/* ap5: ready gate (hakios step_apr): GET_STATE every ~200 ms (500 ms per reply) for up to 10 s; AFE only once
 * words[0] == 1. ADSP_APR_NO_GATE=1 = the old single GET_STATE, AFE regardless. */
static int apr_ready, apr_gate_wait;	/* apr_gate_wait: 1 = reply pending, 0 = next poll due at apr_due */
static uint64_t apr_gate_end;
static const uint32_t apr_q[3] = { AVCS_GET_STATE, AVCS_GET_FWK_VERSION, AVCS_GET_VERSIONS };

static void apr_next(int ok);

static void apr_send(uint32_t opcode, uint32_t token)
{
	struct apr_hdr h = {
		.hdr_field = APR_HDR_FIELD, .pkt_size = sizeof(h),
		.src_svc = APR_SVC_CORE, .src_domain = APR_DOMAIN_APPS, .src_port = 0,
		.dest_svc = APR_SVC_CORE, .dest_domain = APR_DOMAIN_ADSP, .dest_port = 0,
		.token = token, .opcode = opcode,
	};
	printf("apr: tx svc %x op %x token %u\n", APR_SVC_CORE, opcode, token);
	chan_send(apr_chan, &h, sizeof(h));
	apr_due = now_ms() + (opcode == AVCS_GET_STATE && !getenv("ADSP_APR_NO_GATE") ? 500 : 3000);
	apr_gate_wait = 1;
}

/* ap5: a GET_STATE reply (or 500 ms without one): ready -> next query; else poll again in 200 ms until 10 s. */
static void apr_gate(int replied, uint32_t state)
{
	if (replied && state == 1) {
		apr_ready = 1;
		apr_next(1);
	} else if (now_ms() >= apr_gate_end) {
		printf("apr: ADSP audio not ready after 10 s\n");
		apr_next(0);
	} else if (replied) {
		apr_gate_wait = 0;
		apr_due = now_ms() + 200;
	} else {
		apr_send(AVCS_GET_STATE, 1);
	}
}

/* Next query: step 1 GET_STATE, 2 FWK_VERSION, 3 GET_VERSIONS (only if 2 failed), then done. */
static void apr_next(int ok)
{
	if (apr_step == 2 && ok)
		apr_step = 3;
	if (!apr_step)
		apr_gate_end = now_ms() + 10000;
	if (++apr_step <= 3)
		apr_send(apr_q[apr_step - 1], (uint32_t)apr_step);
	else
		printf("apr: queries done\n");
}

/*
 * ap4: `--afe` (implies --apr): step 2 of apr-plan.md, the SECONDARY_MI2S_RX port alone (no amps, ASM, ADM or PCM).
 * After the queries: bit clock on (AFE_SVC_CMD_SET_PARAM 0x100F3; ap5: the V2 0x100FC fallback is gone), I2S config, DEVICE_START,
 * 2 s hold, DEVICE_STOP, clock off. Each step waits up to 3 s for its APR basic response; the first non-zero status
 * ends the sequence, but stop and clock off still run once start/clock on succeeded. Layouts: hakios
 * sound_sm7150.c afe_service_param/afe_port_param, afe_clock_set, afe_i2s (= Linux apr_audio-v2.h).
 */
#define APR_SVC_AFE		0x4
#define AFE_SVC_CMD_SET_PARAM	0x000100f3
#define AFE_SVC_CMD_SET_PARAM_V2 0x000100fc
#define AFE_PORT_CMD_SET_PARAM_V2 0x000100ef
#define AFE_PORT_CMD_DEVICE_START 0x000100e5
#define AFE_PORT_CMD_DEVICE_STOP 0x000100e6
#define AFE_MODULE_CLOCK_SET	0x0001028f
#define AFE_PARAM_ID_CLOCK_SET	0x00010290
#define AFE_MODULE_AUDIO_DEV_INTERFACE 0x0001020c
#define AFE_PARAM_ID_I2S_CONFIG	0x0001020d
#define AFE_PORT_SEC_MI2S_RX	0x1002
#define AFE_CLK_SEC_MI2S_IBIT	0x102

struct afe_param {
	uint32_t module, param;
	uint16_t size, reserved;
} __attribute__((packed));

struct afe_clock_set {		/* afe_clk_set: version, clk_id, freq, u16 attri, u16 root, enable (20 B) */
	uint32_t version, id, hz;
	uint16_t attribute, root;
	uint32_t enable;
} __attribute__((packed));

struct afe_i2s {		/* afe_param_id_i2s_cfg */
	uint32_t version;
	uint16_t bits, channel_mode, mono_stereo, ws_src;
	uint32_t rate;
	uint16_t data_format, reserved;
} __attribute__((packed));

enum { AFE_IDLE, AFE_CLK_F3, AFE_CLK_FC, AFE_CFG, AFE_START, AFE_HOLD, AFE_STOP, AFE_CLK_OFF, AFE_DONE };
static int afe_on, afe_step, afe_clk_ok, afe_started;
static uint32_t afe_op, afe_clk_op;	/* afe_op: the opcode whose basic response we wait for */

static void afe_send(uint16_t dest_port, uint32_t opcode, const void *pl, uint16_t len)
{
	uint8_t pkt[sizeof(struct apr_hdr) + 64];
	struct apr_hdr h = {
		.hdr_field = APR_HDR_FIELD, .pkt_size = (uint16_t)(sizeof(h) + len),
		.src_svc = APR_SVC_AFE, .src_domain = APR_DOMAIN_APPS, .src_port = 0,
		.dest_svc = APR_SVC_AFE, .dest_domain = APR_DOMAIN_ADSP, .dest_port = dest_port,
		.token = 0x100u + (uint32_t)afe_step, .opcode = opcode,
	};
	memcpy(pkt, &h, sizeof(h));
	memcpy(pkt + sizeof(h), pl, len);
	printf("afe: step %d tx op %x (%u B)\n", afe_step, opcode, (unsigned)h.pkt_size);
	chan_send(apr_chan, pkt, h.pkt_size);
	afe_op = opcode;
	apr_due = now_ms() + 3000;
}

static void afe_clock(uint32_t opcode, int enable)
{
	uint8_t b[16 + sizeof(struct afe_param) + sizeof(struct afe_clock_set)] = { 0 };
	struct afe_param p = { AFE_MODULE_CLOCK_SET, AFE_PARAM_ID_CLOCK_SET, sizeof(struct afe_clock_set), 0 };
	struct afe_clock_set c = { 1, AFE_CLK_SEC_MI2S_IBIT, 48000 * 16 * 2, 1 /* COUPLE_NO */, 0, (uint32_t)enable };
	uint32_t size = sizeof(p) + sizeof(c);	/* payload size; address 0/0 and mem_map_handle 0: in band */
	memcpy(b, &size, 4);
	memcpy(b + 16, &p, sizeof(p));
	memcpy(b + 16 + sizeof(p), &c, sizeof(c));
	afe_send(0, opcode, b, sizeof(b));
}

/* Run the step afe_step now holds; AFE_HOLD only arms the 2 s timer. */
static void afe_run(void)
{
	uint16_t port = AFE_PORT_SEC_MI2S_RX;
	switch (afe_step) {
	case AFE_CLK_F3: afe_clock(AFE_SVC_CMD_SET_PARAM, 1); break;
	case AFE_CLK_FC: afe_clock(AFE_SVC_CMD_SET_PARAM_V2, 1); break;
	case AFE_CFG: {
		uint8_t b[16 + sizeof(struct afe_param) + sizeof(struct afe_i2s)] = { 0 };
		struct afe_param p = { AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_I2S_CONFIG, sizeof(struct afe_i2s), 0 };
		struct afe_i2s c = { 1, 16, 1 /* SD0 */, 1 /* stereo */, 1 /* ws internal */, 48000, 0, 0 };
		uint16_t size = sizeof(p) + sizeof(c);
		memcpy(b, &port, 2);
		memcpy(b + 2, &size, 2);
		memcpy(b + 16, &p, sizeof(p));
		memcpy(b + 16 + sizeof(p), &c, sizeof(c));
		afe_send(port, AFE_PORT_CMD_SET_PARAM_V2, b, sizeof(b));
		break;
	}
	case AFE_START:
	case AFE_STOP: {
		uint16_t b[2] = { port, 0 };
		afe_send(port, afe_step == AFE_START ? AFE_PORT_CMD_DEVICE_START : AFE_PORT_CMD_DEVICE_STOP, b, sizeof(b));
		break;
	}
	case AFE_HOLD:
		printf("afe: port started, holding 2 s\n");
		afe_op = 0;
		apr_due = now_ms() + 2000;
		break;
	case AFE_CLK_OFF: afe_clock(afe_clk_op, 0); break;
	default:
		afe_op = 0;
		printf("afe: done (clock %s via %x, start %s)\n", afe_clk_ok ? "ok" : "failed", afe_clk_op,
		       afe_started ? "ok" : "not done");
	}
}

/* A step finished: ok = basic response status 0. Picks the next step (stop/clock off always run once needed). */
static void afe_next(int ok, uint32_t status)
{
	int s = afe_step;
	if (afe_op)
		printf("afe: step %d op %x: %s (status %x)\n", s, afe_op, ok ? "ok" : "FAILED", status);
	if ((s == AFE_CLK_F3 || s == AFE_CLK_FC) && !ok)
		afe_step = AFE_DONE;	/* ap5: no 0x100FC fallback, clock set only via 0x100F3 */
	else if ((s == AFE_CLK_F3 || s == AFE_CLK_FC) && ok) {
		afe_clk_ok = 1;
		afe_clk_op = afe_op;
		printf("afe: clock set works with op %x\n", afe_op);
		afe_step = AFE_CFG;
	} else if (s == AFE_CFG || s == AFE_START) {
		if (ok && s == AFE_START)
			afe_started = 1;
		afe_step = ok ? s + 1 : AFE_CLK_OFF;
	} else
		afe_step = s + 1;
	if (afe_step == AFE_CLK_OFF && !afe_clk_ok)
		afe_step = AFE_DONE;
	afe_run();
}

static void apr_rx(const uint8_t *p, size_t n)
{
	struct apr_hdr h;
	uint32_t pl[64], len;
	if (n < sizeof(h)) {
		hex32("apr: rx short", p, n);
		return;
	}
	memcpy(&h, p, sizeof(h));
	if (getenv("ADSP_APR_HEX"))	/* af2: GET_STATE replies were 36 B with an odd first word: show the raw packet */
		hex32("apr: raw", p, n);
	len = (uint32_t)(n - sizeof(h)) / 4;
	if (len > 64)
		len = 64;
	memcpy(pl, p + sizeof(h), len * 4);
	uint32_t status = len ? pl[h.opcode == APR_BASIC_RSP && len > 1 ? 1 : 0] : 0;
	printf("apr: rx svc %x op %x token %u status %x (len %zu, from %u/%u port %u%s%x)\n", h.src_svc, h.opcode,
	       h.token, status, n, h.src_domain, h.src_svc, h.src_port, h.opcode == APR_BASIC_RSP ? ", for op " : "",
	       h.opcode == APR_BASIC_RSP ? pl[0] : 0);
	if (h.opcode == AVCS_GET_STATE_RSP)
		printf("apr: adsp state words[0]=%u (%s)\n", len ? pl[0] : 0, len && pl[0] == 1 ? "ready" : "not ready");
	else if (h.opcode == AVCS_GET_FWK_VERSION_RSP && len >= 5) {
		/* build major, minor, branch, subbranch, num_services; then {service id, api version, api branch} */
		printf("apr: fwk %u.%u.%u.%u, %u services\n", pl[0], pl[1], pl[2], pl[3], pl[4]);
		for (uint32_t i = 0; i < pl[4] && 5 + 3 * i + 2 < len; i++)
			printf("apr:   svc %x api %u branch %u\n", pl[5 + 3 * i], pl[6 + 3 * i], pl[7 + 3 * i]);
	} else if (h.opcode == AVCS_GET_VERSIONS_RSP && len) {
		printf("apr: %u services\n", pl[0]);
		for (uint32_t i = 0; i < pl[0] && 1 + 2 * i + 1 < len; i++)
			printf("apr:   svc %x version %x\n", pl[1 + 2 * i], pl[2 + 2 * i]);
	} else if (h.opcode != APR_BASIC_RSP && len) {
		hex32("apr: payload", p + sizeof(h), n - sizeof(h));
	}
	if (apr_step == 1 && !getenv("ADSP_APR_NO_GATE") && (h.opcode == AVCS_GET_STATE_RSP ||
	    (h.opcode == APR_BASIC_RSP && len && pl[0] == AVCS_GET_STATE)))
		apr_gate(h.opcode == AVCS_GET_STATE_RSP, h.opcode == AVCS_GET_STATE_RSP && len ? pl[0] : 0);
	else if (apr_step >= 1 && apr_step <= 3 && (h.opcode == apr_q[apr_step - 1] + 1 ||
	    (h.opcode == APR_BASIC_RSP && len && pl[0] == apr_q[apr_step - 1])))
		apr_next(h.opcode != APR_BASIC_RSP);
	else if (afe_step > AFE_IDLE && afe_step < AFE_DONE && afe_op && h.opcode == APR_BASIC_RSP && len > 1 &&
	    pl[0] == afe_op)
		afe_next(pl[1] == 0, pl[1]);
}

/* --apr: start the queries once apr_audio_svc is open both ways; 3 s per query, then the next one. */
static void apr_poll(void)
{
	if (apr_on && apr_step > 3) {	/* ap4: --afe sequence after the queries */
		if (afe_on && afe_step == AFE_IDLE && !apr_ready && !getenv("ADSP_APR_NO_GATE")) {
			printf("afe: skipped, ADSP audio not ready\n");	/* ap5 */
			afe_step = AFE_DONE;
		} else if (afe_on && afe_step == AFE_IDLE) {
			afe_step = AFE_CLK_F3;
			afe_run();
		} else if (afe_step > AFE_IDLE && afe_step < AFE_DONE && now_ms() >= apr_due) {
			if (afe_op)
				printf("afe: op %x: no reply in 3 s\n", afe_op);
			afe_next(afe_step == AFE_HOLD, afe_op ? 0xffffffff : 0);
		}
		return;
	}
	if (!apr_on)
		return;
	if (!apr_step) {
		for (int i = 0; i < nch; i++)
			if (ch[i].up && !strcmp(ch[i].name, "apr_audio_svc")) {
				apr_chan = &ch[i];
				printf("apr: apr_audio_svc up (lcid %u, rcid %u)\n", ch[i].lcid, ch[i].rcid);
				apr_next(0);
			}
	} else if (apr_step == 1 && !getenv("ADSP_APR_NO_GATE") && now_ms() >= apr_due) {
		if (apr_gate_wait)
			printf("apr: GET_STATE: no reply in 500 ms\n");
		apr_gate(0, 0);
	} else if (now_ms() >= apr_due) {
		printf("apr: op %x: no reply in 3 s\n", apr_q[apr_step - 1]);
		apr_next(0);
	}
}

/* Parse one glink command; 0 = nothing (more) complete. Bounds: every read is checked against rx_avail. */
static int pump_one(void)
{
	if (glink_rx_avail() < 8)
		return 0;
	uint16_t h[2];
	uint32_t p2;
	glink_rx_peek(h, 0, 4);
	glink_rx_peek(&p2, 4, 4);
	size_t n = 8;
	struct chan *c = by_rcid(h[1]);
	switch (h[0]) {
	case GLINK_CMD_VERSION:
		glink_send(GLINK_CMD_VERSION_ACK, h[1], p2 & 1, NULL, 0);
		printf("adsp: glink remote version %u features %x\n", h[1], p2);
		ver_seen = 1;
		break;
	case GLINK_CMD_VERSION_ACK:
		printf("adsp: glink version acked (%u, %x)\n", h[1], p2);
		ver_seen = 1;
		break;
	case GLINK_CMD_OPEN: {
		if (p2 > 1024)
			return -1;
		n = 8 + ((p2 + 7) & ~7u);
		if (glink_rx_avail() < n)
			return 0;
		char name[64] = { 0 };
		glink_rx_peek(name, 8, p2 < 63 ? p2 : 63);
		printf("adsp: channel \"%s\" (rcid %u)\n", name, h[1]);
		if (!c && nch < NCHAN) {
			c = &ch[nch];
			memset(c, 0, sizeof(*c));
			strncpy(c->name, name, sizeof(c->name) - 1);
			c->rcid = h[1];
			c->lcid = (uint16_t)(++nch);
			glink_send(GLINK_CMD_OPEN_ACK, h[1], 0, NULL, 0);
			size_t l = strlen(c->name) + 1;
			glink_send(GLINK_CMD_OPEN, c->lcid, (uint32_t)l, c->name, l);
		}
		break;
	}
	case GLINK_CMD_OPEN_ACK:
		for (int i = 0; i < nch; i++)
			if (ch[i].lcid == h[1]) {
				printf("adsp: %s: OPEN_ACK (lcid %u)\n", ch[i].name, h[1]);
				if (apr_on && !strcmp(ch[i].name, "apr_audio_svc")) {	/* ap3: 8 x 1024 as aprtest */
					give_intents(&ch[i], 4, 1024);
					give_intents(&ch[i], 4, 1024);
				} else {
					give_intents(&ch[i], 4, 4096);
				}
				ch[i].up = 1;
			}
		break;
	case GLINK_CMD_CLOSE:
		printf("adsp: remote CLOSE cid %u (%s)\n", h[1], c ? c->name : "?");
		glink_send(GLINK_CMD_CLOSE_ACK, h[1], 0, NULL, 0);
		break;
	case GLINK_CMD_INTENT:
		if (p2 > 64)
			return -1;
		n = 8 + 8ull * p2;
		if (glink_rx_avail() < n)
			return 0;
		for (uint32_t i = 0; c && i < p2; i++) {
			uint32_t e[2];
			glink_rx_peek(e, 8 + 8 * i, 8);
			if (c->r_n < 64) {
				c->r_isz[c->r_n] = e[0];
				c->r_iid[c->r_n++] = e[1];
			} else {
				printf("adsp: %s: remote intent %u (%u bytes) dropped, table full\n", c->name, e[1], e[0]);
			}
		}
		if (!c)
			printf("adsp: %u intents for unknown cid %u dropped\n", p2, h[1]);
		if (c)
			flush_pend(c);
		break;
	case GLINK_CMD_RX_INTENT_REQ:
		printf("adsp: %s asks for an intent of %u bytes\n", c ? c->name : "?", p2);
		if (c) {
			glink_send(GLINK_CMD_RX_INTENT_REQ_ACK, c->lcid, 1, NULL, 0);
			give_intents(c, 1, 4096);
		}
		break;
	case GLINK_CMD_RX_INTENT_REQ_ACK:
		if (!p2)
			printf("adsp: %s: intent request refused (%d queued)\n", c ? c->name : "?", c ? c->npend : 0);
		break;
	case GLINK_CMD_TX_DATA:
	case GLINK_CMD_TX_DATA_CONT: {
		if (glink_rx_avail() < 16)
			return 0;
		uint32_t cl[2];
		glink_rx_peek(cl, 8, 8);
		if (cl[0] > 65536)
			return -1;
		n = (16 + cl[0] + 7) & ~(size_t)7;
		if (glink_rx_avail() < n)
			return 0;
		if (h[0] == GLINK_CMD_TX_DATA) {
			rx_have = 0;
			rx_iid = p2;
		}
		if (rx_have + cl[0] <= sizeof(rxbuf)) {
			glink_rx_peek(rxbuf + rx_have, 16, cl[0]);
			rx_have += cl[0];
		}
		if (!cl[1] && !c)
			printf("adsp: data for unknown cid %u (%zu bytes) dropped\n", h[1], rx_have);
		if (!cl[1] && c) {
			glink_send(GLINK_CMD_RX_DONE, c->lcid, rx_iid, NULL, 0);
			give_intents(c, 1, 4096);
			if (!strcmp(c->name, "IPCRTR"))
				qrtr_rx(c, rxbuf, rx_have);
			else if (rpc_on && c == rpc_chan)
				fastrpc_rx(rxbuf, rx_have);
			else if (apr_on && c == apr_chan)
				apr_rx(rxbuf, rx_have);
			else if (c->ndata++ < 4) {
				char tag[48];
				snprintf(tag, sizeof(tag), "%s data", c->name);
				hex32(tag, rxbuf, rx_have);
			}
		}
		break;
	}
	case GLINK_CMD_RX_DONE:
	case GLINK_CMD_RX_DONE_W_REUSE:
	case GLINK_CMD_SIGNALS:
	case GLINK_CMD_READ_NOTIF:
		break;
	case GLINK_CMD_CLOSE_ACK:
		close_acks++;
		break;
	default:
		printf("adsp: glink cmd %u (p1 %u p2 %u): skipped as 8 bytes\n", h[0], h[1], p2);
	}
	glink_rx_advance(n);
	return 1;
}

static void close_all(void);

/* Wait up to ms for the rx FIFO (item 480), attach and send our VERSION. */
static int edge_open(int ms)
{
	if (edge_up)
		return 0;
	int t;
	for (t = 0; t < ms / 100; t++) {
		uint64_t pa;
		uint32_t sz;
		if (smem_get(SMEM_HOST_ADSP, 480, &pa, &sz) == 0)
			break;
		usleep(100000);
	}
	glink_select(SMEM_HOST_ADSP, IPC_ADSP_GLINK);
	if (t >= ms / 100 || glink_init()) {
		printf("adsp: glink lpass edge: no rx fifo (item 480) in %d ms\n", ms);
		return -1;
	}
	printf("adsp: glink lpass edge: rx fifo after %d00 ms ok\n", t);
	glink_send(GLINK_CMD_VERSION, 1, 1, NULL, 0);	/* version 1, intent reuse */
	edge_up = 1;
	atexit(close_all);	/* ap3 */
	return 0;
}

/* Pump the edge for ms; with stop, poll SMP2P each second: 1 = ready, 2 = err-fatal, 0 = time up. */
static void rpc_send(const void *p, size_t n)
{
	chan_send(rpc_chan, p, n);
}

/* --rpc: start the FastRPC listener once fastrpcglink-apps-dsp is open both ways. */
static void rpc_poll(void)
{
	if (!rpc_on || rpc_started)
		return;
	for (int i = 0; i < nch; i++)
		if (ch[i].up && !strcmp(ch[i].name, "fastrpcglink-apps-dsp")) {
			rpc_chan = &ch[i];
			rpc_started = 1;
			fastrpc_start(rpc_send);
		}
}

static void heartbeat(void)
{
	char b[200] = "";
	uint32_t v = 0;
	int np = 0;
	smp2p_in(&v);
	for (int i = 0; i < nch; i++)
		np += ch[i].npend;
	if (rpc_on)
		fastrpc_status(b, sizeof(b));
	printf("adsp: t=%llus state %x, %s, pending tx %d\n", (unsigned long long)(now_ms() - run_t0) / 1000, v,
	       rpc_on ? b : "no rpc", np);
}

static int edge_loop(int ms, int stop, uint64_t t0)
{
	uint64_t end = now_ms() + (uint64_t)ms, next = 0, beat = now_ms() + 10000;
	if (watch_end && end > watch_end)
		end = watch_end;
	while (now_ms() < end) {
		if (got_sig) {
			printf("adsp: signal %d: stopping\n", (int)got_sig);
			heartbeat();
			if (rpc_on) {
				fastrpc_summary();
				rpc_faults();
			}
			exit(1);
		}
		if (now_ms() >= beat) {
			beat = now_ms() + 10000;
			heartbeat();
		}
		int r;
		while ((r = pump_one()) > 0)
			;
		if (r < 0) {
			printf("adsp: glink rx garbage: stop parsing\n");
			return 0;
		}
		rpc_poll();
		apr_poll();
		if (stop && now_ms() >= next) {
			next = now_ms() + 1000;
			uint32_t v;
			if (smp2p_in(&v) == 0 && (v & 1)) {
				printf("adsp: CRASH after %llu ms (state %x)\n", (unsigned long long)(now_ms() - t0), v);
				crash_reason();
				return 2;
			}
			if (smp2p_in(&v) == 0 && (v & 2) && !ready_seen) {
				printf("adsp: READY after %llu ms (state %x)\n", (unsigned long long)(now_ms() - t0), v);
				ready_seen = 1;
				if (!rpc_on && !apr_on)
					return 1;	/* --rpc: keep serving until the watch ends or a crash */
			}
		}
		usleep(2000);
	}
	return 0;
}

static int glink_watch(int secs)
{
	if (edge_open(secs * 1000))
		return -1;
	edge_loop(rpc_on || apr_on ? secs * 1000 : 5000, 0, 0);	/* --rpc: up to watch_end (au14) */
	printf("adsp: glink %s, %d channels\n", ver_seen ? "up" : "no version from the ADSP", nch);
	return ver_seen ? 0 : -1;
}

/*
 * ap3: CLOSE every channel we opened when adspboot exits (atexit), so a later run on the same ADSP boot starts
 * clean; only while the ADSP is up and not crashed. Off by default since apr3 (fastrpc_port_s.c:176 assert); ADSP_CLOSE=1 turns it on.
 */
static void close_all(void)
{
	uint32_t v;
	if (!edge_up || !nch || !getenv("ADSP_CLOSE") || smp2p_in(&v) || (v & 1) || !(v & 2))
		return;
	for (int i = 0; i < nch; i++)
		glink_send(GLINK_CMD_CLOSE, ch[i].lcid, 0, NULL, 0);
	uint64_t end = now_ms() + 500;
	while (close_acks < nch && now_ms() < end) {
		while (pump_one() > 0)
			;
		usleep(2000);
	}
	printf("adsp: CLOSE %d channels, %d acked\n", nch, close_acks);
}

/* SCM PIL shutdown (cmd 6), as wland: lets a rerun after a crash boot again without a reboot. */
static void pas_shutdown(void)
{
	uint64_t r1 = 0;
	int64_t rc = scm(6, 1, PAS_ADSP, 0, 0, &r1);
	printf("adsp: pas shutdown(%d) -> %lld/%llx\n", PAS_ADSP, (long long)rc, (unsigned long long)r1);
}

static int status(void)
{
	uint32_t v;
	uint64_t pa;
	uint32_t sz;
	if (smp2p_in(&v))
		printf("adsp: smp2p in item %u: none\n", SMP2P_IN);
	else
		printf("adsp: state %x%s%s%s%s\n", v, v & 1 ? " ERR-FATAL" : "", v & 2 ? " ready" : "",
		       v & 4 ? " handover" : "", v & 8 ? " stop-ack" : "");
	for (unsigned it = 478; it <= 480; it++)
		printf("adsp: glink item %u: %s\n", it, smem_get(SMEM_HOST_ADSP, it, &pa, &sz) ? "none" : "present");
	crash_reason();
	return 0;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	run_t0 = now_ms();
	signal(SIGTERM, on_sig);
	signal(SIGINT, on_sig);
	if (smem_init())
		return 1;
	printf("adsp: smem ok\n");
	int watch = 20, early = 1, smmu = 0, unsafe = 0, i;
	uint32_t sid = 0, mask = 0;
	int one = 0, sstatus = 0, sdump = 0, root = 0, sensors = 0;
	const char *cmd = "boot";
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--watch") && i + 1 < argc)
			watch = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--no-early"))
			early = 0;
		else if (!strcmp(argv[i], "--no-sleepstate"))
			sleepstate = 0;
		else if (!strcmp(argv[i], "--no-pdmap"))
			pdmap = 0;
		else if (!strcmp(argv[i], "--smmu"))
			smmu = 1;
		else if (!strcmp(argv[i], "--unsafe"))
			unsafe = 1;
		else if (!strcmp(argv[i], "--rpc"))
			rpc_on = smmu = 1;
		else if (!strcmp(argv[i], "--status"))
			sstatus = 1;
		else if (!strcmp(argv[i], "--dump"))
			sdump = 1;
		else if (!strcmp(argv[i], "--root"))
			fastrpc_root(root = 1);
		else if (!strcmp(argv[i], "--sensors"))
			sensors = 1;
		else if (!strcmp(argv[i], "--apr"))
			apr_on = 1;
		else if (!strcmp(argv[i], "--afe"))	/* ap4: AFE port start without amps; implies --apr */
			apr_on = afe_on = 1;
		else if (!strcmp(argv[i], "--stub-svc") && i + 1 < argc && nstub < MAX_STUB) {
			char *e;
			stub[nstub].svc = (uint32_t)strtoul(argv[++i], &e, 0);
			stub[nstub].ver = *e == ':' ? (uint32_t)strtoul(e + 1, &e, 0) : 1;
			stub[nstub].inst = *e == ':' ? (uint32_t)strtoul(e + 1, &e, 0) : 0;
			nstub++;
		}
		else if (!strcmp(argv[i], "--dry"))
			smmu_dry = 1;
		else if (!strcmp(argv[i], "--sid") && i + 2 < argc) {
			sid = (uint32_t)strtoul(argv[++i], NULL, 0);
			mask = (uint32_t)strtoul(argv[++i], NULL, 0);
			one = 1;
		}
		else
			cmd = argv[i];
	}
	if (sensors && !root) {	/* step 6: the sensors PD after the root PD needs the root session */
		fprintf(stderr, "adspboot: --sensors needs --rpc --root\n");
		return 2;
	}
	fastrpc_sensors(sensors);
	if (!strcmp(cmd, "status"))
		return status();
	if (!strcmp(cmd, "shutdown")) {
		pas_shutdown();
		return 0;
	}
	if (rpc_on && !unsafe && strcmp(cmd, "smmu")) {
		/* rpc2 (buffer at 0xe0f80000, identity) and rpc3 (pm_alloc buffer at 32-bit IOVA 0x80000000) both reset
		 * the SoC right after the attach: the DSP reaching any HLOS buffer is fatal until the memory is shared
		 * with its VM. */
		fprintf(stderr, "adspboot: --rpc resets the phone (rpc2, rpc3, docs/features/audio.md); add --unsafe\n");
		return 2;
	}
	if (!strcmp(cmd, "smmu") && sstatus) {
		smmu_status("ADSP rpc", 0x1b23);
		if (sdump) {	/* audio step 6 */
			smmu_dump("ADSP rpc", 0x1b23, 0x80000000ull);
			smmu_dump("ADSP rpc", 0x1b23, 0x380000000ull);
		}
		return 0;
	}
	if (!strcmp(cmd, "smmu"))
		return adsp_smmu(one, sid, mask) ? 1 : 0;
	if (strcmp(cmd, "boot")) {
		fprintf(stderr, "usage: adspboot [boot|status|shutdown|smmu] [--watch S] [--no-early] [--no-pdmap] [--smmu] [--rpc [--root [--sensors]]] [--apr] [--afe] [--dry] [--status] [--sid SID MASK]\n");
		return 2;
	}
	if (smmu && adsp_smmu(one, sid, mask))
		return 1;
	if (rpc_on || apr_on)	/* au14: --watch N is the whole serve, not N per phase; ADSP_WATCH_PER_PHASE=1 for the old way */
		watch_end = getenv("ADSP_WATCH_PER_PHASE") ? 0 : now_ms() + (uint64_t)watch * 1000;

	uint32_t v;
	if (smp2p_in(&v) == 0 && (v & 2) && !(v & 1)) {
		printf("adsp: already running (state %x), skipping the boot\n", v);
	} else {
		/* adsp3: shutdown of a never-started image blocked forever (fresh boot, state 0): only after an earlier run. */
		int sv = smp2p_in(&v);
		printf("adsp: smp2p in %d state %x\n", sv, v);
		if (sv == 0 && v)
			pas_shutdown();
		if (boot())
			return 1;
		uint64_t t0 = now_ms();
		int st = 0;
		if (early && edge_open(2000) == 0) {
			st = edge_loop(watch * 1000, 1, t0);
			if (st == 0 && ready_seen)
				st = 1;
			printf("adsp: early watch: %s, %d channels\n", st == 1 && !(watch_end && now_ms() >= watch_end) ? "ready" :
			       st == 1 ? "ready, watch over" : st == 2 ? "crash" : "time up",
			       nch);
			if (st == 2) {
				if (rpc_on) {
					fastrpc_summary();
					rpc_faults();
				}
				return 1;
			}
		}
		int t;
		for (t = 0; st != 1 && t < 100; t++) {
			if (smp2p_in(&v) == 0 && (v & 3))
				break;
			if (edge_up)
				edge_loop(100, 0, 0);
			else
				usleep(100000);
		}
		if (st != 1) {
			if (t >= 100) {
				printf("adsp: no ready in 10 s\n");
				status();
				return 1;
			}
			if (v & 1) {
				printf("adsp: ERR-FATAL after %d00 ms (state %x)\n", t, v);
				crash_reason();
				return 1;
			}
			printf("adsp: ready after %d00 ms (state %x)\n", t, v);
		}
	}
	int rc = glink_watch(rpc_on || apr_on ? watch : 10) ? 1 : 0;
	if (rpc_on) {
		fastrpc_summary();
		rpc_faults();
	}
	return rc;
}
