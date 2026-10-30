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
#include <mach/mach_time.h>
#include <notify.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "sensor.h"
#include "sns_run.h"
#include "dm_fusion.h"

enum { M_NONE, M_LIST, M_STREAM, M_MOTION };
static int mode, secs, delay_s = 10, dump, done, tries, one_port, unregistered;
static float rate;
static const char *stream_type, *motion_dev = MOTION_DEV;
static uint32_t svc_node, svc_port;
static int have_svc;
static long svc_ms, t0, next_try, last_print, last_want;
static int motion_fd = -1, orient_clients_seen, sns_rot;
static struct motion_sample last_orient;
static int have_orient;
static long last_orient_ms;
static unsigned long reposts;
static uint64_t written, write_errors;
/*
 * o12: screen blanked (backboardd's notify_set_state on "com.apple.springboard.hasBlankedScreen", 1 = blanked):
 * --sns-rot alone no longer keeps the accelerometer on, and adspboot's loop sleeps 20 ms instead of 2.
 * SNS_DARK=0 turns it off.
 */
static int dark_token = -1, dark, dark_off;
static struct sensor ss[8];
static int nss;
/*
 * B46 (kernel 0243): SNS_DEVMOTION=1 runs dm_fusion on accel + gyro and writes one type-3 device-motion report per
 * ~10 ms for the {0xff0c, 4} service (sweetmotion bit 0x80). Accel and gyro then stream at DM_HZ while that service
 * exists and the screen is not blanked; samples iOS did not ask for are not posted (dm_only). Default off.
 */
#define DM_HZ 100.0f
static int dm_env, dm_want, dm_only[8];
static int dm_gate_env = 1;	/* O13: SNS_DM_GATE=0 = old behaviour (fusion whenever the service exists) */
static struct dm_gate dm_gate;
static const char *dm_why = "no service";
static struct dm_fusion dm;
static uint64_t dm_last_report, dm_written;
/*
 * B46 dm2: type-2 (9-axis, x = magnetic north) reports for Compass (fUseNorthRef=1). The compass streams too while
 * the fusion runs; hard-iron box in MAGCAL_PATH (saved every 30 s). SNS_DM_TYPE=3 turns this off (type 3 as dm1);
 * default: type 2 while mag samples arrive (last one < 1 s ago), else 3. /var/local/dm-yaw: test yaw offset, deg.
 */
#define MAGCAL_PATH "/var/local/magcal.txt"
#define DM_YAW_PATH "/var/local/dm-yaw"
static int dm_type_env;		/* 0 auto, 2, 3 */
static int dm_type_last = 3;
static uint64_t dm_last_mag;
static long dm_last_save, dm_last_yaw;
static float dm_yaw;
/*
 * B46 dm5: SEE's background calibrations (mag_cal, gyro_cal; on-change streams) run while the fusion does; the mag
 * bias replaces the min/max box while its status is >= 1 (the box keeps learning as the fallback). SNS_SEE_CAL=0
 * turns them off (no lookup).
 */
static int see_cal;

#define ORIENT_HZ 15.0f		/* sensd: accel rate when it runs for orientation only */
#define MAX_TRIES 5		/* lookups that found nothing: again every 10 s (the sensors PD may still load) */

/* B46: feed the fusion; after a gyro sample, a report once >= 10 ms since the last one. */
static void dm_feed(const struct motion_sample *m)
{
	uint64_t ts = m->ts_mach ? m->ts_mach : mach_absolute_time();
	if (m->type == MOTION_TYPE_ACCEL) {
		dm_accel(&dm, m->v);
		return;
	}
	if (m->type == MOTION_TYPE_MAGCAL || m->type == MOTION_TYPE_GYROCAL) {	/* dm5 */
		static int last_st[2] = { -2, -2 };
		int g = m->type == MOTION_TYPE_GYROCAL, st = (int)m->v[3];
		if (st != last_st[g])
			printf("sns: %s status %d bias %.3f,%.3f,%.3f\n", g ? "gyro_cal" : "mag_cal", st, m->v[0], m->v[1],
			    m->v[2]);
		last_st[g] = st;
		(g ? dm_gyro_cal_bias : dm_mag_cal_bias)(&dm, m->v, st);
		return;
	}
	if (m->type == MOTION_TYPE_COMPASS) {	/* dm2: uT, same axes as the kernel's compass events */
		if (dm_mag(&dm, m->v) == 0)
			dm_last_mag = ts;
		return;
	}
	dm_gyro(&dm, m->v, ts, 19200000.0);	/* SEE stamps: the 19.2 MHz QTimer (= mach time) */
	if (!dm.have_acc || ts - dm_last_report < 19200000ull / 100)
		return;
	dm_last_report = ts;
	struct motion_devmotion d;
	memset(&d, 0, sizeof(d));
	int t = dm_type_env ? dm_type_env : (dm_last_mag && ts - dm_last_mag < 19200000ull) ? 2 : 3;
	if (t != dm_type_last)
		printf("sns: devmotion report type %d\n", t);
	dm_type_last = t;
	if (t == 2) {
		struct dm_report2 r;
		dm_report2(&dm, ts, dm_yaw, &r);
		d.len = sizeof(r);
		memcpy(d.bytes, &r, sizeof(r));
	} else {
		struct dm_report3 r;
		dm_report3(&dm, ts, dm_yaw, &r);
		d.len = sizeof(r);
		memcpy(d.bytes, &r, sizeof(r));
	}
	d.type = MOTION_TYPE_DEVMOTION;
	d.ts_mach = ts;
	if (write(motion_fd, &d, sizeof(d)) == (ssize_t)sizeof(d))
		dm_written++;
	else if (write_errors++ < 5)
		printf("sns: %s devmotion write: %s\n", motion_dev, strerror(errno));
}

static void out(void *ctx, struct sensor *s, const struct motion_sample *m)
{
	(void)ctx;
	if (mode != M_MOTION || motion_fd < 0)
		return;
	if (m->type == MOTION_TYPE_MAGCAL || m->type == MOTION_TYPE_GYROCAL) {	/* dm5: fusion only, never posted */
		if (dm_want)
			dm_feed(m);
		return;
	}
	if (dm_want && (m->type == MOTION_TYPE_ACCEL || m->type == MOTION_TYPE_GYRO ||
	    (m->type == MOTION_TYPE_COMPASS && dm_type_env != 3)))
		dm_feed(m);
	if (dm_only[s - ss] && !(s->orient_only && m->type == MOTION_TYPE_ORIENTATION))
		return;		/* B46: streamed only for the fusion */
	if (s->orient_only && m->type != MOTION_TYPE_ORIENTATION)
		return;
	struct motion_sample t;
	if (m->type == MOTION_TYPE_ORIENTATION) {
		/* u10c: orientation from the DSP's QTimer stamp rotated only when the NEXT event arrived; the kernel's fake
		 * path (mach time, now) rotated at once. Stamp orientation with mach time at posting, so re-posts never go
		 * back in time either. */
		t = *m;
		t.ts_mach = mach_absolute_time();
		m = &t;
	}
	if (write(motion_fd, m, sizeof(*m)) == (ssize_t)sizeof(*m))
		written++;
	else if (write_errors++ < 5)
		printf("sns: %s write: %s\n", motion_dev, strerror(errno));
	if (m->type == MOTION_TYPE_ORIENTATION && sns_rot) {
		/* mo3: iOS's own rotation (orientation HID service) runs in SpringBoard but the panel does not show it;
		 * SweetMetal2's "R N" (scene settings, frame kept portrait) is the path that draws (rot10/rot12). So with
		 * --sns-rot, portrait/landscape changes are written as "R N" to /var/local/sweet-orient. */
		int o = (int)m->v[0];
		static int last_r;
		if ((o == 1 || o == 3 || o == 4) && o != last_r) {
			int fd = open("/var/local/sweet-orient", O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (fd >= 0) {
				char c[8];
				int n = snprintf(c, sizeof(c), "R %d\n", o);
				if (write(fd, c, (size_t)n) == n)
					printf("sns: rot: R %d\n", o);
				close(fd);
			}
			last_r = o;
		}
	}
	if (m->type == MOTION_TYPE_ORIENTATION) {
		printf("sns: orientation %.0f posted\n", m->v[0]);
		last_orient = *m;
		have_orient = 1;
	}
}

/* u10: iOS acted on each orientation only when the next one arrived (held landscape for 30 s: no rotation until the
 * phone was turned back, then landscape + portrait at once), while repeating fake events (0.5 s) rotated at once.
 * Re-post the current orientation every 0.5 s with a fresh time, as the fake path does, until the kernel is fixed. */
static void orient_repost(long now)
{
	if (!have_orient || motion_fd < 0 || now - last_orient_ms < 500)
		return;
	last_orient_ms = now;
	struct motion_sample m = last_orient;
	m.ts_mach = mach_absolute_time();
	if (write(motion_fd, &m, sizeof(m)) == (ssize_t)sizeof(m))
		reposts++;
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
	else if (!strcmp(a, "--sns-rot"))	/* mo3: drive SweetMetal2 "R N" from orientation changes */
		sns_rot = 1;
	else if (!strcmp(a, "--sns-one-port"))		/* sns2: off switch: every stream on port 0x40 (sns1) */
		one_port = 1;
	else if (!strcmp(a, "--sns-unregistered"))	/* sns2: off switch: stream to unregistered services too */
		unregistered = 1;
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

/* sns2: stream index -> its own client port (sns_link: SNS_LINK_PORT0 + index), as redfin's socket per sensor. */
static int setup(struct sensor *s, const struct sensor_kind *k, int index)
{
	sensor_init(s, k, one_port ? 0 : index, svc_node, svc_port);
	if (sensor_lookup(&env, s))
		return -1;
	if (sensor_get_attrs(&env, s))
		printf("sns: %s: no attributes\n", k->alias);
	else
		sensor_print_attrs(s);
	return 0;
}

static int setup_cb(void *ctx, struct sensor *s, const struct sensor_kind *k, int index)
{
	(void)ctx;
	return setup(s, k, index);
}

/* One try of the mode's setup; 0 = running/done, -1 = nothing found. */
static int start(void)
{
	nss = 0;
	if (mode == M_LIST) {
		for (int i = 0; i < sensor_nkinds; i++)
			if (setup(&ss[nss], &sensor_kinds[i], nss) == 0)
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
		if (setup(&ss[0], k, 0))
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
	const char *e = getenv("SNS_DEVMOTION");
	dm_env = e && !strcmp(e, "1");
	e = getenv("SNS_DM_GATE");
	dm_gate_env = !(e && !strcmp(e, "0"));
	e = getenv("SNS_SEE_CAL");
	see_cal = dm_env && !(e && !strcmp(e, "0"));
	nss = sensor_setup_motion(ss, (int)(sizeof(ss) / sizeof(ss[0])), see_cal, setup_cb, NULL);	/* dm5b */
	printf("sns: motion: %d sensors\n", nss);
	env.orient = 1;
	last_want = 0;
	dm_init(&dm, 0.04f);
	e = getenv("SNS_HEADING_LP");	/* dm7: heading low-pass, 0 = off */
	if (e && !strcmp(e, "0"))
		dm.cal.heading_tc = 0;
	e = getenv("SNS_DM_TYPE");
	dm_type_env = e && (!strcmp(e, "2") || !strcmp(e, "3")) ? atoi(e) : 0;
	if (dm_env)
		printf("sns: SNS_DEVMOTION=1: fusion reports for the devmotion service (type %s), magcal %s\n",
		    dm_type_env ? e : "auto", dm_magcal_load(&dm, MAGCAL_PATH) ? "none" : "loaded");
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
	int orient_want = (have && (w.present & (1u << MOTION_ORIENT)) && w.clients[MOTION_ORIENT] > 0) || (sns_rot && !dark);	/* o12: not while blanked; u10f: --sns-rot needs orientation without an iOS client (sweetmotion=0x37) */
	if (orient_want != orient_clients_seen) {
		if (orient_want)
			env.orient_cur = env.orient_cand = 0;	/* a new client: post the current orientation again */
		printf("sns: orientation %s\n", orient_want ? "has clients: on" : "has no clients: off");
	}
	orient_clients_seen = orient_want;
	int dm_now = dm_env && have && (w.present & (1u << MOTION_DEVMOTION)) && !dark;	/* B46; o12: idle while blanked */
	dm_why = !dm_env ? "SNS_DEVMOTION off" : !have || !(w.present & (1u << MOTION_DEVMOTION)) ? "no service" :
	    dark ? "blanked" : "service";
	if (dm_now && dm_gate_env) {	/* O13: only while CoreMotion (devmotion) or locationd (devmotion8) asks */
		char st[2048];
		ssize_t n = pread(motion_fd, st, sizeof(st) - 1, 0);
		st[n > 0 ? n : 0] = 0;
		unsigned i4 = w.interval_set[MOTION_DEVMOTION] ? w.interval_us[MOTION_DEVMOTION] : 0;
		unsigned i8 = dm_status_interval(st, "devmotion8");
		dm_now = dm_gate_step(&dm_gate, i4 || i8, sensor_now_ms());
		dm_why = i4 && i8 ? "devmotion+devmotion8" : i4 ? "devmotion" : i8 ? "devmotion8" :
		    dm_now ? "idle, stopping" : "no client";
	} else {
		dm_gate.on = 0, dm_gate.idle_since = 0;
	}
	if (dm_now != dm_want) {
		printf("sns: devmotion fusion %s (%s)\n", dm_now ? "on" : "off", dm_why);
		if (dm_now)
			dm_init(&dm, 0.04f);	/* keeps the fit and the SEE biases */
		else if (dm.cal.have_cal && dm_magcal_save(&dm, MAGCAL_PATH))	/* O13: save on stop */
			printf("sns: %s: %s\n", MAGCAL_PATH, strerror(errno));
	}
	dm_want = dm_now;
	for (int i = 0; i < nss; i++) {
		struct sensor *s = &ss[i];
		int idx = s->k->motion_index, on = 1, orient_only = 0;
		if (idx < 0) {	/* dm5: SEE cal sensors, on while the 9-axis fusion runs */
			on = dm_want && dm_type_env != 3 && sensor_motion_streaming(ss, nss);	/* dm5b: after accel/gyro */
			if (on && !s->active) {
				printf("sns: %s: start (on change)\n", s->k->alias);
				if (sensor_start(&env, s, 0))
					return -1;
			} else if (!on && s->active) {
				printf("sns: %s: stop (devmotion fusion off)\n", s->k->alias);
				if (sensor_stop(&env, s))
					return -1;
			}
			continue;
		}
		float hz = s->k->default_hz;
		if (have && w.interval_set[idx]) {
			on = w.interval_us[idx] != 0;
			if (on && !s->k->on_change) {
				hz = 1e6f / (float)w.interval_us[idx];
				hz = hz < 1 ? 1 : (hz > 200 ? 200 : hz);
			}
		}
		/*
		 * sns2: no service for it (sweetmotion's default 0x77 leaves prox out): the kernel would reject every
		 * sample (mo1: "4 rejected" were prox), so don't stream it. --sns-unregistered: as sns1.
		 */
		if (have && !unregistered && !(w.present & (1u << idx)))
			on = 0;
		if (!on && orient_want && idx == MOTION_ACCEL) {
			on = orient_only = 1;
			hz = ORIENT_HZ;
		}
		int dmo = 0;
		if (dm_want && (idx == MOTION_ACCEL || idx == MOTION_GYRO)) {	/* B46: the fusion needs both at DM_HZ */
			dmo = !on || orient_only;
			on = 1;
			hz = hz < DM_HZ ? DM_HZ : hz;
		} else if (dm_want && idx == MOTION_COMPASS && dm_type_env != 3) {	/* dm2: and the mag */
			dmo = !on;
			on = 1;
		}
		if (dm_only[i] != dmo)
			printf("sns: %s: %s\n", s->k->alias, dmo ? "kept for devmotion fusion (samples not posted)" :
			    "devmotion fusion: back to normal");
		dm_only[i] = dmo;
		if (s->orient_only != orient_only)	/* as redfin's sensd */
			printf("sns: %s: %s\n", s->k->alias, orient_only ?
			    "iOS wants none: kept for orientation only (samples not posted)" : "posting samples again");
		s->orient_only = orient_only;
		if (on && (!s->active || (!s->k->on_change && (s->hz < hz * 0.9f || s->hz > hz * 1.1f)))) {
			printf("sns: %s: start%s %.1f Hz\n", s->k->alias, s->active ? " (new rate)" : "",
			    s->k->on_change ? 0.0 : hz);
			if (sensor_start(&env, s, hz))
				return -1;
		} else if (!on && s->active) {
			printf("sns: %s: stop (%s)\n", s->k->alias, have && !(w.present & (1u << idx)) ?
			    "service not registered" : "iOS set ReportInterval 0");
			if (sensor_stop(&env, s))
				return -1;
		}
	}
	return 0;
}

/* o12: poll the blanked state (a shared-memory check); 1 on a change. */
static int dark_check(void)
{
	int changed = 0;
	uint64_t st = 0;
	if (dark_off)
		return 0;
	if (dark_token < 0) {
		const char *e = getenv("SNS_DARK");
		if ((e && !strcmp(e, "0")) || notify_register_check("com.apple.springboard.hasBlankedScreen", &dark_token) !=
		    NOTIFY_STATUS_OK) {
			dark_off = 1;
			return 0;
		}
		changed = 1;
	} else if (notify_check(dark_token, &changed) != NOTIFY_STATUS_OK)
		return 0;
	if (!changed || notify_get_state(dark_token, &st) != NOTIFY_STATUS_OK || (st != 0) == dark)
		return 0;
	dark = st != 0;
	printf("sns: %s\n", dark ? "screen blanked: sensors idle" : "screen on: sensors back");
	return 1;
}

int sns_dark(void)
{
	return mode == M_MOTION && t0 && dark;
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
	if (mode == M_MOTION && orient_clients_seen)
		orient_repost(now);
	static long last_acc;	/* u10: raw accel once a second, to check the 4a's axes against the orientation posted */
	if (mode == M_MOTION && now - last_acc >= 1000) {
		last_acc = now;
		for (int i = 0; i < nss; i++)
			if (ss[i].active && ss[i].k->motion_type == MOTION_TYPE_ACCEL)
				printf("sns: acc %.2f %.2f %.2f orient %d (cand %d)\n", ss[i].v[0], ss[i].v[1], ss[i].v[2],
				    env.orient_cur, env.orient_cand);
	}
	if (mode == M_MOTION && dark_check())
		last_want = 0;	/* o12: follow now; un-blank re-posts the orientation as for a new client */
	if (mode == M_MOTION && now - last_want >= 2000) {
		last_want = now;
		follow();
	}
	if (dm_want && now - dm_last_yaw >= 1000) {	/* dm2: test knob, re-read every second */
		FILE *fp = fopen(DM_YAW_PATH, "r");
		float y = 0;
		if (fp) {
			if (fscanf(fp, "%f", &y) != 1 || !isfinite(y))
				y = 0;
			fclose(fp);
		}
		dm_yaw = y;
		dm_last_yaw = now;
	}
	if (dm_env && dm.cal.have_cal && now - dm_last_save >= 30000) {	/* dm2/dm7: keep the hard-iron fit across reboots */
		if (dm_last_save && dm_magcal_save(&dm, MAGCAL_PATH))
			printf("sns: %s: %s\n", MAGCAL_PATH, strerror(errno));
		dm_last_save = now;
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
		printf(", written %llu", (unsigned long long)written);
		if (dm_env)
			printf(", dm %s (%s)", dm_want ? "on" : "off", dm_why);	/* O13 */
		if (dm_env) {
			float b[3];
			dm_mag_bias(&dm, b);
			printf(", devmotion %llu, dm type %d acc %d cal %s r %.1f res %.2f n %d ring %d bias %.1f,%.1f,%.1f "
			    "st %d yaw %.1f gcal st %d %.4f,%.4f,%.4f", (unsigned long long)dm_written, dm_type_last,
			    dm_mag_accuracy(&dm), dm_mag_cal_dsp(&dm) ? "dsp" : dm.cal.have_cal ? "fit" : "none", dm.cal.r,
			    dm.cal.res, dm.cal.n, dm.cal.ring_n, b[0], b[1], b[2], dm.see_mag_st, dm_yaw, dm.see_gyro_st,
			    dm.see_gyro_bias[0], dm.see_gyro_bias[1], dm.see_gyro_bias[2]);
		}
		printf("\n");
		last_print = now;
	}
	if (secs && now - t0 >= secs * 1000L) {	/* --sns-secs 0 = no limit (launchd job) */
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
