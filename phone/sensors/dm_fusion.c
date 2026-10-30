/* sunfish B46 (kernel 0243): see dm_fusion.h. Madgwick's IMU update (gradient descent, 2010 report), float only. */
#include <math.h>
#include <string.h>
#include "dm_fusion.h"

#define G_MS2 9.80665f

void dm_init(struct dm_fusion *f, float beta)
{
	memset(f, 0, sizeof(*f));
	f->q[0] = 1.0f;
	f->beta = beta;
}

/* Tilt the quaternion straight to the accel direction (no yaw): the start, so the filter need not converge. */
static void dm_seed(struct dm_fusion *f)
{
	float ax = f->acc[0], ay = f->acc[1], az = f->acc[2];
	float n = sqrtf(ax * ax + ay * ay + az * az);
	if (n < 1e-3f)
		return;
	ax /= n, ay /= n, az /= n;
	/* rotation taking device "up" (a) to earth z: q = (1 + a.z, a × z) normalised = (1 + az, ay, -ax, 0) */
	float w = 1.0f + az, x = ay, y = -ax;
	n = sqrtf(w * w + x * x + y * y);
	if (n < 1e-6f) {	/* face down: 180° about x */
		f->q[0] = 0, f->q[1] = 1, f->q[2] = 0, f->q[3] = 0;
		return;
	}
	f->q[0] = w / n, f->q[1] = x / n, f->q[2] = y / n, f->q[3] = 0;
}

void dm_accel(struct dm_fusion *f, const float a[3])
{
	memcpy(f->acc, a, sizeof(f->acc));
	if (!f->have_acc)
		dm_seed(f);
	f->have_acc = 1;
}

void dm_gyro(struct dm_fusion *f, const float g[3], uint64_t ts, double ticks_per_s)
{
	memcpy(f->gyro, g, sizeof(f->gyro));
	f->have_gyro = 1;
	if (!ts || !f->last_ts || ts <= f->last_ts) {
		f->last_ts = ts;
		return;
	}
	float dt = (float)((double)(ts - f->last_ts) / ticks_per_s);
	f->last_ts = ts;
	if (dt > 0.1f)
		dt = 0.1f;	/* a gap (sensor restarted): integrate at most 100 ms */
	float q0 = f->q[0], q1 = f->q[1], q2 = f->q[2], q3 = f->q[3];
	float gx = g[0], gy = g[1], gz = g[2];
	float qd0 = 0.5f * (-q1 * gx - q2 * gy - q3 * gz);
	float qd1 = 0.5f * (q0 * gx + q2 * gz - q3 * gy);
	float qd2 = 0.5f * (q0 * gy - q1 * gz + q3 * gx);
	float qd3 = 0.5f * (q0 * gz + q1 * gy - q2 * gx);
	float ax = f->acc[0], ay = f->acc[1], az = f->acc[2];
	float an = sqrtf(ax * ax + ay * ay + az * az);
	if (f->have_acc && an > 1e-3f) {
		ax /= an, ay /= an, az /= an;
		float _2q0 = 2 * q0, _2q1 = 2 * q1, _2q2 = 2 * q2, _2q3 = 2 * q3;
		float _4q0 = 4 * q0, _4q1 = 4 * q1, _4q2 = 4 * q2, _8q1 = 8 * q1, _8q2 = 8 * q2;
		float q0q0 = q0 * q0, q1q1 = q1 * q1, q2q2 = q2 * q2, q3q3 = q3 * q3;
		float s0 = _4q0 * q2q2 + _2q2 * ax + _4q0 * q1q1 - _2q1 * ay;
		float s1 = _4q1 * q3q3 - _2q3 * ax + 4 * q0q0 * q1 - _2q0 * ay - _4q1 + _8q1 * q1q1 + _8q1 * q2q2 + _4q1 * az;
		float s2 = 4 * q0q0 * q2 + _2q0 * ax + _4q2 * q3q3 - _2q3 * ay - _4q2 + _8q2 * q1q1 + _8q2 * q2q2 + _4q2 * az;
		float s3 = 4 * q1q1 * q3 - _2q1 * ax + 4 * q2q2 * q3 - _2q2 * ay;
		float sn = sqrtf(s0 * s0 + s1 * s1 + s2 * s2 + s3 * s3);
		if (sn > 1e-9f) {
			qd0 -= f->beta * s0 / sn;
			qd1 -= f->beta * s1 / sn;
			qd2 -= f->beta * s2 / sn;
			qd3 -= f->beta * s3 / sn;
		}
	}
	q0 += qd0 * dt, q1 += qd1 * dt, q2 += qd2 * dt, q3 += qd3 * dt;
	float n = sqrtf(q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3);
	if (n < 1e-6f || !isfinite(n)) {
		dm_init(f, f->beta);
		return;
	}
	f->q[0] = q0 / n, f->q[1] = q1 / n, f->q[2] = q2 / n, f->q[3] = q3 / n;
}

void dm_report3(const struct dm_fusion *f, uint64_t ts, struct dm_report3 *r)
{
	float q0 = f->q[0], q1 = f->q[1], q2 = f->q[2], q3 = f->q[3];
	memset(r, 0, sizeof(*r));
	r->type = 3;
	r->seq_or_ts = ts;
	r->q[0] = q1, r->q[1] = q2, r->q[2] = q3, r->q[3] = q0;
	memcpy(r->rot_rate, f->gyro, sizeof(r->rot_rate));
	/* earth "up" in the device frame; iOS gravity points down (face up: z = -1 g) */
	float ux = 2 * (q1 * q3 - q0 * q2), uy = 2 * (q0 * q1 + q2 * q3), uz = q0 * q0 - q1 * q1 - q2 * q2 + q3 * q3;
	/* iOS accel = -(Android m/s²) / g (the kernel's accsign -1); userAccel = accel - gravity = accel + up */
	r->user_accel[0] = -f->acc[0] / G_MS2 + ux;
	r->user_accel[1] = -f->acc[1] / G_MS2 + uy;
	r->user_accel[2] = -f->acc[2] / G_MS2 + uz;
}
