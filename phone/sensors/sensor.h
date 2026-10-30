/*
 * From ios-redfin (DenialOfGravity, https://github.com/DenialOfGravity/ios-redfin) phone/sensors/sensor.h,
 * Copyright (c) 2026 dipa-studio, MIT License (see LICENSE.redfin). Sunfish changes are marked "sunfish".
 */
/* sensd's sensor streams: SUID lookup, attributes, streaming/on-change configuration, sample conversion. */
#ifndef SENSOR_H
#define SENSOR_H
#include <stdint.h>
#include <stddef.h>
#include "sns.h"
#include "motion.h"

struct sensor_kind {
	const char *data_type;	/* SEE data type */
	const char *alias;	/* sensd's short name */
	uint32_t motion_type;	/* struct motion_sample type */
	int motion_index;	/* MOTION_* (struct motion_want); -1: fusion-only (sunfish dm5) */
	int on_change;		/* 514 instead of 513 */
	float default_hz;
};
extern const struct sensor_kind sensor_kinds[];
extern const int sensor_nkinds;
const struct sensor_kind *sensor_kind_find(const char *name);	/* data type or alias */

struct sensor {
	const struct sensor_kind *k;
	int fd;				/* own QRTR port */
	uint32_t node, port;		/* the SNS service */
	uint16_t txn;
	struct sns_suid suid;
	int have_suid, nsuids;
	struct sns_attrs attrs;
	int have_attrs;
	uint64_t client_id;
	int active;
	float hz;
	/* last sample */
	float v[4];
	int nv, status;
	uint64_t ts, count, count_at_print;
	long last_data_ms;
	int last_error;
	int raw_logged;			/* sunfish B46 dm5: cal events printed raw so far */
	int orient_only;		/* streamed only for orientation (iOS set ReportInterval 0): samples not posted */
};

/* Where samples go (sensd: print + /dev/redwood-motion; tests: capture). */
typedef void (*sensor_out_fn)(void *ctx, struct sensor *s, const struct motion_sample *m);
struct sensor_env {
	sensor_out_fn out;
	void *out_ctx;
	int verbose;		/* --dump: every QMI message in hex */
	int timeout_ms;		/* per request (default 3000) */
	int orient;		/* derive type-10 orientation samples from accel */
	/* orientation filter state */
	int orient_cur, orient_cand;
	uint64_t orient_cand_ts;
};

void sensor_init(struct sensor *s, const struct sensor_kind *k, int fd, uint32_t node, uint32_t port);
/* Each returns 0 on success, -1 (message printed) otherwise. */
int sensor_lookup(struct sensor_env *e, struct sensor *s);
int sensor_get_attrs(struct sensor_env *e, struct sensor *s);
int sensor_start(struct sensor_env *e, struct sensor *s, float hz);
int sensor_stop(struct sensor_env *e, struct sensor *s);
/* Drain/handle whatever has arrived on s->fd within timeout_ms; returns datagrams handled or -1. */
int sensor_poll(struct sensor_env *e, struct sensor *s, int timeout_ms);
/* Handle one received QMI message (exposed for tests). */
void sensor_handle(struct sensor_env *e, struct sensor *s, const uint8_t *d, size_t n);
void sensor_print_attrs(const struct sensor *s);
void hexdump(const char *tag, const uint8_t *d, size_t n);
long sensor_now_ms(void);

/* UIDeviceOrientation (1 portrait, 2 upside down, 3 landscape left, 4 landscape right) from an Android-frame
 * accel sample, or 0 when flat/ambiguous. */
int sensor_orientation_of(float x, float y, float z);
int sensor_orientation_hyst(float x, float y, float z, int cur);

/*
 * sunfish B46 dm5b: a motion try's setups into ss (at most max): the motion services first, then (see_cal) the
 * fusion-only cal sensors, but only if a motion service was found: with only cal sensors the try counted as a
 * success and never retried, and they sat on ports 0x40/0x41 getting the late lookup replies of the first six.
 * Returns the count.
 */
typedef int (*sensor_setup_fn)(void *ctx, struct sensor *s, const struct sensor_kind *k, int index);
int sensor_setup_motion(struct sensor *ss, int max, int see_cal, sensor_setup_fn setup, void *ctx);
/* sunfish dm5b: 1 once an accel or gyro stream is active and has delivered samples (cal sensors start after). */
int sensor_motion_streaming(const struct sensor *ss, int n);

#endif
