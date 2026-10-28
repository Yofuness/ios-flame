/*
 * adspboot: boot the Pixel 4a's audio DSP (ADSP / LPASS) through TrustZone
 * PAS from userland (/dev/pmem), the way wland boots the modem (audio M0,
 * docs/features/audio.md). No audio: it reports whether the glink "lpass"
 * edge comes up and which channels the ADSP opens.
 *
 *   adspboot [boot]   votes, load adsp.mdt + .bNN, PAS init / mem setup / auth-and-reset, SMP2P ready, glink
 *   adspboot status   read-only: SMP2P state, glink items, crash reason
 *
 * Facts (recon/live.dts): qcom,lpass@62400000 pas-id 1, smem-id 423 (crash reason), proxy rails lcx.lvl /
 * lmx.lvl (>= 385); pil_adsp_mem 0x96b00000+0x2200000; smp2p-adsp out 443 / in 429, IPC bit 26; glink_adsp
 * remote-pid 2, IPC bit 24. adsp.mdt is relocatable (linked at 0x93c00000): segments go to
 * region + (paddr - lowest paddr), as Linux qcom_mdt_load.
 * Never touches the modem: SMEM host 2 only, APCS bits 24/26, AOSS bit 0. Exit 0 = edge up.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pmem.h"
#include "smem.h"
#include "glink.h"

#define PAS_ADSP	1
#define ADSP_PA		0x96b00000ull
#define ADSP_SIZE	0x02200000ull
#define FW_DIR		"/var/local/fw/adsp"
#define SMEM_HOST_ADSP	2
#define CRASH_ITEM	423
#define SMP2P_OUT	443
#define SMP2P_IN	429
#define APCS_IPC	0x17c0000cull
#define IPC_AOSS	0
#define IPC_ADSP_GLINK	24
#define IPC_ADSP_SMP2P	26
#define SCM_PIL(cmd)	(0x42000000ull | (2u << 8) | (cmd))
#define MDT_RELOC	(1u << 27)

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

static int smp2p_create(void)
{
	uint64_t pa;
	if (smem_alloc(SMEM_HOST_ADSP, SMP2P_OUT, 20 + 16 * 20, &pa)) {
		fprintf(stderr, "adsp: smp2p: cannot allocate item %u\n", SMP2P_OUT);
		return -1;
	}
	if (rd32(pa) != 0x504d5324) {
		wr32(pa + 8, 0 | (2u << 16));		/* local pid 0, remote pid 2 */
		wr32(pa + 12, 16 | (1u << 16));
		wr32(pa + 16, 0);
		wr_name(pa + 20, "master-kernel");
		wr32(pa + 36, 0);
		wr32(pa + 0, 0x504d5324);		/* "$SMP" */
		__asm__ volatile("dsb sy" ::: "memory");
		wr32(pa + 4, 1 | (1u << 8));
	}
	kick(IPC_ADSP_SMP2P);
	printf("adsp: smp2p out item %u at %llx ok\n", SMP2P_OUT, (unsigned long long)pa);
	return 0;
}

/* The ADSP's "slave-kernel" bits (0 err-fatal, 1 ready, 2 handover, 3 stop-ack); -1 = no item yet. */
static int smp2p_in(uint32_t *v)
{
	uint64_t pa;
	uint32_t sz;
	if (smem_get(SMEM_HOST_ADSP, SMP2P_IN, &pa, &sz))
		return -1;
	uint32_t n = rd32(pa + 12) >> 16;
	for (uint32_t i = 0; i < n && i < 16; i++) {
		uint64_t e = pa + 20 + 20ull * i;
		char nm[17];
		for (int k = 0; k < 16; k++)
			nm[k] = (char)(rd32(e + (k & ~3)) >> (8 * (k & 3)));
		nm[16] = 0;
		if (!strcmp(nm, "slave-kernel")) {
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
	if (rc || r1 != 1)
		return -1;
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

/* ---- glink "lpass" edge: handshake and the channels the ADSP opens ---- */

static int glink_watch(int secs)
{
	int t;
	for (t = 0; t < secs * 10; t++) {
		uint64_t pa;
		uint32_t sz;
		if (smem_get(SMEM_HOST_ADSP, 480, &pa, &sz) == 0)
			break;
		usleep(100000);
	}
	glink_select(SMEM_HOST_ADSP, IPC_ADSP_GLINK);
	if (t >= secs * 10 || glink_init()) {
		printf("adsp: glink lpass edge: no rx fifo (item 480) in %d s\n", secs);
		return -1;
	}
	printf("adsp: glink lpass edge: rx fifo after %d00 ms ok\n", t);
	glink_send(GLINK_CMD_VERSION, 1, 1, NULL, 0);	/* version 1, intent reuse */
	int up = 0, chans = 0;
	for (t = 0; t < 50; t++) {
		while (glink_rx_avail() >= 8) {
			uint16_t h[2];
			uint32_t p2;
			glink_rx_peek(h, 0, 4);
			glink_rx_peek(&p2, 4, 4);
			size_t n = 8;
			if (h[0] == GLINK_CMD_VERSION) {
				glink_send(GLINK_CMD_VERSION_ACK, h[1], p2 & 1, NULL, 0);
				printf("adsp: glink remote version %u features %x\n", h[1], p2);
				up = 1;
			} else if (h[0] == GLINK_CMD_VERSION_ACK) {
				printf("adsp: glink version acked (%u, %x)\n", h[1], p2);
				up = 1;
			} else if (h[0] == GLINK_CMD_OPEN) {
				n = 8 + ((p2 + 7) & ~7u);
				if (glink_rx_avail() < n)
					break;
				char name[64] = { 0 };
				glink_rx_peek(name, 8, p2 < 63 ? p2 : 63);
				printf("adsp: channel \"%s\" (rcid %u)\n", name, h[1]);
				chans++;
			} else if (h[0] == GLINK_CMD_INTENT) {
				n = 8 + 8ull * p2;	/* param1 = channel id, param2 = intent count (8 bytes each) */
				if (glink_rx_avail() < n)
					break;
			} else if (h[0] != GLINK_CMD_CLOSE && h[0] != GLINK_CMD_SIGNALS && h[0] != GLINK_CMD_READ_NOTIF) {
				printf("adsp: glink cmd %u (p1 %u p2 %u): stop parsing\n", h[0], h[1], p2);
				return up ? 0 : -1;
			}
			glink_rx_advance(n);
		}
		usleep(100000);
	}
	printf("adsp: glink %s, %d channels\n", up ? "up" : "no version from the ADSP", chans);
	return up ? 0 : -1;
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
	if (smem_init())
		return 1;
	printf("adsp: smem ok\n");
	if (argc > 1 && !strcmp(argv[1], "status"))
		return status();
	if (argc > 1 && strcmp(argv[1], "boot")) {
		fprintf(stderr, "usage: adspboot [boot|status]\n");
		return 2;
	}

	uint32_t v;
	if (smp2p_in(&v) == 0 && (v & 2) && !(v & 1)) {
		printf("adsp: already running (state %x), skipping the boot\n", v);
	} else {
		if (boot())
			return 1;
		int t;
		for (t = 0; t < 100; t++) {
			if (smp2p_in(&v) == 0 && (v & 3))
				break;
			usleep(100000);
		}
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
	return glink_watch(10) ? 1 : 0;
}
