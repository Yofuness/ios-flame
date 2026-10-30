/*
 * camd: camera stage f1, the rear IMX363 for clients without /dev/pmem (research/notes/camera-stage-f.md). Root,
 * foreground (a launchd job later). Interface: phone/camera/sunfish_camera.h (socket commands, shared frame slots).
 *   camd [--log FILE]      default /var/local/camd.log (cut to 0 past 1 MB); everything also the tools' output
 * Idle = the camera powered down. The first SUNFISH_CAMERA_START brings it up in the order of
 * tools/sunfish-cam-rx.sh (camclk pll2 on, pll0 on, camid --keep, camclk mclk 0 24, then camrx_lib clocks on, phy
 * on) and runs camrx's e1 stream ring (camrx_lib stream_run with hooks): camid --ctl streams the sensor and takes
 * exposure/gain lines on its stdin (group hold, between frames: one process owns the CCI). Each WM0 done slot is
 * read out of the carveout (/dev/pmem) by a copier thread into the next shared slot; while it reads, the ring
 * keeps the WM on the other slot (frames dropped, never torn). Down on STOP / last disconnect / SIGTERM: camid Q
 * (stream off), WM off, CSID down, IFE reset, SMMU park + restore (cap_down), rx stop, phy off, clocks off,
 * camclk off, camid power-down: the script's order. A failed bring-up or a fault (SMMU/bank fault, writes past
 * the ring) = state FAULT: no more starts until camd restarts. The safety rules are camrx's (camrx.c header).
 * cam36: a session with no or too few frames, or whose frames stopped, is no FAULT: its clients get -EIO or lose
 * their start and the next START retries. Off: CAM36_OFF=1 in camd's environment (also the rear start exposure on
 * the front, and see camrx_lib.h).
 * cam-f1: SENSOR a=2 picks the front IMX355 (mode 2, 1640x924, CSIPHY2, MCLK2, camid --front), a=1 the rear
 * (default). A switch while streaming stops the session (full power-down) and starts it again on the other sensor.
 * Off: CAMD_NO_FRONT=1 in camd's environment (SENSOR 2 -> -EINVAL, as before).
 * cam38: the frames' headers say the exposure a frame was really taken with (CAM38_LATENCY frames after camid's
 * read-back, not at once: AE saw old levels with new numbers and ran away); longer frames than the mode's, down to
 * 15 fps; one exposure command at camid at a time (a queue of them held up its Q for seconds). Off: CAM38_OFF=1 in
 * camd's environment.
 */
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../wifi/pmem.h"
#include "../camera/sunfish_camera.h"
#include "camrx_lib.h"

#define BIN		"/var/local/bin/"
#define TABLE		"/var/local/cam/imx363-mode8.txt"
#define TABLE_F		"/var/local/cam/imx355-mode2.txt"	/* cam-f1 */
#define LOG_MAX		(1 << 20)
#define MAXCL		8
/* IMX363 mode 8 (camera.md cam22): frame_length 1608 lines at 60.1 fps measured -> 10347 ns a line */
#define FPS_MILLI	60100u
#define LINE_NS		10347u
#define MARGIN		40u	/* camid EXP_MARGIN */
#define GAIN_UNITY	256u
#define GAIN_MAX	(8u * GAIN_UNITY)	/* camid GAIN_CODE_MAX 448 = 8x */
#define BLACK		64u	/* assumed (camera.md e3) */
#define BAYER		0u	/* RGGB, unverified (camera.md e3) */
/* cam-f1: IMX355 mode 2 (module file: line 1836, frame 2614 lines, pixel clock 144 MHz -> 12750 ns a line, 30 fps;
 * not measured yet), camid EXP_MARGIN_F 10, gain code 1024 - 1024/g (GAIN_BASE_F) */
#define FPS_MILLI_F	30000u
#define LINE_NS_F	12750u
#define MARGIN_F	10u
#define FL_F		2614
#define START_LINES_F	2604	/* cam36: the table's 0x0202/0x0203 */
#define FL_R		1608
#define FL_MAX_R	6443	/* cam38: 15 fps */
#define FL_MAX_F	5228
#define CAM38_LATENCY	3	/* frames from the last one done to the first taken with what was just written (not measured:
				 * the frame under way, the one whose exposure has begun, then this one) */
#define CAM38_QUEUE	16

static volatile int sensor_want = 1;	/* SENSOR command: 1 rear, 2 front */
static int front;			/* the running session's sensor (camrx_lib cam_select) */
static unsigned gain_base = 512;	/* Sony analog gain code base: 512 IMX363, 1024 IMX355 */

static struct sunfish_camera_shm *sh;
static uint8_t *shm_base;
static int logfd = -1;
static int fast;	/* B43: CAMRX_FAST=1 */
static volatile sig_atomic_t term;
static mach_timebase_info_data_t tb;

/* session (camera thread) <-> main thread */
static pthread_t sess_th;
static volatile int sess_running, want_stop, powered, sess_failed;
static volatile int exp_pending, exp_lines_req = -1, exp_code_req = -1, exp_fl_req = -1;
static int cam38_off, exp_in_flight;	/* cam38: an E at camid not answered yet */
static double exp_sent_at;
static volatile uint64_t last_done_seq;
static struct { uint64_t from; int fl, lines, code; } took[CAM38_QUEUE];	/* mu; what the frames from `from' on were taken with */
static unsigned took_head, took_tail;
static int hdr_fl, hdr_lines, hdr_code;	/* the copier's: the exposure of the frame in hand */
static volatile int cur_fl = 1608, cur_lines = -1, cur_code = -1;	/* camid's read-backs */
static int camid_in = -1, camid_out = -1;

/* the copier */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static volatile int copy_slot = -1;	/* ring slot being read out, -1 none */
static uint64_t job_seq, job_done_mach, job_sof_mach;

static double mono(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* B43: "+1.234 s " = time since the session's START (stage_t0), for the start-time breakdown */
static const char *since(char b[24])
{
	b[0] = 0;
	if (stage_t0 > 0)
		snprintf(b, 24, "+%.3f s ", mono() - stage_t0);
	return b;
}

static uint64_t ns_to_mach(double ns)
{
	return (uint64_t)(ns * tb.denom / tb.numer);
}

/* fork/exec a tool, its output into the log; waited for (30 s, then SIGTERM); 0 = exit 0 */
static int run(const char *const *av)
{
	char line[512];
	int n = 0;
	for (int i = 0; av[i] && n < (int)sizeof line - 2; i++)
		n += snprintf(line + n, sizeof line - n, "%s%s", i ? " " : "", av[i]);
	char t[24];
	printf("%scamd: run %s\n", since(t), line);
	fflush(stdout);
	pid_t p = fork();
	if (p < 0)
		return -1;
	if (!p) {
		execv(av[0], (char *const *)av);
		perror(av[0]);
		_exit(127);
	}
	int st = 0;
	for (int t = 0;; t++) {
		pid_t r = waitpid(p, &st, WNOHANG);
		if (r == p)
			break;
		if (r < 0)
			return -1;
		if (t == 3000) {
			printf("camd: %s still running after 30 s, SIGTERM\n", av[0]);
			kill(p, SIGTERM);
		}
		usleep(10000);
	}
	int rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128;
	printf("%scamd: -> exit %d\n", since(t), rc);
	return rc;
}

/* ---- stream hooks (called from stream_run in the session thread) ---- */

static int h_spawn(pid_t *pid)
{
	int ci[2], co[2];
	if (pipe(ci))
		return 1;
	if (pipe(co)) {
		close(ci[0]), close(ci[1]);
		return 1;
	}
	static const char *const avr[] = { BIN "camid", "--addr", "0x1a", "--keep", "--mclk-asis", "--stream", TABLE,
					   "--ctl", NULL };
	static const char *const avf[] = { BIN "camid", "--front", "--addr", "0x1a", "--keep", "--mclk-asis", "--stream",
					   TABLE_F, "--ctl", NULL };
	const char *const *av = front ? avf : avr;
	printf("camd: stream command: camid %s--addr 0x1a --keep --mclk-asis --stream %s --ctl\n", front ? "--front " : "",
	       front ? TABLE_F : TABLE);
	fflush(stdout);
	*pid = fork();
	if (*pid < 0)
		return 1;
	if (!*pid) {
		dup2(ci[0], 0);
		dup2(co[1], 3);
		for (int fd = 4; fd < 256; fd++)
			if (fd != 1 && fd != 2)
				close(fd);
		signal(SIGINT, SIG_IGN);	/* as camrx run_cmd: the sensor command stops the stream itself */
		execv(av[0], (char *const *)av);
		perror(av[0]);
		_exit(127);
	}
	close(ci[0]), close(co[1]);
	camid_in = ci[1], camid_out = co[0];
	fcntl(camid_out, F_SETFL, O_NONBLOCK);
	return 0;
}

static int h_busy(void)
{
	return __atomic_load_n(&copy_slot, __ATOMIC_ACQUIRE);
}

static void h_frame(int slot, uint64_t seq, double sof)
{
	uint64_t now = mach_absolute_time();
	last_done_seq = seq;	/* cam38 */
	if (__atomic_load_n(&copy_slot, __ATOMIC_ACQUIRE) >= 0) {
		sh->dropped++;
		return;
	}
	pthread_mutex_lock(&mu);
	job_seq = seq;
	job_done_mach = now;
	double ago = (mono() - sof) * 1e9;
	job_sof_mach = sof > 0 && ago > 0 ? now - ns_to_mach(ago) : 0;
	__atomic_store_n(&copy_slot, slot, __ATOMIC_RELEASE);
	pthread_cond_signal(&cv);
	pthread_mutex_unlock(&mu);
}

static void camid_replies(void)
{
	static char buf[256];
	static size_t n;
	ssize_t k = read(camid_out, buf + n, sizeof buf - 1 - n);
	if (k <= 0)
		return;
	n += (size_t)k;
	buf[n] = 0;
	char *nl;
	while ((nl = strchr(buf, '\n'))) {
		*nl = 0;
		int fl, l, c;
		if (buf[0] == 'E' || buf[0] == 'X')
			exp_in_flight = 0;	/* cam38 */
		if (sscanf(buf, "%*c %d %d %d", &fl, &l, &c) == 3 && (buf[0] == 'S' || buf[0] == 'E')) {
			cur_fl = fl, cur_lines = l, cur_code = c;
			pthread_mutex_lock(&mu);	/* cam38 */
			if (buf[0] == 'S') {
				took_head = took_tail = 0;
				hdr_fl = fl, hdr_lines = l, hdr_code = c;
			} else {
				if (took_tail - took_head == CAM38_QUEUE)
					took_head++;
				took[took_tail % CAM38_QUEUE].from = last_done_seq + CAM38_LATENCY;
				took[took_tail % CAM38_QUEUE].fl = fl, took[took_tail % CAM38_QUEUE].lines = l;
				took[took_tail % CAM38_QUEUE].code = c;
				took_tail++;
			}
			pthread_mutex_unlock(&mu);
			printf("camd: sensor %s: frame_length %d, exposure %d lines, gain code %d\n",
			       buf[0] == 'S' ? "streaming" : "exposure set", fl, l, c);
		} else {
			printf("camd: camid reply \"%s\"\n", buf);
		}
		n -= (size_t)(nl + 1 - buf);
		memmove(buf, nl + 1, n + 1);
	}
	if (n >= sizeof buf - 1)
		n = 0;
}

static int h_tick(void)
{
	camid_replies();
	/* cam38: the newest request once camid has answered the last (1 s: it lost one; on) */
	if (exp_in_flight && mono() - exp_sent_at > 1)
		exp_in_flight = 0;
	if (exp_pending && camid_in >= 0 && (cam38_off || !exp_in_flight)) {
		exp_pending = 0;
		if (cam38_off) {
			dprintf(camid_in, "E %d %d\n", exp_lines_req, exp_code_req);
		} else {
			dprintf(camid_in, "E %d %d %d\n", exp_lines_req, exp_code_req, exp_fl_req);
			exp_in_flight = 1, exp_sent_at = mono();
		}
	}
	return want_stop || term;
}

static void h_ended(void)
{
	if (camid_in >= 0) {
		dprintf(camid_in, "Q\n");
		close(camid_in);
		camid_in = -1;
	}
}

static const struct cam_stream_hooks hooks = { h_spawn, h_busy, h_frame, h_tick, h_ended };

/* ---- the copier ---- */

static void *copier(void *arg)
{
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&mu);
		while (__atomic_load_n(&copy_slot, __ATOMIC_ACQUIRE) < 0)
			pthread_cond_wait(&cv, &mu);
		int ring = copy_slot;
		uint64_t seq = job_seq, tdone = job_done_mach, tsof = job_sof_mach;
		pthread_mutex_unlock(&mu);
		uint32_t s = (sh->latest_slot + 1) % SUNFISH_CAMERA_SLOTS;
		uint8_t *slot = shm_base + sh->head_bytes + (size_t)s * sh->slot_bytes;
		struct sunfish_camera_slot *hd = (struct sunfish_camera_slot *)slot;
		__atomic_add_fetch(&hd->gen, 1, __ATOMIC_ACQ_REL);	/* odd: being written */
		double t0 = mono();
		uint64_t pa = CAP_CO_PA + (uint64_t)ring * PLACE_BYTES;
		int bad = 0;
		for (uint32_t o = 0; o < FRAME_BYTES && !bad; o += 65536)
			bad = pm_rd(pa + o, slot + SUNFISH_CAMERA_DATA_OFF + o, FRAME_BYTES - o < 65536 ? FRAME_BYTES - o : 65536);
		sh->copy_us = (uint32_t)((mono() - t0) * 1e6);
		struct redwood_camera_frame *f = &hd->frame;
		memset(f, 0, sizeof *f);
		memcpy(f->magic, REDWOOD_CAMERA_MAGIC, 8);
		f->header_bytes = sizeof *f;
		f->width = FRAME_W, f->height = FRAME_H, f->stride = FRAME_STRIDE, f->format = 1;
		f->bayer = BAYER, f->black = BLACK;
		f->sequence = seq, f->time = tdone;
		/* cam38: as this frame was taken, not as the sensor's registers are now */
		pthread_mutex_lock(&mu);
		while (took_head != took_tail && took[took_head % CAM38_QUEUE].from <= seq) {
			hdr_fl = took[took_head % CAM38_QUEUE].fl, hdr_lines = took[took_head % CAM38_QUEUE].lines;
			hdr_code = took[took_head % CAM38_QUEUE].code;
			took_head++;
		}
		int h_fl = cam38_off ? cur_fl : hdr_fl, h_lines = cam38_off ? cur_lines : hdr_lines, h_code = cam38_off ? cur_code : hdr_code;
		pthread_mutex_unlock(&mu);
		f->exposure_lines = h_lines < 0 ? 0 : (uint32_t)h_lines;
		f->line_ns = front ? LINE_NS_F : LINE_NS;
		f->gain = h_code < 0 || (unsigned)h_code >= gain_base ? GAIN_UNITY :
			  (uint32_t)(gain_base * GAIN_UNITY / (gain_base - (uint32_t)h_code));
		f->gain_unity = GAIN_UNITY, f->gain_max = GAIN_MAX;
		f->frame_lines = (uint32_t)h_fl;
		f->frame_lines_min = cam38_off ? (uint32_t)cur_fl : front ? FL_F : FL_R;
		f->frame_lines_max = cam38_off ? (uint32_t)cur_fl : front ? FL_MAX_F : FL_MAX_R;
		f->margin = front ? MARGIN_F : MARGIN, f->sensor = (uint32_t)cam->sensor, f->focus = REDWOOD_CAMERA_NO_FOCUS;
		f->reserved[REDWOOD_CAMERA_R_SOF_LO] = (uint32_t)tsof;
		f->reserved[REDWOOD_CAMERA_R_SOF_HI] = (uint32_t)(tsof >> 32);
		f->reserved[REDWOOD_CAMERA_R_FPS_MILLI] = front ? FPS_MILLI_F : FPS_MILLI;
		__atomic_add_fetch(&hd->gen, 1, __ATOMIC_ACQ_REL);	/* even: done */
		__atomic_store_n(&copy_slot, -1, __ATOMIC_RELEASE);	/* the ring may hand this slot to the WM again */
		if (bad) {
			printf("camd: pmem read of ring slot %d failed\n", ring);
			continue;
		}
		__atomic_store_n(&sh->latest_slot, s, __ATOMIC_RELEASE);
		__atomic_store_n(&sh->latest_seq, seq, __ATOMIC_RELEASE);
		sh->published++;
		if (sh->state == SUNFISH_CAMERA_STARTING) {
			char t[24];
			sh->state = SUNFISH_CAMERA_STREAMING;
			printf("%scamd: first frame published (seq %llu, exposure %u lines, gain code %d)\n", since(t),
			       (unsigned long long)seq, f->exposure_lines, cur_code);
		}
	}
	return NULL;
}

/* ---- session ---- */

static void power_down(void)
{
	if (!powered)
		return;
	powered = 0;
	printf("camd: == power down\n");
	if (!precheck()) {	/* camrx's commands need it too; the script's down runs them anyway */
		rx_stop();
		phy_off();
		clocks_off();
	}
	static const char *const off[] = { BIN "camclk", "off", NULL }, *const pd[] = { BIN "camid", "--addr", "0x1a", NULL },
				 *const pdf[] = { BIN "camid", "--front", "--addr", "0x1a", NULL };
	run(off);
	run(front ? pdf : pd);
	printf("camd: powered down\n");
	fflush(stdout);
}

static void *session(void *arg)
{
	(void)arg;
	static const char *const steps[][6] = {
		{ BIN "camclk", "pll2", "on", NULL }, { BIN "camclk", "pll0", "on", NULL },
		{ BIN "camid", "--addr", "0x1a", "--keep", NULL }, { BIN "camclk", "mclk", "0", "24", NULL },
	}, *const steps_f[][6] = {	/* cam-f1 */
		{ BIN "camclk", "pll2", "on", NULL }, { BIN "camclk", "pll0", "on", NULL },
		{ BIN "camid", "--front", "--addr", "0x1a", "--keep", NULL }, { BIN "camclk", "mclk", "2", "24", NULL },
	};
	int rc = 0, latch;
	const char *why = "bring-up failed";
	cam_select(front);	/* (front: set by the main loop before the thread, cam-f2) */
	gain_base = front ? 1024 : 512;
	sh->latest_seq = 0, sh->published = 0, sh->dropped = 0;
	sh->data_bytes = FRAME_BYTES;
	/* B43: the tables' own exposure (0x0202/0x0203 = 1592 lines, gain 0) until camid's "S" read-back, not -1 -> 0
	 * lines (isp_ae then started from 4 lines = black). cam36: the front table's is 2604. */
	int front_start = front && !cam36_off();
	cur_lines = front_start ? START_LINES_F : 1592, cur_code = 0;
	cur_fl = front ? FL_F : FL_R;
	pthread_mutex_lock(&mu);	/* cam38 */
	took_head = took_tail = 0;
	hdr_fl = cur_fl, hdr_lines = cur_lines, hdr_code = cur_code;
	pthread_mutex_unlock(&mu);
	exp_in_flight = 0, last_done_seq = 0, exp_fl_req = -1;
	stage_t0 = mono();
	char t[24];
	printf("%scamd: == session start, sensor %d (%s)\n", since(t), cam->sensor, cam->name);
	if (fast) {	/* B43: an indoor start exposure (~1500 lines, 2x gain; cam36: front 2604), sent on the first tick */
		exp_lines_req = front_start ? START_LINES_F : 1500, exp_code_req = (int)(gain_base / 2);
		exp_pending = 1;
	}
	powered = 1;
	for (unsigned i = 0; i < sizeof steps / sizeof steps[0] && !rc && !want_stop && !term; i++)
		rc = run(front ? steps_f[i] : steps[i]) != 0;
	if (!rc && !want_stop && !term)
		rc = precheck() || clocks_on() || phy_on(-1);
	printf("%scamd: clocks + phy %s\n", since(t), rc ? "failed" : "on");
	latch = rc;
	if (!rc && !want_stop && !term) {
		stop = 0;
		stream_hooks = &hooks;
		rc = stream_run(0, NULL, "camid --ctl (camd)");
		stream_hooks = NULL;
		/* cam36: FAULT = SMMU fault, a real stray write (old pixels can't cause it now) or a stream setup that
		 * failed before its loop (its SMMU state is not restored, no retry on that) */
		static const char *const whys[] = { "ok", "stream setup failed", "SMMU fault", "writes past the ring",
						    "no frames", "frames stopped", "frame not saved" };
		why = whys[stream_end];
		latch = cam36_off() ? rc : rc == 2 || stream_end == CAM_END_PAST_RING || stream_end == CAM_END_SETUP;
		if (rc && !sh->published && (want_stop || term) && (cam36_off() || !latch))
			rc = latch = 0;	/* stopped before a frame: "no completed frame" is no fault */
		if (camid_out >= 0)
			close(camid_out), camid_out = -1;
	}
	while (__atomic_load_n(&copy_slot, __ATOMIC_ACQUIRE) >= 0)
		usleep(1000);
	power_down();
	/* cam36: not started again by itself: the clients are told, the next START retries */
	sess_failed = !latch && !want_stop && !term && (rc || !sh->published) && !cam36_off();
	if (sess_failed && !rc)
		why = "too few frames";
	sh->state = latch ? SUNFISH_CAMERA_FAULT : SUNFISH_CAMERA_IDLE;
	printf("camd: == session end: %s%s, %llu frames published, %llu dropped\n",
	       latch ? "FAULT (no more starts): " : sess_failed ? "failed, next START retries: " : "ok",
	       latch || sess_failed ? why : "", (unsigned long long)sh->published, (unsigned long long)sh->dropped);
	fflush(stdout);
	sess_running = 0;
	return NULL;
}

/* ---- clients ---- */

static struct client {
	int fd, started, pending;
} cl[MAXCL];

static void reply(struct client *c, uint32_t cmd, int st)
{
	struct sunfish_camera_reply r = { cmd, st };
	if (send(c->fd, &r, sizeof r, MSG_DONTWAIT) != sizeof r)
		printf("camd: client fd %d: reply lost\n", c->fd);
}

static void drop(struct client *c)
{
	printf("camd: client fd %d gone\n", c->fd);
	close(c->fd);
	c->fd = -1, c->started = c->pending = 0;
}

static int read_full(int fd, void *b, size_t n)
{
	for (size_t o = 0; o < n;) {
		ssize_t k = read(fd, (char *)b + o, n - o);
		if (k <= 0)
			return -1;
		o += (size_t)k;
	}
	return 0;
}

static void command(struct client *c)
{
	struct redwood_camera_command m;
	if (read_full(c->fd, &m, sizeof m)) {
		drop(c);
		return;
	}
	static char junk[65536];
	for (uint32_t left = m.bytes; left;) {	/* KEEP's picture etc.: skipped */
		uint32_t k = left < sizeof junk ? left : sizeof junk;
		if (read_full(c->fd, junk, k)) {
			drop(c);
			return;
		}
		left -= k;
	}
	switch (m.command) {
	case SUNFISH_CAMERA_START:
		if (sh->state == SUNFISH_CAMERA_FAULT) {
			reply(c, m.command, -EIO);
		} else if (!c->started) {
			c->started = c->pending = 1;
			printf("camd: client fd %d: start\n", c->fd);
		} else {
			reply(c, m.command, 0);
		}
		break;
	case SUNFISH_CAMERA_STOP:
		if (c->pending)
			reply(c, SUNFISH_CAMERA_START, -ECANCELED);
		c->started = c->pending = 0;
		printf("camd: client fd %d: stop\n", c->fd);
		reply(c, m.command, 0);
		break;
	case REDWOOD_CAMERA_EXPOSURE: {
		uint32_t g = m.b;
		int code = -1;
		if (g) {
			g = g < GAIN_UNITY ? GAIN_UNITY : g > GAIN_MAX ? GAIN_MAX : g;
			code = (int)(gain_base - gain_base * GAIN_UNITY / g);	/* Sony 512 - 512/g (front 1024) */
		}
		exp_lines_req = m.a ? (int)(m.a > 0xffff ? 0xffff : m.a) : -1;
		exp_code_req = code;
		/* cam38: the frame that holds the exposure (c: as asked, 0: the shortest), within the mode's and 15 fps */
		if (m.a) {
			uint32_t fl = m.c ? m.c : m.a + (front ? MARGIN_F : MARGIN), least = front ? FL_F : FL_R,
				 most = front ? FL_MAX_F : FL_MAX_R;
			exp_fl_req = (int)(fl < least ? least : fl > most ? most : fl);
		} else {
			exp_fl_req = -1;
		}
		exp_pending = 1;	/* the session's next tick passes it to camid */
		printf("camd: client fd %d: exposure %u lines, gain %u -> code %d\n", c->fd, m.a, m.b, code);
		reply(c, m.command, 0);
		break;
	}
	case REDWOOD_CAMERA_SENSOR:
		if (m.a == 2 && getenv("CAMD_NO_FRONT") && !strcmp(getenv("CAMD_NO_FRONT"), "1")) {
			reply(c, m.command, -EINVAL);
			break;
		}
		if (m.a == 1 || m.a == 2) {	/* cam-f1: the main loop restarts a running session on the other one */
			if (sensor_want != (int)m.a)
				printf("camd: client fd %d: sensor %u\n", c->fd, m.a);
			sensor_want = (int)m.a;
			reply(c, m.command, 0);
		} else {
			reply(c, m.command, -EINVAL);
		}
		break;
	case REDWOOD_CAMERA_FOCUS:
	case REDWOOD_CAMERA_FLASH:
	case REDWOOD_CAMERA_KEEP:
		reply(c, m.command, 0);
		break;
	default:
		reply(c, m.command, -EINVAL);
	}
}

static void on_term(int s)
{
	(void)s;
	term = 1;
}

static int shm_create(void)
{
	size_t len = SUNFISH_CAMERA_HEAD + (size_t)SUNFISH_CAMERA_SLOTS * SUNFISH_CAMERA_SLOT_BYTES;
	_Static_assert(SUNFISH_CAMERA_DATA_OFF >= sizeof(struct sunfish_camera_slot), "slot header");
	_Static_assert(SUNFISH_CAMERA_DATA_OFF + FRAME_BYTES_MAX <= SUNFISH_CAMERA_SLOT_BYTES, "slot size");
	_Static_assert(sizeof(struct sunfish_camera_shm) <= SUNFISH_CAMERA_HEAD, "shm head");
	unlink(SUNFISH_CAMERA_SHM);
	int fd = open(SUNFISH_CAMERA_SHM, O_RDWR | O_CREAT | O_TRUNC, 0666);
	if (fd < 0 || fchmod(fd, 0666) || ftruncate(fd, (off_t)len)) {
		perror(SUNFISH_CAMERA_SHM);
		return 1;
	}
	shm_base = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (shm_base == MAP_FAILED) {
		perror("camd: mmap");
		return 1;
	}
	sh = (struct sunfish_camera_shm *)shm_base;
	memset(sh, 0, sizeof *sh);
	sh->version = 1;
	sh->head_bytes = SUNFISH_CAMERA_HEAD;
	sh->slots = SUNFISH_CAMERA_SLOTS, sh->slot_bytes = SUNFISH_CAMERA_SLOT_BYTES;
	sh->data_offset = SUNFISH_CAMERA_DATA_OFF, sh->data_bytes = FRAME_BYTES;
	sh->latest_slot = SUNFISH_CAMERA_SLOTS - 1;
	sh->pid = (uint32_t)getpid();
	__atomic_thread_fence(__ATOMIC_RELEASE);
	memcpy(sh->magic, SUNFISH_CAMERA_SHM_MAGIC, 8);
	printf("camd: %s: %zu B, %d slots of %u (data at +%u, %u B: 2016x1512 RAW10 rear, the extra lines cut off)\n",
	       SUNFISH_CAMERA_SHM, len, SUNFISH_CAMERA_SLOTS, SUNFISH_CAMERA_SLOT_BYTES, SUNFISH_CAMERA_DATA_OFF, FRAME_BYTES);
	return 0;
}

static int sock_create(void)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof a.sun_path, "%s", SUNFISH_CAMERA_SOCK);
	unlink(SUNFISH_CAMERA_SOCK);
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) || chmod(SUNFISH_CAMERA_SOCK, 0666) || listen(s, 4)) {
		perror(SUNFISH_CAMERA_SOCK);
		return -1;
	}
	return s;
}

int main(int argc, char **argv)
{
	const char *logp = "/var/local/camd.log";
	if (argc == 3 && !strcmp(argv[1], "--log"))
		logp = argv[2];
	else if (argc != 1) {
		fprintf(stderr, "usage: camd [--log FILE]\n");
		return 2;
	}
	logfd = open(logp, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (logfd < 0) {
		perror(logp);
		return 1;
	}
	dup2(logfd, 1);
	dup2(logfd, 2);
	setvbuf(stdout, NULL, _IOLBF, 0);
	/* B43: camrx_lib's stage() without its 300 ms bisect sleeps and the carveout zeroed once per process;
	 * CAMRX_FAST=0 in camd's environment keeps the old behaviour (also no start exposure) */
	setenv("CAMRX_FAST", "1", 0);
	fast = !strcmp(getenv("CAMRX_FAST"), "1");
	cam38_off = getenv("CAM38_OFF") && !strcmp(getenv("CAM38_OFF"), "1");
	printf("camd: start, pid %d, CAMRX_FAST %d\n", getpid(), fast);
	mach_timebase_info(&tb);
	pm_init();
	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	atexit(power_down);	/* also after camrx_lib's exit(1) (BLOCKED write): runs after its cap_down */
	int ls;
	if (shm_create() || (ls = sock_create()) < 0)
		return 1;
	for (int i = 0; i < MAXCL; i++)
		cl[i].fd = -1;
	pthread_t ct;
	if (pthread_create(&ct, NULL, copier, NULL)) {
		printf("camd: no copier thread\n");
		return 1;
	}
	int joined = 1;
	while (!term || sess_running) {
		struct pollfd p[MAXCL + 1];
		int n = 0, idx[MAXCL];
		p[n].fd = ls, p[n].events = POLLIN, p[n++].revents = 0;
		for (int i = 0; i < MAXCL; i++)
			if (cl[i].fd >= 0)
				idx[n - 1] = i, p[n].fd = cl[i].fd, p[n].events = POLLIN, p[n++].revents = 0;
		if (poll(p, (nfds_t)n, 100) > 0) {
			if (p[0].revents & POLLIN) {
				int fd = accept(ls, NULL, NULL), k;
				for (k = 0; fd >= 0 && k < MAXCL && cl[k].fd >= 0; k++)
					;
				if (fd >= 0 && k == MAXCL) {
					printf("camd: too many clients\n");
					close(fd);
				} else if (fd >= 0) {
					cl[k].fd = fd, cl[k].started = cl[k].pending = 0;
					printf("camd: client fd %d connected\n", fd);
				}
			}
			for (int j = 1; j < n; j++)
				if (p[j].revents & (POLLIN | POLLHUP | POLLERR) && cl[idx[j - 1]].fd >= 0)
					command(&cl[idx[j - 1]]);
		}
		if (sess_failed && !sess_running) {	/* cam36 */
			sess_failed = 0;
			for (int i = 0; i < MAXCL; i++) {
				if (cl[i].fd >= 0 && cl[i].pending)
					reply(&cl[i], SUNFISH_CAMERA_START, -EIO);
				cl[i].started = cl[i].pending = 0;
			}
		}
		int want = 0;
		for (int i = 0; i < MAXCL; i++)
			want |= cl[i].fd >= 0 && cl[i].started;
		want = want && !term && sh->state != SUNFISH_CAMERA_FAULT;
		if (!sess_running && !joined) {
			pthread_join(sess_th, NULL);
			joined = 1;
		}
		if (want && !sess_running && joined) {
			want_stop = 0;
			/* cam-f2: the sensor picked here, not in the thread: else the check below saw the old one and
			 * stopped every new front session at once */
			front = sensor_want == 2;
			sh->state = SUNFISH_CAMERA_STARTING;
			sess_running = 1;
			if (pthread_create(&sess_th, NULL, session, NULL)) {
				sess_running = 0;
				sh->state = SUNFISH_CAMERA_FAULT;
			} else {
				joined = 0;
			}
		}
		if (want && sess_running && !want_stop && (sensor_want == 2) != front) {	/* cam-f1 */
			printf("camd: sensor %d asked: stopping, then starting it\n", sensor_want);
			want_stop = 1;
			sh->state = SUNFISH_CAMERA_STOPPING;
		}
		if (!want && sess_running && !want_stop) {
			printf("camd: no client wants the camera: stopping\n");
			want_stop = 1;
			sh->state = SUNFISH_CAMERA_STOPPING;
		}
		for (int i = 0; i < MAXCL; i++) {	/* START's answer: once frames flow, or the failure */
			if (cl[i].fd < 0 || !cl[i].pending)
				continue;
			if (sh->state == SUNFISH_CAMERA_STREAMING)
				cl[i].pending = 0, reply(&cl[i], SUNFISH_CAMERA_START, 0);
			else if (sh->state == SUNFISH_CAMERA_FAULT || term)
				cl[i].pending = cl[i].started = 0, reply(&cl[i], SUNFISH_CAMERA_START, -EIO);
		}
		struct stat st;
		if (!fstat(logfd, &st) && st.st_size > LOG_MAX && !ftruncate(logfd, 0))
			printf("camd: log cut at %d B\n", LOG_MAX);
	}
	if (!joined)
		pthread_join(sess_th, NULL);
	printf("camd: exit (signal)\n");
	unlink(SUNFISH_CAMERA_SOCK);
	return 1;	/* not "successful": the launchd job (KeepAlive SuccessfulExit false) starts camd again after a kill */
}
