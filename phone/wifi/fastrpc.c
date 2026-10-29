/*
 * FastRPC over glink "fastrpcglink-apps-dsp": attach to the ADSP's static sensors PD (as `adsprpcd sensorspd`),
 * open adsp_default_listener and serve its reverse calls (apps_remotectl + apps_std file access) from the AP.
 * Wire format: sunfish downstream adsprpc.c (smq_msg, remote_arg64 + page lists in one DMA buffer); the
 * listener / apps_std argument layouts: quic/fastrpc (adsp_listener next2, qaic skel conventions).
 * Files: /vendor/etc/sensors -> /var/local/fw/sensors (read-only), /persist/sensors (and /mnt/vendor/persist/sensors)
 * -> /var/local/sensors-persist (the only writable place). Everything else is refused.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "pmem.h"
#include "fastrpc.h"
#include "smmu.h"

#define SC_MAKE(m, in, out)	((((uint32_t)(m) & 0x1f) << 24) | (((in) & 0xff) << 16) | (((out) & 0xff) << 8))
#define SC_METHOD(sc)		(((sc) >> 24) & 0x1f)
#define SC_IN(sc)		(((sc) >> 16) & 0xff)
#define SC_OUT(sc)		(((sc) >> 8) & 0xff)
#define SC_HANDLES(sc)		((((sc) >> 4) & 0xf) + ((sc) & 0xf))

#define PD_SENSORS	2	/* downstream FASTRPC_INIT_ATTACH_SENSORS: fl->pd = 2 */
#define H_PROCESS_GROUP	1
#define H_REMOTECTL	0
#define H_LISTENER	3
#define BALIGN		128
#define M_FDLIST	16
#define M_CRCLIST	64
#define MAXARGS		16
#define DMA_SIZE	FASTRPC_DMA_SIZE
/* A pm_alloc buffer (rpc1: 0x1062a0000): the attach reached the sensor PD, which then died with
 * EX:sensor_process:frpck_0_0:0x93. rpc2 with the buffer at 0xe0f80000 (top of radio.c's unmanaged pool) reset the
 * whole SoC right after the attach was sent: never hand the DSP memory outside managed DRAM again. Since rpc3 the
 * buffer is managed DRAM above 4 GB seen by the DSP at a 32-bit IOVA through a translating SMMU bank
 * (adspboot --rpc, smmu_translate): dsp_base is that IOVA, dma_pa what the CPU uses. */
#define BUFS_LEN	0x10000u	/* our next2 in-buffer for the DSP's packed in bufs + out lens */
#define OBUF_MAX	0x20000u	/* our packed out bufs (prevbufs) */

#define AEE_EFAILED	1
#define AEE_EBADPARM	14
#define AEE_EUNSUPPORTED 20

#define VENDOR_SNS	"/vendor/etc/sensors"
#define VENDOR_DIR	"/var/local/fw/sensors"
#define PERSIST_DIR	"/var/local/sensors-persist"

/* Reverse handles we hand out from apps_remotectl open. guess: any nonzero value works (quic uses table ids). */
#define RH_APPS_STD	0x11
#define RH_APPS_MEM	0x12
#define RH_MSGD		0x13

struct smq_msg {
	uint32_t pid, tid;
	uint64_t ctx;
	uint32_t handle, sc;
	uint64_t addr, size;
};
struct rarg {
	void *p;
	uint32_t len;
};

static fastrpc_send_fn tx;
static uint64_t dma_pa, dsp_base;
static uint8_t meta[DMA_SIZE];
static uint32_t tgid, seq;
static int state;	/* 0 off, 1 attach, 2 remotectl open, 3 register, 4 init2, 5 next2 loop, 6 dead */
static uint64_t cur_ctx;
static uint32_t cur_sc, out_off[MAXARGS];
static struct rarg cur[MAXARGS];
static uint32_t listener_h;
static int root_mode;	/* audio step 5: attach to the root PD (pd 0) only, report, stop */

/* next2 state */
static uint32_t n2_in[4], n2_out[4];
static uint8_t n2_bufs[BUFS_LEN], obuf[OBUF_MAX];
static uint32_t obuf_len, prev_ctx;
static int prev_result = -1, n2_retry;

/* summary */
static unsigned n_std[40], n_ctl, n_other, n_err, n_files;
static char files[32][96];

static const char *std_name[] = { "fopen", "freopen", "fflush", "fclose", "fread", "fwrite", "fgetpos", "fsetpos",
	"ftell", "fseek", "flen", "rewind", "feof", "ferror", "clearerr", "print_string", "getenv", "setenv",
	"unsetenv", "fopen_with_env", "fgets", "get_search_paths_with_env", "fileExists", "fsync", "fremove",
	"fdopen_decrypt", "opendir", "closedir", "readdir", "mkdir", "rmdir", "stat", "ftrunc", "frename",
	"fopen_fd", "fclose_fd", "fopen_with_env_fd" };

static uint32_t align(uint32_t v, uint32_t a)
{
	return (v + a - 1) & ~(a - 1);
}

/* whole 32-bit words (safe for any pmem memory) */
static void dev_rd(uint64_t pa, void *p, uint32_t len)
{
	static uint8_t tmp[DMA_SIZE];
	if (len > DMA_SIZE)
		return;
	pm_rd(pa, tmp, (len + 3) & ~3u);
	memcpy(p, tmp, len);
}

/* ---- forward invoke: everything (metadata + copies of all buffers) in one pm_alloc buffer, PA = DSP address ---- */

static int invoke(uint32_t handle, uint32_t sc, struct rarg *a)
{
	uint32_t n = SC_IN(sc) + SC_OUT(sc);
	if (n > MAXARGS)
		return -1;
	uint32_t metalen = n * (16 + 8 + 16) + 8 * M_FDLIST + 4 * M_CRCLIST, off = metalen;
	memset(meta, 0, metalen);
	uint8_t *rpra = meta, *list = meta + 16 * n, *pages = list + 8 * n;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t pg[2] = { a[i].len ? 1u : 0u, i };
		memcpy(list + 8 * i, pg, 8);
		uint64_t pv = 0, len = a[i].len, pp[2] = { 0, 0 };
		if (a[i].len) {
			off = align(off, BALIGN);
			if (off + a[i].len > DMA_SIZE)
				return -1;
			pv = dsp_base + off;	/* guess: downstream puts its kernel VA here; the DSP only needs the page offset */
			pp[0] = pv & ~0xfffull;
			pp[1] = (((pv + len - 1) >> 12) - (pv >> 12) + 1) << 12;
			if (i < SC_IN(sc))
				memcpy(meta + off, a[i].p, a[i].len);
			else
				memset(meta + off, 0, a[i].len);
			out_off[i] = off;
			off += a[i].len;
		}
		uint64_t r[2] = { pv, len };
		memcpy(rpra + 16 * i, r, 16);
		memcpy(pages + 16 * i, pp, 16);
	}
	if (pm_wr(dma_pa, meta, align(off, 4)))
		return -1;
	seq++;
	cur_ctx = root_mode ? (uint64_t)(seq % 255 + 1) << 4 :	/* step 5: n << 4 | pd, as the SM7150 reference */
		  ((uint64_t)seq << 12) | (1u << 4);	/* downstream: (ctx ptr & ~0xfff) | (table index << 4) */
	cur_sc = sc;
	if (n)
		memcpy(cur, a, n * sizeof(*a));
	/* guess: downstream sends pid 0 for kernel-originated invokes (the attach); we use tgid for all user ones */
	struct smq_msg m = { state == 1 ? 0 : tgid, tgid, cur_ctx | (root_mode ? 0 : PD_SENSORS), handle, sc, dsp_base,
			     align(off, 4096) };
	printf("rpc: -> h %u m %u in %u out %u (sc %08x) ctx %llx, %u bytes\n", handle, SC_METHOD(sc), SC_IN(sc),
	       SC_OUT(sc), sc, (unsigned long long)m.ctx, off);
	tx(&m, sizeof(m));
	return 0;
}

/* Copy the out buffers back from the DMA buffer. */
static void fetch_outs(void)
{
	for (uint32_t i = SC_IN(cur_sc); i < SC_IN(cur_sc) + SC_OUT(cur_sc); i++)
		if (cur[i].len)
			dev_rd(dma_pa + out_off[i], cur[i].p, cur[i].len);
}

/* ---- apps_std ---- */

static FILE *ftab[32];
static DIR *dtab[8];

static int under(const char *p, const char *pre)
{
	size_t l = strlen(pre);
	return !strncmp(p, pre, l) && (p[l] == 0 || p[l] == '/');
}

/* DSP path -> our path; *w = writable. 0 or an errno. */
static int map_path(const char *in, char *out, size_t sz, int *w)
{
	*w = 0;
	if (strstr(in, ".."))
		return EACCES;
	const char *rest;
	if (under(in, VENDOR_SNS)) {
		rest = in + strlen(VENDOR_SNS);
		snprintf(out, sz, VENDOR_DIR "%s", rest);
	} else if (under(in, "/persist/sensors") || under(in, "/mnt/vendor/persist/sensors")) {
		rest = strstr(in, "/persist/sensors") + strlen("/persist/sensors");
		snprintf(out, sz, PERSIST_DIR "%s", rest);
		*w = 1;
	} else {
		return ENOENT;
	}
	return 0;
}

/* mkdir -p for the parent dirs of path (only below PERSIST_DIR). */
static void mkparents(const char *path)
{
	char b[256];
	snprintf(b, sizeof(b), "%s", path);
	mkdir(PERSIST_DIR, 0755);
	for (char *s = b + strlen(PERSIST_DIR) + 1; (s = strchr(s, '/')); s++) {
		*s = 0;
		mkdir(b, 0755);
		*s = '/';
	}
}

static uint32_t g32(const struct rarg *a, uint32_t o)
{
	uint32_t v = 0;
	if (a->len >= o + 4)
		memcpy(&v, (uint8_t *)a->p + o, 4);
	return v;
}

static void p32(struct rarg *a, uint32_t o, uint32_t v)
{
	if (a->len >= o + 4)
		memcpy((uint8_t *)a->p + o, &v, 4);
}

static const char *str(const struct rarg *a)
{
	if (!a->len || ((char *)a->p)[a->len - 1])
		return NULL;
	return a->p;
}

static int do_fopen(const char *name, const char *mode, struct rarg *ro)
{
	char path[256];
	int w, wantw = strpbrk(mode, "wa+") != NULL;
	int e = map_path(name, path, sizeof(path), &w);
	if (!e && wantw && !w)
		e = EROFS;
	if (e)
		return e;
	if (wantw)
		mkparents(path);
	int fd;
	for (fd = 0; fd < 32 && ftab[fd]; fd++)
		;
	if (fd >= 32)
		return EMFILE;
	FILE *f = fopen(path, mode);
	if (!f)
		return errno ? errno : AEE_EFAILED;
	ftab[fd] = f;
	if (n_files < 32)
		snprintf(files[n_files], sizeof(files[0]), "%s", name);
	n_files++;
	p32(ro, 0, (uint32_t)fd + 1);	/* guess: apps_std_FILE = our index + 1 (0 never handed out) */
	return 0;
}

static FILE *getf(uint32_t h)
{
	return h >= 1 && h <= 32 ? ftab[h - 1] : NULL;
}

static int search_env(const char *env, char *val, size_t sz)
{
	/* guess: only ADSP_LIBRARY_PATH is set, to what adsprpcd uses on Android; others unset */
	if (!strcmp(env, "ADSP_LIBRARY_PATH")) {
		snprintf(val, sz, "/vendor/lib/rfsa/adsp;/vendor/dsp/sensorspd;/vendor/dsp");
		return 0;
	}
	return -1;
}

/* One apps_std call; returns the result code, logs one line. */
static int apps_std(uint32_t sc, struct rarg *a)
{
	uint32_t mid = SC_METHOD(sc), nin = SC_IN(sc), nout = SC_OUT(sc);
	struct rarg *p0 = &a[0];
	char what[160] = "";
	int r = 0;
	if (mid == 31) {	/* extended: the method id is primIn word 0 */
		if (nin < 1 || p0->len < 4)
			return AEE_EBADPARM;
		mid = g32(p0, 0);
	}
	if (mid < 40)
		n_std[mid]++;
	switch (mid) {
	case 0: {	/* fopen: in {nameLen, modeLen} name mode, out {fd} */
		const char *n = nin == 3 ? str(&a[1]) : NULL, *m = nin == 3 ? str(&a[2]) : NULL;
		if (!n || !m || nout != 1) {
			r = AEE_EBADPARM;
			break;
		}
		snprintf(what, sizeof(what), "%s \"%s\"", n, m);
		r = do_fopen(n, m, &a[3]);
		break;
	}
	case 19: {	/* fopen_with_env: in {4 lens} env delim name mode, out {fd} */
		const char *e = nin == 5 ? str(&a[1]) : NULL, *d = nin == 5 ? str(&a[2]) : NULL;
		const char *n = nin == 5 ? str(&a[3]) : NULL, *m = nin == 5 ? str(&a[4]) : NULL;
		if (!e || !d || !n || !m || nout != 1) {
			r = AEE_EBADPARM;
			break;
		}
		snprintf(what, sizeof(what), "%s:%s \"%s\"", e, n, m);
		char val[256];
		r = ENOENT;
		if (n[0] == '/' || search_env(e, val, sizeof(val))) {
			r = do_fopen(n, m, &a[5]);
			break;
		}
		for (char *s = strtok(val, d[0] ? d : ";"); s && r; s = strtok(NULL, d[0] ? d : ";")) {
			char full[256];
			snprintf(full, sizeof(full), "%s/%s", s, n);
			r = do_fopen(full, m, &a[5]);
		}
		break;
	}
	case 2: case 3: case 11: case 23: {	/* fflush / fclose / rewind / fsync: in {fd} */
		uint32_t h = g32(p0, 0);
		FILE *f = getf(h);
		snprintf(what, sizeof(what), "fd %u", h);
		if (!f)
			r = EBADF;
		else if (mid == 3) {
			fclose(f);
			ftab[h - 1] = NULL;
		} else if (mid == 11)
			rewind(f);
		else
			r = fflush(f) ? errno : 0;
		break;
	}
	case 4: case 20: {	/* fread: in {fd, len}, out {read, eof} buf; fgets: in {fd, len}, out {eof} buf */
		uint32_t h = g32(p0, 0);
		FILE *f = getf(h);
		if (nin != 1 || nout != 2) {
			r = AEE_EBADPARM;
			break;
		}
		struct rarg *ro = &a[1], *b = &a[2];
		if (!f) {
			r = EBADF;
			break;
		}
		if (mid == 4) {
			size_t got = b->len ? fread(b->p, 1, b->len, f) : 0;
			if (!got && b->len && ferror(f)) {
				r = AEE_EFAILED;
				break;
			}
			p32(ro, 0, (uint32_t)got);
			p32(ro, 4, got <= b->len ? (uint32_t)!!feof(f) : 0);
			snprintf(what, sizeof(what), "fd %u len %u -> %zu%s", h, b->len, got, feof(f) ? " eof" : "");
		} else {
			if (b->len && !fgets(b->p, (int)b->len, f))
				((char *)b->p)[0] = 0;
			p32(ro, 0, (uint32_t)!!feof(f));
			snprintf(what, sizeof(what), "fd %u len %u", h, b->len);
		}
		break;
	}
	case 5: {	/* fwrite: in {fd, len} buf, out {written, eof}; files are writable only if opened in persist */
		uint32_t h = g32(p0, 0);
		FILE *f = getf(h);
		if (nin != 2 || nout != 1) {
			r = AEE_EBADPARM;
			break;
		}
		if (!f) {
			r = EBADF;
			break;
		}
		size_t put = a[1].len ? fwrite(a[1].p, 1, a[1].len, f) : 0;
		p32(&a[2], 0, (uint32_t)put);
		p32(&a[2], 4, 0);
		if (!put && a[1].len)
			r = AEE_EFAILED;	/* read-only stream (EROFS refused at fopen) */
		snprintf(what, sizeof(what), "fd %u len %u -> %zu", h, a[1].len, put);
		break;
	}
	case 8: case 12: case 13: {	/* ftell / feof / ferror: in {fd}, out {int} */
		uint32_t h = g32(p0, 0);
		FILE *f = getf(h);
		if (!f || nout != 1) {
			r = f ? AEE_EBADPARM : EBADF;
			break;
		}
		long v = mid == 8 ? ftell(f) : mid == 12 ? feof(f) : ferror(f);
		p32(&a[1], 0, (uint32_t)v);
		snprintf(what, sizeof(what), "fd %u -> %ld", h, v);
		break;
	}
	case 9: {	/* fseek: in {fd, off, whence} */
		uint32_t h = g32(p0, 0), wh = g32(p0, 8);
		int o = (int)g32(p0, 4);
		FILE *f = getf(h);
		snprintf(what, sizeof(what), "fd %u off %d whence %u", h, o, wh);
		if (!f)
			r = EBADF;
		else if (wh > 2)
			r = EINVAL;
		else
			r = fseek(f, o, wh == 0 ? SEEK_SET : wh == 1 ? SEEK_CUR : SEEK_END) ? errno : 0;
		break;
	}
	case 10: {	/* flen: in {fd}, out {u64} */
		uint32_t h = g32(p0, 0);
		FILE *f = getf(h);
		struct stat st;
		if (!f || nout != 1 || fstat(fileno(f), &st)) {
			r = f ? AEE_EBADPARM : EBADF;
			break;
		}
		uint64_t l = (uint64_t)st.st_size;
		if (a[1].len >= 8)
			memcpy(a[1].p, &l, 8);
		snprintf(what, sizeof(what), "fd %u -> %llu", h, (unsigned long long)l);
		break;
	}
	case 15: {	/* print_string: in {len} str */
		const char *s = nin == 2 ? str(&a[1]) : NULL;
		snprintf(what, sizeof(what), "\"%.120s\"", s ? s : "?");
		break;
	}
	case 16: {	/* getenv: in {nameLen, valLen} name, out {valLenReq} val */
		const char *n = nin == 2 ? str(&a[1]) : NULL;
		char val[256];
		if (!n || nout != 2) {
			r = AEE_EBADPARM;
			break;
		}
		snprintf(what, sizeof(what), "%s", n);
		if (search_env(n, val, sizeof(val))) {
			r = -1;	/* guess: an unset variable returns an error (quic: ERRNO, -1 when errno is 0) */
			break;
		}
		p32(&a[2], 0, (uint32_t)strlen(val) + 1);
		if (a[3].len > strlen(val))
			memcpy(a[3].p, val, strlen(val) + 1);
		else
			r = AEE_EBADPARM;
		break;
	}
	case 21: {	/* get_search_paths_with_env */
		/* guess: in {envLen, delimLen, pathsLen} env delim [u32 len per path], out {numPaths, maxPathLen u16}
		 * then one buffer per path (qaic out sequence of strings) */
		const char *e = nin >= 3 ? str(&a[1]) : NULL, *d = nin >= 3 ? str(&a[2]) : NULL;
		if (!e || !d || nin != 4 || nout < 1) {
			r = AEE_EUNSUPPORTED;
			snprintf(what, sizeof(what), "layout in %u out %u not understood", nin, nout);
			break;
		}
		char val[256];
		snprintf(what, sizeof(what), "%s", e);
		if (search_env(e, val, sizeof(val))) {
			r = -1;
			break;
		}
		uint32_t np = 0, mx = 0;
		for (char *s = strtok(val, d[0] ? d : ";"); s; s = strtok(NULL, d[0] ? d : ";"), np++) {
			uint32_t l = (uint32_t)strlen(s) + 1;
			mx = l > mx ? l : mx;
			struct rarg *o = &a[nin + 1 + np];
			if (nin + 1 + np < nin + nout && o->len >= l)
				memcpy(o->p, s, l);
		}
		p32(&a[nin], 0, np);
		if (a[nin].len >= 6)
			memcpy((uint8_t *)a[nin].p + 4, &(uint16_t){ (uint16_t)mx }, 2);
		break;
	}
	case 22: case 26: case 29: case 31: {	/* fileExists / opendir / mkdir / stat: a path in a[1] */
		const char *n = nin == 2 ? str(&a[1]) : NULL;
		char path[256];
		int w;
		if (!n) {
			r = AEE_EBADPARM;
			break;
		}
		snprintf(what, sizeof(what), "%s", n);
		r = map_path(n, path, sizeof(path), &w);
		if (mid == 22) {	/* out {bool} */
			struct stat st;
			if (nout == 1 && a[2].len)
				((uint8_t *)a[2].p)[0] = !r && !stat(path, &st);
			r = nout == 1 ? 0 : AEE_EBADPARM;
		} else if (r) {
			break;
		} else if (mid == 26) {	/* out {u64 dir} */
			int i;
			for (i = 0; i < 8 && dtab[i]; i++)
				;
			DIR *dp = i < 8 ? opendir(path) : NULL;
			if (!dp) {
				r = i < 8 ? errno : EMFILE;
				break;
			}
			dtab[i] = dp;
			uint64_t hd = (uint64_t)i + 1;
			if (nout == 1 && a[2].len >= 8)
				memcpy(a[2].p, &hd, 8);
		} else if (mid == 29) {	/* in {nameLen, mode} */
			if (!w)
				r = EROFS;
			else {
				mkparents(path);
				r = mkdir(path, 0755) && errno != EEXIST ? errno : 0;
			}
		} else {	/* stat, out 96 bytes apps_std_STAT */
			struct stat st;
			if (stat(path, &st)) {
				r = errno;
				break;
			}
			uint64_t s[12] = { 96, (uint64_t)st.st_dev, (uint64_t)st.st_ino,
					   (uint64_t)st.st_mode | ((uint64_t)st.st_nlink << 32), (uint64_t)st.st_rdev,
					   (uint64_t)st.st_size, (uint64_t)st.st_atimespec.tv_sec,
					   (uint64_t)st.st_atimespec.tv_nsec, (uint64_t)st.st_mtimespec.tv_sec,
					   (uint64_t)st.st_mtimespec.tv_nsec, (uint64_t)st.st_ctimespec.tv_sec,
					   (uint64_t)st.st_ctimespec.tv_nsec };
			/* guess: tsz = sizeof(apps_std_STAT) */
			if (nout == 1 && a[2].len >= 96)
				memcpy(a[2].p, s, 96);
			else
				r = AEE_EBADPARM;
			snprintf(what, sizeof(what), "%s size %lld mode %o", n, (long long)st.st_size, st.st_mode);
		}
		break;
	}
	case 27: case 28: {	/* closedir: in {u64}; readdir: in {u64}, out {ino, name[255], pad} eof@260 */
		uint64_t hd = 0;
		if (p0->len >= 8)
			memcpy(&hd, p0->p, 8);
		DIR *dp = hd >= 1 && hd <= 8 ? dtab[hd - 1] : NULL;
		snprintf(what, sizeof(what), "dir %llu", (unsigned long long)hd);
		if (!dp) {
			r = EBADF;
			break;
		}
		if (mid == 27) {
			closedir(dp);
			dtab[hd - 1] = NULL;
			break;
		}
		if (nout != 1 || a[1].len < 264) {
			r = AEE_EBADPARM;
			break;
		}
		struct dirent *de = readdir(dp);
		memset(a[1].p, 0, 264);
		if (de) {
			p32(&a[1], 0, (uint32_t)de->d_ino);
			snprintf((char *)a[1].p + 4, 255, "%s", de->d_name);
			snprintf(what, sizeof(what), "dir %llu -> %s", (unsigned long long)hd, de->d_name);
		} else {
			p32(&a[1], 260, 1);
			snprintf(what, sizeof(what), "dir %llu -> eof", (unsigned long long)hd);
		}
		break;
	}
	case 24: case 30: case 33: case 32: {	/* fremove / rmdir / frename / ftrunc: refused (no deletes) */
		const char *n = nin >= 2 ? str(&a[1]) : NULL;
		snprintf(what, sizeof(what), "%s refused", n ? n : "");
		r = EROFS;
		break;
	}
	default:
		r = AEE_EUNSUPPORTED;
		snprintf(what, sizeof(what), "unsupported (in %u out %u)", nin, nout);
	}
	printf("rpc: <- apps_std.%s %s -> %d\n", mid < sizeof(std_name) / sizeof(std_name[0]) ? std_name[mid] : "?",
	       what, r);
	return r;
}

/* apps_remotectl (reverse handle 0): open in {nameLen, dlerrLen} name, out {handle, nErr} dlerr; close m 1. */
static int remotectl(uint32_t sc, struct rarg *a)
{
	n_ctl++;
	if (SC_METHOD(sc) == 1) {
		printf("rpc: <- apps_remotectl.close %u -> 0\n", g32(&a[0], 0));
		if (SC_OUT(sc) >= 1)
			p32(&a[1], 0, 0);
		return 0;
	}
	const char *n = SC_METHOD(sc) == 0 && SC_IN(sc) == 2 && SC_OUT(sc) == 2 ? str(&a[1]) : NULL;
	if (!n) {
		printf("rpc: <- apps_remotectl m %u sc %08x -> unsupported\n", SC_METHOD(sc), sc);
		return AEE_EUNSUPPORTED;
	}
	uint32_t h = !strcmp(n, "apps_std") ? RH_APPS_STD : !strcmp(n, "apps_mem") ? RH_APPS_MEM :
		     !strcmp(n, "adspmsgd_apps") ? RH_MSGD : 0;
	int r = h ? 0 : 6;	/* guess: AEE_EUNABLETOLOAD (6) for modules we don't have, in nErr and the result */
	p32(&a[2], 0, h);
	p32(&a[2], 4, (uint32_t)r);
	if (!h && a[3].len)
		snprintf(a[3].p, a[3].len, "not on this AP");
	printf("rpc: <- apps_remotectl.open \"%s\" -> handle %x err %d\n", n, h, r);
	return r;
}

static int dispatch(uint32_t h, uint32_t sc, struct rarg *a)
{
	if (h == H_REMOTECTL)
		return remotectl(sc, a);
	if (h == RH_APPS_STD)
		return apps_std(sc, a);
	n_other++;
	printf("rpc: <- handle %x m %u in %u out %u: unsupported\n", h, SC_METHOD(sc), SC_IN(sc), SC_OUT(sc));
	return AEE_EUNSUPPORTED;
}

/* ---- the listener loop: adsp_listener_next2 (handle 3, method 4, in 2 out 2) ---- */

static void next2(void)
{
	n2_in[0] = prev_ctx;
	n2_in[1] = (uint32_t)prev_result;
	n2_in[2] = prev_result ? 0 : obuf_len;	/* quic: no out bufs with an error result */
	n2_in[3] = BUFS_LEN;
	struct rarg a[4] = { { n2_in, 16 }, { obuf, n2_in[2] }, { n2_out, 16 }, { n2_bufs, BUFS_LEN } };
	if (invoke(H_LISTENER, SC_MAKE(4, 2, 2), a))
		state = 6;
}

/* Unpack the DSP's packed in bufs + out lens (quic listener_buf.h), run it, pack the out bufs for the reply. */
static void serve(void)
{
	uint32_t ctx = n2_out[0], h = n2_out[1], sc = n2_out[2], req = n2_out[3];
	uint32_t nin = SC_IN(sc), nout = SC_OUT(sc), o = 0;
	struct rarg a[MAXARGS];
	int r = 0;
	prev_ctx = ctx;
	obuf_len = 0;
	if (req > BUFS_LEN || nin + nout > MAXARGS || SC_HANDLES(sc)) {
		printf("rpc: <- ctx %x h %x sc %08x: in bufs %u bytes / handles: refused\n", ctx, h, sc, req);
		prev_result = AEE_EBADPARM;
		n_err++;
		return;
	}
	for (uint32_t i = 0; i < nin && !r; i++) {
		uint32_t l;
		if (o + 4 > req) {
			r = AEE_EBADPARM;
			break;
		}
		memcpy(&l, n2_bufs + o, 4);
		o += 4;
		if (l)
			o = align(o, 8);
		if (o + l > req)
			r = AEE_EBADPARM;
		a[i].p = n2_bufs + o;
		a[i].len = l;
		o += l;
	}
	uint32_t w = 0;
	for (uint32_t i = 0; i < nout && !r; i++) {
		uint32_t l;
		if (o + 4 > req) {
			r = AEE_EBADPARM;
			break;
		}
		memcpy(&l, n2_bufs + o, 4);
		o += 4;
		if (w + 4 > OBUF_MAX) {
			r = AEE_EBADPARM;
			break;
		}
		memcpy(obuf + w, &l, 4);
		w += 4;
		if (l)
			w = align(w, 8);
		if (w + l > OBUF_MAX) {
			r = AEE_EBADPARM;
			break;
		}
		memset(obuf + w, 0, l);
		a[nin + i].p = obuf + w;
		a[nin + i].len = l;
		w += l;
	}
	if (r) {
		printf("rpc: <- ctx %x h %x sc %08x: bad packing (%u bytes)\n", ctx, h, sc, req);
	} else {
		r = dispatch(h, sc, a);
		obuf_len = w;
	}
	if (r)
		n_err++;
	prev_result = r;
}

void fastrpc_rx(const void *p, size_t n)
{
	uint64_t ctx;
	int32_t ret;
	if (n < 12) {
		printf("rpc: short rx %zu bytes\n", n);
		return;
	}
	memcpy(&ctx, p, 8);
	memcpy(&ret, (const uint8_t *)p + 8, 4);
	if ((ctx & ~3ull) != (cur_ctx & ~3ull)) {	/* downstream compares rsp->ctx & ~3 */
		printf("rpc: rx for ctx %llx (waiting for %llx), dropped\n", (unsigned long long)ctx,
		       (unsigned long long)cur_ctx);
		return;
	}
	printf("rpc: <- rsp ctx %llx ret %d (%zu bytes)\n", (unsigned long long)ctx, ret, n);
	if (!ret)
		fetch_outs();
	if (root_mode && state == 1) {	/* step 5: report the reply and the SMMU state, nothing more */
		printf("rpc: root PD attach -> %d\n", ret);
		smmu_status("ADSP rpc", 0x1b23);
		state = 6;
		return;
	}
	switch (state) {
	case 1: {	/* attach done: remotectl_open("adsp_default_listener") */
		if (ret) {
			printf("rpc: sensors PD attach failed (%d)\n", ret);
			state = 6;
			return;
		}
		static char name[] = "adsp_default_listener", dlerr[256];
		static uint32_t pin[2], pout[2];
		pin[0] = sizeof(name);
		pin[1] = sizeof(dlerr);
		struct rarg a[4] = { { pin, 8 }, { name, sizeof(name) }, { pout, 8 }, { dlerr, sizeof(dlerr) } };
		state = 2;
		invoke(H_REMOTECTL, SC_MAKE(0, 2, 2), a);
		return;
	}
	case 2: {
		uint32_t *pout = cur[2].p;
		listener_h = pout[0];
		printf("rpc: remotectl_open adsp_default_listener -> ret %d handle %x nErr %d%s%.64s\n", ret, pout[0],
		       (int)pout[1], ret ? " dlerror " : "", ret ? (char *)cur[3].p : "");
		if (ret || pout[1]) {
			state = 6;
			return;
		}
		state = 3;
		/* guess: adsp_default_listener_register = method 0 on the opened handle, no args */
		invoke(listener_h, SC_MAKE(0, 0, 0), NULL);
		return;
	}
	case 3:
		printf("rpc: adsp_default_listener_register -> %d\n", ret);
		if (ret) {
			state = 6;
			return;
		}
		state = 4;
		invoke(H_LISTENER, SC_MAKE(3, 0, 0), NULL);	/* adsp_listener_init2 */
		return;
	case 4:
		printf("rpc: adsp_listener_init2 -> %d\n", ret);
		if (ret) {
			state = 6;
			return;
		}
		state = 5;
		next2();
		return;
	case 5:
		if (ret) {	/* quic: retry once with the error as the result, then give up */
			if (n2_retry++) {
				printf("rpc: next2 failed twice (%d): listener stops\n", ret);
				state = 6;
				return;
			}
			prev_result = ret;
			next2();
			return;
		}
		n2_retry = 0;
		serve();
		next2();
		return;
	}
}

void fastrpc_root(int on)
{
	root_mode = on;
}

void fastrpc_buffer(uint64_t pa, uint64_t dsp)
{
	dma_pa = pa;
	dsp_base = dsp;
}

int fastrpc_start(fastrpc_send_fn send)
{
	tx = send;
	if (!dma_pa && !(dma_pa = pm_alloc(DMA_SIZE, 0x1000))) {
		printf("rpc: no DMA buffer\n");
		return -1;
	}
	if (!dsp_base)
		dsp_base = dma_pa;
	tgid = (uint32_t)getpid();
	printf("rpc: DMA buffer %llx+%x (DSP sees %llx), tgid %u, attaching to the %s PD (pd %u)\n",
	       (unsigned long long)dma_pa, DMA_SIZE, (unsigned long long)dsp_base, tgid, root_mode ? "root" : "sensors",
	       root_mode ? 0 : PD_SENSORS);
	if (root_mode)
		smmu_status("ADSP rpc", 0x1b23);	/* step 4/5: fault state before the DSP gets the address */
	state = 1;
	static uint32_t t;
	t = tgid;
	struct rarg a[1] = { { &t, 4 } };
	return invoke(H_PROCESS_GROUP, SC_MAKE(0, 1, 0), a);
}

void fastrpc_summary(void)
{
	static const char *st[] = { "off", "attach", "remotectl_open", "register", "init2", "serving", "stopped" };
	printf("rpc: summary: state %s, %u invokes, %u remotectl calls, %u other-handle calls, %u errors\n", st[state],
	       seq, n_ctl, n_other, n_err);
	for (unsigned i = 0; i < 40; i++)
		if (n_std[i])
			printf("rpc:   apps_std.%s x%u\n", i < sizeof(std_name) / sizeof(std_name[0]) ? std_name[i] : "?",
			       n_std[i]);
	printf("rpc:   files opened: %u\n", n_files);
	for (unsigned i = 0; i < n_files && i < 32; i++)
		printf("rpc:     %s\n", files[i]);
}
