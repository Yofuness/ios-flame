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
 *   adspboot boot --rpc [--watch 120]  (0 = no limit) also serve FastRPC (fastrpc.c): attach to the sensors PD on
 *                     fastrpcglink-apps-dsp, run adsp_default_listener and answer its apps_std file calls
 *                     from /var/local/fw/sensors (+ /var/local/sensors-persist); keeps serving after ready,
 *                     summary at the end. Implies --smmu; the FastRPC streams then go through a translating bank
 *                     (32-bit IOVA 0x80000000 -> a pm_alloc buffer). `adspboot smmu --rpc --dry` shows that plan.
 *   adspboot boot --rpc --root --unsafe  attach to the root PD (pd 0), print the reply and the SMMU fault state
 *                     (audio step 5), then serve its listener (apps_std files, step 6) for --watch seconds
 *                     [--sensors]  then also attach the sensors PD (second half of the buffer, step 6)
 *   adspboot boot --afe [--watch 40]  ap4: --apr, then the SEC_MI2S_RX port: clock, I2S config, start, 2 s, stop, clock off
 *                     sp2: first an LPASS core hw vote (sp2b: 0x100F4, --vote-block N), LPI pin mux (gpio8-10) after
 *                     DEVICE_START, undone before DEVICE_STOP; --no-lpi skips both
 *   adspboot boot --swr-probe [--swr-clk]  hj0: --afe's LPASS hw vote, then READ ONLY the TX/RX SoundWire masters,
 *                     the TX/VA macros and LPI gpio24 (codec reset); --swr-clk also votes the va/tx core + tx npl
 *                     codec clocks (0x30b-0x30d) around a second read; devote, exit. Never writes those blocks.
 *                     hj1: --swr-only N (1 gpio24, 2 va, 3 tx-macro, 4 rx-swr, 5 tx-swr), --swr-nohw (group 1, no vote);
 *                     every swr/vote line also goes to /var/local/swr-probe.log (fsync per line)
 *                     hj2: --swr-clk-first (clock votes before any read), --swr-newlog
 *                     hj3: + rx clocks 0x30e/0x30f; --swr-clk-first reads each group only if its own clocks are on
 *                     hj4: --swr-init a|b|c|d: S2 staged TX SoundWire bring-up (macro+pins, codec reset, master init,
 *                     DevID), teardown after; implies --swr-clk-first, refused unless 0x30c/0x30d acked
 *                     hj5: --swr-mbhc SECONDS [--swr-mbhc-unmask]: S3 WCD937x mech plug poll on top of --swr-init d
 *   adspboot boot --pcm --amp --rec [--rec-hex]  sp7: also record the left amp's VMON/IMON over SEC_MI2S_TX (0x1003,
 *                     SD1, gpio11) around the tone: RMS/peak per 100 ms and baseline vs tone (vi-loopback.md)
 *   adspboot boot --pcm [--pcm-sid 0x1b21] [--pcm-shift 1]  ap6: --afe, and while the port runs a 3 s 1 kHz -20 dBFS
 *                     sine through ASM + ADM (buffer through a translating bank for the SID; amps untouched)
 *   [--intent-once]  ap7: delete a remote intent on tx instead of waiting for the ADSP's RX_DONE[_W_REUSE] (or ADSP_INTENT_ONCE=1)
 *   [--no-reuse]  ap7: glink rx as before (RX_DONE + a new intent) instead of RX_DONE_W_REUSE (or ADSP_NO_REUSE=1)
 *   adspboot boot --apr [--watch 40]  ap3: also open apr_audio_svc in this process and send the APR core
 *                     queries (GET_STATE, FWK_VERSION); combines with --rpc --root --sensors. Channels are
 *                     CLOSEd at exit only with ADSP_CLOSE=1 (apr3: closing the FastRPC channel crashed the ADSP: fastrpc_port_s.c:176)
 *   adspboot boot --rpc --root --sensors --unsafe --sns-list | --sns-stream TYPE [--sns-rate HZ] [--sns-secs N] |
 *                     --sns-motion [--sns-motion-dev PATH]  sns1: SNS (QMI svc 400, SEE) client on our QRTR
 *                     (../sensors/sns_run.h); without these options nothing changes
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
#include <stdarg.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <math.h>
#include "pmem.h"
#include "smem.h"
#include "glink.h"
#include "smmu.h"
#include "fastrpc.h"
#include "pdmapper.h"
#include "qmi.h"
#include "geni_i2c.h"
#include "cs35l41.h"
#include "../sensors/sns_run.h"

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

/* ap6: `--pcm` buffer (see the ASM/ADM section): its own translating bank for the audio-ion stream (0x1b21 by
 * default, `--pcm-sid SID`), same reference values as 0x1b23's; the DSP gets IOVA | (sid & 0xf) << 32 (`--pcm-shift N`). */
#define PCM_IOVA	0xa0000000ull
#define PCM_PERIODS	8
#define PCM_STRIDE	0x1000		/* one page per period */
#define PCM_BUF		(PCM_PERIODS * PCM_STRIDE)
#define REC_READS	4		/* sp7: --rec READ_V2 buffers after the playback pages, one page each */
#define REC_BYTES	0x1000
static int pcm_on, rec_on, rec_hex;
static uint32_t pcm_buf_len = PCM_BUF;	/* sp7: + REC_READS pages with --rec, same mapping */
static uint32_t pcm_sid = 0x1b21, pcm_shift = 1;
static uint64_t pcm_pa, pcm_dsp;

static int pcm_smmu(void)
{
	uint16_t sids[1] = { (uint16_t)pcm_sid };
	if (rec_on)
		pcm_buf_len = PCM_BUF + REC_READS * REC_BYTES;
	pcm_pa = smmu_dry ? 0x100000000ull : pm_alloc(pcm_buf_len, 0x1000);
	if (!pcm_pa)
		return 1;
	pcm_dsp = PCM_IOVA | (uint64_t)pcm_shift << 32;
	smmu_alias = (uint64_t)pcm_shift << 32;
	int bad = smmu_translate("ADSP pcm", sids, 1, PCM_IOVA, pcm_pa, pcm_buf_len);
	smmu_alias = 3ull << 32;
	printf("pcm: buffer pa %llx (%u B), DSP address %llx (sid %#x, stream bits %u)%s\n", (unsigned long long)pcm_pa,
	       pcm_buf_len, (unsigned long long)pcm_dsp, pcm_sid, pcm_shift, bad ? ": SMMU FAILED" : "");
	if (!bad && !smmu_dry)
		smmu_faults("ADSP pcm", pcm_sid);	/* before */
	return bad;
}

static int adsp_smmu(int one, uint32_t sid, uint32_t mask)
{
	if (one)
		return smmu_identity("ADSP", sid, mask);
	int bad = 0;
	for (unsigned i = 0; i < sizeof(adsp_sids) / sizeof(adsp_sids[0]); i++)
		if ((!rpc_on || adsp_sids[i] != rpc_sids[0]) && !(pcm_on && adsp_sids[i] == pcm_sid))
			bad |= smmu_identity("ADSP", adsp_sids[i], 0);
	if (rpc_on && !bad) {
		uint64_t pa = smmu_dry ? 0x100000000ull : pm_alloc(FASTRPC_DMA_SIZE, 0x1000);
		if (!pa)
			return 1;
		bad |= smmu_translate("ADSP rpc", rpc_sids, 1, RPC_IOVA, pa, FASTRPC_DMA_SIZE);
		if (!bad && !smmu_dry)
			fastrpc_buffer(pa, RPC_DSP_ADDR);
	}
	if (pcm_on && !bad)
		bad |= pcm_smmu();
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
#define NPEND		64	/* ap7: 64 (was 4): --pcm WRITEs wait here for an intent */
#define NRINT		512	/* ap7: remote intents per channel (was 64 and dropped the rest: the ADSP still counted them) */
#define NLOCAL		8	/* ap7: free local rx intents kept queued per channel */

struct chan {
	char name[32];
	uint16_t rcid, lcid;
	uint32_t *r_iid, *r_isz;	/* ap7: NRINT each, allocated on the first INTENT */
	uint8_t *r_use;	/* ap7: remote intent carries our tx, until the ADSP's RX_DONE[_W_REUSE] */
	int r_n, r_busy, ndata, up;
	uint32_t ndone, nreuse;	/* ap7: RX_DONE / RX_DONE_W_REUSE received */
	int l_n;	/* ap7: our rx intents the remote still holds unused */
	uint32_t nrx, nreq;	/* ap7: packets received, RX_INTENT_REQs sent */
	uint8_t pend[NPEND][512];	/* (pd-mapper replies are ~150 bytes) */
	uint32_t pend_len[NPEND];
	int npend;
};
static int intent_once;	/* ap7: --intent-once / ADSP_INTENT_ONCE=1: a remote intent is gone after one tx (the old way) */
static int no_reuse;	/* ap7: --no-reuse / ADSP_NO_REUSE=1: RX_DONE + a fresh intent per packet (the old way) */
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
	c->l_n += n;
}

/* Send one packet on c using a remote intent; without one, queue it and ask (RX_INTENT_REQ). */
static void chan_send(struct chan *c, const void *pkt, size_t n)
{
	int i;
	for (i = 0; i < c->r_n && (c->r_isz[i] < n || c->r_use[i]); i++)
		;
	if (i >= c->r_n) {
		if (c->npend < NPEND && n <= sizeof(c->pend[0])) {
			memcpy(c->pend[c->npend], pkt, n);
			c->pend_len[c->npend++] = (uint32_t)n;
			glink_send(GLINK_CMD_RX_INTENT_REQ, c->lcid, (uint32_t)n, NULL, 0);
			if (c->nreq++ < 8)	/* ap7 */
				printf("adsp: %s: no remote intent for %zu bytes: queued (%d), RX_INTENT_REQ sent\n", c->name,
				       n, c->npend);
		} else {
			printf("adsp: %s: tx %zu bytes dropped (no intent, queue full)\n", c->name, n);
		}
		return;
	}
	uint32_t iid = c->r_iid[i];
	if (c == apr_chan)	/* ap5: which remote intent carried the APR packet */
		printf("adsp: %s: tx %zu bytes on remote intent %u (%u bytes, %d free)\n", c->name, n, iid, c->r_isz[i],
		       c->r_n - c->r_busy - 1);
	if (intent_once) {
		c->r_iid[i] = c->r_iid[--c->r_n];
		c->r_isz[i] = c->r_isz[c->r_n];
	} else {	/* ap7: held until the ADSP's RX_DONE (gone) or RX_DONE_W_REUSE (free again) */
		c->r_use[i] = 1;
		c->r_busy++;
	}
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

/*
 * sns1: the SNS client's QRTR ports. sns2: one per sensor stream (SNS_LINK_PORT0 + i, as redfin's socket per
 * sensor); qrtr_rx hands their DATA to sns_link's per-port queues, and sns_link pumps the edge through sns_pump while
 * sensor.c waits for a response (FastRPC keeps being served meanwhile).
 */
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
	} else if (ty == 4 && blen >= 20 && sns_wanted()) {	/* sns1: the ADSP's SNS service */
		uint32_t k[5];
		memcpy(k, body, 20);
		if (k[1] == SNS_QMI_SERVICE && (k[3] || k[4]))
			sns_service(k[3], k[4]);
	} else if (ty == 1 && sns_link_is_port(h[7]) && sns_wanted()) {	/* sns1, sns2: any stream's port */
		if (h[4]) {			/* RESUME_TX names the port the packet was for */
			uint32_t k[5] = { QRTR_TYPE_RESUME_TX, QRTR_NODE_AP, h[7], 0, 0 };
			qrtr_send(QRTR_TYPE_RESUME_TX, QRTR_PORT_CTRL, h[2], h[3], k, sizeof(k));
		}
		sns_link_push(h[7], h[2], h[3], body, blen);
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
/* sp2: LPASS core hw vote before the clock set (research/notes/amp-clock.md; layout as apr_tone.c --lpi-pins).
 * sp2b: opcodes 0x100F4/F5/F6 as Linux apr_audio-v2.h (0x1027C got status 3, EUNSUPPORTED in sp2a); a refused or
 * silent vote is logged and the run goes on without a handle. The LPI TLMM mux now runs only after DEVICE_START
 * succeeded (the port runs, so the LPASS core clocks are on and LPI register reads can't hang the bus) and is
 * restored before DEVICE_STOP. `--vote-block N` (default 2 = AFE_LPASS_CORE_HW_MACRO_BLOCK, the block
 * pinctrl-lpi votes for), `--no-lpi` skips vote and pins. */
#define AFE_CMD_REMOTE_LPASS_CORE_HW_VOTE_REQUEST 0x000100f4
#define AFE_CMD_RSP_REMOTE_LPASS_CORE_HW_VOTE_REQUEST 0x000100f5
#define AFE_CMD_REMOTE_LPASS_CORE_HW_DEVOTE_REQUEST 0x000100f6
#define LPI_PA			0x62b40000ull	/* LPI TLMM, gpio n CFG at + 0x1000 * n (live.dts:22897) */

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

enum { AFE_IDLE, AFE_VOTE, AFE_CLK_F3, AFE_CLK_FC, AFE_CFG, AFE_START, AFE_HOLD, AFE_STOP, AFE_CLK_OFF, AFE_SWR_CLK, AFE_SWR_UNCLK,
       AFE_DEVOTE, AFE_DONE };	/* hj0: SWR steps only via --swr-probe */
static int afe_on, afe_step, afe_clk_ok, afe_started;
static int no_lpi, hw_handle = -1, lpi_set;	/* sp2 */
static uint32_t vote_block = 2;	/* sp2b: --vote-block */
static uint32_t lpi_saved[4];
static uint32_t afe_op, afe_clk_op;	/* afe_op: the opcode whose basic response we wait for */
/* hj0: --swr-probe (headphone jack S0, research/notes/headphone-jack.md). Addresses from recon/live.dts:
 * tx_swr_master swrm-io-base 0x62ed0000 (:20799), rx_swr_master 0x62ef0000 (:20833), tx-macro@62ec0000 (:20784),
 * va-macro@62f20000 (:20776), LPI gpio24 = LPI_PA + 0x18000 (lpi_pinctrl@62b40000 :22897, gpio24 :22944).
 * --swr-clk: va_core 0x30b, tx_core 0x30c, tx_npl 0x30d, each 0x124f800 = 19.2 MHz (:23505, :23532, :23541). */
static int swr_on, swr_clk, swr_i;
static int swr_only, swr_nohw;	/* hj1: --swr-only N (0 all, 1 gpio24 .. 5 tx-swr), --swr-nohw (group 1, no vote) */
static int swr_fd = -1;
static int swr_clk_first, swr_newlog;	/* hj2: --swr-clk-first (clock votes before any read), --swr-newlog */
static unsigned swr_clk_ok;	/* bit i: swr_ids[i] voted, to un-vote */
#define SWR_NCLK 5
/* hj3: + rx_core 0x30e, rx_npl 0x30f at 0x1588800 = 22.5792 MHz (live.dts rx_core_clk/rx_npl_clk, ~:23510-:23524) */
static const uint32_t swr_ids[SWR_NCLK] = { 0x30b, 0x30c, 0x30d, 0x30e, 0x30f };
static const uint32_t swr_hz[SWR_NCLK] = { 0x124f800, 0x124f800, 0x124f800, 0x1588800, 0x1588800 };
/* hj3: clocks each group needs (bits of swr_ids): 1 none, 2 va_core, 3/5 tx_core+tx_npl, 4 rx_core+rx_npl */
static const unsigned swr_need[6] = { 0, 0, 1u << 0, 3u << 1, 3u << 3, 3u << 1 };
static int swr_gate;	/* hj3: swr_read skips a group whose clocks are not all in swr_clk_ok */

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

static int amp_invert_clk;	/* sp8: --amp-invert-clk */
static int i2s_bits = 16;	/* pcm32: --i2s-32: 32-bit I2S slots, BCLK 48000*32*2 */
static int adm_bits;		/* pcm32: --adm-bits N (0 = 16, or 24 with --i2s-32 as ios-redfin) */

static void afe_clock_id(uint32_t opcode, uint32_t id, uint32_t hz, uint16_t attr, int enable)
{
	uint8_t b[16 + sizeof(struct afe_param) + sizeof(struct afe_clock_set)] = { 0 };
	struct afe_param p = { AFE_MODULE_CLOCK_SET, AFE_PARAM_ID_CLOCK_SET, sizeof(struct afe_clock_set), 0 };
	struct afe_clock_set c = { 1, id, hz, attr, 0, (uint32_t)enable };
	uint32_t size = sizeof(p) + sizeof(c);	/* payload size; address 0/0 and mem_map_handle 0: in band */
	memcpy(b, &size, 4);
	memcpy(b + 16, &p, sizeof(p));
	memcpy(b + 16 + sizeof(p), &c, sizeof(c));
	afe_send(0, opcode, b, sizeof(b));
}

static void afe_clock(uint32_t opcode, int enable)
{
	afe_clock_id(opcode, AFE_CLK_SEC_MI2S_IBIT, 48000u * (uint32_t)i2s_bits * 2,
		     amp_invert_clk ? 4 /* sp8: INVERT_COUPLE_NO, ios-redfin */ : 1 /* COUPLE_NO */, enable);
}

/* hj1: stdout plus /var/local/swr-probe.log (write + fsync per line: survives a reset), ms timestamp */
static void swr_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void swr_log(const char *fmt, ...)
{
	char b[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	fputs(b, stdout);
	fflush(stdout);
	if (!swr_on)
		return;
	if (swr_fd < 0)
		swr_fd = open("/var/local/swr-probe.log", O_WRONLY | O_APPEND | O_CREAT, 0644);
	if (swr_fd < 0)
		return;
	char l[300];
	int n = snprintf(l, sizeof(l), "[%llu ms] %s", (unsigned long long)now_ms(), b);
	if (n > (int)sizeof(l) - 1)
		n = sizeof(l) - 1;
	if (write(swr_fd, l, (size_t)n) == n)
		fsync(swr_fd);
}

/* hj0: one read with a stage line first (a bus hang or reset then shows which register) */
static uint32_t swr_rd(const char *what, uint64_t pa)
{
	swr_log("swr: stage read %s %#llx\n", what, (unsigned long long)pa);
	uint32_t v = rd32(pa);
	swr_log("swr: %s %#llx = %#010x\n", what, (unsigned long long)pa, v);
	return v;
}

/* hj1: groups safest first: 1 gpio24, 2 va-macro, 3 tx-macro, 4 rx-swr, 5 tx-swr; --swr-only N runs one */
static void swr_read(const char *when)
{
	swr_log("swr: --- reads (%s, group %d) ---\n", when, swr_only);
	for (int g = 1; swr_gate && g <= 5; g++) {	/* hj3: per-group clock check */
		unsigned miss = swr_need[g] & ~swr_clk_ok;
		if ((swr_only && swr_only != g) || !miss)
			continue;
		swr_log("swr: group %d skipped: clock %#x not on\n", g, swr_ids[__builtin_ctz(miss)]);
		if (swr_only)
			return;
	}
	if (swr_gate && !swr_only) {	/* hj3: all groups: read only the clocked ones */
		int keep = swr_only;
		for (int g = 1; g <= 5; g++)
			if (!(swr_need[g] & ~swr_clk_ok)) {
				swr_gate = 0, swr_only = g;
				swr_read(when);
				swr_gate = 1;
			}
		swr_only = keep;
		return;
	}
	if (!swr_only || swr_only == 1) {
		swr_rd("lpi gpio24 cfg", LPI_PA + 0x1000u * 24);
		swr_rd("lpi gpio24 in/out", LPI_PA + 0x1000u * 24 + 4);
	}
	if (!swr_only || swr_only == 2)
		swr_rd("va-macro (live.dts va-macro@62f20000)", 0x62f20000ull);
	if (!swr_only || swr_only == 3)
		swr_rd("tx-macro (live.dts tx-macro@62ec0000)", 0x62ec0000ull);
	if (!swr_only || swr_only == 4)
		swr_rd("rx-swr version", 0x62ef0000ull);
	if (!swr_only || swr_only == 5)
		for (unsigned i = 0; i < 0x10; i++)
			swr_rd(i ? "tx-swr" : "tx-swr version", 0x62ed0000ull + 4 * i);
}

/* hj4: --swr-init a|b|c|d (headphone jack S2, research/notes/headphone-jack.md "S2 plan"): staged TX SoundWire
 * bring-up after the tx clocks 0x30c/0x30d are acked, then teardown in reverse. Only the whitelisted registers. */
#define SWR_TXM		0x62ec0000ull	/* tx-macro */
#define SWR_TXS		0x62ed0000ull	/* tx swrm, v1.5.0 */
static int swr_stage;	/* hj4: 0 off, 'a'..'d' */

static int swr_allowed(uint64_t pa)
{
	static const uint32_t m[] = { 0x00, 0x04, 0x08, 0x88, 0x90 };
	static const uint32_t s[] = { 0x04, 0x14, 0x100, 0x200, 0x204, 0x208, 0x300, 0x304, 0x308, 0x30c, 0x314, 0x318, 0x500, 0x530,
				      0x534, 0x101c, 0x1044, 0x1048, 0x1090 };
	for (unsigned i = 0; i < sizeof(m) / sizeof(m[0]); i++)
		if (pa == SWR_TXM + m[i])
			return 1;
	for (unsigned i = 0; i < sizeof(s) / sizeof(s[0]); i++)
		if (pa == SWR_TXS + s[i])
			return 1;
	for (unsigned g = 18; g <= 24; g++)
		if ((g <= 20 || g == 24) && (pa == LPI_PA + 0x1000u * g || pa == LPI_PA + 0x1000u * g + 4))
			return 1;
	return 0;
}

static int swr_refused;
static int swr_mbhc, swr_mbhc_unmask;	/* hj5: --swr-mbhc SECONDS (S3 plug poll), --swr-mbhc-unmask (0x346B = 0xEF) */
static uint32_t swr_r(const char *what, uint64_t pa)	/* logged read, whitelist only */
{
	if (!swr_allowed(pa)) {
		swr_log("swr: REFUSED read %#llx (not whitelisted)\n", (unsigned long long)pa);
		swr_refused = 1;
		return 0;
	}
	return swr_rd(what, pa);
}

static uint32_t swr_q(uint64_t pa)	/* quiet read for polls (caller logs changes) */
{
	if (!swr_allowed(pa)) {
		swr_log("swr: REFUSED read %#llx (not whitelisted)\n", (unsigned long long)pa);
		swr_refused = 1;
		return 0;
	}
	return rd32(pa);
}

static void swr_w(const char *what, uint64_t pa, uint32_t v)
{
	if (!swr_allowed(pa)) {
		swr_log("swr: REFUSED wr %#llx = %#x (not whitelisted)\n", (unsigned long long)pa, v);
		swr_refused = 1;
		return;
	}
	uint32_t old = rd32(pa);
	swr_log("swr: stage wr %s %#llx %#x -> %#x\n", what, (unsigned long long)pa, old, v);
	wr32(pa, v);
	swr_log("swr: wr %#llx %#x -> %#x, reads %#x\n", (unsigned long long)pa, old, v, rd32(pa));
}

static void swr_poll(const char *what, uint64_t pa, int n, unsigned us, uint32_t mask)	/* log every change */
{
	uint32_t last = swr_q(pa);
	swr_log("swr: poll %s %#llx start %#010x\n", what, (unsigned long long)pa, last);
	for (int i = 1; i <= n; i++) {
		usleep(us);
		uint32_t v = swr_q(pa);
		if (v != last)
			swr_log("swr: poll %s at %d x %u us: %#010x -> %#010x\n", what, i, us, last, v);
		last = v;
		if (mask && (v & mask)) {
			swr_log("swr: poll %s: mask %#x set after %d\n", what, mask, i);
			return;
		}
	}
	swr_log("swr: poll %s end %#010x%s\n", what, last, mask ? " (mask never set)" : "");
}

/* hj5: --swr-mbhc (headphone jack S3, research/notes/headphone-jack.md "S3"): WCD937x register access over the TX
 * swrm command FIFO (dev 1), codec whitelist only; every access logged with old/new values. */
static const uint16_t cdc_ok[] = { 0x34cd, 0x34c0, 0x34c1, 0x3103, 0x312a, 0x3029, 0x3001, 0x31b1, 0x3014, 0x3122,
				   0x3120, 0x3015, 0x346e, 0x3471, 0x346b, 0x3019, 0x3125 };
static uint32_t cdc_wid, cdc_rid;	/* rolling cmd ids 0..14 */

static int cdc_allowed(uint32_t reg, int wr)
{
	if (wr && (reg == 0x3023 || reg == 0x3009 || reg == 0x3008))	/* micbias / HPH PA / RX supplies: never */
		return 0;
	for (unsigned i = 0; i < sizeof(cdc_ok) / sizeof(cdc_ok[0]); i++)
		if (reg == cdc_ok[i])
			return 1;
	return 0;
}

static int cdc_rd1(uint32_t reg, uint32_t *v)	/* one read attempt: 0 ok */
{
	uint32_t id = cdc_rid, st = 0;
	cdc_rid = (cdc_rid + 1) % 15;
	wr32(SWR_TXS + 0x304, reg | id << 16 | 1u << 20 | 1u << 24);
	usleep(250);
	for (int i = 0; i < 20 && !((st >> 16) & 0x1f); i++) {
		st = swr_q(SWR_TXS + 0x30c);
		if (!((st >> 16) & 0x1f))
			usleep(250);
	}
	if (!((st >> 16) & 0x1f))
		return -1;
	uint32_t r = swr_q(SWR_TXS + 0x318);
	if (((r >> 8) & 0xf) != id)
		return -2;
	*v = r & 0xff;
	return 0;
}

static int cdc_r(uint32_t reg, uint32_t *v, int quiet)	/* 0 ok; mismatch/no data: flush + retry once */
{
	if (!cdc_allowed(reg, 0)) {
		swr_log("swr: cdc REFUSED read %#x (not whitelisted)\n", reg);
		swr_refused = 1;
		return -1;
	}
	int e = cdc_rd1(reg, v);
	if (e) {
		swr_log("swr: cdc read %#x failed (%s), flush + retry\n", reg, e == -1 ? "no data" : "id mismatch");
		wr32(SWR_TXS + 0x308, 1);
		usleep(500);
		e = cdc_rd1(reg, v);
	}
	if (e)
		swr_log("swr: cdc read %#x FAILED\n", reg);
	else if (!quiet)
		swr_log("swr: cdc rd %#x = %#04x\n", reg, *v);
	return e;
}

static void cdc_w(uint32_t reg, uint32_t v)
{
	uint32_t old = 0, st = 0, now = 0;
	if (!cdc_allowed(reg, 1)) {
		swr_log("swr: cdc REFUSED wr %#x = %#x (not whitelisted / forbidden)\n", reg, v);
		swr_refused = 1;
		return;
	}
	int oe = cdc_r(reg, &old, 1);
	uint32_t id = cdc_wid;
	cdc_wid = (cdc_wid + 1) % 15;
	wr32(SWR_TXS + 0x300, reg | id << 16 | 1u << 20 | (v & 0xff) << 24);
	int i = 0;
	for (; i < 20; i++) {
		st = swr_q(SWR_TXS + 0x30c);
		if (!((st >> 8) & 0x1f))
			break;
		usleep(500);
	}
	usleep(150);
	int ne = cdc_r(reg, &now, 1);
	swr_log("swr: cdc wr %#x %s%#04x -> %#04x, reads %s%#04x%s\n", reg, oe ? "?" : "", old, v & 0xff, ne ? "?" : "",
		now, i == 20 ? " (WR FIFO never drained)" : "");
}

/* hj5: S3 init, poll, restore. Saved old values are restored in reverse write order. */
static void swr_mbhc_run(void)
{
	uint32_t sreg[24], sval[24], e29 = 0, e16 = 0, e17 = 0, v = 0;
	int n = 0;
#define CDC_SET(reg, val) do { uint32_t o_ = 0; if (!cdc_r(reg, &o_, 0) && n < 24) { int k_ = 0;		\
	for (; k_ < n && sreg[k_] != (reg); k_++) ;							\
	if (k_ == n) sreg[n] = (reg), sval[n++] = o_; } cdc_w(reg, val); } while (0)
#define CDC_RMW(reg, clr, set) do { uint32_t c_ = 0; if (!cdc_r(reg, &c_, 1)) CDC_SET(reg, (c_ & ~(uint32_t)(clr)) | (set)); } while (0)
	swr_log("swr: --- mbhc S3 (%d s%s) ---\n", swr_mbhc, swr_mbhc_unmask ? ", unmask" : "");
	cdc_wid = cdc_rid = 0;
	if (cdc_r(0x34cd, &e29, 0) || cdc_r(0x34c0, &e16, 0) || cdc_r(0x34c1, &e17, 0)) {
		swr_log("swr: mbhc: efuse reads failed, stop\n");
		return;
	}
	uint32_t bg = (e16 == 2 || e17 > 9) ? (e29 & 0x0f) : 0x0e;
	CDC_RMW(0x3103, 0x0e, bg & 0x0e);	/* Linux wcd937x_io_init: update_bits(SLEEP_CTL, 0x0E, e29 & 0x0F), no shift */
	CDC_RMW(0x3103, 0, 0x80);
	usleep(1000);
	CDC_RMW(0x3103, 0, 0x40);
	usleep(1000);
	CDC_RMW(0x312a, 0x10, 0);
	CDC_RMW(0x3029, 0xf0, (e16 == 1 || e16 == 2) ? 0xb0 : 0x80);
	CDC_RMW(0x3001, 0, 0x80);
	CDC_RMW(0x3001, 0, 0x40);
	usleep(10000);
	CDC_RMW(0x3001, 0x40, 0);
	CDC_RMW(0x31b1, 0x1f, 0x08);	/* Linux HS_PULLUP_I_DEFAULT (-1) -> forced to HS_PULLUP_I_2P0_UA = 0x08 */
	CDC_SET(0x3014, 0xff);
	CDC_RMW(0x3122, 0x0f, 0x06);
	CDC_RMW(0x3120, 0x03, 0x02);
	CDC_RMW(0x3015, 0, 0x01);
	CDC_RMW(0x3120, 0, 0x80);
	if (swr_mbhc_unmask)
		CDC_SET(0x346b, 0xef);
	if (!swr_refused) {
		static const uint16_t pr[4] = { 0x346e, 0x3019, 0x3125, 0x3014 };
		uint32_t last[4] = { 0x100, 0x100, 0x100, 0x100 };
		uint64_t t0 = now_ms();
		while (now_ms() - t0 < (uint64_t)swr_mbhc * 1000) {
			for (int k = 0; k < 4; k++)
				if (!cdc_r(pr[k], &v, 1) && v != last[k]) {
					swr_log("swr: mbhc t=%llu ms %#x %#04x -> %#04x\n", (unsigned long long)(now_ms() - t0),
						pr[k], last[k] & 0xff, v);
					last[k] = v;
				}
			if (last[0] != 0x100 && (last[0] & 0x10)) {
				uint32_t m = 0;
				cdc_r(0x3014, &m, 1);
				swr_log("swr: mbhc t=%llu ms PLUG %s (mech %#04x)\n", (unsigned long long)(now_ms() - t0),
					(m & 0x20) ? "INSERTED" : "REMOVED", m);
				cdc_w(0x3471, 0x10);
				cdc_w(0x3471, 0x00);
				cdc_w(0x3014, m ^ 0x20);
				last[0] = 0x100;
			}
			usleep(100000);
		}
	}
	/* restore in reverse; the notes' teardown values where they differ from the saved one */
	swr_log("swr: mbhc restore (%d regs)\n", n);
	for (int k = n - 1; k >= 0; k--) {
		int nv = -1;
		switch (sreg[k]) {
		case 0x3014: nv = 0x39; break;
		case 0x3015: nv = 0x08; break;
		case 0x346b: nv = 0xff; break;
		case 0x3001: nv = 0x00; break;
		}
		if (nv >= 0 && (uint32_t)nv != sval[k])
			swr_log("swr: mbhc %#x saved %#04x, notes teardown %#04x: using the notes value\n", sreg[k], sval[k], nv);
		cdc_w(sreg[k], nv >= 0 ? (uint32_t)nv : sval[k]);
	}
#undef CDC_RMW
#undef CDC_SET
}

static void swr_init(void)
{
	static const uint32_t pin_want[3] = { 0x0ca, 0x0ce, 0x0ca };	/* gpio18 clk func2, 19 data1 func3, 20 data2 func2 */
	uint32_t pin_saved[3] = { 0 }, g24_cfg = 0, g24_val = 0;
	int pin_set[3] = { 0 }, g24_set = 0, mst_set = 0;
	uint64_t g24 = LPI_PA + 0x1000u * 24;
	if ((swr_clk_ok & (3u << 1)) != (3u << 1)) {
		swr_log("swr: --swr-init %c REFUSED: tx clocks 0x30c/0x30d not both acked (mask %#x)\n", swr_stage,
			swr_clk_ok);
		return;
	}
	swr_log("swr: --- init stage %c ---\n", swr_stage);
	swr_log("swr: supplies: not read (no PMIC/SPMI read path in adspboot)\n");
	/* a: macro, pins */
	for (unsigned o = 0; o <= 0x90; o += (o < 8 ? 4 : o == 8 ? 0x80 : 8))
		swr_r("tx-macro", SWR_TXM + o);
	uint32_t pin_old[3];
	for (int i = 0; i < 3; i++)
		pin_old[i] = swr_r("lpi swr pin cfg", LPI_PA + 0x1000u * (18 + i));
	swr_w("tx-macro MCLK_EN", SWR_TXM + 0x00, swr_q(SWR_TXM + 0x00) | 1);
	swr_w("tx-macro FS_CNT_EN", SWR_TXM + 0x04, swr_q(SWR_TXM + 0x04) | 1);
	swr_w("tx-macro SWR_RESET", SWR_TXM + 0x08, swr_q(SWR_TXM + 0x08) | 2);
	swr_w("tx-macro SWR_CLK_EN", SWR_TXM + 0x08, swr_q(SWR_TXM + 0x08) | 1);
	swr_w("tx-macro SWR_RESET off", SWR_TXM + 0x08, swr_q(SWR_TXM + 0x08) & ~2u);
	for (int i = 0; i < 3; i++) {
		if (((pin_old[i] >> 2) & 0xf) == ((pin_want[i] >> 2) & 0xf)) {
			swr_log("swr: gpio%d cfg %#x already func %u: left\n", 18 + i, pin_old[i], (pin_old[i] >> 2) & 0xf);
			continue;
		}
		pin_saved[i] = pin_old[i];
		pin_set[i] = 1;
		swr_w("lpi swr pin cfg", LPI_PA + 0x1000u * (18 + i), pin_want[i]);
	}
	swr_r("tx-swr COMP_STATUS", SWR_TXS + 0x14);
	swr_r("tx-swr COMP_PARAMS", SWR_TXS + 0x100);
	if (swr_stage >= 'b' && !swr_refused) {	/* b: codec reset release */
		g24_cfg = swr_r("lpi gpio24 cfg", g24);
		g24_val = swr_r("lpi gpio24 value", g24 + 4);
		g24_set = 1;
		swr_w("lpi gpio24 value low", g24 + 4, 0);
		swr_w("lpi gpio24 cfg out", g24, 0x3c8 | (g24_cfg & 0x400));
		usleep(50);
		swr_w("lpi gpio24 value high", g24 + 4, 0x2);
		usleep(50);
	}
	if (swr_stage >= 'c' && !swr_refused) {	/* c: master init, Linux qcom_swrm_init order */
		mst_set = 1;
		swr_w("FRAME_CTRL_BANK0", SWR_TXS + 0x101c, 0x0f);
		swr_w("ENUMERATOR_CFG", SWR_TXS + 0x500, 1);
		swr_w("INT_MASK", SWR_TXS + 0x204, 0x1ffff);
		swr_w("MCP_CFG no-pings", SWR_TXS + 0x1048, (swr_q(SWR_TXS + 0x1048) & ~(0x1fu << 17)) | (0x1fu << 17));
		swr_w("MCP_BUS_CTRL CLK_START", SWR_TXS + 0x1044, 0x2);
		swr_w("CMD_FIFO_CFG", SWR_TXS + 0x314, 0x7);
		swr_w("COMP_CFG", SWR_TXS + 0x04, 1);
		swr_w("COMP_CFG", SWR_TXS + 0x04, 2);
		swr_w("INT_CLEAR", SWR_TXS + 0x208, 0xffffffffu);
		swr_w("COMP_CFG", SWR_TXS + 0x04, 3);
		swr_poll("COMP_STATUS", SWR_TXS + 0x14, 100, 500, 1);
		swr_r("tx-swr COMP_PARAMS", SWR_TXS + 0x100);
		swr_poll("MCP_SLV_STATUS", SWR_TXS + 0x1090, 100, 1000, 0);
		uint32_t is = swr_r("INT_STATUS", SWR_TXS + 0x200);
		swr_log("swr: int status %#x: attached %u, status change %u, cmd error %u, auto-enum failed %u\n", is,
			(is >> 1) & 1, (is >> 2) & 1, (is >> 7) & 1, (is >> 11) & 1);
		swr_w("INT_CLEAR", SWR_TXS + 0x208, is);
	}
	if (swr_stage >= 'd' && !swr_refused) {	/* d: DevID of device 1 */
		uint32_t b1 = swr_r("ENUM DEV_ID_1(1)", SWR_TXS + 0x530), b2 = swr_r("ENUM DEV_ID_2(1)", SWR_TXS + 0x534);
		uint64_t a = (uint64_t)((b2 >> 8) & 0xff) | (uint64_t)(b2 & 0xff) << 8 | (uint64_t)((b1 >> 24) & 0xff) << 16 |
			     (uint64_t)((b1 >> 16) & 0xff) << 24 | (uint64_t)((b1 >> 8) & 0xff) << 32 |
			     (uint64_t)(b1 & 0xff) << 40;
		swr_log("swr: dev1 enum addr %#014llx (want mfr 0x0217 part 0x010a)\n", (unsigned long long)a);
		for (uint32_t reg = 0x50, id = 1; reg <= 0x55; reg++, id++) {
			swr_w("RD_CMD", SWR_TXS + 0x304, reg | id << 16 | 1u << 20 | 1u << 24);
			usleep(250);
			uint32_t st = 0;
			for (int i = 0; i < 20 && !((st >> 16) & 0x1f); i++) {
				st = swr_r("CMD_FIFO_STATUS", SWR_TXS + 0x30c);
				if (!((st >> 16) & 0x1f))
					usleep(250);
			}
			if (!((st >> 16) & 0x1f)) {
				swr_log("swr: devid reg %#x: no read data\n", reg);
				continue;
			}
			uint32_t r = swr_r("RD_FIFO", SWR_TXS + 0x318);
			swr_log("swr: devid reg %#x = %#04x (cmd id %u, want %u%s)\n", reg, r & 0xff, (r >> 8) & 0xf, id,
				((r >> 8) & 0xf) == id ? "" : " MISMATCH");
		}
	}
	if (swr_mbhc && swr_stage >= 'd' && !swr_refused)	/* hj5: S3 plug poll with the bus up */
		swr_mbhc_run();
	/* teardown in reverse */
	swr_log("swr: --- teardown ---\n");
	if (mst_set) {
		swr_w("COMP_CFG off", SWR_TXS + 0x04, 0);
		swr_w("MCP_BUS_CTRL off", SWR_TXS + 0x1044, 0);
		swr_w("ENUMERATOR_CFG off", SWR_TXS + 0x500, 0);
	}
	if (g24_set) {
		swr_w("lpi gpio24 value low", g24 + 4, 0);
		swr_w("lpi gpio24 cfg restore", g24, g24_cfg);
		swr_log("swr: gpio24 value before init was %#x\n", g24_val);
	}
	for (int i = 2; i >= 0; i--)
		if (pin_set[i])
			swr_w("lpi swr pin cfg restore", LPI_PA + 0x1000u * (18 + i), pin_saved[i]);
	swr_w("tx-macro SWR_CONTROL off", SWR_TXM + 0x08, 0);
	swr_w("tx-macro FS_CNT off", SWR_TXM + 0x04, swr_q(SWR_TXM + 0x04) & ~1u);
	swr_w("tx-macro MCLK off", SWR_TXM + 0x00, swr_q(SWR_TXM + 0x00) & ~1u);
	swr_log("swr: --- init stage %c done%s ---\n", swr_stage, swr_refused ? " (a refused access stopped it)" : "");
}

static void pcm_start(void);

/* SET_PARAM_V2 body with the I2S config of PORT (48 kHz, 16 bit, stereo, ws internal) on lane LANE (1 SD0, 2 SD1) */
#define AFE_I2S_PL	(16 + sizeof(struct afe_param) + sizeof(struct afe_i2s))
static void afe_i2s_pl(uint8_t *b, uint16_t port, uint16_t lane)
{
	struct afe_param p = { AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_I2S_CONFIG, sizeof(struct afe_i2s), 0 };
	struct afe_i2s c = { 1, (uint16_t)i2s_bits, lane, 1 /* stereo */, 1 /* ws internal */, 48000, 0, 0 };
	uint16_t size = sizeof(p) + sizeof(c);
	memset(b, 0, AFE_I2S_PL);
	memcpy(b, &port, 2);
	memcpy(b + 2, &size, 2);
	memcpy(b + 16, &p, sizeof(p));
	memcpy(b + 16 + sizeof(p), &c, sizeof(c));
}

/* sp2: gpio8 SCK func3, gpio9 WS func3, gpio10 SD0 func4, 8 mA, no pull (live.dts:23261-23340). 0 = all read back.
 * sp7: --rec adds gpio11 SD1 func2 (0xc8: same 8 mA / no pull, function field 2; vi-loopback.md section 1). */
static int lpi_pins(int on)
{
	static const uint32_t want[4] = { 0xcc, 0xcc, 0xd0, 0xc8 };
	int bad = 0;
	for (int i = 0; i < (rec_on ? 4 : 3); i++) {
		uint64_t pa = LPI_PA + 0x1000u * (8 + i);
		uint32_t cur = rd32(pa), v = on ? want[i] : lpi_saved[i];
		if (on)
			lpi_saved[i] = cur;
		wr32(pa, v);
		uint32_t back = rd32(pa);
		printf("lpi: gpio%d cfg %#llx: %#x -> %#x, read back %#x%s\n", 8 + i, (unsigned long long)pa, cur, v, back,
		       back == v ? "" : " MISMATCH");
		bad |= back != v;
	}
	lpi_set = on;
	return bad;
}

static void lpi_restore(void)	/* atexit too: pins back even if the devote never runs */
{
	if (lpi_set)
		lpi_pins(0);
}

/* Run the step afe_step now holds; AFE_HOLD only arms the 2 s timer (--pcm: runs the PCM sequence instead). */
static void afe_run(void)
{
	uint16_t port = AFE_PORT_SEC_MI2S_RX;
	switch (afe_step) {
	case AFE_VOTE: {	/* sp2: {u32 hw_block_id, char name[8]} as apr_tone hw_vote */
		uint8_t b[12] = { 0, 0, 0, 0, 'a', 'd', 's', 'p', 'b', 't', 0, 0 };
		memcpy(b, &vote_block, 4);
		if (swr_on)
			swr_log("swr: stage hw vote request (block %u)\n", vote_block);	/* hj1 */
		afe_send(0, AFE_CMD_REMOTE_LPASS_CORE_HW_VOTE_REQUEST, b, sizeof(b));
		break;
	}
	case AFE_DEVOTE: {
		lpi_restore();	/* sp2: pins back while the core is still voted */
		if (hw_handle < 0) {
			afe_step = AFE_DONE;
			afe_run();
			break;
		}
		uint32_t b[2] = { vote_block, (uint32_t)hw_handle };
		if (swr_on)
			swr_log("swr: stage hw devote request (handle %d)\n", hw_handle);	/* hj1 */
		afe_send(0, AFE_CMD_REMOTE_LPASS_CORE_HW_DEVOTE_REQUEST, b, sizeof(b));
		break;
	}
	case AFE_CLK_F3: afe_clock(AFE_SVC_CMD_SET_PARAM, 1); break;
	case AFE_CLK_FC: afe_clock(AFE_SVC_CMD_SET_PARAM_V2, 1); break;
	case AFE_CFG: {
		uint8_t b[AFE_I2S_PL];
		afe_i2s_pl(b, port, 1 /* SD0 */);
		afe_send(port, AFE_PORT_CMD_SET_PARAM_V2, b, sizeof(b));
		break;
	}
	case AFE_STOP:
		lpi_restore();	/* sp2b: pins back while the port (and the core clock) still runs */
		/* fall through */
	case AFE_START: {
		uint16_t b[2] = { port, 0 };
		afe_send(port, afe_step == AFE_START ? AFE_PORT_CMD_DEVICE_START : AFE_PORT_CMD_DEVICE_STOP, b, sizeof(b));
		break;
	}
	case AFE_HOLD:
		afe_op = 0;
		if (pcm_on) {	/* ap6: the port stays started until the PCM sequence is over */
			printf("afe: port started, PCM next\n");
			apr_due = UINT64_MAX;
			pcm_start();
			break;
		}
		printf("afe: port started, holding 2 s\n");
		apr_due = now_ms() + 2000;
		break;
	case AFE_CLK_OFF: afe_clock(afe_clk_op, 0); break;
	case AFE_SWR_CLK:	/* hj0: COUPLE_NO, root 0 as Linux audio-ref-clk */
		swr_log("swr: stage clock %#x on request\n", swr_ids[swr_i]);	/* hj1 */
		afe_clock_id(AFE_SVC_CMD_SET_PARAM, swr_ids[swr_i], swr_hz[swr_i], 1, 1);	/* hj3: DT rate */
		break;
	case AFE_SWR_UNCLK:
		while (swr_i < SWR_NCLK && !(swr_clk_ok & (1u << swr_i)))
			swr_i++;
		if (swr_i >= SWR_NCLK) {
			afe_step = AFE_DEVOTE;
			afe_run();
			break;
		}
		swr_log("swr: stage clock %#x off request\n", swr_ids[swr_i]);	/* hj1 */
		afe_clock_id(AFE_SVC_CMD_SET_PARAM, swr_ids[swr_i], swr_hz[swr_i], 1, 0);
		break;
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
	if (s == AFE_VOTE) {	/* sp2 */
		printf("afe: LPASS core hw vote (block %u) %s, handle %d%s\n", vote_block, ok ? "ok" : "FAILED",
		       hw_handle, ok ? "" : "; going on without it");	/* sp2b: no abort */
		if (swr_on) {	/* hj0: no reads without the vote (unclocked bus) */
			swr_log("swr: hw vote reply %s, handle %d (status %x)\n", ok ? "ok" : "FAILED", hw_handle, status);	/* hj1 */
			if (ok && swr_clk_first)	/* hj2: va-macro read right after the vote resets (unclocked) */
				swr_log("swr: --swr-clk-first: clock votes before any read\n");
			else if (ok)
				swr_read("hw vote only");
			else
				swr_log("swr: no hw vote: reads skipped\n");
			swr_i = 0;
			afe_step = ok && swr_clk ? AFE_SWR_CLK : AFE_DEVOTE;
			afe_run();
			return;
		}
		afe_step = AFE_CLK_F3;
		afe_run();
		return;
	}
	if (s == AFE_SWR_CLK || s == AFE_SWR_UNCLK) {	/* hj0 */
		swr_log("swr: clock %#x %s: %s\n", swr_ids[swr_i], s == AFE_SWR_CLK ? "on" : "off", ok ? "ok" : "FAILED");
		if (!ok)	/* hj3: why: APR status and the clock_set used (v1, id, hz, attr COUPLE_NO, root 0) */
			swr_log("swr: clock %#x refused: APR status %#x (clock_set v1 id %#x hz %u attr 1 root 0 enable %d)\n",
				swr_ids[swr_i], status, swr_ids[swr_i], swr_hz[swr_i], s == AFE_SWR_CLK);
		if (s == AFE_SWR_CLK && ok)
			swr_clk_ok |= 1u << swr_i;
		if (++swr_i < SWR_NCLK) {
			afe_run();
			return;
		}
		swr_i = 0;
		if (s == AFE_SWR_CLK) {
			swr_log("swr: clock ok mask %#x\n", swr_clk_ok);	/* hj3 */
			swr_gate = swr_clk_first;	/* hj3: each group only with its own clocks */
			swr_read("hw vote + codec clocks");
			swr_gate = 0;
			if (swr_stage)	/* hj4 */
				swr_init();
			afe_step = AFE_SWR_UNCLK;
		} else
			afe_step = AFE_DEVOTE;
		afe_run();
		return;
	}
	if (s == AFE_DEVOTE) {
		if (swr_on)	/* hj1 */
			swr_log("afe: LPASS core hw devote (handle %d) %s\n", hw_handle, ok ? "ok" : "FAILED");
		else
			printf("afe: LPASS core hw devote (handle %d) %s\n", hw_handle, ok ? "ok" : "FAILED");
		hw_handle = -1;
	}
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
		if (ok && s == AFE_START && !no_lpi && lpi_pins(1)) {	/* sp2b: pins only once the port runs */
			printf("lpi: read-back mismatch: stopping the port\n");
			afe_step = AFE_STOP;
		}
	} else
		afe_step = s == AFE_CLK_OFF ? AFE_DEVOTE : s + 1;	/* hj0: skip the SWR steps */
	if (afe_step == AFE_CLK_OFF && !afe_clk_ok)
		afe_step = AFE_DONE;
	if (afe_step == AFE_DONE && s < AFE_DEVOTE && (hw_handle >= 0 || lpi_set))
		afe_step = AFE_DEVOTE;	/* sp2: every end after the vote passes the pin restore + devote */
	afe_run();
}

/*
 * ap6: `--pcm` (implies --afe): step 3 of apr-plan.md, a 1 kHz sine at -20 dBFS (48 kHz, 16-bit stereo) for 3 s
 * through ASM + ADM while the AFE holds SEC_MI2S_RX started (amps untouched: no sound). Order as the SM7150
 * reference (sound_sm7150.c stream_on/play/stream_off): ASM maps our buffer, ADM opens a COPP on the port, ASM opens
 * session 1 stream 1 for writing, the matrix routes session 1 to the COPP, media format, run, 10 ms writes refilled
 * on every WRITE_DONE; then 8 zero periods, pause, close, ADM close, unmap, and the AFE stop/clock off. The first
 * failed setup step goes straight to the teardown of what was opened. Payloads: Linux apr_audio-v2.h layouts as the
 * reference sends them, but 16-bit PCM and bit width (the reference: 24 in 32).
 */
#define APR_SVC_ASM		0x7
#define APR_SVC_ADM		0x8
#define ASM_CMD_SHARED_MEM_MAP_REGIONS	0x00010d92
#define ASM_CMDRSP_SHARED_MEM_MAP_REGIONS 0x00010d93
#define ASM_CMD_SHARED_MEM_UNMAP_REGIONS 0x00010d94
#define ASM_STREAM_CMD_OPEN_WRITE_V3	0x00010db3
#define ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2 0x00010d98
#define ASM_SESSION_CMD_RUN_V2		0x00010daa
#define ASM_SESSION_CMD_PAUSE		0x00010bd3	/* Linux apr_audio-v2.h; not in the reference (it closes running) */
#define ASM_DATA_CMD_WRITE_V2		0x00010dab
#define ASM_DATA_EVENT_WRITE_DONE_V2	0x00010d99
#define ASM_SESSION_EVENT_RX_UNDERFLOW	0x00010c17
#define ASM_STREAM_CMD_CLOSE		0x00010bcd
#define ASM_TOPOLOGY_NONE		0x00010c68
#define ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2 0x00010da5
#define ASM_SHMEM_4K_POOL		3
#define ADM_CMD_DEVICE_OPEN_V5		0x00010326
#define ADM_CMDRSP_DEVICE_OPEN_V5	0x00010329
#define ADM_CMD_DEVICE_CLOSE_V5		0x00010327
#define ADM_CMD_MATRIX_MAP_ROUTINGS_V5	0x00010325
#define ADM_TOPOLOGY_NONE		0x00010312	/* the reference's default COPP topology */
#define PCM_SESSION		1
#define PCM_STREAM_PORT		(PCM_SESSION << 8 | 1)
#define PCM_FRAMES		480		/* 10 ms at 48 kHz */
#define PCM_BYTES		(PCM_FRAMES * 4)	/* 16-bit stereo: 1920 B */
#define PCM_TONE_PERIODS	300		/* 3 s */
#define PCM_WTOKEN		0x80000000u	/* | period: what a write's WRITE_DONE carries back */

/*
 * sp7: `--rec` (needs --pcm --amp): records the left amp's VMON/IMON back over SEC_MI2S_TX (0x1003, SD1) while the
 * tone plays (research/notes/vi-loopback.md). Setup steps R_* run inside the PCM sequence (AFE 0x1003 config + start,
 * ADM open TX, ASM session 2 OPEN_READ_V3 + ENC_CFG_BLK, matrix 1 route, RUN); a failed R_* step only drops the
 * recording, the tone goes on. The tone gets 0.5 s of silence before (baseline) and after; REC_READS READ_V2 of
 * 4096 B stay queued in pages after the playback ones; RMS/peak per channel every 100 ms. Teardown in reverse:
 * pause, close session 2, ADM close, AFE stop 0x1003 (no matrix unmap: V5 has none), amp TX registers back.
 * `--rec-hex` dumps the first READ_DONE payload. Session 2 CLOSE is the ASM stream close as for session 1 (not
 * the glink CLOSE that ADSP_CLOSE gates).
 */
#define ASM_STREAM_CMD_OPEN_READ_V3	0x00010db4
#define ASM_STREAM_CMD_SET_ENCDEC_PARAM	0x00010c10
#define ASM_PARAM_ID_ENCDEC_ENC_CFG_BLK_V2 0x00010da3
#define ASM_DATA_CMD_READ_V2		0x00010dac
#define ASM_DATA_EVENT_READ_DONE_V2	0x00010d9a
#define AFE_PORT_SEC_MI2S_TX	0x1003
#define REC_SESSION		2
#define REC_STREAM_PORT		(REC_SESSION << 8 | 1)
#define REC_RTOKEN		0x40000000u	/* | buffer: what a read's READ_DONE carries back */
#define REC_LEAD_PERIODS	50		/* 0.5 s of silence before the tone (baseline) and after it */

enum { P_IDLE, P_MAP, R_AFE_CFG, R_AFE_START, R_ADM_OPEN, R_OPEN_READ, R_ENC, R_ROUTE, P_ADM_OPEN, P_OPEN_WRITE,
       P_ROUTE, P_FMT, P_RUN, R_RUN, P_PLAY, R_PAUSE, R_CLOSE, R_ADM_CLOSE, R_AFE_STOP, P_PAUSE, P_CLOSE, P_ADM_CLOSE,
       P_UNMAP, P_DONE };
#define P_TEARDOWN		R_PAUSE
static int rec_failed, rec_afe, rec_copp_open, rec_open, rec_running, rec_hexed;
static uint32_t rec_copp, rec_lead, rec_reads, rec_errs;
static uint64_t rec_t0;
static uint64_t rec_win_ss[2], rec_ph_ss[3][2], rec_ph_n[3];	/* 100 ms window, per phase: baseline/tone/after */
static uint32_t rec_win_n, rec_win_pk[2], rec_ph_pk[3][2];
static int pcm_step, pcm_mapped, pcm_copp_open, pcm_stream_open, pcm_running, pcm_failed;
static uint32_t pcm_op, pcm_rsp, pcm_handle, pcm_copp, pcm_tone_at;
static uint32_t pcm_sent, pcm_done, pcm_out, pcm_underflow, pcm_werr;
static uint64_t pcm_due, pcm_t0, pcm_t1;
static int16_t pcm_sine[48];
static uint32_t pcm_tone_periods = PCM_TONE_PERIODS;
static int pcm_dbfs = -20;	/* sp5: tone level, clamped to -40..-6 dBFS */

/*
 * sp1: `--amp` (implies --pcm) turns the left amp (0x40 only) on for the tone: configured and read back before the
 * ADSP boot, GLOBAL_EN only once the stream runs (first WRITE_DONE: BCLK present), IRQ1 errors polled every 50 ms,
 * at most AMP_MAX_MS on and off while >= 0.5 s of tone is still queued; powered down before the PCM/AFE teardown,
 * on signals, errors and exit. `--amp-check` = probe, configure, read back, restore; never GLOBAL_EN.
 */
#define AMP_MAX_MS		3000
#define AMP_TONE_PERIODS	380	/* 3.8 s with --amp: the tone outlasts the amp window */
#define AMP_TAIL_PERIODS	50	/* off with 0.5 s of tone still to play */
static int amp_on, amp_check, amp_db = -30, amp_ready, amp_used;
/* sp8: --amp-unmute / --amp-clk on by default (ios-redfin 0953 sequence), --amp-boost / --amp-invert-clk off */
static struct cs_amp amp = { .addr = 0x40, .out_mute = 1, .clk = 1 };
static int amp_boost, amp_mid_read;
static uint64_t amp_t0, amp_next;
/* sp6: snapshots at 0.5 / 2.5 s; --amp-pcm-stop-test then stops the PCM refills, reads again 300 ms later, powers
 * down (the stalled stream then ends through the 1 s WRITE_DONE timeout) */
static int amp_diag_n, amp_pcm_stop_test, pcm_stopped;
static uint64_t amp_stop_t;

/* sp7: --rec ASP TX routing (vi-loopback.md step 2): VMON/IMON on, TX1 = VMON slot 0, TX2 = IMON slot 1, 16 bit,
 * TX1/2 enabled. on = 1 saves + writes + reads back, 0 restores (PWR_CTRL2 is left to cs_power_down: it zeroes it). */
static const struct { const char *n; uint32_t reg, mask, val; } amp_txr[] = {
	{ "pwr_ctrl2", 0x2018, 0x3000, 0x3000 }, { "asp_tx1_src", 0x4c20, 0xffffffffu, 0x18 },
	{ "asp_tx2_src", 0x4c24, 0xffffffffu, 0x19 }, { "sp_tx_wl", 0x4830, 0xffffffffu, 16 },
	{ "sp_frame_tx_slot", 0x4810, 0xffffffffu, 0x0100 }, { "sp_enables", 0x4800, 0x3, 0x3 },
};
static uint32_t amp_tx_saved[6];
static int amp_tx_set;

static void amp_tx(int on)
{
	if (!rec_on || (on ? amp_tx_set : !amp_tx_set))
		return;
	for (unsigned i = 0; i < 6; i++) {
		unsigned k = on ? i : 5 - i;
		uint32_t cur = 0, v, back = 0;
		if (!on && amp_txr[k].reg == 0x2018)
			continue;
		if (cs_read(&amp, amp_txr[k].reg, &cur))
			continue;
		if (on)
			amp_tx_saved[k] = cur;
		v = on ? (cur & ~amp_txr[k].mask) | amp_txr[k].val : amp_tx_saved[k];
		cs_write(&amp, amp_txr[k].reg, v);
		cs_read(&amp, amp_txr[k].reg, &back);
		printf("amp: rec %s %s %#x: %#x -> %#x, read back %#x%s\n", on ? "set" : "restore", amp_txr[k].n,
		       amp_txr[k].reg, cur, v, back, back == v ? "" : " MISMATCH");
	}
	amp_tx_set = on;
}

static void amp_off(void)
{
	if (!amp.configured)
		return;
	printf("amp: power down%s\n", amp.up ? ", was on" : "");
	cs_power_down(&amp);
	amp_tx(0);	/* sp7: after GLOBAL_EN 0 */
	if (amp_t0)
		printf("amp: on for %llu ms\n", (unsigned long long)(now_ms() - amp_t0));
	amp_ready = 0;
}

/* probe + configure + read back (no GLOBAL_EN); 0 = ready for amp_start */
static int amp_prepare(void)
{
	if (gi2c_init() || cs_probe(&amp))
		return -1;
	if (cs_config(&amp, amp_db, amp_boost) || cs_verify(&amp)) {
		amp_off();
		return -1;
	}
	amp_ready = 1;
	return 0;
}

/* first WRITE_DONE: once per run */
static void amp_start(void)
{
	if (!amp_on || !amp_ready || amp_used)
		return;
	amp_used = 1;
	printf("amp: power up (%d dB)\n", amp_db);
	uint32_t cd;
	cs_read(&amp, 0x14000, &cd);	/* sp2: CLOCK_DETECT_1 before GLOBAL_EN (logged by cs_read) */
	amp_tx(1);	/* sp7 */
	int r = cs_power_up(&amp);
	cs_read(&amp, 0x14000, &cd);	/* and after */
	if (r) {
		amp_off();
		return;
	}
	amp_t0 = now_ms();
	amp_next = amp_t0 + 50;
}

/* while on: IRQ1 errors every 50 ms, the 3 s cap, the tone tail */
static void amp_poll(void)
{
	if (!amp.up || now_ms() < amp_next)
		return;
	amp_next = now_ms() + 50;
	uint64_t on = now_ms() - amp_t0;
	if (!amp_mid_read && on >= AMP_MAX_MS / 2) {	/* sp8: AMP_OUT_MUTE once at mid-tone */
		amp_mid_read = 1;
		cs_out_mute(&amp, "mid-tone");
	}
	if (amp_diag_n == 0 && on >= 500) {
		amp_diag_n++;
		cs_diag(&amp, "0.5s");
	} else if (amp_diag_n == 1 && on >= 2500) {
		amp_diag_n++;
		cs_diag(&amp, "2.5s");
		if (amp_pcm_stop_test) {
			printf("amp: pcm stop test: no new periods from here (%u sent)\n", pcm_sent);
			pcm_stopped = 1;
			amp_stop_t = now_ms();
		}
	} else if (pcm_stopped && amp_diag_n == 2 && now_ms() - amp_stop_t >= 300) {
		amp_diag_n++;
		cs_diag(&amp, "stopped");
		printf("amp: pcm stop test done: off\n");
		amp_off();
		return;
	}
	if (!amp.up)
		return;
	if (cs_check(&amp)) {
		printf("amp: IRQ1 error: power down now\n");
		amp_off();
	} else if (now_ms() - amp_t0 >= AMP_MAX_MS) {
		printf("amp: %d ms cap\n", AMP_MAX_MS);
		amp_off();
	} else if (pcm_sent + AMP_TAIL_PERIODS >= rec_lead + pcm_tone_periods) {
		printf("amp: tone ends in <= 0.5 s: off\n");
		amp_off();
	}
}

static void pcm_tx(uint8_t svc, uint16_t port, uint32_t token, uint32_t opcode, const void *pl, uint16_t len)
{
	uint8_t pkt[sizeof(struct apr_hdr) + 64];
	struct apr_hdr h = {
		.hdr_field = APR_HDR_FIELD, .pkt_size = (uint16_t)(sizeof(h) + len),
		.src_svc = svc, .src_domain = APR_DOMAIN_APPS, .src_port = svc == APR_SVC_ASM ? port : 0,
		.dest_svc = svc, .dest_domain = APR_DOMAIN_ADSP, .dest_port = port,
		.token = token, .opcode = opcode,
	};
	memcpy(pkt, &h, sizeof(h));
	if (len)
		memcpy(pkt + sizeof(h), pl, len);
	chan_send(apr_chan, pkt, h.pkt_size);
}

/* A command of step pcm_step; RSP = its own response opcode (0 = the basic response). 3 s for the reply. */
static void pcm_send(uint8_t svc, uint16_t port, uint32_t opcode, const void *pl, uint16_t len, uint32_t rsp)
{
	printf("pcm: step %d tx svc %x port %x op %x (%u B)\n", pcm_step, svc, port, opcode, (unsigned)(sizeof(struct apr_hdr) + len));
	pcm_tx(svc, port, 0x200u + (uint32_t)pcm_step, opcode, pl, len);
	pcm_op = opcode;
	pcm_rsp = rsp;
	pcm_due = now_ms() + 3000;
}

static void pcm_fill(unsigned p)
{
	int16_t s[PCM_FRAMES * 2];
	int tone = pcm_sent >= rec_lead && pcm_sent < rec_lead + pcm_tone_periods;	/* sp7: rec_lead 0 without --rec */
	for (unsigned f = 0; f < PCM_FRAMES; f++)
		s[2 * f] = s[2 * f + 1] = tone ? pcm_sine[pcm_tone_at++ % 48] : 0;
	pm_wr(pcm_pa + (uint64_t)PCM_STRIDE * p, s, sizeof(s));	/* pm_wr cleans to PoC: the DSP reads DRAM */
}

static void pcm_write(unsigned p)
{
	uint64_t a = pcm_dsp + (uint64_t)PCM_STRIDE * p;
	/* asm_data_cmd_write_v2: buf addr lsw/msw, mem_map_handle, buf_size, seq_id, timestamp lsw/msw, flags */
	uint32_t w[8] = { (uint32_t)a, (uint32_t)(a >> 32), pcm_handle, PCM_BYTES, p, 0, 0, 0 };
	pcm_fill(p);
	pcm_tx(APR_SVC_ASM, PCM_STREAM_PORT, PCM_WTOKEN | p, ASM_DATA_CMD_WRITE_V2, w, sizeof(w));
	pcm_sent++;
	pcm_out++;
}

static void afe_next(int ok, uint32_t status);

/* sp7: queue READ_V2 into rec buffer b (pages after the playback ones) */
static unsigned long rec_untouched, rec_zero, rec_other;	/* sp13: A5-pattern check */

static void rec_read(unsigned b)
{
	uint64_t a = pcm_dsp + PCM_BUF + (uint64_t)REC_BYTES * b;
	/* sp13: pre-fill with 0xA5 so READ_DONE shows whether the DSP wrote into OUR pages at all (still A5 = it writes
	 * somewhere else: the shared-memory mapping is wrong; zeros = it really records silence) */
	static uint8_t pat[REC_BYTES];
	if (!pat[0])
		memset(pat, 0xa5, sizeof(pat));
	pm_wr(pcm_pa + PCM_BUF + (uint64_t)REC_BYTES * b, pat, sizeof(pat));
	/* asm_data_cmd_read_v2: buf addr lsw/msw, mem_map_handle, buf_size, seq_id */
	uint32_t r[5] = { (uint32_t)a, (uint32_t)(a >> 32), pcm_handle, REC_BYTES, b };
	pcm_tx(APR_SVC_ASM, REC_STREAM_PORT, REC_RTOKEN | b, ASM_DATA_CMD_READ_V2, r, sizeof(r));
	rec_reads++;
}

/* sp7: phase of what is being recorded now: 0 baseline (before the tone), 1 tone with the amp on, 2 the rest */
static int rec_phase(void)
{
	if (pcm_done < rec_lead)
		return 0;
	return amp.up && pcm_done < rec_lead + pcm_tone_periods ? 1 : 2;
}

/* sp7: a READ_DONE_V2 (status, addr lsw/msw, handle, enc_framesize, offset, ts lsw/msw, flags, frames, seq): stats,
 * then requeue while the tone plays */
static void rec_back(unsigned b, const uint32_t *pl, uint32_t len)
{
	if (len < 6 || pl[0]) {
		if (!rec_errs++)
			printf("rec: READ_DONE buffer %u status %x (len %u)\n", b, len ? pl[0] : 0xffffffff, len);
	} else {
		uint32_t n = pl[4], off = pl[5];
		if (off > REC_BYTES || n > REC_BYTES - off)
			n = off > REC_BYTES ? 0 : REC_BYTES - off;
		int16_t d[REC_BYTES / 2];
		if (n && !pm_rd(pcm_pa + PCM_BUF + (uint64_t)REC_BYTES * b + off, d, (n + 3) & ~3u)) {
			if (rec_hex && !rec_hexed++) {
				hex32("rec: READ_DONE payload", (const uint8_t *)pl, len * 4);
				hex32("rec: first data", (const uint8_t *)d, n < 64 ? n : 64);
			}
			for (uint32_t i = 0; i < n / 2; i++) {	/* sp13 */
				if ((uint16_t)d[i] == 0xa5a5)
					rec_untouched++;
				else if (!d[i])
					rec_zero++;
				else
					rec_other++;
			}
			int ph = rec_phase();
			for (uint32_t i = 0; i + 1 < n / 2; i += 2)
				for (int c = 0; c < 2; c++) {
					int v = d[i + c], a = v < 0 ? -v : v;
					rec_win_ss[c] += (uint64_t)(v * v);
					rec_ph_ss[ph][c] += (uint64_t)(v * v);
					if ((uint32_t)a > rec_win_pk[c])
						rec_win_pk[c] = a;
					if ((uint32_t)a > rec_ph_pk[ph][c])
						rec_ph_pk[ph][c] = a;
				}
			rec_win_n += n / 4;
			rec_ph_n[ph] += n / 4;
			if (rec_win_n >= 4800) {	/* 100 ms */
				printf("rec t=%.1f ch0 rms %d peak %u ch1 rms %d peak %u\n", (now_ms() - rec_t0) / 1000.0,
				       (int)sqrt((double)rec_win_ss[0] / rec_win_n), rec_win_pk[0],
				       (int)sqrt((double)rec_win_ss[1] / rec_win_n), rec_win_pk[1]);
				rec_win_n = rec_win_pk[0] = rec_win_pk[1] = 0;
				rec_win_ss[0] = rec_win_ss[1] = 0;
			}
		}
	}
	if (pcm_step == P_PLAY && rec_running)
		rec_read(b);
}

static void rec_summary(void)
{
	static const char *nm[3] = { "baseline", "tone", "after" };
	printf("rec: %u reads queued, %u errors\n", rec_reads, rec_errs);
	printf("rec: sp13 pattern check: %lu samples still A5A5 (DSP did not write our pages), %lu zero, %lu other\n",
	    rec_untouched, rec_zero, rec_other);
	for (int p = 0; p < 3; p++)
		printf("rec: %s %llu frames: ch0 (VMON) rms %d peak %u, ch1 (IMON) rms %d peak %u\n", nm[p],
		       (unsigned long long)rec_ph_n[p],
		       rec_ph_n[p] ? (int)sqrt((double)rec_ph_ss[p][0] / rec_ph_n[p]) : 0, rec_ph_pk[p][0],
		       rec_ph_n[p] ? (int)sqrt((double)rec_ph_ss[p][1] / rec_ph_n[p]) : 0, rec_ph_pk[p][1]);
}

/* Run step pcm_step; teardown steps with nothing to undo are skipped. P_DONE hands back to the AFE (stop). */
static void pcm_run(void)
{
	if (pcm_step > P_PLAY)
		amp_off();	/* sp1: amp down before any PCM/AFE teardown */
	int rec_go = rec_on && !rec_failed;	/* sp7: R_* setup steps */
	for (;; pcm_step++) {
		switch (pcm_step) {
		case R_AFE_CFG: {
			if (!rec_go)
				break;
			uint8_t b[AFE_I2S_PL];
			afe_i2s_pl(b, AFE_PORT_SEC_MI2S_TX, 2 /* SD1 */);
			pcm_send(APR_SVC_AFE, AFE_PORT_SEC_MI2S_TX, AFE_PORT_CMD_SET_PARAM_V2, b, sizeof(b), 0);
			return;
		}
		case R_AFE_START:
		case R_AFE_STOP: {
			if (pcm_step == R_AFE_START ? !rec_go : !rec_afe)
				break;
			uint16_t b[2] = { AFE_PORT_SEC_MI2S_TX, 0 };
			pcm_send(APR_SVC_AFE, AFE_PORT_SEC_MI2S_TX, pcm_step == R_AFE_START ? AFE_PORT_CMD_DEVICE_START :
				 AFE_PORT_CMD_DEVICE_STOP, b, sizeof(b), 0);
			return;
		}
		case R_ADM_OPEN: {
			if (!rec_go)
				break;
			/* as P_ADM_OPEN, mode 2 (live rec), endpoint the TX port */
			struct { uint16_t flags, mode, ep1, ep2; uint32_t topo; uint16_t ch, bits; uint32_t rate; uint8_t map[8]; }
			__attribute__((packed)) o = { 0, 2, AFE_PORT_SEC_MI2S_TX, 0xffff, ADM_TOPOLOGY_NONE, 2, 16, 48000, { 1, 2 } };
			pcm_send(APR_SVC_ADM, AFE_PORT_SEC_MI2S_TX, ADM_CMD_DEVICE_OPEN_V5, &o, sizeof(o), ADM_CMDRSP_DEVICE_OPEN_V5);
			return;
		}
		case R_OPEN_READ: {
			if (!rec_go)
				break;
			/* asm_stream_cmd_open_read_v3: u32 mode flags, src endpoint (0 matrix), preproc topology, enc format,
			 * u16 bits, reserved */
			struct { uint32_t flags, src, topo, fmt; uint16_t bits, res; } __attribute__((packed)) r = {
				0, 0, ASM_TOPOLOGY_NONE, ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2, 16, 0 };
			pcm_send(APR_SVC_ASM, REC_STREAM_PORT, ASM_STREAM_CMD_OPEN_READ_V3, &r, sizeof(r), 0);
			return;
		}
		case R_ENC: {
			if (!rec_go)
				break;
			/* param id, size 28; frames per buf 0, blk size 20; u16 ch, bits, u32 rate, u16 signed, reserved, map[8] */
			struct { uint32_t id, size, frames, blk; uint16_t ch, bits; uint32_t rate; uint16_t sign, res; uint8_t map[8]; }
			__attribute__((packed)) e = { ASM_PARAM_ID_ENCDEC_ENC_CFG_BLK_V2, 28, 0, 20, 2, 16, 48000, 1, 0, { 1, 2 } };
			pcm_send(APR_SVC_ASM, REC_STREAM_PORT, ASM_STREAM_CMD_SET_ENCDEC_PARAM, &e, sizeof(e), 0);
			return;
		}
		case R_ROUTE: {
			if (!rec_go)
				break;
			struct { uint32_t matrix, sessions; uint16_t session, copps, copp[2]; } __attribute__((packed)) r = {
				1 /* audio tx */, 1, REC_SESSION, 1, { (uint16_t)rec_copp, 0 } };
			pcm_send(APR_SVC_ADM, 0, ADM_CMD_MATRIX_MAP_ROUTINGS_V5, &r, sizeof(r), 0);
			return;
		}
		case R_RUN: {
			if (!rec_go)
				break;
			uint32_t r[3] = { 0, 0, 0 };
			pcm_send(APR_SVC_ASM, REC_STREAM_PORT, ASM_SESSION_CMD_RUN_V2, r, sizeof(r), 0);
			return;
		}
		case R_PAUSE:
			if (!rec_running)
				break;
			pcm_send(APR_SVC_ASM, REC_STREAM_PORT, ASM_SESSION_CMD_PAUSE, NULL, 0, 0);
			return;
		case R_CLOSE:
			if (!rec_open)
				break;
			pcm_send(APR_SVC_ASM, REC_STREAM_PORT, ASM_STREAM_CMD_CLOSE, NULL, 0, 0);
			return;
		case R_ADM_CLOSE:
			if (!rec_copp_open)
				break;
			pcm_send(APR_SVC_ADM, (uint16_t)rec_copp, ADM_CMD_DEVICE_CLOSE_V5, NULL, 0, 0);
			return;
		case P_MAP: {
			/* asm_cmd_shared_mem_map_regions: u16 pool, u16 regions, u32 property; region: addr lsw/msw, size */
			struct { uint16_t pool, regions; uint32_t prop, lo, hi, size; } __attribute__((packed)) m = {
				ASM_SHMEM_4K_POOL, 1, 0, (uint32_t)pcm_dsp, (uint32_t)(pcm_dsp >> 32), pcm_buf_len };
			pcm_send(APR_SVC_ASM, 0, ASM_CMD_SHARED_MEM_MAP_REGIONS, &m, sizeof(m), ASM_CMDRSP_SHARED_MEM_MAP_REGIONS);
			return;
		}
		case P_ADM_OPEN: {
			/* adm_cmd_device_open_v5: u16 flags, mode (1 playback), endpoint 1, endpoint 2 (0xffff), u32 topology,
			 * u16 channels, bit width, u32 rate, u8 channel map[8]; sent to the AFE port as the reference does */
			struct { uint16_t flags, mode, ep1, ep2; uint32_t topo; uint16_t ch, bits; uint32_t rate; uint8_t map[8]; }
			__attribute__((packed)) o = { 0, 1, AFE_PORT_SEC_MI2S_RX, 0xffff, ADM_TOPOLOGY_NONE, 2, (uint16_t)adm_bits, 48000,
						      { 1, 2 } /* FL FR */ };
			pcm_send(APR_SVC_ADM, AFE_PORT_SEC_MI2S_RX, ADM_CMD_DEVICE_OPEN_V5, &o, sizeof(o), ADM_CMDRSP_DEVICE_OPEN_V5);
			return;
		}
		case P_OPEN_WRITE: {
			/* asm_stream_cmd_open_write_v3: u32 mode flags, u16 sink endpoint (0 matrix), bits, u32 pp topology, format */
			struct { uint32_t flags; uint16_t sink, bits; uint32_t topo, fmt; } __attribute__((packed)) w = {
				0, 0, 16, ASM_TOPOLOGY_NONE, ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2 };
			pcm_send(APR_SVC_ASM, PCM_STREAM_PORT, ASM_STREAM_CMD_OPEN_WRITE_V3, &w, sizeof(w), 0);
			return;
		}
		case P_ROUTE: {
			/* adm_cmd_matrix_map_routings_v5: u32 matrix (0 audio rx), sessions; per session: u16 id, copps, copp[] */
			struct { uint32_t matrix, sessions; uint16_t session, copps, copp[2]; } __attribute__((packed)) r = {
				0, 1, PCM_SESSION, 1, { (uint16_t)pcm_copp, 0 } };
			pcm_send(APR_SVC_ADM, 0, ADM_CMD_MATRIX_MAP_ROUTINGS_V5, &r, sizeof(r), 0);
			return;
		}
		case P_FMT: {
			/* media_fmt_update_v2: u32 block size (20); multi_channel_pcm_fmt_blk_v2: u16 channels, bits, u32 rate,
			 * u16 signed, reserved, u8 channel map[8] */
			struct { uint32_t size; uint16_t ch, bits; uint32_t rate; uint16_t sign, res; uint8_t map[8]; }
			__attribute__((packed)) f = { 20, 2, 16, 48000, 1, 0, { 1, 2 } };
			pcm_send(APR_SVC_ASM, PCM_STREAM_PORT, ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2, &f, sizeof(f), 0);
			return;
		}
		case P_RUN: {
			uint32_t r[3] = { 0, 0, 0 };	/* asm_session_cmd_run_v2: flags 0 (run immediately), time lsw/msw */
			pcm_send(APR_SVC_ASM, PCM_STREAM_PORT, ASM_SESSION_CMD_RUN_V2, r, sizeof(r), 0);
			return;
		}
		case P_PLAY:
			printf("pcm: playing 1 kHz %d dBFS for %u x 10 ms, %u periods of %u B in flight\n", pcm_dbfs, pcm_tone_periods,
			       PCM_PERIODS, PCM_BYTES);
			pcm_op = 0;
			pcm_t0 = pcm_t1 = now_ms();
			for (unsigned p = 0; p < PCM_PERIODS; p++)
				pcm_write(p);
			if (rec_running) {	/* sp7 */
				printf("rec: %u reads of %u B queued, tone after %u x 10 ms\n", REC_READS, REC_BYTES, rec_lead);
				rec_t0 = now_ms();
				for (unsigned b = 0; b < REC_READS; b++)
					rec_read(b);
			}
			pcm_due = now_ms() + 1000;
			return;
		case P_PAUSE:
			if (!pcm_running)
				break;
			pcm_send(APR_SVC_ASM, PCM_STREAM_PORT, ASM_SESSION_CMD_PAUSE, NULL, 0, 0);
			return;
		case P_CLOSE:
			if (!pcm_stream_open)
				break;
			pcm_send(APR_SVC_ASM, PCM_STREAM_PORT, ASM_STREAM_CMD_CLOSE, NULL, 0, 0);
			return;
		case P_ADM_CLOSE:
			if (!pcm_copp_open)
				break;
			pcm_send(APR_SVC_ADM, (uint16_t)pcm_copp, ADM_CMD_DEVICE_CLOSE_V5, NULL, 0, 0);
			return;
		case P_UNMAP:
			if (!pcm_mapped)
				break;
			pcm_send(APR_SVC_ASM, 0, ASM_CMD_SHARED_MEM_UNMAP_REGIONS, &pcm_handle, sizeof(pcm_handle), 0);
			return;
		default:
			pcm_step = P_DONE;
			pcm_op = 0;
			printf("pcm: done: %s; %u periods written, %u done (%u errors) in %llu ms, %u underflows, handle %x copp %u\n",
			       pcm_failed ? "FAILED" : "ok", pcm_sent, pcm_done, pcm_werr,
			       (unsigned long long)(pcm_t1 - pcm_t0), pcm_underflow, pcm_handle, pcm_copp);
			if (rec_on)
				rec_summary();	/* sp7 */
			smmu_faults("ADSP pcm", pcm_sid);	/* after */
			afe_next(1, 0);	/* AFE_HOLD -> stop, clock off */
			return;
		}
	}
}

/* Step pcm_step answered (ok = status 0). A failed setup step goes to the teardown; teardown steps always go on. */
static void pcm_next(int ok, uint32_t status)
{
	int s = pcm_step;
	printf("pcm: step %d op %x: %s (status %x)\n", s, pcm_op, ok ? "ok" : "FAILED", status);
	if (s == P_MAP)
		pcm_mapped = ok;
	else if (s == P_ADM_OPEN)
		pcm_copp_open = ok;
	else if (s == P_OPEN_WRITE)
		pcm_stream_open = ok;
	else if (s == P_RUN)
		pcm_running = ok;
	else if (s == P_PAUSE)
		pcm_running = 0;
	else if (s == P_CLOSE)
		pcm_stream_open = 0;
	else if (s == P_ADM_CLOSE)
		pcm_copp_open = 0;
	else if (s == P_UNMAP)
		pcm_mapped = 0;
	else if (s == R_AFE_START || s == R_AFE_STOP)	/* sp7 */
		rec_afe = s == R_AFE_START && ok;
	else if (s == R_ADM_OPEN || s == R_ADM_CLOSE)
		rec_copp_open = s == R_ADM_OPEN && ok;
	else if (s == R_OPEN_READ || s == R_CLOSE)
		rec_open = s == R_OPEN_READ && ok;
	else if (s == R_RUN || s == R_PAUSE)
		rec_running = s == R_RUN && ok;
	if (!ok && ((s >= R_AFE_CFG && s <= R_ROUTE) || s == R_RUN)) {
		printf("rec: op %x failed (status %x): tone without recording\n", pcm_op, status);	/* sp7: no abort */
		rec_failed = 1;
		pcm_step = s + 1;
	} else if (!ok && s < P_PLAY) {
		pcm_failed = 1;
		pcm_step = P_TEARDOWN;
	} else
		pcm_step = s + 1;
	pcm_run();
}

/* A period came back: refill and resend it (tone, then 8 zero periods); once all are back, the teardown. */
static void pcm_back(unsigned p)
{
	pcm_done++;
	if (pcm_out)
		pcm_out--;
	pcm_t1 = now_ms();
	pcm_due = pcm_t1 + 1000;
	if (pcm_step != P_PLAY)
		return;
	if (pcm_done == 1)
		amp_start();	/* sp1: stream running, BCLK present */
	if (pcm_stopped)
		return;	/* sp6: --amp-pcm-stop-test */
	if (pcm_sent < rec_lead * 2 + pcm_tone_periods + PCM_PERIODS)	/* sp7: + silence before and after */
		pcm_write(p % PCM_PERIODS);
	else if (!pcm_out) {
		pcm_step = P_TEARDOWN;
		pcm_run();
	}
}

/* The APR packets of --pcm; 1 = consumed. */
static int pcm_rx(const struct apr_hdr *h, const uint32_t *pl, uint32_t len)
{
	if (h->opcode == ASM_DATA_EVENT_WRITE_DONE_V2 && (h->token & PCM_WTOKEN)) {
		if (len >= 4 && pl[3]) {	/* buf addr lsw/msw, mem_map_handle, status */
			if (!pcm_werr)
				printf("pcm: WRITE_DONE period %u status %x\n", h->token & 0xff, pl[3]);
			pcm_werr++;
		}
		pcm_back(h->token & 0xff);
		return 1;
	}
	if (h->opcode == APR_BASIC_RSP && (h->token & PCM_WTOKEN)) {	/* a write refused outright */
		printf("pcm: write period %u refused (status %x)\n", h->token & 0xff, len > 1 ? pl[1] : 0);
		pcm_werr++;
		pcm_back(h->token & 0xff);
		return 1;
	}
	if (h->opcode == ASM_DATA_EVENT_READ_DONE_V2 && (h->token & REC_RTOKEN)) {	/* sp7 */
		rec_back(h->token % REC_READS, pl, len);
		return 1;
	}
	if (h->opcode == APR_BASIC_RSP && (h->token & REC_RTOKEN)) {
		printf("rec: read %u refused (status %x)\n", h->token % REC_READS, len > 1 ? pl[1] : 0);
		rec_errs++;
		return 1;
	}
	if (h->opcode == ASM_SESSION_EVENT_RX_UNDERFLOW) {
		if (!pcm_underflow++)
			printf("pcm: underflow (first, after %u periods done)\n", pcm_done);
		return 1;
	}
	if (!pcm_op)
		return 0;
	if (pcm_rsp && h->opcode == pcm_rsp && h->opcode == ASM_CMDRSP_SHARED_MEM_MAP_REGIONS) {
		pcm_handle = len ? pl[0] : 0;
		printf("pcm: mem_map_handle %x%s\n", pcm_handle, pcm_handle ? "" : " (zero: unexpected)");
		pcm_next(len > 0, len ? 0 : 0xffffffff);
		return 1;
	}
	if (pcm_rsp && h->opcode == pcm_rsp) {	/* ADM_CMDRSP_DEVICE_OPEN_V5: status, copp_id (low 16 bits) */
		uint32_t st = len ? pl[0] : 0xffffffff;
		uint32_t *cp = pcm_step == R_ADM_OPEN ? &rec_copp : &pcm_copp;	/* sp7: the TX copp */
		*cp = len > 1 ? pl[1] & 0xffff : 0;
		printf("pcm: ADM open status %x copp %u\n", st, *cp);
		pcm_next(st == 0, st);
		return 1;
	}
	if (h->opcode == APR_BASIC_RSP && len > 1 && pl[0] == pcm_op) {
		pcm_next(pl[1] == 0, pl[1]);
		return 1;
	}
	return 0;
}

/* AFE_HOLD with --pcm: the sine table, then step 1 */
static void pcm_start(void)
{
	for (int i = 0; i < 48; i++)
		pcm_sine[i] = (int16_t)lrint(32767.0 * pow(10, pcm_dbfs / 20.0) * sin(2 * M_PI * i / 48));	/* pcm_dbfs peak */
	printf("pcm: i2s %d-bit slots, BCLK %u Hz, adm bits %d\n", i2s_bits, 48000u * (uint32_t)i2s_bits * 2, adm_bits);
	pcm_step = P_MAP;
	pcm_run();
}

/* replies overdue: 3 s per command, 1 s between WRITE_DONEs */
static void pcm_poll(void)
{
	if (pcm_step <= P_IDLE || pcm_step >= P_DONE || now_ms() < pcm_due)
		return;
	if (pcm_step == P_PLAY) {
		printf("pcm: no WRITE_DONE in 1 s (%u written, %u done)\n", pcm_sent, pcm_done);
		pcm_failed = 1;
		pcm_step = P_TEARDOWN;
		pcm_run();
	} else {
		printf("pcm: op %x: no reply in 3 s\n", pcm_op);
		pcm_next(0, 0xffffffff);
	}
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
	/* af3: the ADSP's replies carry an 8-word header (hdr_field 0x0081: header length in bits 7:4, in words),
	 * not our 5-word one; the payload starts after hdr_len words */
	size_t hl = ((h.hdr_field >> 4) & 0xf) * 4;
	if (hl < sizeof(h) || hl > n)
		hl = sizeof(h);
	len = (uint32_t)(n - hl) / 4;
	if (len > 64)
		len = 64;
	memcpy(pl, p + hl, len * 4);
	uint32_t status = len ? pl[h.opcode == APR_BASIC_RSP && len > 1 ? 1 : 0] : 0;
	if ((h.opcode == ASM_DATA_EVENT_WRITE_DONE_V2 || h.opcode == ASM_DATA_EVENT_READ_DONE_V2) && !getenv("ADSP_APR_HEX")) {	/* ap6: 308 per run, counted */
		pcm_rx(&h, pl, len);
		return;
	}
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
		hex32("apr: payload", p + hl, n - hl);
	}
	if (apr_step == 1 && !getenv("ADSP_APR_NO_GATE") && (h.opcode == AVCS_GET_STATE_RSP ||
	    (h.opcode == APR_BASIC_RSP && len && pl[0] == AVCS_GET_STATE)))
		apr_gate(h.opcode == AVCS_GET_STATE_RSP, h.opcode == AVCS_GET_STATE_RSP && len ? pl[0] : 0);
	else if (apr_step >= 1 && apr_step <= 3 && (h.opcode == apr_q[apr_step - 1] + 1 ||
	    (h.opcode == APR_BASIC_RSP && len && pl[0] == apr_q[apr_step - 1])))
		apr_next(h.opcode != APR_BASIC_RSP);
	else if (afe_step == AFE_VOTE && h.opcode == AFE_CMD_RSP_REMOTE_LPASS_CORE_HW_VOTE_REQUEST && len) {
		hw_handle = (int)pl[0];	/* sp2: the response carries the client handle */
		afe_next(1, 0);
	} else if (afe_step > AFE_IDLE && afe_step < AFE_DONE && afe_op && h.opcode == APR_BASIC_RSP && len > 1 &&
	    pl[0] == afe_op)
		afe_next(pl[1] == 0, pl[1]);
	else if (pcm_step > P_IDLE && pcm_step < P_DONE)
		pcm_rx(&h, pl, len);
}

/* --apr: start the queries once apr_audio_svc is open both ways; 3 s per query, then the next one. */
static void apr_poll(void)
{
	pcm_poll();
	if (apr_on && apr_step > 3) {	/* ap4: --afe sequence after the queries */
		if (afe_on && afe_step == AFE_IDLE && !apr_ready && !getenv("ADSP_APR_NO_GATE")) {
			printf("afe: skipped, ADSP audio not ready\n");	/* ap5 */
			afe_step = AFE_DONE;
		} else if (afe_on && afe_step == AFE_IDLE && swr_on && swr_nohw) {	/* hj1: group 1 only, no vote */
			swr_log("swr: --swr-nohw: no hw vote\n");
			swr_read("no hw vote");
			afe_step = AFE_DONE;
		} else if (afe_on && afe_step == AFE_IDLE) {
			afe_step = no_lpi && !swr_on ? AFE_CLK_F3 : AFE_VOTE;	/* hj0: the probe always votes */	/* sp2 */
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
			if (!c->r_iid) {	/* ap7 */
				c->r_iid = calloc(NRINT, sizeof(uint32_t));
				c->r_isz = calloc(NRINT, sizeof(uint32_t));
				c->r_use = calloc(NRINT, 1);
				if (!c->r_iid || !c->r_isz || !c->r_use)
					return -1;
			}
			if (c->r_n < NRINT) {
				c->r_isz[c->r_n] = e[0];
				c->r_use[c->r_n] = 0;
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
		else if (c && c->nreq <= 8)	/* ap7: granted: an INTENT should follow */
			printf("adsp: %s: intent request granted (%d queued)\n", c->name, c->npend);
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
			if (c)
				c->l_n--;	/* ap7: this packet used one of our intents */
		}
		if (rx_have + cl[0] <= sizeof(rxbuf)) {
			glink_rx_peek(rxbuf + rx_have, 16, cl[0]);
			rx_have += cl[0];
		}
		if (!cl[1] && !c)
			printf("adsp: data for unknown cid %u (%zu bytes) dropped\n", h[1], rx_have);
		if (!cl[1] && c) {
			c->nrx++;
			if (no_reuse) {
				glink_send(GLINK_CMD_RX_DONE, c->lcid, rx_iid, NULL, 0);
				give_intents(c, 1, 4096);
			} else {	/* ap7: hand the same intent back, as Linux qcom_glink does for reusable intents */
				glink_send(GLINK_CMD_RX_DONE_W_REUSE, c->lcid, rx_iid, NULL, 0);
				c->l_n++;
			}
			while (c->l_n < NLOCAL)	/* ap7: keep NLOCAL free intents queued */
				give_intents(c, NLOCAL - c->l_n > 4 ? 4 : NLOCAL - c->l_n, 4096);
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
	case GLINK_CMD_RX_DONE_W_REUSE: {	/* ap7: the ADSP consumed our tx on its intent p2 */
		int i;
		if (!c || !c->r_use)
			break;
		int re = h[0] == GLINK_CMD_RX_DONE_W_REUSE;
		if (c->ndone + c->nreuse < 8)
			printf("adsp: %s: remote %s intent %u\n", c->name, re ? "RX_DONE_W_REUSE" : "RX_DONE", p2);
		re ? c->nreuse++ : c->ndone++;
		for (i = 0; i < c->r_n && !(c->r_iid[i] == p2 && c->r_use[i]); i++)
			;
		if (i >= c->r_n)
			break;	/* intent_once already dropped it */
		c->r_busy--;
		if (re) {
			c->r_use[i] = 0;
		} else {
			c->r_n--;
			c->r_iid[i] = c->r_iid[c->r_n];
			c->r_isz[i] = c->r_isz[c->r_n];
			c->r_use[i] = c->r_use[c->r_n];
		}
		flush_pend(c);
		break;
	}
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
	for (int i = 0; i < nch; i++)	/* ap7: intent accounting per channel */
		if (ch[i].up)
			printf("adsp:   %s: remote intents %d (in use %d), local intents %d, tx queued %d, rx %u, intent reqs %u, "
			       "rx_done %u, rx_done_reuse %u\n", ch[i].name, ch[i].r_n, ch[i].r_busy, ch[i].l_n, ch[i].npend,
			       ch[i].nrx, ch[i].nreq, ch[i].ndone, ch[i].nreuse);
}

/* sns1: sns_link's hooks over our QRTR. */
static int sns_send(uint32_t lport, uint32_t node, uint32_t port, const void *d, size_t n)
{
	return qrtr_send(QRTR_TYPE_DATA, lport, node, port, d, (uint32_t)n);
}

static int sns_pump(void)
{
	return pump_one();
}

static const struct sns_link_ops sns_ops = { sns_send, sns_pump };

static int edge_loop(int ms, int stop, uint64_t t0)
{
	uint64_t end = now_ms() + (uint64_t)ms, next = 0, beat = now_ms() + 10000;
	if (watch_end && end > watch_end)
		end = watch_end;
	while (now_ms() < end) {
		if (got_sig) {
			printf("adsp: signal %d: stopping\n", (int)got_sig);
			amp_off();	/* sp1 */
			heartbeat();
			sns_summary();	/* sns1 */
			if (rpc_on) {
				fastrpc_summary();
				rpc_faults();
			}
			exit(1);
		}
		if (pcm_running && beat > now_ms() + 1000)	/* ap7: the first beat was 10 s out, after the pcm run */
			beat = now_ms() + 1000;
		if (now_ms() >= beat) {
			beat = now_ms() + (pcm_running ? 1000 : 10000);	/* ap7: 1 s while --pcm plays */
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
		if (swr_on && afe_step == AFE_DONE) {	/* hj0: probe over, exit */
			swr_log("swr: probe done\n");
			return 0;
		}
		amp_poll();	/* sp1 */
		sns_tick(&sns_ops);	/* sns1 */
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
	no_reuse = getenv("ADSP_NO_REUSE") != NULL;	/* ap7 */
	intent_once = getenv("ADSP_INTENT_ONCE") != NULL;
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
		else if (!strcmp(argv[i], "--no-reuse"))
			no_reuse = 1;
		else if (!strcmp(argv[i], "--intent-once"))
			intent_once = 1;
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
		else if (!strcmp(argv[i], "--swr-probe"))	/* hj0: read-only SoundWire/macro/gpio24 probe; implies --afe */
			apr_on = afe_on = swr_on = 1;
		else if (!strcmp(argv[i], "--swr-clk"))	/* hj0: + codec clock votes 0x30b-0x30d around a second read */
			swr_clk = 1;
		else if (!strcmp(argv[i], "--swr-only") && i + 1 < argc)	/* hj1: one read group per run (1-5) */
			swr_only = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--swr-clk-first"))	/* hj2: clocks before the first read; implies --swr-clk */
			swr_clk = swr_clk_first = 1;
		else if (!strcmp(argv[i], "--swr-init") && i + 1 < argc && argv[i + 1][0] >= 'a' && argv[i + 1][0] <= 'd')
			swr_stage = argv[++i][0], swr_clk = swr_clk_first = 1;	/* hj4: staged bring-up a-d; implies --swr-clk-first */
		else if (!strcmp(argv[i], "--swr-mbhc") && i + 1 < argc)	/* hj5: S3 plug poll; implies --swr-init d */
			swr_mbhc = atoi(argv[++i]), swr_stage = 'd', swr_clk = swr_clk_first = 1;
		else if (!strcmp(argv[i], "--swr-mbhc-unmask"))	/* hj5: 0x346B = 0xEF before polling */
			swr_mbhc_unmask = 1;
		else if (!strcmp(argv[i], "--swr-newlog"))	/* hj2: truncate /var/local/swr-probe.log at start */
			swr_newlog = 1;
		else if (!strcmp(argv[i], "--swr-nohw"))	/* hj1: skip the hw vote; group 1 only */
			swr_nohw = 1;
		else if (!strcmp(argv[i], "--no-lpi"))	/* sp2: no LPASS hw vote, no LPI pin mux */
			no_lpi = 1;
		else if (!strcmp(argv[i], "--vote-block") && i + 1 < argc)	/* sp2b: hw_block_id of the vote */
			vote_block = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--pcm"))	/* ap6: ASM + ADM PCM to the AFE port, amps untouched; implies --afe */
			apr_on = afe_on = pcm_on = smmu = 1;
		else if (!strcmp(argv[i], "--amp"))	/* sp1: left amp on for the --pcm tone; implies --pcm */
			amp_on = apr_on = afe_on = pcm_on = smmu = 1;
		else if (!strcmp(argv[i], "--rec"))	/* sp7: VMON/IMON loopback over SEC_MI2S_TX; needs --pcm --amp */
			rec_on = 1;
		else if (!strcmp(argv[i], "--rec-hex"))	/* sp7: hex of the first READ_DONE */
			rec_hex = 1;
		else if (!strcmp(argv[i], "--amp-pcm-stop-test"))	/* sp6: let the ADSP underflow with the amp on */
			amp_pcm_stop_test = 1;
		else if (!strcmp(argv[i], "--amp-check"))	/* sp1: configure + read back only, no GLOBAL_EN, then exit */
			amp_check = 1;
		else if (!strcmp(argv[i], "--amp-unmute"))	/* sp8: AMP_OUT_MUTE set at config, cleared after GLOBAL_EN */
			amp.out_mute = 1;
		else if (!strcmp(argv[i], "--amp-no-unmute"))
			amp.out_mute = 0;
		else if (!strcmp(argv[i], "--amp-clk"))	/* sp8: SP_RATE_CTRL = BCLK code */
			amp.clk = 1;
		else if (!strcmp(argv[i], "--amp-fsmon"))	/* sp8: + TST_FS_MON0 16|36<<12 (Linux's >6 MHz value) */
			amp.clk = 2;
		else if (!strcmp(argv[i], "--amp-no-clk"))
			amp.clk = 0;
		else if (!strcmp(argv[i], "--amp-boost"))	/* sp8: internal boost (BST_EN 2), stock DT values */
			amp_boost = 1;
		else if (!strcmp(argv[i], "--amp-invert-clk"))	/* sp8: AFE clock attri INVERT_COUPLE_NO (4) */
			amp_invert_clk = 1;
		else if (!strcmp(argv[i], "--i2s-32"))	/* pcm32: 32-bit I2S slots end to end (AFE + amp ASP), implies --adm-bits 24 */
			i2s_bits = 32;
		else if (!strcmp(argv[i], "--adm-bits") && i + 1 < argc)	/* pcm32: ADM_CMD_DEVICE_OPEN_V5 bit width */
			adm_bits = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--amp-addr") && i + 1 < argc)	/* ep1: 0x41 = bottom loudspeaker */
			amp.addr = (uint8_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--amp-db") && i + 1 < argc)
			amp_db = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--pcm-dbfs") && i + 1 < argc) {	/* sp5: clamped -40..-6 */
			pcm_dbfs = atoi(argv[++i]);
			if (pcm_dbfs > -6)
				pcm_dbfs = -6;
			if (pcm_dbfs < -40)
				pcm_dbfs = -40;
		} else if (!strcmp(argv[i], "--amp-gain") && i + 1 < argc)	/* sp3: analog gain steps 0..10 (cs_config clamps) */
			amp.amp_gain = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--pcm-sid") && i + 1 < argc) {
			pcm_sid = (uint32_t)strtoul(argv[++i], NULL, 0);
			pcm_shift = pcm_sid & 0xf;
		}
		else if (!strcmp(argv[i], "--pcm-shift") && i + 1 < argc)
			pcm_shift = (uint32_t)strtoul(argv[++i], NULL, 0) & 0xf;
		else if (!strcmp(argv[i], "--stub-svc") && i + 1 < argc && nstub < MAX_STUB) {
			char *e;
			stub[nstub].svc = (uint32_t)strtoul(argv[++i], &e, 0);
			stub[nstub].ver = *e == ':' ? (uint32_t)strtoul(e + 1, &e, 0) : 1;
			stub[nstub].inst = *e == ':' ? (uint32_t)strtoul(e + 1, &e, 0) : 0;
			nstub++;
		}
		else if (!strncmp(argv[i], "--sns-", 6)) {	/* sns1 */
			if (sns_opt(argc, argv, &i) != 1) {
				fprintf(stderr, "adspboot: %s: unknown or missing its value\n", argv[i]);
				return 2;
			}
		} else if (!strcmp(argv[i], "--dry"))
			smmu_dry = 1;
		else if (!strcmp(argv[i], "--sid") && i + 2 < argc) {
			sid = (uint32_t)strtoul(argv[++i], NULL, 0);
			mask = (uint32_t)strtoul(argv[++i], NULL, 0);
			one = 1;
		}
		else
			cmd = argv[i];
	}
	if (!adm_bits)	/* pcm32: ios-redfin runs 24-bit ADM on its 32-bit TDM slots */
		adm_bits = i2s_bits == 32 ? 24 : 16;
	amp.slot32 = i2s_bits == 32;
	if (sensors && !root) {	/* step 6: the sensors PD after the root PD needs the root session */
		fprintf(stderr, "adspboot: --sensors needs --rpc --root\n");
		return 2;
	}
	fastrpc_sensors(sensors);
	if (sns_wanted() && !sensors) {	/* sns1: the SNS service lives in the sensors PD */
		fprintf(stderr, "adspboot: --sns-* needs --rpc --root --sensors\n");
		return 2;
	}
	if (rec_on && (!pcm_on || !amp_on)) {	/* sp7 */
		fprintf(stderr, "adspboot: --rec needs --pcm --amp\n");
		return 2;
	}
	if (rec_on)
		rec_lead = REC_LEAD_PERIODS;
	if (amp_db > -20)
		amp_db = -20;	/* sp1: ceiling (cs_config clamps too) */
	atexit(lpi_restore);	/* sp2: runs after amp_off (atexit is LIFO) */
	atexit(amp_off);	/* sp1: every exit path powers the amp down */
	if (amp_check) {
		int r = amp_prepare();
		printf("amp: check %s (%d dB); GLOBAL_EN never set\n", r ? "FAILED" : "ok", amp_db);
		amp_off();
		return r ? 1 : 0;
	}
	if (amp_on) {
		if (amp_prepare()) {
			printf("amp: not ready: stop before the ADSP boot\n");
			return 1;
		}
		pcm_tone_periods = AMP_TONE_PERIODS;
	}
	if (!strcmp(cmd, "status"))
		return status();
	if (!strcmp(cmd, "shutdown")) {
		pas_shutdown();
		return 0;
	}
	if (swr_on && (swr_only < 0 || swr_only > 5 || (swr_nohw && (swr_only != 1 || swr_clk)))) {	/* hj1 */
		fprintf(stderr, "adspboot: --swr-only takes 0-5; --swr-nohw needs --swr-only 1 and no --swr-clk\n");
		return 2;
	}
	if (swr_on && swr_newlog) {	/* hj2: fresh log without an external rm; fsync file + dir, settle 200 ms */
		swr_fd = open("/var/local/swr-probe.log", O_WRONLY | O_APPEND | O_CREAT | O_TRUNC, 0644);
		if (swr_fd >= 0)
			fsync(swr_fd);
		int dfd = open("/var/local", O_RDONLY);
		if (dfd >= 0) {
			fsync(dfd);
			close(dfd);
		}
		sync();
		usleep(200000);
		swr_log("swr: --swr-newlog: log truncated\n");
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
		fprintf(stderr, "usage: adspboot [boot|status|shutdown|smmu] [--watch S] [--no-early] [--no-pdmap] [--smmu] [--rpc [--root [--sensors]]] [--apr] [--afe [--no-lpi] [--vote-block N]] [--swr-probe [--swr-clk | --swr-clk-first] [--swr-newlog] [--swr-only N] [--swr-init a|b|c|d] [--swr-mbhc SECONDS [--swr-mbhc-unmask]] [--swr-nohw]] [--pcm [--pcm-sid SID] [--pcm-shift N] [--pcm-dbfs N]] [--amp [--amp-addr 0x40|0x41] [--amp-db N] [--amp-gain N] [--amp-pcm-stop-test] [--amp-no-unmute] [--amp-no-clk] [--amp-fsmon] [--amp-boost] [--amp-invert-clk] [--rec [--rec-hex]]] [--amp-check [--amp-db N] [--amp-gain N]] [--dry] [--status] [--sid SID MASK] [--sns-list | --sns-stream TYPE [--sns-rate HZ] | --sns-motion [--sns-motion-dev PATH]] [--sns-secs N] [--sns-delay S] [--sns-dump] [--sns-one-port] [--sns-unregistered]\n");
		return 2;
	}
	if (smmu && adsp_smmu(one, sid, mask))
		return 1;
	if (watch <= 0 && (rpc_on || apr_on))	/* sns9: --watch 0 = no limit (launchd job); ~23 days fits the int ms math */
		watch = 2000000;
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
	sns_summary();	/* sns1 */
	if (rpc_on) {
		fastrpc_summary();
		rpc_faults();
	}
	return rc;
}
