/*
 * From ios-redfin (DenialOfGravity, https://github.com/DenialOfGravity/ios-redfin) phone/sensors/sensor.c,
 * Copyright (c) 2026 dipa-studio, MIT License (see LICENSE.redfin). Sunfish changes are marked "sunfish".
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "sns_link.h"	/* sunfish: the shim over adspboot's QRTR, instead of rqrtr.h */
#include "sensor.h"
#include "../wifi/qmi.h"

const struct sensor_kind sensor_kinds[] = {
	{ "accel",         "accel",    MOTION_TYPE_ACCEL,    MOTION_ACCEL,    0, 50 },
	{ "gyro",          "gyro",     MOTION_TYPE_GYRO,     MOTION_GYRO,     0, 50 },
	{ "ambient_light", "als",      MOTION_TYPE_ALS,      MOTION_ALS,      1, 0 },
	{ "proximity",     "prox",     MOTION_TYPE_PROX,     MOTION_PROX,     1, 0 },
	{ "mag",           "compass",  MOTION_TYPE_COMPASS,  MOTION_COMPASS,  0, 25 },
	{ "pressure",      "pressure", MOTION_TYPE_PRESSURE, MOTION_PRESSURE, 0, 5 },
	/* sunfish B46 dm5: SEE's background calibrations, for the devmotion fusion only (no motion service: -1) */
	{ "mag_cal",       "magcal",   MOTION_TYPE_MAGCAL,   -1,              1, 0 },
	{ "gyro_cal",      "gyrocal",  MOTION_TYPE_GYROCAL,  -1,              1, 0 },
};
const int sensor_nkinds = sizeof(sensor_kinds) / sizeof(sensor_kinds[0]);

#define TICKS_PER_S 19200000ull		/* SEE timestamps: the 19.2 MHz QTimer (= mach_absolute_time here) */

const struct sensor_kind *sensor_kind_find(const char *name)
{
	for (int i = 0; i < sensor_nkinds; i++)
		if (!strcmp(name, sensor_kinds[i].data_type) || !strcmp(name, sensor_kinds[i].alias))
			return &sensor_kinds[i];
	if (!strcmp(name, "mag") || !strcmp(name, "magnetometer"))
		return sensor_kind_find("compass");
	if (!strcmp(name, "light"))
		return sensor_kind_find("als");
	return NULL;
}

long sensor_now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

void hexdump(const char *tag, const uint8_t *d, size_t n)
{
	printf("%s %zu bytes:", tag, n);
	for (size_t i = 0; i < n; i++)
		printf("%s%02x", i % 32 ? " " : "\n  ", d[i]);
	printf("\n");
}

void sensor_init(struct sensor *s, const struct sensor_kind *k, int fd, uint32_t node, uint32_t port)
{
	memset(s, 0, sizeof(*s));
	s->k = k;
	s->fd = fd;
	s->node = node;
	s->port = port;
	s->txn = 1;
}

int sensor_orientation_of(float x, float y, float z)
{
	float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, az = z < 0 ? -z : z;
	if (ax + ay < 3.0f || az > 1.6f * (ax > ay ? ax : ay))
		return 0;	/* flat (face up/down) or in free fall: keep the last orientation */
	if (ay > 1.3f * ax)
		return y > 0 ? 1 : 2;
	if (ax > 1.3f * ay)
		return x > 0 ? 3 : 4;
	return 0;		/* near 45°: ambiguous */
}

static void emit(struct sensor_env *e, struct sensor *s, uint32_t type, uint32_t flags, uint64_t ts,
    const float *v, int nv)
{
	struct motion_sample m;
	memset(&m, 0, sizeof(m));
	m.type = type;
	m.flags = flags;
	m.ts_mach = ts;
	for (int i = 0; i < nv && i < 4; i++)
		m.v[i] = v[i];
	if (e->out)
		e->out(e->out_ctx, s, &m);
}

/*
 * Face up / face down (UIDeviceOrientation 5/6) when gravity is mostly along z; else sensor_orientation_of.
 * cur is the orientation posted last: leaving it needs a clearer reading than entering it (hysteresis).
 */
int sensor_orientation_hyst(float x, float y, float z, int cur)
{
	float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, az = z < 0 ? -z : z, m = ax > ay ? ax : ay;
	if (cur >= 5 ? az > 4.5f && az > 1.2f * m : az > 7.0f && az > 1.6f * m)
		return z > 0 ? 5 : 6;
	int o = sensor_orientation_of(x, y, z);
	if (o && cur >= 1 && cur <= 4 && o != cur) {
		/* portrait <-> landscape: the new axis must lead by 1.6x, not 1.3x */
		if ((cur <= 2) != (o <= 2) && (o <= 2 ? ay < 1.6f * ax : ax < 1.6f * ay))
			return cur;
	}
	return o;
}

static void orient_filter(struct sensor_env *e, struct sensor *s, uint64_t ts, const float *v)
{
	int o = sensor_orientation_hyst(v[0], v[1], v[2], e->orient_cur);
	if (!o)
		return;
	if (o != e->orient_cand) {
		e->orient_cand = o;
		e->orient_cand_ts = ts;
	}
	if (o != e->orient_cur && ts - e->orient_cand_ts >= TICKS_PER_S * 3 / 10) {
		float f = (float)o;
		e->orient_cur = o;
		emit(e, s, MOTION_TYPE_ORIENTATION, 3, ts, &f, 1);
	}
}

static void sample(struct sensor_env *e, struct sensor *s, uint32_t msg_id, uint64_t ts, const uint8_t *p, size_t n)
{
	float d[16], v[4] = { 0, 0, 0, 0 };
	int status = 0, nd, nv = 0;

	if (s->k->motion_type == MOTION_TYPE_MAGCAL || s->k->motion_type == MOTION_TYPE_GYROCAL) {
		/* sunfish B46 dm5: sns_cal_event; the first 3 raw, as its field numbers are from memory */
		char tag[32];
		if (s->raw_logged < 3) {
			s->raw_logged++;
			snprintf(tag, sizeof(tag), "sensd: %s raw", s->k->alias);
			sns_print_fields(tag, msg_id, p, n);
		}
		if (sns_parse_cal_event(p, n, v, &status) < 3) {
			printf("sensd: %s: msg %u not understood (%zu bytes)\n", s->k->alias, msg_id, n);
			return;
		}
		v[3] = (float)status;
		nv = 4;
	} else if (s->k->motion_type == MOTION_TYPE_PROX) {
		int near;
		uint32_t raw;
		if (sns_parse_prox_event(p, n, &near, &raw, &status) == 0) {
			v[0] = (float)near;
			v[1] = (float)raw;
			nv = 2;
		} else if ((nd = sns_parse_sensor_event(p, n, d, 16, &status)) > 0) {
			v[0] = d[0] >= 0.5f ? 1.0f : 0.0f;	/* assumption: a std event's [0] is 1 near / 0 far */
			v[1] = nd > 1 ? d[1] : d[0];
			nv = 2;
		} else {
			printf("sensd: %s: msg %u not understood (%zu bytes)\n", s->k->alias, msg_id, n);
			return;
		}
	} else {
		nd = sns_parse_sensor_event(p, n, d, 16, &status);
		if (nd <= 0) {
			printf("sensd: %s: msg %u: no data (%zu bytes)\n", s->k->alias, msg_id, n);
			return;
		}
		switch (s->k->motion_type) {
		case MOTION_TYPE_PRESSURE:
			v[0] = d[0] / 10.0f;	/* hPa -> kPa */
			nv = 1;
			break;
		case MOTION_TYPE_ALS:
			v[0] = d[0];		/* lux */
			nv = 2;			/* [1] colour temperature: not reported by SEE's ambient_light */
			break;
		default:
			nv = nd < 3 ? nd : 3;
			memcpy(v, d, sizeof(float) * (size_t)nv);
		}
	}
	memcpy(s->v, v, sizeof(v));
	s->nv = nv;
	s->status = status;
	s->ts = ts;
	s->count++;
	s->last_data_ms = sensor_now_ms();
	emit(e, s, s->k->motion_type, (uint32_t)status & MOTION_FLAG_ACCURACY, ts, v, nv);
	if (e->orient && s->k->motion_type == MOTION_TYPE_ACCEL)
		orient_filter(e, s, ts ? ts : (uint64_t)sensor_now_ms() * (TICKS_PER_S / 1000), v);
}

struct ev_ctx {
	struct sensor_env *e;
	struct sensor *s;
};

static void ev_cb(void *ctx, const struct sns_suid *suid, uint32_t msg_id, uint64_t ts, const uint8_t *p, size_t n)
{
	struct ev_ctx *c = ctx;
	struct sensor *s = c->s;
	int lookup = suid->low == sns_suid_lookup.low && suid->high == sns_suid_lookup.high;

	if (c->e->verbose)
		printf("sensd: %s: event %u from %016llx%016llx ts %llu, %zu bytes\n", s->k->alias, msg_id,
		    (unsigned long long)suid->high, (unsigned long long)suid->low, (unsigned long long)ts, n);
	if (lookup && msg_id == SNS_MSG_SUID_EVENT) {
		char dt[64];
		struct sns_suid suids[8];
		int k = sns_parse_suid_event(p, n, dt, sizeof(dt), suids, 8);
		if (k < 0 || strcmp(dt, s->k->data_type)) {
			printf("sensd: %s: SUID event for \"%s\" (%d)\n", s->k->alias, dt, k);
			return;
		}
		s->nsuids = k;
		if (k > 0) {
			s->suid = suids[0];
			s->have_suid = 1;
		} else {
			s->have_suid = -1;	/* answered: no such sensor */
		}
		return;
	}
	if (s->have_suid > 0 && (suid->low != s->suid.low || suid->high != s->suid.high)) {
		printf("sensd: %s: event %u from another SUID %016llx%016llx\n", s->k->alias, msg_id,
		    (unsigned long long)suid->high, (unsigned long long)suid->low);
		return;
	}
	switch (msg_id) {
	case SNS_MSG_ATTR_EVENT:
		if (sns_parse_attr_event(p, n, &s->attrs) == 0)
			s->have_attrs = 1;
		else
			printf("sensd: %s: bad attribute event\n", s->k->alias);
		return;
	case SNS_MSG_ERROR_EVENT:
		s->last_error = sns_parse_error_event(p, n);
		printf("sensd: %s: error event %d\n", s->k->alias, s->last_error);
		return;
	case SNS_MSG_FLUSH_EVENT:
		return;
	case SNS_MSG_PHYS_CONFIG_EVENT:
		if (c->e->verbose)
			hexdump("sensd: physical config", p, n);
		return;
	}
	if (msg_id >= 1024 || (msg_id == SNS_MSG_PROX_EVENT && s->k->motion_type == MOTION_TYPE_PROX) ||
	    (msg_id == SNS_MSG_CAL_EVENT && s->k->motion_index < 0))	/* sunfish dm5b: cal events are 1022 */
		sample(c->e, s, msg_id, ts, p, n);
	else
		printf("sensd: %s: unhandled event %u (%zu bytes)\n", s->k->alias, msg_id, n);
}

void sensor_handle(struct sensor_env *e, struct sensor *s, const uint8_t *d, size_t n)
{
	struct sns_qmi q;
	if (e->verbose)
		hexdump("sensd: rx", d, n);
	if (sns_qmi_parse(d, n, &q)) {
		printf("sensd: %s: malformed QMI message (%zu bytes)\n", s->k->alias, n);
		return;
	}
	if (q.type == QMI_INDICATION && (q.msg_id == SNS_QMI_REPORT_IND || q.msg_id == SNS_QMI_JUMBO_IND)) {
		struct ev_ctx c = { e, s };
		if (sns_parse_event_msg(q.payload, q.plen, ev_cb, &c) < 0)
			printf("sensd: %s: malformed event message (%zu bytes)\n", s->k->alias, q.plen);
	} else if (e->verbose) {
		printf("sensd: %s: QMI type %u msg %#x txn %u ignored\n", s->k->alias, q.type, q.msg_id, q.txn);
	}
}

static int request(struct sensor_env *e, struct sensor *s, uint32_t msg_id, const uint8_t *pl, size_t plen)
{
	uint8_t req[1024], qmi[1100], rx[RQRTR_MAX];
	size_t rl = sns_enc_client_request(req, sizeof(req), msg_id == SNS_MSG_SUID_REQ ? &sns_suid_lookup : &s->suid,
	    msg_id, pl, plen, SNS_DELIVERY_WAKEUP);
	uint16_t txn = s->txn++;
	size_t ql = rl ? sns_qmi_request(qmi, sizeof(qmi), txn, req, rl, 1) : 0;

	if (!ql) {
		printf("sensd: %s: request %u does not fit\n", s->k->alias, msg_id);
		return -1;
	}
	if (e->verbose)
		hexdump("sensd: tx", qmi, ql);
	if (rqrtr_send(s->fd, s->node, s->port, qmi, ql)) {
		perror("sensd: qrtr write");
		return -1;
	}
	long deadline = sensor_now_ms() + (e->timeout_ms ? e->timeout_ms : 3000);
	for (;;) {
		long left = deadline - sensor_now_ms();
		uint32_t node, port;
		if (left <= 0) {
			printf("sensd: %s: no response to request %u (txn %u)\n", s->k->alias, msg_id, txn);
			return -1;
		}
		int n = rqrtr_recv(s->fd, &node, &port, rx, sizeof(rx), (int)left);
		if (n < 0) {
			perror("sensd: qrtr read");
			return -1;
		}
		if (n == 0)
			continue;
		struct sns_qmi q;
		if (sns_qmi_parse(rx, (size_t)n, &q) == 0 && q.type == QMI_RESPONSE && q.msg_id == SNS_QMI_CLIENT_REQ &&
		    q.txn == txn) {
			if (e->verbose)
				hexdump("sensd: rx", rx, (size_t)n);
			if (q.has_client_id)
				s->client_id = q.client_id;
			if (q.result) {
				printf("sensd: %s: request %u refused: result %u error %u\n", s->k->alias, msg_id,
				    q.result, q.error);
				return -1;
			}
			return 0;
		}
		sensor_handle(e, s, rx, (size_t)n);
	}
}

/* After a request: handle indications until *flag is set or the timeout passes. */
static int wait_for(struct sensor_env *e, struct sensor *s, const int *flag)
{
	long deadline = sensor_now_ms() + (e->timeout_ms ? e->timeout_ms : 3000);
	while (!*flag) {
		long left = deadline - sensor_now_ms();
		if (left <= 0 || sensor_poll(e, s, (int)left) < 0)
			return -1;
	}
	return 0;
}

int sensor_lookup(struct sensor_env *e, struct sensor *s)
{
	uint8_t pl[96];
	size_t n = sns_enc_suid_req(pl, sizeof(pl), s->k->data_type, 1, 1);
	s->have_suid = 0;
	if (request(e, s, SNS_MSG_SUID_REQ, pl, n) || wait_for(e, s, &s->have_suid)) {
		if (!s->have_suid)
			printf("sensd: %s: no SUID event for \"%s\"\n", s->k->alias, s->k->data_type);
		return -1;
	}
	if (s->have_suid < 0) {
		printf("sensd: %s: the DSP has no \"%s\" sensor\n", s->k->alias, s->k->data_type);
		return -1;
	}
	return 0;
}

int sensor_get_attrs(struct sensor_env *e, struct sensor *s)
{
	uint8_t pl[8];
	size_t n = sns_enc_attr_req(pl, sizeof(pl), 0);
	s->have_attrs = 0;
	if (request(e, s, SNS_MSG_ATTR_REQ, pl, n) || wait_for(e, s, &s->have_attrs))
		return -1;
	return 0;
}

int sensor_start(struct sensor_env *e, struct sensor *s, float hz)
{
	uint8_t pl[8];
	int on_change = s->k->on_change || (s->have_attrs && s->attrs.have_stream_type &&
	    s->attrs.stream_type == SNS_STREAM_ON_CHANGE);
	if (on_change) {
		if (request(e, s, SNS_MSG_ONCHANGE_CONFIG, NULL, 0))
			return -1;
	} else {
		if (hz <= 0)
			hz = s->k->default_hz;
		size_t n = sns_enc_sensor_config(pl, sizeof(pl), hz);
		if (request(e, s, SNS_MSG_SENSOR_CONFIG, pl, n))
			return -1;
	}
	s->active = 1;
	s->hz = on_change ? 0 : hz;
	s->last_data_ms = sensor_now_ms();
	return 0;
}

int sensor_stop(struct sensor_env *e, struct sensor *s)
{
	if (!s->active)
		return 0;
	s->active = 0;
	return request(e, s, SNS_MSG_DISABLE, NULL, 0);
}

int sensor_poll(struct sensor_env *e, struct sensor *s, int timeout_ms)
{
	uint8_t rx[RQRTR_MAX];
	uint32_t node, port;
	int handled = 0;
	for (;;) {
		int n = rqrtr_recv(s->fd, &node, &port, rx, sizeof(rx), handled ? 0 : timeout_ms);
		if (n < 0)
			return -1;
		if (n == 0)
			return handled;
		sensor_handle(e, s, rx, (size_t)n);
		handled++;
	}
}

void sensor_print_attrs(const struct sensor *s)
{
	const struct sns_attrs *a = &s->attrs;
	printf("%-9s %-14s suid %016llx%016llx (%d)  name \"%s\" vendor \"%s\" type \"%s\" available %d "
	    "physical %d hw_id %d stream %s",
	    s->k->alias, s->k->data_type, (unsigned long long)s->suid.high, (unsigned long long)s->suid.low,
	    s->nsuids, a->name, a->vendor, a->type, a->available, a->physical, a->hw_id,
	    !a->have_stream_type ? "?" : a->stream_type == SNS_STREAM_ON_CHANGE ? "on-change" :
	    a->stream_type == SNS_STREAM_STREAMING ? "streaming" : "single");
	if (a->nrates) {
		printf(" rates");
		for (int i = 0; i < a->nrates; i++)
			printf(" %g", a->rates[i]);
	}
	if (a->range_max != 0)
		printf(" range %g..%g", a->range_min, a->range_max);
	if (a->resolution != 0)
		printf(" resolution %g", a->resolution);
	printf(" (%d attributes)\n", a->nattrs);
}

/* sunfish B46 dm5b: see sensor.h */
int sensor_setup_motion(struct sensor *ss, int max, int see_cal, sensor_setup_fn setup, void *ctx)
{
	int n = 0;
	for (int i = 0; i < sensor_nkinds && n < max; i++)
		if (sensor_kinds[i].motion_index >= 0 && setup(ctx, &ss[n], &sensor_kinds[i], n) == 0)
			n++;
	for (int i = 0; i < sensor_nkinds && n < max && n && see_cal; i++)
		if (sensor_kinds[i].motion_index < 0 && setup(ctx, &ss[n], &sensor_kinds[i], n) == 0)
			n++;	/* a cal sensor without a SUID is just left out (box fallback) */
	return n;
}

int sensor_motion_streaming(const struct sensor *ss, int n)
{
	for (int i = 0; i < n; i++)
		if (ss[i].active && ss[i].count && (ss[i].k->motion_type == MOTION_TYPE_ACCEL ||
		    ss[i].k->motion_type == MOTION_TYPE_GYRO))
			return 1;
	return 0;
}
