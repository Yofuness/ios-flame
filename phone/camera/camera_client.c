/*
 * camera_client: camd's socket + shared slots behind /dev/redwood-camera's calls (see camera_client.h). Ours.
 */
#include <errno.h>
#include <fcntl.h>
#include <os/log.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "camera_client.h"
#include "sunfish_camera.h"

#define LOG_MAX		(1u << 20)	/* camdev.log starts over past this */
#define START_WAIT_MS	30000		/* camd's bring-up took 6.7 s in cam23 */

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

void
camera_client_log(const char *line)
{
	char out[480];
	int n = snprintf(out, sizeof(out), "sunfish-cam: %s", line);

	if (n < 0) {
		return;
	}
	if (n > (int)sizeof(out) - 2) {
		n = (int)sizeof(out) - 2;
	}
	if (n == 0 || out[n - 1] != '\n') {
		out[n++] = '\n';
		out[n] = 0;
	}
	os_log(OS_LOG_DEFAULT, "%{public}s", out);
	pthread_mutex_lock(&log_lock);
	int fd = open(CAMERA_CLIENT_LOG, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
	if (fd >= 0) {
		struct stat st;
		if (fstat(fd, &st) == 0 && st.st_size > LOG_MAX) {
			(void)ftruncate(fd, 0);
		}
		(void)write(fd, out, (size_t)n);
		close(fd);
	}
	pthread_mutex_unlock(&log_lock);
}

static void
logf_(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void
logf_(const char *format, ...)
{
	char line[400];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	camera_client_log(line);
}

bool
camera_client_enabled(void)
{
	static int enabled = -1;

	if (enabled < 0) {
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		bool file = access(CAMERA_CLIENT_GATE, F_OK) == 0;
		bool arg = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "sweetcamdev=1") != NULL;

		enabled = file || arg;
		logf_("gate: %s (%s %s, boot-arg sweetcamdev=1 %s)", enabled ? "camera on" : "no camera, audio only",
		    CAMERA_CLIENT_GATE, file ? "there" : "absent", arg ? "set" : "not set");
	}
	return enabled;
}

/* the one open handle's state (camera_device.c's device_owner lets one worker have the camera at a time) */
static struct {
	int sock;
	const uint8_t *base;
	size_t length;
	uint64_t last_seq;
	unsigned bad_replies;
} c = { .sock = -1 };

static double
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* replies camd has sent, read without waiting: -1 when the socket is gone */
static int
drain(void)
{
	struct sunfish_camera_reply r;
	ssize_t n;

	while ((n = recv(c.sock, &r, sizeof(r), MSG_DONTWAIT)) == (ssize_t)sizeof(r)) {
		if (r.status != 0 && c.bad_replies++ < 20) {
			logf_("camd: command %#x -> %d", r.command, r.status);
		}
	}
	return n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) ? -1 : 0;
}

static void
drop(void)
{
	if (c.base != NULL) {
		munmap((void *)c.base, c.length);
	}
	if (c.sock >= 0) {
		close(c.sock);
	}
	c.base = NULL;
	c.sock = -1;
}

int
camera_client_open(void)
{
	if (c.sock >= 0) {
		errno = EBUSY;
		return -1;
	}
	int fd = open(SUNFISH_CAMERA_SHM, O_RDONLY | O_CLOEXEC);
	struct stat st;

	if (fd < 0 || fstat(fd, &st) != 0) {
		int e = errno;
		logf_("open: %s: %s (camd not running?)", SUNFISH_CAMERA_SHM, strerror(e));
		if (fd >= 0) {
			close(fd);
		}
		errno = e;
		return -1;
	}
	void *base = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (base == MAP_FAILED) {
		logf_("open: mmap: %s", strerror(errno));
		return -1;
	}
	c.base = base;
	c.length = (size_t)st.st_size;
	const struct sunfish_camera_shm *sh = base;
	if (c.length < sizeof(*sh) || memcmp(sh->magic, SUNFISH_CAMERA_SHM_MAGIC, 8) != 0 || sh->version != 1 ||
	    (uint64_t)sh->head_bytes + (uint64_t)sh->slots * sh->slot_bytes > c.length ||
	    (uint64_t)sh->data_offset + sh->data_bytes > sh->slot_bytes || sh->data_offset < sizeof(struct sunfish_camera_slot)) {
		logf_("open: bad shm header");
		drop();
		errno = EIO;
		return -1;
	}
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", SUNFISH_CAMERA_SOCK);
	if ((c.sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0 || connect(c.sock, (struct sockaddr *)&a, sizeof(a)) != 0) {
		int e = errno;
		logf_("open: %s: %s", SUNFISH_CAMERA_SOCK, strerror(e));
		drop();
		errno = e;
		return -1;
	}
	int one = 1;
	setsockopt(c.sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
	struct redwood_camera_command m = { SUNFISH_CAMERA_START, 0, 0, 0, 0, 0 };
	struct sunfish_camera_reply r = { 0, -ETIMEDOUT };
	double t0 = now_ms();
	bool got = false;

	if (write(c.sock, &m, sizeof(m)) == (ssize_t)sizeof(m)) {
		while (!got && now_ms() - t0 < START_WAIT_MS) {
			struct pollfd p = { c.sock, POLLIN, 0 };
			if (poll(&p, 1, 500) <= 0) {
				continue;
			}
			if (read(c.sock, &r, sizeof(r)) != (ssize_t)sizeof(r)) {
				r.status = -EPIPE;
				break;
			}
			got = r.command == SUNFISH_CAMERA_START;
		}
	}
	logf_("open: START -> %d after %.0f ms (camd pid %u, state %u, %u slots of %u B)", r.status, now_ms() - t0, sh->pid,
	    sh->state, sh->slots, sh->slot_bytes);
	if (!got || r.status != 0) {
		drop();
		errno = r.status == -EIO ? EIO : r.status ? -r.status : ETIMEDOUT;
		return -1;
	}
	c.last_seq = __atomic_load_n(&sh->latest_seq, __ATOMIC_ACQUIRE);	/* (only frames from now on) */
	c.bad_replies = 0;
	return c.sock;
}

ssize_t
camera_client_read(int handle, void *buffer, size_t bytes)
{
	if (handle < 0 || handle != c.sock) {
		errno = EBADF;
		return -1;
	}
	const struct sunfish_camera_shm *sh = (const void *)c.base;
	struct redwood_camera_frame f;
	const size_t hb = sizeof(f);
	double t0 = now_ms();

	for (;;) {
		uint64_t seq;

		while ((seq = __atomic_load_n(&sh->latest_seq, __ATOMIC_ACQUIRE)) == c.last_seq) {
			if (drain() < 0 || sh->state == SUNFISH_CAMERA_FAULT) {
				errno = EIO;
				return -1;
			}
			if (now_ms() - t0 >= 1000) {
				errno = EAGAIN;		/* (as the kernel's: at most a second) */
				return -1;
			}
			usleep(1000);
		}
		uint32_t k = __atomic_load_n(&sh->latest_slot, __ATOMIC_ACQUIRE);
		if (k >= sh->slots) {
			errno = EIO;
			return -1;
		}
		const uint8_t *slot = c.base + sh->head_bytes + (size_t)k * sh->slot_bytes;
		const struct sunfish_camera_slot *hd = (const void *)slot;
		uint32_t g = __atomic_load_n(&hd->gen, __ATOMIC_ACQUIRE);
		size_t data = 0;

		memcpy(&f, &hd->frame, hb);
		if (bytes > hb) {
			data = bytes - hb < sh->data_bytes ? bytes - hb : sh->data_bytes;
			memcpy((uint8_t *)buffer + hb, slot + sh->data_offset, data);
		}
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if ((g & 1) || __atomic_load_n(&hd->gen, __ATOMIC_ACQUIRE) != g || f.sequence != seq) {
			continue;	/* camd rewrote it while we copied: the newer one */
		}
		c.last_seq = seq;
		f.header_bytes = (uint32_t)hb;	/* (the bytes follow the header here) */
		memcpy(buffer, &f, bytes < hb ? bytes : hb);
		return (ssize_t)((bytes < hb ? bytes : hb) + data);
	}
}

ssize_t
camera_client_write(int handle, const void *buffer, size_t bytes)
{
	if (handle < 0 || handle != c.sock) {
		errno = EBADF;
		return -1;
	}
	if (drain() < 0) {
		errno = EIO;
		return -1;
	}
	return send(c.sock, buffer, bytes, 0);
}

int
camera_client_close(int handle)
{
	if (handle < 0 || handle != c.sock) {
		errno = EBADF;
		return -1;
	}
	struct redwood_camera_command m = { SUNFISH_CAMERA_STOP, 0, 0, 0, 0, 0 };

	(void)send(c.sock, &m, sizeof(m), 0);
	logf_("close: STOP after seq %llu", (unsigned long long)c.last_seq);
	drop();
	return 0;
}
