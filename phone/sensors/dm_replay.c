/*
 * dm10: host replay of /var/local/motion-dump.csv through a fresh dm_fusion, for offline tuning. Usage:
 * dm_replay [--cal FILE] dump.csv. Prints one row per 0.5 s, then a summary; gyro_yaw is independent of the fusion
 * (run-wide bias from the still windows, own gyro-propagated up), the short-term truth for heading CHANGES.
 * Env: SNS_HEADING_GRAVITY=lp|q, SNS_HC_K_SLOW, SNS_HC_K_FAST, SNS_DM_BETA, and the phone's switches SNS_HEADING_LP,
 * SNS_MAG_HOLD, SNS_MAG_GATE, SNS_DM_GBIAS, SNS_HEADING_CF (=0: off). SEE's own biases are not in the dump.
 * Yaw about vertical is path-dependent through a tilted tumble (the offset to h moves there, even with a perfect
 * calibration), so the summary also gives the RMS over the flat stretches, each with its own constant.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dm_fusion.h"

#define TICKS 19200000.0

struct smp {
	double t;
	size_t idx;
	char type;
	float v[3];
};

static struct dm_fusion f;	/* static: zero, so dm_init sets the defaults */

static int cmp(const void *pa, const void *pb)
{
	const struct smp *a = pa, *b = pb;
	if (a->t != b->t)
		return a->t < b->t ? -1 : 1;
	return a->idx < b->idx ? -1 : a->idx > b->idx;
}

static int env_off(const char *n)
{
	const char *e = getenv(n);
	return e && !strcmp(e, "0");
}

static float wrap180(float d)
{
	return fmodf(fmodf(d, 360.0f) + 540.0f, 360.0f) - 180.0f;
}

/* dm10: squared error of one flat stretch about its own circular mean, added to the totals; empties the stretch */
static void flat_end(const float *d, int *n, double *e2, int *tot)
{
	double sn = 0, cs = 0;
	for (int i = 0; i < *n; i++)
		sn += sin(d[i] * M_PI / 180), cs += cos(d[i] * M_PI / 180);
	float mean = (float)(atan2(sn, cs) * 180 / M_PI);
	for (int i = 0; i < *n; i++) {
		float x = wrap180(d[i] - mean);
		*e2 += x * x;
	}
	*tot += *n;
	*n = 0;
}

/* In file order; the clock restarts with a boot (the CSV is appended to): such a part goes after the last one. */
static struct smp *load(const char *path, size_t *np)
{
	FILE *fp = fopen(path, "r");
	if (!fp) {
		perror(path);
		exit(2);
	}
	struct smp *s = NULL;
	size_t n = 0, cap = 0;
	char line[128];
	double prev = 0, off = 0;
	while (fgets(line, sizeof(line), fp)) {
		struct smp x;
		char c;
		if (sscanf(line, "%c,%lf,%f,%f,%f", &c, &x.t, &x.v[0], &x.v[1], &x.v[2]) != 5 || (c != 'a' && c != 'g' && c != 'm'))
			continue;	/* h rows, a line cut short */
		if (n && x.t + off < prev - 5.0)
			off = prev + 1.0 - x.t;
		x.t += off, prev = x.t;
		x.type = c, x.idx = n;
		if (n == cap) {
			cap = cap ? cap * 2 : 65536;
			s = realloc(s, cap * sizeof(*s));
			if (!s)
				exit(2);
		}
		s[n++] = x;
	}
	fclose(fp);
	qsort(s, n, sizeof(*s), cmp);
	*np = n;
	return s;
}

/* Mean raw gyro over every window of >= 2 s with all axes below the still limit; 0 when there is none. */
static double still_bias(const struct smp *s, size_t n, float b[3])
{
	double sum[3] = { 0 }, win[3] = { 0 }, tot = 0, wt = 0, last = -1;
	for (size_t i = 0; i <= n; i++) {
		int end = i == n;
		if (!end && s[i].type != 'g')
			continue;
		int calm = !end;
		for (int k = 0; calm && k < 3; k++)
			calm = fabsf(s[i].v[k]) < DM_STILL_GYRO;
		double dt = !end && last >= 0 ? s[i].t - last : 0;
		if (!end)
			last = s[i].t;
		if (calm && dt < 0.1) {
			wt += dt;
			for (int k = 0; k < 3; k++)
				win[k] += s[i].v[k] * dt;
			continue;
		}
		if (wt >= DM_STILL_S) {
			tot += wt;
			for (int k = 0; k < 3; k++)
				sum[k] += win[k];
		}
		wt = 0;
		memset(win, 0, sizeof(win));
	}
	for (int k = 0; k < 3; k++)
		b[k] = tot > 0 ? (float)(sum[k] / tot) : 0;
	return tot;
}

int main(int argc, char **argv)
{
	const char *cal = NULL, *path = NULL, *e;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--cal") && i + 1 < argc)
			cal = argv[++i];
		else
			path = argv[i];
	}
	if (!path) {
		fprintf(stderr, "usage: dm_replay [--cal FILE] dump.csv\n");
		return 2;
	}
	e = getenv("SNS_DM_BETA");
	dm_init(&f, e ? (float)atof(e) : 0.04f);
	if (env_off("SNS_HEADING_LP"))
		f.cal.heading_tc = 0;
	f.cal.hold_off = env_off("SNS_MAG_HOLD"), f.cal.gate_off = env_off("SNS_MAG_GATE");
	f.cal.gbias_off = env_off("SNS_DM_GBIAS"), f.cal.hc_off = env_off("SNS_HEADING_CF");
	e = getenv("SNS_HEADING_GRAVITY");
	f.cal.grav_q = e && !strcmp(e, "q");
	if ((e = getenv("SNS_HC_K_SLOW")))
		f.cal.hc_k_slow = (float)atof(e);
	if ((e = getenv("SNS_HC_K_FAST")))
		f.cal.hc_k_fast = (float)atof(e);
	if (cal && dm_magcal_load(&f, cal)) {
		fprintf(stderr, "%s: no usable calibration\n", cal);
		return 2;
	}
	size_t n;
	struct smp *s = load(path, &n);
	float tb[3], u[3] = { 0 }, yaw = 0, h = 0;
	double still_s = still_bias(s, n, tb), t0 = n ? s[0].t : 0, last_g = -1, last_rep = -1, next_row = t0;
	int have_u = 0, have_h = 0;
	float d_prev = 0, h_prev = 0, step_still = 0, step_move = 0, sdiff_still = 0, sdiff_move = 0;
	double sin_sum = 0, cos_sum = 0;
	int row_prev = 0, prev_still = 0, nv = 0, capv = 0;
	float *dv = NULL, *fl = NULL;
	int nfl = 0, flat_tot = 0;
	double flat_e2 = 0;
	printf("T h hmag gyro_yaw fit acc r res still\n");
	for (size_t i = 0; i < n; i++) {
		const struct smp *x = &s[i];
		uint64_t ts = (uint64_t)((x->t - t0 + 1.0) * TICKS);
		if (x->type == 'a') {
			dm_accel(&f, x->v);
			continue;
		}
		if (x->type == 'm') {
			dm_mag_ts(&f, x->v, ts);
			continue;
		}
		dm_gyro(&f, x->v, ts, TICKS);
		float dt = last_g >= 0 ? (float)(x->t - last_g) : 0, an;
		last_g = x->t;
		dt = dt > 0.1f ? 0.1f : dt;
		an = sqrtf(f.acc[0] * f.acc[0] + f.acc[1] * f.acc[1] + f.acc[2] * f.acc[2]);
		if (f.have_acc && an > 1e-3f) {
			float g[3] = { x->v[0] - tb[0], x->v[1] - tb[1], x->v[2] - tb[2] }, nu;
			if (!have_u) {
				for (int k = 0; k < 3; k++)
					u[k] = f.acc[k] / an;
				have_u = 1;
			}
			yaw -= (u[0] * g[0] + u[1] * g[1] + u[2] * g[2]) * dt * 180.0f / (float)M_PI;
			float c[3] = { u[1] * g[2] - u[2] * g[1], u[2] * g[0] - u[0] * g[2], u[0] * g[1] - u[1] * g[0] };	/* du/dt = u x w */
			float a = fabsf(an - 9.80665f) < 0.98f ? 1.0f - expf(-dt / 2.0f) : 0;
			for (int k = 0; k < 3; k++)
				u[k] += c[k] * dt + a * (f.acc[k] / an - u[k]);
			nu = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
			for (int k = 0; k < 3; k++)
				u[k] /= nu;
		}
		if (!f.have_acc || (last_rep >= 0 && x->t - last_rep < 0.01))
			continue;
		last_rep = x->t;
		struct dm_report2 r;
		dm_report2(&f, ts, 0, &r);
		memcpy(&h, (const char *)&r + offsetof(struct dm_report2, heading), sizeof(h));
		uint16_t status;
		memcpy(&status, (const char *)&r + offsetof(struct dm_report2, status), sizeof(status));
		have_h = (status & DM_STATUS_HEADING) && (f.hc_have || f.cal.hc_off);
		if (x->t < next_row)
			continue;
		next_row += 0.5;
		printf("%.2f %.1f %.1f %.1f %s %d %.1f %.2f %d\n", x->t - t0, h, f.hc_hmag_ok ? f.hc_hmag : -1.0f, yaw,
		    dm_mag_cal_dsp(&f) ? "dsp" : f.cal.have_cal ? "fit" : "none", dm_mag_accuracy(&f), f.cal.r, f.cal.res, f.still);
		if (!have_h) {
			row_prev = 0;
			flat_end(fl, &nfl, &flat_e2, &flat_tot);
			continue;
		}
		float d = wrap180(h - yaw);
		sin_sum += sin(d * M_PI / 180), cos_sum += cos(d * M_PI / 180);
		if (nv == capv) {
			capv = capv ? capv * 2 : 4096;
			dv = realloc(dv, (size_t)capv * sizeof(*dv));
			fl = realloc(fl, (size_t)capv * sizeof(*fl));
			if (!dv || !fl)
				return 2;
		}
		dv[nv++] = d;
		if (fabsf(u[2]) >= cosf(15.0f * (float)M_PI / 180.0f))
			fl[nfl++] = d;
		else
			flat_end(fl, &nfl, &flat_e2, &flat_tot);
		if (row_prev) {
			float st = fabsf(wrap180(h - h_prev)), sd = fabsf(wrap180(d - d_prev));
			float *ps = f.still && prev_still ? &step_still : &step_move, *pd = f.still && prev_still ? &sdiff_still : &sdiff_move;
			*ps = st > *ps ? st : *ps;
			*pd = sd > *pd ? sd : *pd;
		}
		h_prev = h, d_prev = d, row_prev = 1, prev_still = f.still;
	}
	flat_end(fl, &nfl, &flat_e2, &flat_tot);
	double mean = atan2(sin_sum, cos_sum) * 180 / M_PI, e2 = 0;
	for (int i = 0; i < nv; i++) {
		float x = wrap180(dv[i] - (float)mean);
		e2 += x * x;
	}
	double rms = nv ? sqrt(e2 / nv) : -1;
	printf("summary: samples %zu, %.1f s\n", n, n ? s[n - 1].t - t0 : 0.0);
	printf("summary: fit %s c %.2f %.2f %.2f r %.2f res %.2f acc %d n %d, gate rej r %u dip %u\n",
	    f.cal.have_cal ? "yes" : "no", f.cal.c[0], f.cal.c[1], f.cal.c[2], f.cal.r, f.cal.res, f.cal.acc, f.cal.n, f.rej_r, f.rej_dip);
	printf("summary: gyro bias fusion %.4f %.4f %.4f, run-wide %.4f %.4f %.4f (%.0f s still)\n", f.gbias[0], f.gbias[1],
	    f.gbias[2], tb[0], tb[1], tb[2], still_s);
	printf("summary: largest step per 0.5 s: still %.1f deg, moving %.1f deg; less the gyro's: still %.1f, moving %.1f\n",
	    step_still, step_move, sdiff_still, sdiff_move);
	printf("summary: rms(h - gyro_yaw - %.1f) = %.2f deg over %d rows\n", mean, rms, nv);
	printf("summary: rms flat stretches (15 deg) = %.2f deg over %d rows\n", flat_tot ? sqrt(flat_e2 / flat_tot) : -1.0, flat_tot);
	free(dv);
	free(fl);
	free(s);
	return 0;
}
