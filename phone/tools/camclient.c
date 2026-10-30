/*
 * camclient: camera f1 test client for camd (phone/camera/sunfish_camera.h), no mediaserverd.
 *   camclient [-n FRAMES] [-o FILE] [-e LINES GAIN]
 * Connects, START (waits for its reply: frames flowing), optionally EXPOSURE (GAIN 256 = 1x), reads FRAMES (default
 * 120) new frames from the shared slots (polls latest_seq every 1 ms, 1 s timeout a frame), prints seq, the header
 * and the mean byte for the first and every 30th, then fps, seq gaps and torn copies; -o: the last frame as a
 * camisp input (bare 2016x1512 RAW10, stride 2520); STOP.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "../camera/sunfish_camera.h"

static double mono(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int cmd(int s, uint32_t c, uint32_t a, uint32_t b, double timeout)
{
	struct redwood_camera_command m = { c, 0, a, b, 0, 0 };
	if (write(s, &m, sizeof m) != sizeof m)
		return -EPIPE;
	struct timeval tv = { (time_t)timeout, 0 };
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	struct sunfish_camera_reply r;
	for (;;) {
		if (read(s, &r, sizeof r) != sizeof r)
			return -ETIMEDOUT;
		if (r.command == c)
			return r.status;
		printf("camclient: reply to %#x: %d (skipped)\n", r.command, r.status);
	}
}

int main(int argc, char **argv)
{
	int frames = 120, el = -1, eg = -1;
	const char *out = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-n") && i + 1 < argc)
			frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-o") && i + 1 < argc)
			out = argv[++i];
		else if (!strcmp(argv[i], "-e") && i + 2 < argc)
			el = atoi(argv[i + 1]), eg = atoi(argv[i + 2]), i += 2;
		else {
			fprintf(stderr, "usage: camclient [-n FRAMES] [-o FILE] [-e LINES GAIN]\n");
			return 2;
		}
	}
	int fd = open(SUNFISH_CAMERA_SHM, O_RDONLY);
	struct stat st;
	if (fd < 0 || fstat(fd, &st)) {
		perror(SUNFISH_CAMERA_SHM);
		return 1;
	}
	const uint8_t *base = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (base == MAP_FAILED) {
		perror("camclient: mmap");
		return 1;
	}
	const struct sunfish_camera_shm *sh = (const void *)base;
	if (memcmp(sh->magic, SUNFISH_CAMERA_SHM_MAGIC, 8) || sh->version != 1 ||
	    (uint64_t)sh->head_bytes + (uint64_t)sh->slots * sh->slot_bytes > (uint64_t)st.st_size) {
		printf("camclient: bad shm header\n");
		return 1;
	}
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof a.sun_path, "%s", SUNFISH_CAMERA_SOCK);
	if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof a)) {
		perror(SUNFISH_CAMERA_SOCK);
		return 1;
	}
	double t0 = mono();
	int r = cmd(s, SUNFISH_CAMERA_START, 0, 0, 60);
	printf("camclient: START -> %d after %.2f s (camd state %u)\n", r, mono() - t0, sh->state);
	if (r)
		return 1;
	if (el >= 0)
		printf("camclient: EXPOSURE %d lines gain %d -> %d\n", el, eg, cmd(s, REDWOOD_CAMERA_EXPOSURE, (uint32_t)el, (uint32_t)eg, 5));
	uint8_t *buf = malloc(sh->data_bytes);
	struct redwood_camera_frame f;
	uint64_t last = 0, first = 0, gaps = 0, torn = 0, timeouts = 0;
	double tf = 0, tl = 0;
	int got = 0, rc = 0;
	while (got < frames) {
		double w = mono();
		uint64_t seq;
		while ((seq = __atomic_load_n(&sh->latest_seq, __ATOMIC_ACQUIRE)) == last && mono() - w < 1)
			usleep(1000);
		if (seq == last) {
			printf("camclient: no new frame in 1 s (state %u)\n", sh->state);
			if (++timeouts == 3) {
				rc = 1;
				break;
			}
			continue;
		}
		uint32_t k = __atomic_load_n(&sh->latest_slot, __ATOMIC_ACQUIRE);
		const uint8_t *slot = base + sh->head_bytes + (size_t)k * sh->slot_bytes;
		const struct sunfish_camera_slot *hd = (const void *)slot;
		uint32_t g = __atomic_load_n(&hd->gen, __ATOMIC_ACQUIRE);
		memcpy(&f, &hd->frame, sizeof f);
		memcpy(buf, slot + sh->data_offset, sh->data_bytes);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if ((g & 1) || __atomic_load_n(&hd->gen, __ATOMIC_ACQUIRE) != g || f.sequence != seq) {
			torn++;
			continue;
		}
		double now = mono();
		if (!got)
			first = seq, tf = now;
		else
			gaps += seq - last - 1;
		last = seq, tl = now, got++;
		if (got == 1 || got % 30 == 0 || got == frames) {
			uint64_t sum = 0;
			for (uint32_t i = 0; i < sh->data_bytes; i++)
				sum += buf[i];
			printf("camclient: #%d seq %llu %.8s %ux%u stride %u fmt %u bayer %u black %u exp %u lines (%.2f ms) gain "
			       "%u/%u max %u fl %u fps %.3f copy %u us; mean byte %.2f\n", got, (unsigned long long)seq, f.magic,
			       f.width, f.height, f.stride, f.format, f.bayer, f.black, f.exposure_lines,
			       f.exposure_lines * (double)f.line_ns / 1e6, f.gain, f.gain_unity, f.gain_max, f.frame_lines,
			       f.reserved[REDWOOD_CAMERA_R_FPS_MILLI] / 1000.0, sh->copy_us, (double)sum / sh->data_bytes);
		}
	}
	double span = tl - tf;
	printf("camclient: %d frames, seq %llu..%llu, %.2f fps read, %llu seq gaps, %llu torn copies, camd published %llu "
	       "dropped %llu\n", got, (unsigned long long)first, (unsigned long long)last, got > 1 && span > 0 ? (got - 1) / span : 0.0,
	       (unsigned long long)gaps, (unsigned long long)torn, (unsigned long long)sh->published,
	       (unsigned long long)sh->dropped);
	if (out && got) {
		FILE *o = fopen(out, "wb");
		if (!o || fwrite(buf, 1, sh->data_bytes, o) != sh->data_bytes || fclose(o)) {
			perror(out);
			rc = 1;
		} else {
			printf("camclient: last frame (seq %llu) -> %s\n", (unsigned long long)last, out);
		}
	}
	printf("camclient: STOP -> %d\n", cmd(s, SUNFISH_CAMERA_STOP, 0, 0, 5));
	close(s);
	return rc;
}
