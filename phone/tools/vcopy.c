// vcopy: copy a tree with everything iOS keeps on files (xattrs, ACLs, BSD flags, owner, mode, times).
// toybox cp -a drops xattrs/ACLs/flags; a /private/var copied with it made daemons fail with EACCES.
//   vcopy [-x EXCL]... SRC DST      recursive copyfile(3) COPYFILE_ALL; EXCL = path prefix of SRC to skip
//   vcopy -l PATH                   xattr names, st_flags, ACL yes/no of one path
//   vcopy -c SRC DST [-x EXCL]...   compare trees (type, uid, gid, mode, flags, xattr names, size); read-only
//   vcopy -m SRC DST [-x EXCL]...   metadata pass only: owner, ACL, flags of entries that exist in DST (no data)
// SRC DST is contents-to-contents: copyfile alone would nest SRC's basename inside an existing DST, so when DST is an
// existing directory each entry of SRC is copied to DST/name.
// Regular-file data is copied with read/write (see copydata). Sockets are skipped. Exit 1 on any error (or any difference with -c).
#include <copyfile.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/acl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

static const char *excl[64];
static int nexcl;
static long nfiles, ndirs, nlinks, nskip, nerr, nretry;

static int excluded(const char *p)
{
	for (int i = 0; i < nexcl; i++) {
		size_t n = strlen(excl[i]);
		if (!strncmp(p, excl[i], n) && (p[n] == 0 || p[n] == '/'))
			return 1;
	}
	return 0;
}

// Owner again after copyfile's STAT pass, in case it was refused silently (copyfile ignores fchown errors).
static void fixowner(const char *src, const char *dst)
{
	struct stat s, d;
	if (!dst || lstat(src, &s) || lstat(dst, &d))
		return;
	if ((s.st_uid != d.st_uid || s.st_gid != d.st_gid) && lchown(dst, s.st_uid, s.st_gid)) {
		fprintf(stderr, "vcopy: lchown %s: %s\n", dst, strerror(errno));
		nerr++;
	}
}

// ACL and BSD flags explicitly: copyfile(COPYFILE_METADATA|COPYFILE_STAT) left regular files without their ACL
// (~450 files in /private/var) and lost flags 0x80 on one. Flags last: immutable flags would block the rest.
static long nfacl, nfflags;
static void fixmeta(const char *src, const char *dst)
{
	struct stat s, d;
	if (!dst)
		return;
	acl_t a = acl_get_link_np(src, ACL_TYPE_EXTENDED);
	if (a) {
		if (acl_set_link_np(dst, ACL_TYPE_EXTENDED, a)) {
			fprintf(stderr, "vcopy: acl %s: %s\n", dst, strerror(errno));
			nerr++;
		} else
			nfacl++;
		acl_free(a);
	}
	if (lstat(src, &s) || lstat(dst, &d) || s.st_flags == d.st_flags)
		return;
	if (lchflags(dst, s.st_flags)) {
		fprintf(stderr, "vcopy: flags %s: %s\n", dst, strerror(errno));
		nerr++;
	} else
		nfflags++;
}

// Data with plain read/write: copyfile's data path (fcopyfile) got EFAULT on 5691 files (sqlite *.db, -shm, -wal)
// from our block driver; buffered read/write works. align != 0: 4096-byte aligned buffer and 4096-byte reads.
// Returns 0, or -1 with *step set and errno kept.
static int copydata(const char *src, const char *dst, int align, const char **step)
{
	size_t bs = align ? 4096 : 256 * 1024;
	char *buf = NULL;
	int in = -1, out = -1, e = 0;
	*step = "open";
	if (align ? posix_memalign((void **)&buf, 4096, bs) : !(buf = malloc(bs))) {
		e = ENOMEM;
		goto done;
	}
	if ((in = open(src, O_RDONLY)) < 0 || (out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_EXCL, 0600)) < 0) {
		e = errno;
		goto done;
	}
	for (;;) {
		ssize_t n = read(in, buf, bs);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			e = errno;
			*step = "read";
			goto done;
		}
		if (n == 0)
			break;
		for (ssize_t o = 0; o < n;) {
			ssize_t w = write(out, buf + o, n - o);
			if (w < 0 && errno == EINTR)
				continue;
			if (w < 0) {
				e = errno;
				*step = "write";
				goto done;
			}
			o += w;
		}
	}
done:
	if (in >= 0)
		close(in);
	if (out >= 0)
		close(out);
	free(buf);
	errno = e;
	return e ? -1 : 0;
}

// Regular file: data ourselves, then copyfile for ACL, xattrs, mode, owner, flags, times only.
static void copyreg(const char *src, const char *dst)
{
	const char *step;
	int r = copydata(src, dst, 0, &step);
	if (r && errno == EFAULT && !strcmp(step, "read")) {
		nretry++;
		unlink(dst);
		r = copydata(src, dst, 1, &step);
	}
	if (!r && copyfile(src, dst, NULL, COPYFILE_METADATA | COPYFILE_STAT | COPYFILE_NOFOLLOW) < 0) {
		r = -1;
		step = "meta";
	}
	if (r) {
		fprintf(stderr, "vcopy: %s: %s: %s\n", src, step, strerror(errno));
		nerr++;
		return;
	}
	nfiles++;
	fixowner(src, dst);
	fixmeta(src, dst);
}

static int cb(int what, int stage, copyfile_state_t st, const char *src, const char *dst, void *ctx)
{
	(void)st; (void)ctx;
	struct stat s;
	if (stage == COPYFILE_ERR) {
		fprintf(stderr, "vcopy: %s: %s\n", src ? src : "?", strerror(errno));
		nerr++;
		return COPYFILE_SKIP;
	}
	switch (what) {
	case COPYFILE_RECURSE_FILE:
		if (stage == COPYFILE_START) {
			if (excluded(src) || (!lstat(src, &s) && S_ISSOCK(s.st_mode))) {
				nskip++;
				return COPYFILE_SKIP;
			}
			if (!lstat(src, &s) && S_ISREG(s.st_mode)) {
				copyreg(src, dst);
				return COPYFILE_SKIP;
			}
		} else if (stage == COPYFILE_FINISH) {
			if (!lstat(src, &s) && S_ISLNK(s.st_mode))
				nlinks++;
			else
				nfiles++;
			fixowner(src, dst);
		}
		break;
	case COPYFILE_RECURSE_DIR:
		if (stage == COPYFILE_START && excluded(src)) {
			nskip++;
			return COPYFILE_SKIP;
		}
		if (stage == COPYFILE_START)
			ndirs++;
		break;
	case COPYFILE_RECURSE_DIR_CLEANUP:
		if (stage == COPYFILE_FINISH) {
			fixowner(src, dst);
			fixmeta(src, dst);
		}
		break;
	case COPYFILE_RECURSE_ERROR:
		fprintf(stderr, "vcopy: %s: %s\n", src ? src : "?", strerror(errno));
		nerr++;
		return COPYFILE_SKIP;
	}
	return COPYFILE_CONTINUE;
}

// Sorted, NUL-separated xattr names into buf; returns length or -1.
static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static ssize_t xnames(const char *p, char *out, size_t cap)
{
	char raw[8192], *v[512];
	ssize_t n = listxattr(p, raw, sizeof raw, XATTR_NOFOLLOW);
	if (n < 0)
		return -1;
	int k = 0;
	for (char *q = raw; q < raw + n && k < 512; q += strlen(q) + 1)
		v[k++] = q;
	qsort(v, k, sizeof *v, cmpstr);
	size_t o = 0;
	for (int i = 0; i < k; i++) {
		size_t l = strlen(v[i]);
		if (o + l + 2 > cap)
			break;
		memcpy(out + o, v[i], l);
		o += l;
		out[o++] = ' ';
	}
	out[o] = 0;
	return o;
}

static int hasacl(const char *p)
{
	acl_t a = acl_get_link_np(p, ACL_TYPE_EXTENDED);
	if (!a)
		return 0;
	acl_free(a);
	return 1;
}

static int list(const char *p)
{
	struct stat s;
	char x[8192];
	if (lstat(p, &s)) {
		fprintf(stderr, "vcopy: %s: %s\n", p, strerror(errno));
		return 1;
	}
	printf("%s\n  uid %u gid %u mode %o flags 0x%x acl %s\n", p, s.st_uid, s.st_gid, s.st_mode, s.st_flags,
	       hasacl(p) ? "yes" : "no");
	if (xnames(p, x, sizeof x) >= 0)
		printf("  xattrs: %s\n", x);
	return 0;
}

static int compare(const char *src, const char *dst)
{
	char *argv[] = {(char *)src, NULL}, dp[4096], xs[8192], xd[8192];
	long ndiff = 0, nseen = 0;
	size_t sl = strlen(src);
	FTS *f = fts_open(argv, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
	if (!f) {
		perror("fts_open");
		return 1;
	}
	FTSENT *e;
#define DIFF(...) do { if (ndiff++ < 200) printf(__VA_ARGS__); } while (0)
	while ((e = fts_read(f))) {
		if (e->fts_info == FTS_DP)
			continue;
		if (excluded(e->fts_path)) {
			if (e->fts_info == FTS_D)
				fts_set(f, e, FTS_SKIP);
			continue;
		}
		nseen++;
		snprintf(dp, sizeof dp, "%s%s", dst, e->fts_path + sl);
		struct stat d, *s = e->fts_statp;
		if (e->fts_info == FTS_NS || e->fts_info == FTS_ERR || e->fts_info == FTS_DNR) {
			DIFF("%s: unreadable in src: %s\n", e->fts_path, strerror(e->fts_errno));
			continue;
		}
		if (lstat(dp, &d)) {
			if (!S_ISSOCK(s->st_mode))
				DIFF("%s: missing\n", dp);
			continue;
		}
		if ((s->st_mode & S_IFMT) != (d.st_mode & S_IFMT))
			DIFF("%s: type %o vs %o\n", dp, s->st_mode & S_IFMT, d.st_mode & S_IFMT);
		if (s->st_uid != d.st_uid || s->st_gid != d.st_gid)
			DIFF("%s: owner %u:%u vs %u:%u\n", dp, s->st_uid, s->st_gid, d.st_uid, d.st_gid);
		if ((s->st_mode & 07777) != (d.st_mode & 07777))
			DIFF("%s: mode %o vs %o\n", dp, s->st_mode & 07777, d.st_mode & 07777);
		if (s->st_flags != d.st_flags)
			DIFF("%s: flags 0x%x vs 0x%x\n", dp, s->st_flags, d.st_flags);
		if (S_ISREG(s->st_mode) && s->st_size != d.st_size)
			DIFF("%s: size %lld vs %lld\n", dp, (long long)s->st_size, (long long)d.st_size);
		if (xnames(e->fts_path, xs, sizeof xs) >= 0 && xnames(dp, xd, sizeof xd) >= 0 && strcmp(xs, xd))
			DIFF("%s: xattrs [%s] vs [%s]\n", dp, xs, xd);
		if (hasacl(e->fts_path) != hasacl(dp))
			DIFF("%s: acl %d vs %d\n", dp, hasacl(e->fts_path), hasacl(dp));
	}
	fts_close(f);
	printf("compared %ld entries, %ld differences\n", nseen, ndiff);
	return ndiff > 0;
}

// -m: repair owner, ACL and flags of an existing copy without copying data.
static int metapass(const char *src, const char *dst)
{
	char *argv[] = {(char *)src, NULL}, dp[4096];
	size_t sl = strlen(src);
	FTS *f = fts_open(argv, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
	if (!f) {
		perror("fts_open");
		return 1;
	}
	FTSENT *e;
	struct stat d;
	while ((e = fts_read(f))) {
		if (e->fts_info == FTS_DP)
			continue;
		if (excluded(e->fts_path)) {
			if (e->fts_info == FTS_D)
				fts_set(f, e, FTS_SKIP);
			continue;
		}
		if (e->fts_info == FTS_NS || e->fts_info == FTS_ERR || e->fts_info == FTS_DNR)
			continue;
		if (S_ISSOCK(e->fts_statp->st_mode))
			continue;
		snprintf(dp, sizeof dp, "%s%s", dst, e->fts_path + sl);
		if (lstat(dp, &d))
			continue;
		fixowner(e->fts_path, dp);
		fixmeta(e->fts_path, dp);
	}
	fts_close(f);
	printf("fixed-acl %ld fixed-flags %ld errors %ld\n", nfacl, nfflags, nerr);
	return nerr > 0;
}

// Copy SRC to DST, or SRC's entries into DST when DST is an existing directory (contents-to-contents).
static void copytree(const char *src, const char *dst)
{
	struct stat d, s;
	copyfile_state_t st = copyfile_state_alloc();
	copyfile_state_set(st, COPYFILE_STATE_STATUS_CB, (const void *)cb);
	if (stat(dst, &d) || !S_ISDIR(d.st_mode)) {
		if (copyfile(src, dst, st, COPYFILE_ALL | COPYFILE_RECURSIVE | COPYFILE_NOFOLLOW) < 0) {
			fprintf(stderr, "vcopy: copyfile: %s\n", strerror(errno));
			nerr++;
		}
		copyfile_state_free(st);
		return;
	}
	DIR *dir = opendir(src);
	if (!dir) {
		fprintf(stderr, "vcopy: %s: %s\n", src, strerror(errno));
		nerr++;
		copyfile_state_free(st);
		return;
	}
	struct dirent *de;
	char sp[4096], dp[4096];
	while ((de = readdir(dir))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		snprintf(sp, sizeof sp, "%s/%s", src, de->d_name);
		snprintf(dp, sizeof dp, "%s/%s", dst, de->d_name);
		if (excluded(sp) || lstat(sp, &s) || S_ISSOCK(s.st_mode)) {
			nskip++;
			continue;
		}
		if (S_ISREG(s.st_mode)) {
			copyreg(sp, dp);
			continue;
		}
		if (S_ISDIR(s.st_mode))
			ndirs++;
		else if (S_ISLNK(s.st_mode))
			nlinks++;
		if (copyfile(sp, dp, st, COPYFILE_ALL | COPYFILE_RECURSIVE | COPYFILE_NOFOLLOW) < 0) {
			fprintf(stderr, "vcopy: copyfile %s: %s\n", sp, strerror(errno));
			nerr++;
		}
		fixowner(sp, dp);
		fixmeta(sp, dp);
	}
	closedir(dir);
	copyfile_state_free(st);
	fixowner(src, dst);
	fixmeta(src, dst);
}

int main(int argc, char **argv)
{
	int opt, mode = 0;
	while ((opt = getopt(argc, argv, "lcmx:")) != -1) {
		if (opt == 'l')
			mode = 'l';
		else if (opt == 'c')
			mode = 'c';
		else if (opt == 'm')
			mode = 'm';
		else if (opt == 'x' && nexcl < 64)
			excl[nexcl++] = optarg;
		else
			goto usage;
	}
	argc -= optind;
	argv += optind;
	// Allow -x after the positional args too (vcopy -c SRC DST -x EXCL).
	for (int i = 2; i < argc; i += 2) {
		if (strcmp(argv[i], "-x") || i + 1 >= argc || nexcl >= 64)
			goto usage;
		excl[nexcl++] = argv[i + 1];
	}
	if (argc > 2)
		argc = 2;
	if (mode == 'l' && argc == 1)
		return list(argv[0]);
	if (argc < 2)
		goto usage;
	if (mode == 'c')
		return compare(argv[0], argv[1]);
	if (mode == 'm')
		return metapass(argv[0], argv[1]);
	copytree(argv[0], argv[1]);
	printf("files %ld dirs %ld symlinks %ld skipped %ld errors %ld efault-retries %ld\n", nfiles, ndirs, nlinks, nskip,
	       nerr, nretry);
	return nerr > 0;
usage:
	fprintf(stderr, "usage: vcopy [-x EXCL]... SRC DST | vcopy -l PATH | vcopy -c|-m SRC DST [-x EXCL]...\n"
	                "  SRC DST is contents-to-contents (an existing DST gets SRC's entries, not SRC's basename)\n");
	return 2;
}
