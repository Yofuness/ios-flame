/* sunfish B46 (kernel 0243): see dm_fusion.h. Madgwick's IMU update (gradient descent, 2010 report), float only. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "dm_fusion.h"

#define G_MS2 9.80665f

void dm_init(struct dm_fusion *f, float beta)
{
	float mn[3], mx[3];	/* dm2: the hard-iron box survives a restart of the fusion */
	int cal = f->have_cal;
	memcpy(mn, f->mag_min, sizeof(mn)), memcpy(mx, f->mag_max, sizeof(mx));
	memset(f, 0, sizeof(*f));
	f->have_cal = cal;
	memcpy(f->mag_min, mn, sizeof(mn)), memcpy(f->mag_max, mx, sizeof(mx));
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

/* dm2: calibrated, normalised mag in m when the box is usable (accuracy >= 0) */
static int dm_mag_cal(const struct dm_fusion *f, float m[3])
{
	if (!f->have_mag || dm_mag_accuracy(f) < 0)
		return 0;
	float b[3], sp[3];
	dm_mag_bias(f, b, sp);
	for (int i = 0; i < 3; i++)
		m[i] = f->mag[i] - b[i];
	float n = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
	if (n < 1.0f)
		return 0;
	m[0] /= n, m[1] /= n, m[2] /= n;
	return 1;
}

/* dm2: whole attitude from accel (up) and calibrated mag (north) at once (TRIAD), so yaw need not converge */
static void dm_seed_mag(struct dm_fusion *f)
{
	float m[3], u[3], w[3], n[3];
	float an = sqrtf(f->acc[0] * f->acc[0] + f->acc[1] * f->acc[1] + f->acc[2] * f->acc[2]);
	if (an < 1e-3f || !dm_mag_cal(f, m))
		return;
	for (int i = 0; i < 3; i++)
		u[i] = f->acc[i] / an;
	w[0] = u[1] * m[2] - u[2] * m[1], w[1] = u[2] * m[0] - u[0] * m[2], w[2] = u[0] * m[1] - u[1] * m[0];	/* west = up x m */
	float wn = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
	if (wn < 1e-3f)
		return;
	w[0] /= wn, w[1] /= wn, w[2] /= wn;
	n[0] = w[1] * u[2] - w[2] * u[1], n[1] = w[2] * u[0] - w[0] * u[2], n[2] = w[0] * u[1] - w[1] * u[0];	/* north = west x up */
	/* R (device -> earth) has rows north, west, up; quaternion from R */
	float r00 = n[0], r01 = n[1], r02 = n[2], r10 = w[0], r11 = w[1], r12 = w[2], r20 = u[0], r21 = u[1], r22 = u[2];
	float tr = r00 + r11 + r22, q0, q1, q2, q3;
	if (tr > 0) {
		float s = sqrtf(tr + 1.0f) * 2;
		q0 = 0.25f * s, q1 = (r21 - r12) / s, q2 = (r02 - r20) / s, q3 = (r10 - r01) / s;
	} else if (r00 > r11 && r00 > r22) {
		float s = sqrtf(1.0f + r00 - r11 - r22) * 2;
		q0 = (r21 - r12) / s, q1 = 0.25f * s, q2 = (r01 + r10) / s, q3 = (r02 + r20) / s;
	} else if (r11 > r22) {
		float s = sqrtf(1.0f + r11 - r00 - r22) * 2;
		q0 = (r02 - r20) / s, q1 = (r01 + r10) / s, q2 = 0.25f * s, q3 = (r12 + r21) / s;
	} else {
		float s = sqrtf(1.0f + r22 - r00 - r11) * 2;
		q0 = (r10 - r01) / s, q1 = (r02 + r20) / s, q2 = (r12 + r21) / s, q3 = 0.25f * s;
	}
	float qn = sqrtf(q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3);
	f->q[0] = q0 / qn, f->q[1] = q1 / qn, f->q[2] = q2 / qn, f->q[3] = q3 / qn;
	f->mag_seeded = 1;
}

int dm_mag(struct dm_fusion *f, const float m[3])
{
	float n = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
	if (!isfinite(n) || n > DM_MAG_MAX)
		return -1;
	if (f->have_mag) {
		float d0 = m[0] - f->mag[0], d1 = m[1] - f->mag[1], d2 = m[2] - f->mag[2];
		if (sqrtf(d0 * d0 + d1 * d1 + d2 * d2) > DM_MAG_JUMP) {
			memcpy(f->mag, m, sizeof(f->mag));	/* the next sample is judged against this one */
			return -1;
		}
	}
	memcpy(f->mag, m, sizeof(f->mag));
	if (f->have_mag) {	/* the box grows from the second consistent sample */
		for (int i = 0; i < 3; i++) {
			if (!f->have_cal || m[i] < f->mag_min[i])
				f->mag_min[i] = m[i];
			if (!f->have_cal || m[i] > f->mag_max[i])
				f->mag_max[i] = m[i];
		}
		f->have_cal = 1;
	}
	f->have_mag = 1;
	if (!f->mag_seeded && f->have_acc)
		dm_seed_mag(f);
	return 0;
}

int dm_mag_accuracy(const struct dm_fusion *f)
{
	if (!f->have_cal)
		return -1;
	float lo = 1e9f;
	for (int i = 0; i < 3; i++)
		if (f->mag_max[i] - f->mag_min[i] < lo)
			lo = f->mag_max[i] - f->mag_min[i];
	return lo >= DM_MAG_SPAN_HIGH ? 2 : lo >= DM_MAG_SPAN_MED ? 1 : lo >= DM_MAG_SPAN_LOW ? 0 : -1;
}

void dm_mag_bias(const struct dm_fusion *f, float b[3], float span[3])
{
	for (int i = 0; i < 3; i++) {
		b[i] = f->have_cal ? (f->mag_max[i] + f->mag_min[i]) / 2 : 0;
		span[i] = f->have_cal ? f->mag_max[i] - f->mag_min[i] : 0;
	}
}

/* "min x y z max x y z" in uT */
int dm_magcal_load(struct dm_fusion *f, const char *path)
{
	FILE *fp = fopen(path, "r");
	float a[6];
	if (!fp)
		return -1;
	int n = fscanf(fp, "min %f %f %f max %f %f %f", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]);
	fclose(fp);
	if (n != 6)
		return -1;
	for (int i = 0; i < 3; i++)
		if (!isfinite(a[i]) || !isfinite(a[i + 3]) || a[i] > a[i + 3])
			return -1;
	memcpy(f->mag_min, a, sizeof(f->mag_min));
	memcpy(f->mag_max, a + 3, sizeof(f->mag_max));
	f->have_cal = 1;
	return 0;
}

int dm_magcal_save(const struct dm_fusion *f, const char *path)
{
	char tmp[256];
	if (!f->have_cal || snprintf(tmp, sizeof(tmp), "%s.new", path) >= (int)sizeof(tmp))
		return -1;
	FILE *fp = fopen(tmp, "w");
	if (!fp)
		return -1;
	fprintf(fp, "min %.3f %.3f %.3f max %.3f %.3f %.3f\n", f->mag_min[0], f->mag_min[1], f->mag_min[2],
	    f->mag_max[0], f->mag_max[1], f->mag_max[2]);
	if (fclose(fp))
		return -1;
	return rename(tmp, path);
}

void dm_gyro(struct dm_fusion *f, const float g[3], uint64_t ts, double ticks_per_s)
{
	memcpy(f->gyro, g, sizeof(f->gyro));
	f->have_gyro = 1;
	if (!f->mag_seeded && f->have_acc && f->have_mag)
		dm_seed_mag(f);	/* dm2: calibration just reached accuracy 0 */
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
	float m[3];
	if (f->have_acc && an > 1e-3f && dm_mag_cal(f, m)) {
		/* dm2: Madgwick's MARG update (x-io MadgwickAHRS.c): earth field b = (bx, 0, bz), x = magnetic north */
		float mx = m[0], my = m[1], mz = m[2];
		ax /= an, ay /= an, az /= an;
		float _2q0mx = 2 * q0 * mx, _2q0my = 2 * q0 * my, _2q0mz = 2 * q0 * mz, _2q1mx = 2 * q1 * mx;
		float _2q0 = 2 * q0, _2q1 = 2 * q1, _2q2 = 2 * q2, _2q3 = 2 * q3, _2q0q2 = 2 * q0 * q2, _2q2q3 = 2 * q2 * q3;
		float q0q0 = q0 * q0, q0q1 = q0 * q1, q0q2 = q0 * q2, q0q3 = q0 * q3, q1q1 = q1 * q1, q1q2 = q1 * q2;
		float q1q3 = q1 * q3, q2q2 = q2 * q2, q2q3 = q2 * q3, q3q3 = q3 * q3;
		float hx = mx * q0q0 - _2q0my * q3 + _2q0mz * q2 + mx * q1q1 + _2q1 * my * q2 + _2q1 * mz * q3 - mx * q2q2 - mx * q3q3;
		float hy = _2q0mx * q3 + my * q0q0 - _2q0mz * q1 + _2q1mx * q2 - my * q1q1 + my * q2q2 + _2q2 * mz * q3 - my * q3q3;
		float _2bx = sqrtf(hx * hx + hy * hy);
		float _2bz = -_2q0mx * q2 + _2q0my * q1 + mz * q0q0 + _2q1mx * q3 - mz * q1q1 + _2q2 * my * q3 - mz * q2q2 + mz * q3q3;
		float _4bx = 2 * _2bx, _4bz = 2 * _2bz;
		float ea = 2 * q1q3 - _2q0q2 - ax, eb = 2 * q0q1 + _2q2q3 - ay, ec = 1 - 2 * q1q1 - 2 * q2q2 - az;
		float ex = _2bx * (0.5f - q2q2 - q3q3) + _2bz * (q1q3 - q0q2) - mx;
		float ey = _2bx * (q1q2 - q0q3) + _2bz * (q0q1 + q2q3) - my;
		float ez = _2bx * (q0q2 + q1q3) + _2bz * (0.5f - q1q1 - q2q2) - mz;
		float s0 = -_2q2 * ea + _2q1 * eb - _2bz * q2 * ex + (-_2bx * q3 + _2bz * q1) * ey + _2bx * q2 * ez;
		float s1 = _2q3 * ea + _2q0 * eb - 4 * q1 * ec + _2bz * q3 * ex + (_2bx * q2 + _2bz * q0) * ey +
		    (_2bx * q3 - _4bz * q1) * ez;
		float s2 = -_2q0 * ea + _2q3 * eb - 4 * q2 * ec + (-_4bx * q2 - _2bz * q0) * ex + (_2bx * q1 + _2bz * q3) * ey +
		    (_2bx * q0 - _4bz * q2) * ez;
		float s3 = _2q1 * ea + _2q2 * eb + (-_4bx * q3 + _2bz * q1) * ex + (-_2bx * q0 + _2bz * q2) * ey + _2bx * q1 * ez;
		float sn = sqrtf(s0 * s0 + s1 * s1 + s2 * s2 + s3 * s3);
		if (sn > 1e-9f) {
			qd0 -= f->beta * s0 / sn;
			qd1 -= f->beta * s1 / sn;
			qd2 -= f->beta * s2 / sn;
			qd3 -= f->beta * s3 / sn;
		}
	} else if (f->have_acc && an > 1e-3f) {
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

/* q_out = qz(-yaw) * q: the device turns by +yaw about earth z as seen from the reference, heading += yaw */
static void dm_out_q(const struct dm_fusion *f, float yaw_deg, float out[4])
{
	float h = -yaw_deg * (float)M_PI / 360.0f, c = cosf(h), s = sinf(h);
	float q0 = f->q[0], q1 = f->q[1], q2 = f->q[2], q3 = f->q[3];
	float w = c * q0 - s * q3, x = c * q1 - s * q2, y = c * q2 + s * q1, z = c * q3 + s * q0;
	out[0] = x, out[1] = y, out[2] = z, out[3] = w;
}

static void dm_user_accel(const struct dm_fusion *f, float ua[3])
{
	float q0 = f->q[0], q1 = f->q[1], q2 = f->q[2], q3 = f->q[3];
	/* earth "up" in the device frame; iOS gravity points down (face up: z = -1 g) */
	float ux = 2 * (q1 * q3 - q0 * q2), uy = 2 * (q0 * q1 + q2 * q3), uz = q0 * q0 - q1 * q1 - q2 * q2 + q3 * q3;
	/* iOS accel = -(Android m/s²) / g (the kernel's accsign -1); userAccel = accel - gravity = accel + up */
	ua[0] = -f->acc[0] / G_MS2 + ux;
	ua[1] = -f->acc[1] / G_MS2 + uy;
	ua[2] = -f->acc[2] / G_MS2 + uz;
}

void dm_report3(const struct dm_fusion *f, uint64_t ts, float yaw_deg, struct dm_report3 *r)
{
	float q[4], ua[3];	/* packed struct: fill through aligned locals */
	memset(r, 0, sizeof(*r));
	r->type = 3;
	r->seq_or_ts = ts;
	dm_out_q(f, yaw_deg, q);
	dm_user_accel(f, ua);
	memcpy(r->q, q, sizeof(q));
	memcpy(r->rot_rate, f->gyro, sizeof(r->rot_rate));
	memcpy(r->user_accel, ua, sizeof(ua));
}

void dm_report2(const struct dm_fusion *f, uint64_t ts, float yaw_deg, struct dm_report2 *r)
{
	float span[3], q[4], ua[3], b[3];
	int acc = dm_mag_accuracy(f);
	memset(r, 0, sizeof(*r));
	r->type = 2;
	r->mag_accuracy = acc < 0 ? 0xff : (uint8_t)acc;
	r->seq_or_ts = ts;
	memcpy(r->mag_raw, f->mag, sizeof(r->mag_raw));
	dm_out_q(f, yaw_deg, q);
	dm_user_accel(f, ua);
	dm_mag_bias(f, b, span);
	memcpy(r->q, q, sizeof(q));
	memcpy(r->rot_rate, f->gyro, sizeof(r->rot_rate));
	memcpy(r->user_accel, ua, sizeof(ua));
	memcpy(r->mag_bias, b, sizeof(b));
}
