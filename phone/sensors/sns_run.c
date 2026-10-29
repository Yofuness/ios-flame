/*
 * sns1: see sns_run.h. The list/stream/motion logic follows ios-redfin's sensd.c (cmd_list, cmd_stream and the
 * --daemon run loop with its MOTION_GET_WANT follow; MIT, Copyright (c) 2026 dipa-studio); the transport is
 * adspboot's QRTR through sns_link.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "sensor.h"
#include "sns_run.h"

enum { M_NONE, M_LIST, M_STREAM, M_MOTION };
static int mode, secs, delay_s = 10, dump, done, tries;
static float rate;
static const char *stream_type, *motion_dev = MOTION_DEV;
static uint32_t svc_node, svc_port;
static int have_svc;
static long svc_ms, t0, next_try, last_print, last_want;
static int motion_fd = -1, orient_clients_seen;
static uint64_t written, write_errors;
static struct sensor ss[8];
static int nss;

#define ORIENT_HZ 15.0f		/* sensd: accel rate when it runs for orientation only */
#define MAX_TRIES 5		/* lookups that found nothing: again every 10 s (the sensors PD may still load) */

static void out(void *ctx, struct sensor *s, const struct motion_sample *m)
{
	(void)ctx;
	if (mode != M_MOTION || motion_fd < 0)
		return;
	if (s->orient_only && m->type != MOTION_TYPE_ORIENTATION)
		return;
	if (write(motion_fd, m, sizeof(*m)) == (ssize_t)sizeof(*m))
		written++;
	else if (write_errors++ < 5)
		printf("sns: %s write: %s\n", motion_dev, strerror(errno));
	if (m->type == MOTION_TYPE_ORIENTATION)
		printf("sns: orientation %.0f posted\n", m->v[0]);
}

static struct sensor_env env = { .out = out };

int sns_opt(int argc, char **argv, int *i)
{
	const char *a = argv[*i];
	int val = *i + 1 < argc;
	if (!strcmp(a, "--sns-list"))
		mode = M_LIST;
	else if (!strcmp(a, "--sns-motion"))
		mode = M_MOTION;
	else if (!strcmp(a, "--sns-dump"))
		dump = 1;
	else if (!strncmp(a, "--sns-", 6) && !val)
		return -1;
	else if (!strcmp(a, "--sns-stream")) {
		mode = M_STREAM;
		stream_type = argv[++*i];
	} else if (!strcmp(a, "--sns-rate"))
		rate = (float)atof(argv[++*i]);
	else if (!strcmp(a, "--sns-secs"))
		secs = atoi(argv[++*i]);
	else if (!strcmp(a, "--sns-delay"))
		delay_s = atoi(argv[++*i]);
	else if (!strcmp(a, "--sns-motion-dev"))
		motion_dev = argv[++*i];
	else
		return 0;
	return 1;
}

int sns_wanted(void)
{
	return mode != M_NONE;
}

void sns_service(uint32_t node, uint32_t port)
{
	if (have_svc && node == svc_node && port == svc_port)
		return;
	svc_node = node;
	svc_port = port;
	have_svc = 1;
	svc_ms = sensor_now_ms();
	if (mode)
		printf("sns: SNS service %u at %u:%x; starting in %d s\n", SNS_QMI_SERVICE, node, port, delay_s);
}

static int setup(struct sensor *s, const struct sensor_kind *k)
{
	sensor_init(s, k, -1, svc_node, svc_port);
	if (sensor_lookup(&env, s))
		return -1;
	if (sensor_get_attrs(&env, s))
		printf("sns: %s: no attributes\n", k->alias);
	else
		sensor_print_attrs(s);
	return 0;
}

/* One try of the mode's setup; 0 = running/done, -1 = nothing found. */
static int start(void)
{
	nss = 0;
	if (mode == M_LIST) {
		for (int i = 0; i < sensor_nkinds; i++)
			if (setup(&ss[nss], &sensor_kinds[i]) == 0)
				nss++;
		printf("sns: list: %d of %d sensor types found\n", nss, sensor_nkinds);
		if (nss)
			done = 1;
		return nss ? 0 : -1;
	}
	if (mode == M_STREAM) {
		const struct sensor_kind *k = sensor_kind_find(stream_type);
		if (!k) {
			printf("sns: unknown sensor \"%s\"\n", stream_type);
			done = 1;
			return 0;
		}
		if (setup(&ss[0], k))
			return -1;
		if (sensor_start(&env, &ss[0], rate)) {
			done = 1;
			return 0;
		}
		nss = 1;
		printf("sns: %s streaming at %.1f Hz%s\n", k->alias, ss[0].hz, k->on_change ? " (on change)" : "");
		return 0;
	}
	motion_fd = open(motion_dev, O_RDWR | O_NONBLOCK);
	if (motion_fd < 0)
		printf("sns: %s: %s (samples not posted)\n", motion_dev, strerror(errno));
	for (int i = 0; i < sensor_nkinds; i++)
		if (setup(&ss[nss], &sensor_kinds[i]) == 0)
			nss++;
	printf("sns: motion: %d sensors\n", nss);
	env.orient = 1;
	last_want = 0;
	return nss ? 0 : -1;
}

static void stop_all(void)
{
	for (int i = 0; i < nss; i++)
		sensor_stop(&env, &ss[i]);
}

/* sensd's daemon follow: start/stop/re-rate each sensor from what iOS asked the kernel for. */
static int follow(void)
{
	struct motion_want w;
	int have = motion_fd >= 0 && ioctl(motion_fd, MOTION_GET_WANT, &w) == 0;
	int orient_want = have && (w.present & (1u << MOTION_ORIENT)) && w.clients[MOTION_ORIENT] > 0;
	if (orient_want != orient_clients_seen) {
		if (orient_want)
			env.orient_cur = env.orient_cand = 0;	/* a new client: post the current orientation again */
		printf("sns: orientation %s\n", orient_want ? "has clients: on" : "has no clients: off");
	}
	orient_clients_seen = orient_want;
	for (int i = 0; i < nss; i++) {
		struct sensor *s = &ss[i];
		int idx = s->k->motion_index, on = 1, orient_only = 0;
		float hz = s->k->default_hz;
		if (have && w.interval_set[idx]) {
			on = w.interval_us[idx] != 0;
			if (on && !s->k->on_change) {
				hz = 1e6f / (float)w.interval_us[idx];
				hz = hz < 1 ? 1 : (hz > 200 ? 200 : hz);
			}
		}
		if (!on && orient_want && idx == MOTION_ACCEL) {
			on = orient_only = 1;
			hz = ORIENT_HZ;
		}
		s->orient_only = orient_only;
		if (on && (!s->active || (!s->k->on_change && (s->hz < hz * 0.9f || s->hz > hz * 1.1f)))) {
			printf("sns: %s: start%s %.1f Hz\n", s->k->alias, s->active ? " (new rate)" : "",
			    s->k->on_change ? 0.0 : hz);
			if (sensor_start(&env, s, hz))
				return -1;
		} else if (!on && s->active) {
			printf("sns: %s: stop (iOS set ReportInterval 0)\n", s->k->alias);
			if (sensor_stop(&env, s))
				return -1;
		}
	}
	return 0;
}

void sns_tick(const struct sns_link_ops *ops)
{
	static int linked;
	if (!mode || done || !have_svc)
		return;
	long now = sensor_now_ms();
	if (!linked) {
		sns_link_set(ops);
		env.verbose = dump;
		linked = 1;
	}
	if (!t0) {
		if (now - svc_ms < delay_s * 1000L || now < next_try)
			return;
		if (start()) {
			if (++tries >= MAX_TRIES) {
				printf("sns: nothing found after %d tries: giving up\n", tries);
				done = 1;
			} else {
				printf("sns: nothing found (try %d): again in 10 s\n", tries);
				next_try = sensor_now_ms() + 10000;
			}
			return;
		}
		if (done)
			return;
		t0 = last_print = sensor_now_ms();
	}
	for (int i = 0; i < nss; i++)
		if (ss[i].active)
			sensor_poll(&env, &ss[i], 0);
	now = sensor_now_ms();
	if (mode == M_MOTION && now - last_want >= 2000) {
		last_want = now;
		follow();
	}
	if (mode == M_STREAM && now - last_print >= 500) {
		struct sensor *s = &ss[0];
		float dt = (float)(now - last_print) / 1000.0f;
		float m = sqrtf(s->v[0] * s->v[0] + s->v[1] * s->v[1] + s->v[2] * s->v[2]);
		printf("sns: %s %.4f %.4f %.4f |%.4f| %.1f Hz [%d] n=%llu\n", s->k->alias, s->v[0], s->v[1], s->v[2], m,
		    (float)(s->count - s->count_at_print) / dt, s->status, (unsigned long long)s->count);
		s->count_at_print = s->count;
		last_print = now;
	} else if (mode == M_MOTION && now - last_print >= 10000) {
		printf("sns: motion:");
		for (int i = 0; i < nss; i++)
			printf(" %s %llu%s", ss[i].k->alias, (unsigned long long)ss[i].count, ss[i].active ? "" : "(off)");
		printf(", written %llu\n", (unsigned long long)written);
		last_print = now;
	}
	if (secs && now - t0 >= secs * 1000L) {
		stop_all();
		done = 1;
		sns_summary();
	}
}

void sns_summary(void)
{
	if (!mode)
		return;
	printf("sns: summary: service %s", have_svc ? "seen" : "never announced");
	for (int i = 0; i < nss; i++)
		printf(", %s %llu samples", ss[i].k->alias, (unsigned long long)ss[i].count);
	if (mode == M_MOTION)
		printf(", %llu written to %s (%llu errors)", (unsigned long long)written, motion_dev,
		    (unsigned long long)write_errors);
	printf("\n");
}
