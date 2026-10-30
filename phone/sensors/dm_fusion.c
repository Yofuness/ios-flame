/* sunfish B46 (kernel 0243): see dm_fusion.h. Madgwick's IMU update (gradient descent, 2010 report), float only. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "dm_fusion.h"

#define G_MS2 9.80665f

void dm_init(struct dm_fusion *f, float beta)
{
	struct dm_magcal cal = f->cal;	/* dm7: the fit (and its ring) survives a restart of the fusion */
	float smb[3], sgb[3];	/* dm5: and SEE's biases (on-change: they may not come again) */
	float gb[3];		/* dm9: and our gyro bias */
	int gbh = f->beta != 0 && f->gbias_have;
	memcpy(gb, f->gbias, sizeof(gb));
	int init = f->beta != 0, sms = f->see_mag_st, sgs = f->see_gyro_st;
	memcpy(smb, f->see_mag_bias, sizeof(smb)), memcpy(sgb, f->see_gyro_bias, sizeof(sgb));
	memset(f, 0, sizeof(*f));
	if (init)
		f->cal = cal;
	else
		f->cal.heading_tc = DM_HEADING_TC;
	f->see_mag_st = init ? sms : -1, f->see_gyro_st = init ? sgs : -1;
	memcpy(f->see_mag_bias, smb, sizeof(smb)), memcpy(f->see_gyro_bias, sgb, sizeof(sgb));
	if (gbh)
		memcpy(f->gbias, gb, sizeof(gb)), f->gbias_have = 1;
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

/* dm2: calibrated, normalised mag in m when the calibration is usable (accuracy >= 0) */
static int dm_mag_cal(const struct dm_fusion *f, float m[3])
{
	if (!f->have_mag_use || dm_mag_accuracy(f) < 0)
		return 0;
	float b[3];
	dm_mag_bias(f, b);
	for (int i = 0; i < 3; i++)
		m[i] = f->mag_use[i] - b[i];	/* dm8: the last sample that passed the gate */
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

/* dm7: |m|^2 = 2 c.m + k (k = r^2 - |c|^2) is linear in (c, k): 4x4 normal equations, Gaussian elimination */
int dm_sphere_fit(const float (*m)[3], int n, float c[3], float *r, float *res)
{
	double A[4][5] = { { 0 } };
	for (int s = 0; s < n; s++) {
		double row[4] = { 2 * m[s][0], 2 * m[s][1], 2 * m[s][2], 1 };
		double y = (double)m[s][0] * m[s][0] + (double)m[s][1] * m[s][1] + (double)m[s][2] * m[s][2];
		for (int i = 0; i < 4; i++) {
			for (int j = 0; j < 4; j++)
				A[i][j] += row[i] * row[j];
			A[i][4] += row[i] * y;
		}
	}
	for (int i = 0; i < 4; i++) {
		int p = i;
		for (int k = i + 1; k < 4; k++)
			if (fabs(A[k][i]) > fabs(A[p][i]))
				p = k;
		if (fabs(A[p][i]) < 1e-9)
			return -1;
		for (int j = 0; j < 5; j++) {
			double t = A[i][j];
			A[i][j] = A[p][j], A[p][j] = t;
		}
		for (int k = 0; k < 4; k++) {
			if (k == i)
				continue;
			double fct = A[k][i] / A[i][i];
			for (int j = i; j < 5; j++)
				A[k][j] -= fct * A[i][j];
		}
	}
	double x[4];
	for (int i = 0; i < 4; i++)
		x[i] = A[i][4] / A[i][i];
	double r2 = x[3] + x[0] * x[0] + x[1] * x[1] + x[2] * x[2];
	if (!(r2 > 0))
		return -1;
	double rr = sqrt(r2), e2 = 0;
	for (int s = 0; s < n; s++) {
		double d0 = m[s][0] - x[0], d1 = m[s][1] - x[1], d2 = m[s][2] - x[2];
		double e = sqrt(d0 * d0 + d1 * d1 + d2 * d2) - rr;
		e2 += e * e;
	}
	c[0] = (float)x[0], c[1] = (float)x[1], c[2] = (float)x[2];
	*r = (float)rr, *res = (float)sqrt(e2 / n);
	return 0;
}

/* dm7: fit the ring when it covers enough orientations; take the fit as the bias (big steps need two fits) */
static void dm_mag_fit(struct dm_magcal *k)
{
	float lo[3], hi[3], c[3], r, res, sp = 1e9f;
	for (int i = 0; i < 3; i++)
		lo[i] = 1e9f, hi[i] = -1e9f;
	for (int s = 0; s < k->ring_n; s++)
		for (int i = 0; i < 3; i++) {
			lo[i] = k->ring[s][i] < lo[i] ? k->ring[s][i] : lo[i];
			hi[i] = k->ring[s][i] > hi[i] ? k->ring[s][i] : hi[i];
		}
	for (int i = 0; i < 3; i++)
		sp = hi[i] - lo[i] < sp ? hi[i] - lo[i] : sp;
	if (sp < DM_MAG_COVER || dm_sphere_fit((const float (*)[3])k->ring, k->ring_n, c, &r, &res) ||
	    r < DM_MAG_R_MIN || r > DM_MAG_R_MAX || !(res < DM_MAG_RES_MAX))
		return;
	if (k->have_cal) {
		float d0 = c[0] - k->c[0], d1 = c[1] - k->c[1], d2 = c[2] - k->c[2];
		if (sqrtf(d0 * d0 + d1 * d1 + d2 * d2) > DM_MAG_STEP) {
			float e0 = c[0] - k->pend[0], e1 = c[1] - k->pend[1], e2 = c[2] - k->pend[2];
			int agree = k->have_pend && sqrtf(e0 * e0 + e1 * e1 + e2 * e2) <= DM_MAG_STEP;
			memcpy(k->pend, c, sizeof(k->pend));
			k->have_pend = !agree;
			if (!agree)
				return;
		}
	}
	k->have_pend = 0;
	memcpy(k->c, c, sizeof(k->c));
	k->r = r, k->res = res, k->n = k->ring_n;
	k->acc = res < DM_MAG_RES_GOOD && sp >= DM_MAG_COVER_GOOD ? 2 : 1;
	k->have_cal = 1;
}

/* dm8: 1 = use this sample for the correction. Needs a calibration and accel; dips of all samples feed the median. */
static int dm_mag_gate(struct dm_fusion *f, const float m[3])
{
	float b[3], c[3];
	float an = sqrtf(f->acc[0] * f->acc[0] + f->acc[1] * f->acc[1] + f->acc[2] * f->acc[2]);
	/* not before the seed: the seed must come from the current sample, not an older one that passed */
	if (f->cal.gate_off || !f->mag_seeded || dm_mag_accuracy(f) < 0 || !f->have_acc || an < 1e-3f)
		return 1;
	dm_mag_bias(f, b);
	for (int i = 0; i < 3; i++)
		c[i] = m[i] - b[i];
	float n = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
	if (n < 1.0f)
		return 1;
	float cd = (c[0] * f->acc[0] + c[1] * f->acc[1] + c[2] * f->acc[2]) / (n * an);
	float dip = acosf(cd > 1 ? 1 : cd < -1 ? -1 : cd) * 180.0f / (float)M_PI, med = 0;
	int ok = 1;
	if (f->dip_n >= 5) {	/* median of the ring (small: insertion sort of a copy) */
		float t[DM_MAG_DIP_N];
		memcpy(t, f->dip, sizeof(t));
		for (int i = 1; i < f->dip_n; i++)
			for (int j = i; j > 0 && t[j - 1] > t[j]; j--) {
				float x = t[j];
				t[j] = t[j - 1], t[j - 1] = x;
			}
		med = t[f->dip_n / 2];
	}
	f->dip[f->dip_i] = dip;
	f->dip_i = (f->dip_i + 1) % DM_MAG_DIP_N;
	f->dip_n += f->dip_n < DM_MAG_DIP_N;
	if (!dm_mag_cal_dsp(f) && f->cal.r > 0 && fabsf(n - f->cal.r) > DM_MAG_GATE_R * f->cal.r)
		ok = 0, f->rej_r++;
	else if (f->dip_n > 5 && fabsf(dip - med) > DM_MAG_GATE_DIP)
		ok = 0, f->rej_dip++;
	if (ok || ++f->rej_streak > DM_MAG_GATE_STREAK)
		return f->rej_streak = 0, 1;
	return 0;
}

int dm_mag(struct dm_fusion *f, const float m[3])
{
	return dm_mag_ts(f, m, 0);
}

int dm_type_auto(const struct dm_fusion *f, uint64_t ts, double hold_s)
{
	return f->mag_raw_ts && ts >= f->mag_raw_ts && (double)(ts - f->mag_raw_ts) < hold_s * 19200000.0 ? 2 : 3;
}

int dm_mag_ts(struct dm_fusion *f, const float m[3], uint64_t ts)
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
	struct dm_magcal *k = &f->cal;
	if (f->have_mag) {	/* dm7: the ring takes consistent samples, decimated so a still phone can't flood it */
		const float *l = k->ring[(k->ring_i + DM_MAG_RING - 1) % DM_MAG_RING];
		float d0 = m[0] - l[0], d1 = m[1] - l[1], d2 = m[2] - l[2];
		if (!k->ring_n || sqrtf(d0 * d0 + d1 * d1 + d2 * d2) > DM_MAG_DECIM) {
			memcpy(k->ring[k->ring_i], m, sizeof(k->ring[0]));
			k->ring_i = (k->ring_i + 1) % DM_MAG_RING;
			k->ring_n += k->ring_n < DM_MAG_RING;
			if (++k->ring_new >= DM_MAG_FIT_EVERY && k->ring_n >= DM_MAG_FIT_MIN) {
				k->ring_new = 0;
				dm_mag_fit(k);
			}
		}
	}
	f->have_mag = 1;
	f->mag_raw_ts = ts;
	if (dm_mag_gate(f, m)) {	/* dm8 */
		memcpy(f->mag_use, m, sizeof(f->mag_use));
		f->have_mag_use = 1, f->mag_use_ts = ts;
		f->mag_rej = 0;
	} else {
		f->mag_rej = 1;	/* dm9 */
	}
	if (!f->mag_seeded && f->have_acc)
		dm_seed_mag(f);
	return 0;
}

void dm_mag_cal_bias(struct dm_fusion *f, const float b[3], int status)
{
	memcpy(f->see_mag_bias, b, sizeof(f->see_mag_bias));
	f->see_mag_st = status;
	if (dm_mag_cal_dsp(f) && !f->mag_seeded && f->have_acc && f->have_mag)
		dm_seed_mag(f);
}

void dm_gyro_cal_bias(struct dm_fusion *f, const float b[3], int status)
{
	memcpy(f->see_gyro_bias, b, sizeof(f->see_gyro_bias));
	f->see_gyro_st = status;
}

int dm_mag_cal_dsp(const struct dm_fusion *f)
{
	return f->see_mag_st >= 1;
}

int dm_mag_accuracy(const struct dm_fusion *f)
{
	if (dm_mag_cal_dsp(f))
		return f->see_mag_st > 3 ? 2 : f->see_mag_st - 1;	/* dm5: SEE status 1/2/3 -> 0/1/2 */
	return f->cal.have_cal ? f->cal.acc : -1;
}

void dm_mag_bias(const struct dm_fusion *f, float b[3])
{
	for (int i = 0; i < 3; i++)
		b[i] = dm_mag_cal_dsp(f) ? f->see_mag_bias[i] : f->cal.have_cal ? f->cal.c[i] : 0;
}

/* dm7: "fit cx cy cz r res acc" (uT); the dm2 "min ... max ..." box is ignored, so a polluted box can't come back */
int dm_magcal_load(struct dm_fusion *f, const char *path)
{
	FILE *fp = fopen(path, "r");
	float a[5];
	int acc;
	if (!fp)
		return -1;
	int n = fscanf(fp, "fit %f %f %f %f %f %d", &a[0], &a[1], &a[2], &a[3], &a[4], &acc);
	fclose(fp);
	if (n != 6)
		return -1;
	for (int i = 0; i < 5; i++)
		if (!isfinite(a[i]) || fabsf(a[i]) > DM_MAG_MAX)
			return -1;
	if (a[3] < DM_MAG_R_MIN || a[3] > DM_MAG_R_MAX || !(a[4] < DM_MAG_RES_MAX) || acc < 1 || acc > 2)
		return -1;
	memcpy(f->cal.c, a, sizeof(f->cal.c));
	f->cal.r = a[3], f->cal.res = a[4], f->cal.acc = acc, f->cal.n = 0;
	f->cal.have_cal = 1;
	return 0;
}

int dm_magcal_save(const struct dm_fusion *f, const char *path)
{
	char tmp[256];
	if (!f->cal.have_cal || snprintf(tmp, sizeof(tmp), "%s.new", path) >= (int)sizeof(tmp))
		return -1;
	FILE *fp = fopen(tmp, "w");
	if (!fp)
		return -1;
	fprintf(fp, "fit %.3f %.3f %.3f %.3f %.3f %d\n", f->cal.c[0], f->cal.c[1], f->cal.c[2], f->cal.r, f->cal.res,
	    f->cal.acc);
	if (fclose(fp))
		return -1;
	return rename(tmp, path);
}

/* dm9: the still detector, and the gyro bias while still */
static void dm_still_step(struct dm_fusion *f, const float gr[3], float dt)
{
	float an = sqrtf(f->acc[0] * f->acc[0] + f->acc[1] * f->acc[1] + f->acc[2] * f->acc[2]);
	int calm = f->have_acc && an > 1e-3f && fabsf(gr[0]) < DM_STILL_GYRO && fabsf(gr[1]) < DM_STILL_GYRO &&
	    fabsf(gr[2]) < DM_STILL_GYRO;
	if (calm && f->still_t > 0 && fabsf(an - f->still_acc / f->still_t) > DM_STILL_ACC * f->still_acc / f->still_t)
		calm = 0;
	if (!calm) {
		f->still = 0, f->still_t = 0, f->still_acc = 0;
		memset(f->still_sum, 0, sizeof(f->still_sum));
		return;
	}
	f->still_t += dt, f->still_acc += an * dt;
	for (int i = 0; i < 3; i++)
		f->still_sum[i] += gr[i] * dt;
	if (f->still_t < DM_STILL_S)
		return;
	f->still = 1;
	if (f->cal.gbias_off)
		return;
	float a = 1.0f - expf(-dt / DM_GBIAS_TC);
	for (int i = 0; i < 3; i++)
		f->gbias[i] = f->gbias_have ? f->gbias[i] + a * (gr[i] - f->gbias[i]) : f->still_sum[i] / f->still_t;
	f->gbias_have = 1;
	if (f->still_t > 1e4f)	/* keep the float sums precise; the mean of |accel| restarts from itself */
		f->still_acc *= 0.5f, f->still_t *= 0.5f;
}

/* dm9: up in the device frame: low-passed accel, or q's while |accel| is far from 1 g */
static void dm_grav_step(struct dm_fusion *f, float dt)
{
	float q0 = f->q[0], q1 = f->q[1], q2 = f->q[2], q3 = f->q[3];
	float s[3] = { 2 * (q1 * q3 - q0 * q2), 2 * (q0 * q1 + q2 * q3), q0 * q0 - q1 * q1 - q2 * q2 + q3 * q3 };
	float an = sqrtf(f->acc[0] * f->acc[0] + f->acc[1] * f->acc[1] + f->acc[2] * f->acc[2]);
	if (f->have_acc && fabsf(an - G_MS2) <= DM_GRAV_DEV * G_MS2) {
		float a = f->grav_have ? 1.0f - expf(-dt / DM_GRAV_TC) : 1.0f;
		for (int i = 0; i < 3; i++)
			s[i] = f->grav[i] + a * (f->acc[i] / an - f->grav[i]);
	}
	float n = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
	if (n < 1e-3f || !isfinite(n))
		return;
	f->grav[0] = s[0] / n, f->grav[1] = s[1] / n, f->grav[2] = s[2] / n;
	f->grav_have = 1;
}

int dm_heading_mag(const float u[3], const float m[3], float *deg)
{
	float w[3], n[3];
	w[0] = u[1] * m[2] - u[2] * m[1], w[1] = u[2] * m[0] - u[0] * m[2], w[2] = u[0] * m[1] - u[1] * m[0];	/* west = up x m */
	if (sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) < 1e-3f)
		return -1;
	n[0] = w[1] * u[2] - w[2] * u[1], n[1] = w[2] * u[0] - w[0] * u[2], n[2] = w[0] * u[1] - w[1] * u[0];	/* north = west x up */
	/* the device top (y), or the back (-z) when the top is near vertical; same convention as dm_heading_deg */
	int top = fabsf(u[1]) < cosf(DM_HC_UPRIGHT * (float)M_PI / 180.0f);
	float h = atan2f(top ? -w[1] : w[2], top ? n[1] : -n[2]) * 180.0f / (float)M_PI;
	*deg = h < 0 ? h + 360 : h;
	return 0;
}

/* dm9: one complementary-filter step: gyro about world z (clockwise from above raises the heading), then the mag */
static void dm_heading_step(struct dm_fusion *f, float dt, int stale)
{
	float m[3], hm;
	dm_grav_step(f, dt);
	if (!f->grav_have)
		return;
	const float *u = f->grav, *g = f->gyro;
	if (f->hc_have)
		f->hc_h -= (u[0] * g[0] + u[1] * g[1] + u[2] * g[2]) * dt * 180.0f / (float)M_PI;
	f->hc_hmag_ok = 0;
	if (!stale && !f->mag_rej && dm_mag_cal(f, m) && !dm_heading_mag(u, m, &hm)) {
		f->hc_hmag = hm, f->hc_hmag_ok = 1;
		if (!f->hc_have) {
			f->hc_h = hm, f->hc_have = 1;
		} else {
			int fast = fabsf(g[0]) >= DM_HC_FAST || fabsf(g[1]) >= DM_HC_FAST || fabsf(g[2]) >= DM_HC_FAST;
			float d = fmodf(fmodf(hm - f->hc_h, 360.0f) + 540.0f, 360.0f) - 180.0f;
			f->hc_h += (fast ? DM_HC_K_FAST : DM_HC_K_SLOW) * d * dt;
		}
	}
	f->hc_h = fmodf(fmodf(f->hc_h, 360.0f) + 360.0f, 360.0f);
	if (!isfinite(f->hc_h))
		f->hc_have = 0;
}

void dm_gyro(struct dm_fusion *f, const float gr[3], uint64_t ts, double ticks_per_s)
{
	float g[3];	/* dm5: minus SEE's gyro_cal bias once it has one (status >= 1); dm9: else our own */
	for (int i = 0; i < 3; i++)
		g[i] = gr[i] - (f->see_gyro_st >= 1 ? f->see_gyro_bias[i] : f->gbias[i]);
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
	dm_still_step(f, gr, dt);	/* dm9 */
	float beta = f->still && !f->cal.hc_off ? DM_BETA_STILL : f->beta;
	float q0 = f->q[0], q1 = f->q[1], q2 = f->q[2], q3 = f->q[3];
	float gx = g[0], gy = g[1], gz = g[2];
	float qd0 = 0.5f * (-q1 * gx - q2 * gy - q3 * gz);
	float qd1 = 0.5f * (q0 * gx + q2 * gz - q3 * gy);
	float qd2 = 0.5f * (q0 * gy - q1 * gz + q3 * gx);
	float qd3 = 0.5f * (q0 * gz + q1 * gy - q2 * gx);
	float ax = f->acc[0], ay = f->acc[1], az = f->acc[2];
	float an = sqrtf(ax * ax + ay * ay + az * az);
	float m[3];
	/* dm8: a stale mag (gap) is not used: IMU-only until a fresh one comes */
	int stale = !f->cal.hold_off && f->mag_use_ts && ts > f->mag_use_ts &&
	    (double)(ts - f->mag_use_ts) > DM_MAG_STALE_S * ticks_per_s;
	if (f->have_acc && an > 1e-3f && !stale && dm_mag_cal(f, m)) {
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
			qd0 -= beta * s0 / sn;
			qd1 -= beta * s1 / sn;
			qd2 -= beta * s2 / sn;
			qd3 -= beta * s3 / sn;
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
			qd0 -= beta * s0 / sn;
			qd1 -= beta * s1 / sn;
			qd2 -= beta * s2 / sn;
			qd3 -= beta * s3 / sn;
		}
	}
	q0 += qd0 * dt, q1 += qd1 * dt, q2 += qd2 * dt, q3 += qd3 * dt;
	float n = sqrtf(q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3);
	if (n < 1e-6f || !isfinite(n)) {
		dm_init(f, f->beta);
		return;
	}
	f->q[0] = q0 / n, f->q[1] = q1 / n, f->q[2] = q2 / n, f->q[3] = q3 / n;
	if (!f->cal.hc_off)
		dm_heading_step(f, dt, stale);	/* dm9 */
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

float dm_heading_deg(const float qv[4])
{
	float x = qv[0], y = qv[1], z = qv[2], w = qv[3];
	/* R * (0, 1, 0): device top in the reference frame (x = north, y = west, z = up) */
	float tx = 2 * (x * y - w * z), ty = 1 - 2 * (x * x + z * z);
	float h = atan2f(-ty, tx) * 180.0f / (float)M_PI;	/* east = -y */
	return h < 0 ? h + 360 : h;
}

void dm_report2(struct dm_fusion *f, uint64_t ts, float yaw_deg, struct dm_report2 *r)
{
	float q[4], ua[3], b[3];
	int acc = dm_mag_accuracy(f);
	if (!f->cal.hold_off && f->mag_raw_ts && ts > f->mag_raw_ts && (double)(ts - f->mag_raw_ts) > DM_MAG_HOLD_S * 19200000.0)
		acc = -1;	/* dm8: no mag for 5 s: heading invalid */
	memset(r, 0, sizeof(*r));
	r->type = 2;
	r->mag_accuracy = acc < 0 ? 0xff : (uint8_t)acc;
	r->seq_or_ts = ts;
	memcpy(r->mag_raw, f->mag, sizeof(r->mag_raw));
	dm_out_q(f, yaw_deg, q);
	dm_user_accel(f, ua);
	dm_mag_bias(f, b);
	memcpy(r->q, q, sizeof(q));
	memcpy(r->rot_rate, f->gyro, sizeof(r->rot_rate));
	memcpy(r->user_accel, ua, sizeof(ua));
	memcpy(r->mag_bias, b, sizeof(b));
	/* dm3: locationd takes the heading ready-made from @0x44/@0x48 and needs status bit 8 */
	float h = dm_heading_deg(q), dt = f->hd_have ? (float)((double)(ts - f->hd_ts) / 19200000.0) : 0;
	if (f->hc_have && !f->cal.hc_off) {	/* dm9: smooth already; the 0.3 s low-pass would lag a turn by ~13 deg at 45 deg/s */
		h = fmodf(fmodf(f->hc_h + yaw_deg, 360.0f) + 360.0f, 360.0f);
	} else if (f->cal.heading_tc > 0 && f->hd_have && ts > f->hd_ts && dt < 1.0f) {	/* dm7: wrap-aware low-pass */
		float d = fmodf(h - f->hd_out + 540.0f, 360.0f) - 180.0f;
		h = fmodf(f->hd_out + d * (1.0f - expf(-dt / f->cal.heading_tc)) + 360.0f, 360.0f);
	}
	f->hd_out = h, f->hd_ts = ts, f->hd_have = 1;
	r->heading = h;
	r->heading_acc = acc >= 2 ? 10.0f : acc == 1 ? 20.0f : acc == 0 ? 35.0f : -1.0f;
	r->status = acc >= 0 ? DM_STATUS_HEADING : 0;
}

/* O13: "devmotion8  page 0xff0c usage 0x08 clients 1 ReportInterval 20000 us (set by iOS) ..." (SweetHID status) */
unsigned dm_status_interval(const char *text, const char *name)
{
	size_t n = strlen(name);
	for (const char *l = text; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : NULL) {
		unsigned us;
		const char *ri, *nl = strchr(l, '\n');
		if (strncmp(l, name, n) || l[n] != ' ')
			continue;
		ri = strstr(l, "ReportInterval ");
		if (!ri || (nl && ri > nl) || sscanf(ri, "ReportInterval %u us", &us) != 1)
			return 0;
		const char *set = strstr(ri, "(set by iOS)");
		return set && (!nl || set < nl) ? us : 0;
	}
	return 0;
}

int dm_gate_step(struct dm_gate *g, int interest, long now_ms)
{
	if (interest) {
		g->on = 1;
		g->idle_since = 0;
	} else if (g->on) {
		if (!g->idle_since)
			g->idle_since = now_ms ? now_ms : 1;
		else if (now_ms - g->idle_since >= DM_GATE_HOLD_MS)
			g->on = 0, g->idle_since = 0;
	}
	return g->on;
}
