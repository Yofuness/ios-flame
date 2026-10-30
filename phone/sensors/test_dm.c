/*
 * B46 dm2: host test of dm_fusion's type-2 output. Heading as Compass shows it: degrees clockwise from magnetic
 * north of the device's +y (top) axis, from the report quaternion (device -> x north, y west, z up).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "dm_fusion.h"
#include "sns.h"

static int fails;

static float heading(const struct dm_report2 *r)
{
	float q[4];
	memcpy(q, r->q, sizeof(q));	/* packed struct: no direct member pointer */
	return dm_heading_deg(q);
}

static void check(const char *what, float got, float want, float tol)
{
	float d = fabsf(got - want);
	d = d > 180 ? 360 - d : d;
	printf("%s %s: %.2f (want %.1f)\n", d <= tol ? "ok  " : "FAIL", what, got, want);
	fails += d > tol;
}

/* flat, face up; mag (uT, device frame) = hard iron + earth field; run 5 s of 100 Hz gyro steps at rest */
static float run(const float earth_dev[3], float yaw, struct dm_report2 *r)
{
	struct dm_fusion f;
	const float bias[3] = { 12, -30, 5 }, acc[3] = { 0, 0, 9.81f }, g[3] = { 0, 0, 0 };
	memset(&f, 0, sizeof(f));
	dm_init(&f, 0.04f);
	f.cal.have_cal = 1, f.cal.acc = 2, f.cal.r = 45;	/* as if loaded from magcal.txt */
	memcpy(f.cal.c, bias, sizeof(f.cal.c));
	dm_accel(&f, acc);
	float m[3] = { earth_dev[0] + bias[0], earth_dev[1] + bias[1], earth_dev[2] + bias[2] };
	for (int i = 0; i < 500; i++) {
		dm_mag(&f, m);
		dm_gyro(&f, g, 1000000ull + i * 192000ull, 19200000.0);
	}
	dm_report2(&f, 1, yaw, r);
	return heading(r);
}

/* dm9: field 20 uT north, 40 uT down + hard iron; top at heading th (deg), rolled by roll deg about the top axis */
static const float t9_bias[3] = { 12, -30, 5 };
static void t9_sense(float th, float roll, float m[3], float a[3], float noise)
{
	float t = th * (float)M_PI / 180, r = roll * (float)M_PI / 180, c = cosf(r), s = sinf(r);
	float x0 = -20 * sinf(t), y0 = 20 * cosf(t), z0 = -40;
	static unsigned seed = 9;
	m[0] = x0 * c + z0 * s + t9_bias[0], m[1] = y0 + t9_bias[1], m[2] = -x0 * s + z0 * c + t9_bias[2];
	for (int i = 0; i < 3; i++) {
		seed = seed * 1103515245u + 12345u;
		m[i] += noise * ((seed >> 8 & 0xffff) / 32768.0f - 1);
	}
	a[0] = 9.81f * s, a[1] = 0, a[2] = 9.81f * c;
}

static void t9_init(struct dm_fusion *f)
{
	memset(f, 0, sizeof(*f));
	dm_init(f, 0.04f);
	f->cal.have_cal = 1, f->cal.acc = 2, f->cal.r = 45;
	memcpy(f->cal.c, t9_bias, sizeof(f->cal.c));
}

/* secs at 100 Hz gyro, 50 Hz mag, turning at rate deg/s (heading up); gb = gyro bias. Returns the largest heading
 * step; *worst = the worst error from second "from" on */
static float t9_run(struct dm_fusion *f, uint64_t *ts, float *th, float rate, float secs, float roll, const float gb[3],
    float noise, float from, float *worst)
{
	float m[3], a[3], w = 0, step = 0, last = -1;
	struct dm_report2 r;
	float rc = cosf(roll * (float)M_PI / 180), rs = sinf(roll * (float)M_PI / 180), wz = -rate * (float)M_PI / 180;
	int n = (int)(secs * 100);
	for (int i = 0; i < n; i++, *ts += 192000) {
		*th += rate * 0.01f;
		t9_sense(*th, roll, m, a, noise);
		float g[3] = { gb[0] + wz * rs, gb[1], gb[2] + wz * rc };	/* world z in the device frame */
		dm_accel(f, a);
		if (i % 2 == 0)
			dm_mag_ts(f, m, *ts);
		dm_gyro(f, g, *ts, 19200000.0);
		dm_report2(f, *ts, 0, &r);
		float h = r.heading, d = fabsf(fmodf(h - *th + 3600 + 180, 360) - 180);
		if (last >= 0) {
			float e = fabsf(fmodf(h - last + 540, 360) - 180);
			step = e > step ? e : step;
		}
		last = h;
		if (i * 0.01f >= from && d > w)
			w = d;
	}
	if (worst)
		*worst = w;
	return step;
}

static float t9_err(float got, float want)
{
	float d = fabsf(fmodf(got - want + 3600, 360));
	return d > 180 ? 360 - d : d;
}

static void t9_tests(void)
{
	struct dm_fusion f;
	struct dm_report2 r;
	float th, worst, q[4], m[3], a[3], h;
	uint64_t ts;
	const float none[3] = { 0, 0, 0 };
#define T9_INIT(h0) (t9_init(&f), th = (h0), ts = 19200000, t9_sense(th, 0, m, a, 0), dm_accel(&f, a))
#define T9_OK(c, ...) do { int o_ = (c); printf("%s ", o_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); fails += !o_; } while (0)
	/* (a) a 0.5 deg/s bias, still, 60 s */
	const float gb[3] = { 0.003f, -0.005f, 0.5f * (float)M_PI / 180 };
	T9_INIT(40);
	float st = t9_run(&f, &ts, &th, 0, 60, 0, gb, 0.5f, 0, &worst);
	dm_report2(&f, ts, 0, &r);
	memcpy(q, r.q, sizeof(q));
	T9_OK(worst < 3, "dm9a bias 0.5 deg/s, 60 s: worst heading error %.2f, largest step %.3f", worst, st);
	T9_OK(fabsf(f.gbias[2] - gb[2]) < 2e-4f && fabsf(f.gbias[0] - gb[0]) < 2e-4f && f.still,
	    "dm9a gbias %.5f,%.5f,%.5f (true %.5f,%.5f,%.5f) still %d", f.gbias[0], f.gbias[1], f.gbias[2], gb[0], gb[1], gb[2], f.still);
	T9_OK(t9_err(dm_heading_deg(q), th) < 3, "dm9a q yaw error %.2f", t9_err(dm_heading_deg(q), th));
	/* the reinit keeps the bias; rotRate is unbiased */
	float rr[3];
	memcpy(rr, r.rot_rate, sizeof(rr));
	T9_OK(fabsf(rr[2]) < 2e-4f, "dm9a rotRate z %.5f", rr[2]);
	dm_init(&f, 0.04f);
	T9_OK(f.gbias_have && fabsf(f.gbias[2] - gb[2]) < 2e-4f, "dm9 gbias survives dm_init");
	/* SEE gyro_cal wins */
	const float sb[3] = { 0.001f, 0.002f, 0.003f };
	dm_gyro_cal_bias(&f, sb, 1);
	dm_gyro(&f, sb, 0, 19200000.0);
	T9_OK(f.gyro[0] == 0 && f.gyro[1] == 0 && f.gyro[2] == 0, "dm9 SEE gyro_cal status 1 wins over gbias");
	/* (b) start 50 deg wrong, still; q is put wrong too */
	T9_INIT(100);
	t9_run(&f, &ts, &th, 0, 3, 0, none, 0, 0, NULL);
	f.hc_h = 150;
	float qz = 25 * (float)M_PI / 180, cz = cosf(qz), sz = sinf(qz), *fq = f.q;
	float w0 = fq[0], x0 = fq[1], y0 = fq[2], z0 = fq[3];
	fq[0] = cz * w0 - sz * z0, fq[1] = cz * x0 - sz * y0, fq[2] = cz * y0 + sz * x0, fq[3] = cz * z0 + sz * w0;
	t9_run(&f, &ts, &th, 0, 5, 0, none, 0, 0, NULL);
	dm_report2(&f, ts, 0, &r);
	memcpy(q, r.q, sizeof(q));
	h = r.heading;
	T9_OK(t9_err(h, th) < 5, "dm9b 50 deg wrong, 5 s later error %.2f; q yaw error %.2f", t9_err(h, th),
	    t9_err(dm_heading_deg(q), th));
	/* (c) a 90 deg turn at 45 deg/s, across north */
	T9_INIT(320);
	t9_run(&f, &ts, &th, 0, 3, 0, none, 0, 0, NULL);
	t9_run(&f, &ts, &th, 45, 2, 0, none, 0, 0, &worst);
	dm_report2(&f, ts, 0, &r);
	h = r.heading;
	T9_OK(worst < 5 && t9_err(h, th) < 2, "dm9c turn 90 deg at 45 deg/s: worst %.2f, end %.2f (heading %.1f want %.1f)",
	    worst, t9_err(h, th), h, th);
	/* (d) 30 deg roll, still and turning */
	T9_INIT(70);
	t9_sense(th, 30, m, a, 0), dm_accel(&f, a);
	t9_run(&f, &ts, &th, 0, 5, 30, none, 0, 0, &worst);
	T9_OK(worst < 3, "dm9d roll 30 deg: worst %.2f", worst);
	t9_run(&f, &ts, &th, 45, 2, 30, none, 0, 0, &worst);
	T9_OK(worst < 5, "dm9d roll 30 deg, turn 90 deg: worst %.2f", worst);
	/* (e) a single 30 uT spike */
	T9_INIT(200);
	t9_run(&f, &ts, &th, 0, 5, 0, none, 0, 0, NULL);
	dm_report2(&f, ts, 0, &r);
	float h0 = r.heading, big = 0;
	t9_sense(th, 0, m, a, 0);
	float spk[3] = { m[0] + 30, m[1], m[2] };
	unsigned rj = f.rej_r + f.rej_dip;
	dm_mag_ts(&f, spk, ts);
	for (int i = 0; i < 100; i++, ts += 192000) {
		if (i == 2)
			dm_mag_ts(&f, m, ts);
		dm_gyro(&f, none, ts, 19200000.0);
		dm_report2(&f, ts, 0, &r);
		big = t9_err(r.heading, h0) > big ? t9_err(r.heading, h0) : big;
	}
	T9_OK(big < 2 && f.rej_r + f.rej_dip == rj + 1, "dm9e 30 uT spike: heading moved %.3f", big);
	/* convention and the upright fallback */
	const float up[3] = { 0, 0, 1 }, upr[3] = { 0, 1, 0 };
	const float mn[3] = { 0, 20, -40 }, me[3] = { -20, 0, -40 }, mw[3] = { 20, 0, -40 };
	const float bn[3] = { 0, -40, -20 }, be[3] = { -20, -40, 0 };	/* upright: the back points north / east */
	float hn, he, hw, hbn, hbe;
	dm_heading_mag(up, mn, &hn), dm_heading_mag(up, me, &he), dm_heading_mag(up, mw, &hw);
	dm_heading_mag(upr, bn, &hbn), dm_heading_mag(upr, be, &hbe);
	T9_OK(t9_err(hn, 0) < 0.01f && t9_err(he, 90) < 0.01f && t9_err(hw, 270) < 0.01f && t9_err(hbn, 0) < 0.01f &&
	    t9_err(hbe, 90) < 0.01f, "dm9 h_mag flat n/e/w %.1f %.1f %.1f, upright back n/e %.1f %.1f", hn, he, hw, hbn, hbe);
	/* a bump: gravity comes from q, the heading stays; SNS_HEADING_CF=0: q's yaw */
	T9_INIT(30);
	t9_run(&f, &ts, &th, 0, 3, 0, none, 0, 0, NULL);
	const float bump[3] = { 6, 0, 14 };
	dm_accel(&f, bump);
	t9_sense(th, 0, m, a, 0);
	for (int i = 0; i < 30; i++, ts += 192000) {
		dm_mag_ts(&f, m, ts);
		dm_gyro(&f, none, ts, 19200000.0);
	}
	dm_report2(&f, ts, 0, &r);
	h = r.heading;
	T9_OK(t9_err(h, th) < 3, "dm9 accel bump 0.3 s: error %.2f", t9_err(h, th));
	T9_INIT(30);
	f.cal.hc_off = 1, f.cal.gbias_off = 1;
	t9_run(&f, &ts, &th, 0, 3, 0, none, 0, 0, NULL);
	T9_OK(!f.hc_have && !f.gbias_have && f.still, "dm9 switches off: no filter, no gbias");
}

/* dm11: quaternion step by body rate w over dt, and the world vector v in the device frame (q: device -> world) */
static void t11_step(float q[4], const float w[3], float dt)
{
	float h = 0.5f * dt, a = q[0], b = q[1], c = q[2], d = q[3];
	q[0] = a - h * (b * w[0] + c * w[1] + d * w[2]);
	q[1] = b + h * (a * w[0] + c * w[2] - d * w[1]);
	q[2] = c + h * (a * w[1] - b * w[2] + d * w[0]);
	q[3] = d + h * (a * w[2] + b * w[1] - c * w[0]);
	float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	for (int i = 0; i < 4; i++)
		q[i] /= n;
}

static void t11_to_dev(const float q[4], const float v[3], float o[3])
{
	float a = q[0], b = q[1], c = q[2], d = q[3];
	o[0] = (1 - 2 * (c * c + d * d)) * v[0] + 2 * (b * c + a * d) * v[1] + 2 * (b * d - a * c) * v[2];
	o[1] = 2 * (b * c - a * d) * v[0] + (1 - 2 * (b * b + d * d)) * v[1] + 2 * (c * d + a * b) * v[2];
	o[2] = 2 * (b * d + a * c) * v[0] + 2 * (c * d - a * b) * v[1] + (1 - 2 * (b * b + c * c)) * v[2];
}

/* secs at 100 Hz gyro, 50 Hz mag in the field fld; rate(t) as in the tumble or a flat turn. Biases as on the phone */
static void t11_run(struct dm_fusion *f, float q[4], uint64_t *ts, float secs, const float fld[3], int mode)
{
	const float hb[3] = { -34, -41, -60 }, gb[3] = { 0.01f, -0.005f, 0.004f }, up[3] = { 0, 0, 9.81f };
	static float t;
	static unsigned seed = 11;
	for (int i = 0; i < (int)(secs * 100); i++, *ts += 192000, t += 0.01f) {
		float w[3] = { 0, 0, 0 }, a[3], m[3], g[3];
		if (mode == 1)
			w[0] = 1.3f * sinf(0.53f * t), w[1] = 1.1f * sinf(0.37f * t + 1), w[2] = 0.9f * sinf(0.23f * t + 2);
		else if (mode == 2)
			w[2] = 0.8f;
		t11_step(q, w, 0.01f);
		t11_to_dev(q, up, a);
		t11_to_dev(q, fld, m);
		for (int k = 0; k < 3; k++) {
			seed = seed * 1103515245u + 12345u;
			m[k] += hb[k] + 0.3f * ((seed >> 8 & 0xffff) / 32768.0f - 1);
			g[k] = w[k] + gb[k];
		}
		dm_accel(f, a);
		if (i % 2 == 0)
			dm_mag_ts(f, m, *ts);
		dm_gyro(f, g, *ts, 19200000.0);
	}
}

/* dm11: the field strength differs between two places (as desk and room): the sphere is pulled off, the gyro fit is not */
static void t11_tests(void)
{
	static struct dm_fusion f, f2;
	const float room[3] = { 21, 0, -44 }, desk[3] = { 11, 0, -52 }, hb[3] = { -34, -41, -60 };
	float q[4] = { 1, 0, 0, 0 }, b[3];
	uint64_t ts = 19200000;
	dm_init(&f, 0.04f);
	t11_run(&f, q, &ts, 4, room, 0);	/* still: the gyro bias */
	t11_run(&f, q, &ts, 90, room, 1);
	t11_run(&f, q, &ts, 120, desk, 2);
	float sph = sqrtf((f.cal.c[0] - hb[0]) * (f.cal.c[0] - hb[0]) + (f.cal.c[1] - hb[1]) * (f.cal.c[1] - hb[1]) +
	    (f.cal.c[2] - hb[2]) * (f.cal.c[2] - hb[2]));	/* here the pull is in z: the flat turn is symmetric in x, y */
	dm_mag_bias(&f, b);
	float gm = hypotf(b[0] - hb[0], b[1] - hb[1]), gz = fabsf(b[2] - hb[2]);
	int o = f.cal.gm_ok && !strcmp(dm_mag_src(&f), "gyro") && gm < 1 && gz < 1 && dm_mag_accuracy(&f) == 2;
	printf("%s dm11 two places: gyro-aided %.2f,%.2f,%.2f xy err %.2f z err %.2f acc %d res %.2f\n", o ? "ok  " : "FAIL", b[0], b[1], b[2],
	    gm, gz, dm_mag_accuracy(&f), f.cal.gm_res);
	fails += !o;
	o = f.cal.have_cal && sph > 1.5f;
	printf("%s dm11 two places: sphere %.2f,%.2f,%.2f err %.2f r %.1f\n", o ? "ok  " : "FAIL", f.cal.c[0], f.cal.c[1], f.cal.c[2], sph, f.cal.r);
	fails += !o;
	/* magcal.txt: the state comes back; a line without it still gives the bias */
	const char *path = "out/magcal-test.txt";
	o = !dm_magcal_save(&f, path);
	dm_init(&f2, 0.04f);
	o = o && !dm_magcal_load(&f2, path) && f2.cal.gm_ok && fabsf(f2.cal.gm_c[0] - b[0]) < 0.01f &&
	    fabs(f2.cal.gm_ata[0] - f.cal.gm_ata[0]) < 1e-3 * f.cal.gm_ata[0] && dm_mag_accuracy(&f2) == dm_mag_accuracy(&f);
	printf("%s dm11 magcal gm line round trip\n", o ? "ok  " : "FAIL");
	fails += !o;
	/* one axis of rotation: z has no information and keeps the prior; flat turning alone gives accuracy 1 */
	memset(&f2, 0, sizeof(f2));
	dm_init(&f2, 0.04f);
	f2.cal.have_cal = 1, f2.cal.acc = 1, f2.cal.r = 50;
	f2.cal.c[0] = -30, f2.cal.c[1] = -45, f2.cal.c[2] = -50;
	float q2[4] = { 1, 0, 0, 0 };
	t11_run(&f2, q2, &ts, 4, room, 0);
	t11_run(&f2, q2, &ts, 60, room, 2);
	dm_mag_bias(&f2, b);
	o = f2.cal.gm_ok && hypotf(b[0] - hb[0], b[1] - hb[1]) < 1 && fabsf(b[2] + 50) < 1 && dm_mag_accuracy(&f2) == 1;
	printf("%s dm11 flat turn only: %.2f,%.2f,%.2f (z stays -50) acc %d\n", o ? "ok  " : "FAIL", b[0], b[1], b[2], dm_mag_accuracy(&f2));
	fails += !o;
	/* SEE wins, and SNS_MAG_GYROFIT=0 stays with the sphere */
	const float sb[3] = { 1, 2, 3 };
	dm_mag_cal_bias(&f2, sb, 1);
	o = !strcmp(dm_mag_src(&f2), "dsp");
	memset(&f2, 0, sizeof(f2));
	dm_init(&f2, 0.04f);
	f2.cal.gm_off = 1;
	t11_run(&f2, q2, &ts, 4, room, 0);
	t11_run(&f2, q2, &ts, 30, room, 2);
	o = o && !f2.cal.gm_ok && f2.cal.gm_pairs == 0;
	printf("%s dm11 source order: SEE first; switch off: no pairs\n", o ? "ok  " : "FAIL");
	fails += !o;
}

int main(void)
{
	struct dm_report2 r;
	const float north[3] = { 0, 20, -40 };	/* top points north; the field dips down (-z when face up) */
	check("top north: heading", run(north, 0, &r), 0, 1);
	printf("     acc %d bias %.0f,%.0f,%.0f ua %.3f,%.3f,%.3f\n", r.mag_accuracy, r.mag_bias[0], r.mag_bias[1],
	    r.mag_bias[2], r.user_accel[0], r.user_accel[1], r.user_accel[2]);
	fails += r.type != 2 || r.mag_accuracy != 2;
	const float east[3] = { -20, 0, -40 };	/* turned 90 deg clockwise (top east): north is on the left (-x) */
	check("top east: heading", run(east, 0, &r), 90, 1);
	const float west[3] = { 20, 0, -40 };
	check("top west: heading", run(west, 0, &r), 270, 1);
	check("top north, yaw 30: heading", run(north, 30, &r), 30, 1);
	check("report heading field", r.heading, 30, 1);
	check("report status 0x100", r.status == DM_STATUS_HEADING, 1, 0);
	check("report heading accuracy", r.heading_acc, 10, 0);
	/* no calibration: accuracy 0xff */
	struct dm_fusion f;
	memset(&f, 0, sizeof(f));
	dm_init(&f, 0.04f);
	dm_mag(&f, north);
	dm_mag(&f, north);
	dm_report2(&f, 1, 0, &r);
	printf("%s uncalibrated accuracy 0x%x\n", r.mag_accuracy == 0xff ? "ok  " : "FAIL", r.mag_accuracy);
	fails += r.mag_accuracy != 0xff;
	/* dm5: a hand-built sns_cal_event (msg 1026): 1 packed bias {-7, 25, 18} uT, 2 comp_matrix (unpacked, 1 value),
	 * 3 status 3 (high), 4 cal_id 9. No box: the SEE bias alone must give the heading and accuracy 2. */
	static const uint8_t ev[] = { 0x0a, 12, 0, 0, 0xe0, 0xc0, 0, 0, 0xc8, 0x41, 0, 0, 0x90, 0x41,
		0x15, 0, 0, 0x80, 0x3f, 0x18, 3, 0x20, 9 };
	float cb[3];
	int st, nb = sns_parse_cal_event(ev, sizeof(ev), cb, &st);
	printf("%s cal event: %d bias %.1f,%.1f,%.1f status %d\n", nb == 3 && st == 3 && cb[0] == -7 && cb[1] == 25 &&
	    cb[2] == 18 ? "ok  " : "FAIL", nb, cb[0], cb[1], cb[2], st);
	fails += !(nb == 3 && st == 3 && cb[0] == -7 && cb[1] == 25 && cb[2] == 18);
	sns_print_fields("     raw", 1026, ev, sizeof(ev));
	const float acc[3] = { 0, 0, 9.81f }, g0[3] = { 0, 0, 0 };
	memset(&f, 0, sizeof(f));
	dm_init(&f, 0.04f);
	dm_mag_cal_bias(&f, cb, st);
	dm_accel(&f, acc);
	const float m[3] = { -20 + cb[0], 0 + cb[1], -40 + cb[2] };	/* top east + the SEE bias */
	for (int i = 0; i < 500; i++) {
		dm_mag(&f, m);
		dm_gyro(&f, g0, 1000000ull + i * 192000ull, 19200000.0);
	}
	dm_report2(&f, 1, 0, &r);
	check("SEE mag_cal, top east: heading", heading(&r), 90, 1);
	printf("%s SEE mag_cal accuracy %d dsp %d\n", r.mag_accuracy == 2 && dm_mag_cal_dsp(&f) ? "ok  " : "FAIL",
	    r.mag_accuracy, dm_mag_cal_dsp(&f));
	fails += !(r.mag_accuracy == 2 && dm_mag_cal_dsp(&f));
	/* status 0: back to the box (here none: uncalibrated); gyro_cal bias is subtracted from rotRate */
	dm_mag_cal_bias(&f, cb, 0);
	const float gb[3] = { 0.01f, -0.02f, 0.03f };
	dm_gyro_cal_bias(&f, gb, 2);
	dm_gyro(&f, gb, 0, 19200000.0);
	dm_report2(&f, 1, 0, &r);
	float rr[3];
	memcpy(rr, r.rot_rate, sizeof(rr));
	int ok = r.mag_accuracy == 0xff && fabsf(rr[0]) + fabsf(rr[1]) + fabsf(rr[2]) < 1e-6f;
	printf("%s SEE status 0 -> accuracy 0x%x, gyro_cal rotRate %.4f,%.4f,%.4f\n", ok ? "ok  " : "FAIL",
	    r.mag_accuracy, rr[0], rr[1], rr[2]);
	fails += !ok;
	/* dm7: sphere fit. Samples on a 48 uT sphere around (-20, 40, -70), 1 uT noise, only the upper ~2/3 covered */
	{
		const float c0[3] = { -20, 40, -70 };
		unsigned seed = 1;
#define RND() ((seed = seed * 1103515245u + 12345u) >> 8 & 0xffff) / 65536.0f
		memset(&f, 0, sizeof(f));
		dm_init(&f, 0.04f);
		dm_accel(&f, acc);
		for (int i = 0; i < 400; i++) {
			float th = acosf(1 - 1.4f * RND()), ph = 6.2832f * RND(), m[3];
			float u[3] = { sinf(th) * cosf(ph), sinf(th) * sinf(ph), cosf(th) };
			for (int j = 0; j < 3; j++)
			{
				float n1 = RND(), n2 = RND(), n3 = RND();
				m[j] = c0[j] + 48 * u[j] + (n1 + n2 + n3 - 1.5f) * 2;	/* ~1 uT sd */
			}
			if (i == 100 || i == 250) {	/* outliers: rejected, never in the ring */
				float o[3] = { 300, 0, 0 }, j2[3] = { m[0] + 80, m[1], m[2] };
				ok = dm_mag(&f, o) == -1 && dm_mag(&f, j2) == -1;
				fails += !ok;
			}
			dm_mag(&f, m);
		}
		float b[3];
		dm_mag_bias(&f, b);
		float e = sqrtf((b[0] - c0[0]) * (b[0] - c0[0]) + (b[1] - c0[1]) * (b[1] - c0[1]) +
		    (b[2] - c0[2]) * (b[2] - c0[2]));
		ok = f.cal.have_cal && e < 2 && fabsf(f.cal.r - 48) < 2 && dm_mag_accuracy(&f) >= 1;
		printf("%s sphere fit: bias %.2f,%.2f,%.2f err %.2f r %.2f res %.2f acc %d n %d\n", ok ? "ok  " : "FAIL",
		    b[0], b[1], b[2], e, f.cal.r, f.cal.res, dm_mag_accuracy(&f), f.cal.n);
		fails += !ok;
		/* heading after the fit: flat, top east */
		const float east[3] = { -20, 0, -40 };
		float m[3] = { east[0] + c0[0], east[1] + c0[1], east[2] + c0[2] }, ring_n = f.cal.ring_n;
		f.mag_seeded = 0;
		for (int i = 0; i < 500; i++) {
			dm_mag(&f, m);
			dm_gyro(&f, g0, 1000000ull + i * 192000ull, 19200000.0);
		}
		dm_report2(&f, 1, 0, &r);
		check("after the fit, top east: heading", heading(&r), 90, 1);
		ok = f.cal.ring_n == ring_n + 1;
		printf("%s still phone adds one ring sample (%d)\n", ok ? "ok  " : "FAIL", f.cal.ring_n);
		fails += !ok;
		/* save/load round trip; an old box file is ignored */
		const char *path = "out/magcal-test.txt";
		struct dm_fusion f2;
		memset(&f2, 0, sizeof(f2));
		ok = !dm_magcal_save(&f, path) && !dm_magcal_load(&f2, path) && fabsf(f2.cal.c[1] - f.cal.c[1]) < 0.01f &&
		    f2.cal.acc == f.cal.acc;
		FILE *fp = fopen(path, "w");
		fprintf(fp, "min -110.5 -115.0 -187.3 max 24.5 1.8 -11.2\n");
		fclose(fp);
		memset(&f2, 0, sizeof(f2));
		ok = ok && dm_magcal_load(&f2, path) == -1 && !f2.cal.have_cal;
		printf("%s magcal fit line round trip, old min/max box ignored\n", ok ? "ok  " : "FAIL");
		fails += !ok;
		/* a still phone (one orientation, noise only): no fit */
		memset(&f, 0, sizeof(f));
		dm_init(&f, 0.04f);
		for (int i = 0; i < 2000; i++) {
			float n1 = RND(), n2 = RND(), n3 = RND();
			float m1[3] = { -20 + (n1 - 0.5f) * 12, 88 + (n2 - 0.5f) * 12, -70 + (n3 - 0.5f) * 12 };
			dm_mag(&f, m1);
		}
		ok = !f.cal.have_cal && dm_mag_accuracy(&f) == -1;
		printf("%s still phone: no fit (ring %d)\n", ok ? "ok  " : "FAIL", f.cal.ring_n);
		fails += !ok;
		/* heading low-pass: a 90 deg step across north is reached smoothly (wrap-aware) */
		memset(&f, 0, sizeof(f));
		dm_init(&f, 0.04f);
		dm_accel(&f, acc);
		f.cal.have_cal = 1, f.cal.acc = 2;
		dm_report2(&f, 19200000, 80, &r);	/* q = identity: heading 270 + yaw knob */
		dm_report2(&f, 19200000 + 1920000, 110, &r);	/* 0.1 s later the target is 30 deg ahead */
		float h1 = r.heading;
		for (int i = 2; i < 30; i++)
			dm_report2(&f, 19200000 + i * 1920000ull, 110, &r);
		ok = h1 > 355 && h1 < 360 && fabsf(r.heading - 20) < 0.5f;
		printf("%s heading low-pass across north: %.1f after 0.1 s, %.1f after 2.9 s\n", ok ? "ok  " : "FAIL", h1,
		    r.heading);
		fails += !ok;
	}
	/* dm8: a 1.5 s mag gap keeps type 2 and the heading; one 30 uT spike is gated */
	{
		const float bias[3] = { 12, -30, 5 }, north[3] = { 0, 20, -40 };
		float mg[3] = { north[0] + bias[0], north[1] + bias[1], north[2] + bias[2] };
		memset(&f, 0, sizeof(f));
		dm_init(&f, 0.04f);
		f.cal.have_cal = 1, f.cal.acc = 2, f.cal.r = 45;
		memcpy(f.cal.c, bias, sizeof(f.cal.c));
		dm_accel(&f, acc);
		uint64_t ts = 19200000;
		const float gd[3] = { 0, 0, 0.002f };	/* a little gyro drift: IMU-only yaw moves slowly */
		for (int i = 0; i < 500; i++, ts += 192000) {
			if (i % 2 == 0)
				dm_mag_ts(&f, mg, ts);
			dm_gyro(&f, gd, ts, 19200000.0);
		}
		dm_report2(&f, ts, 0, &r);
		float h0 = heading(&r);
		int t2 = 1;
		for (int i = 0; i < 150; i++, ts += 192000) {	/* 1.5 s without mag */
			dm_gyro(&f, gd, ts, 19200000.0);
			t2 &= dm_type_auto(&f, ts, DM_MAG_HOLD_S) == 2;
		}
		dm_report2(&f, ts, 0, &r);
		check("1.5 s mag gap: heading kept", heading(&r), h0, 2);
		ok = t2 && r.status == DM_STATUS_HEADING && r.heading_acc == 10;
		printf("%s 1.5 s mag gap: type 2, status 0x%x acc %.0f\n", ok ? "ok  " : "FAIL", r.status, r.heading_acc);
		fails += !ok;
		for (int i = 0; i < 100; i++, ts += 192000) {
			if (i % 2 == 0)
				dm_mag_ts(&f, mg, ts);
			dm_gyro(&f, gd, ts, 19200000.0);
		}
		dm_report2(&f, ts, 0, &r);
		h0 = heading(&r);
		float sp[3] = { mg[0] + 30, mg[1], mg[2] };
		unsigned rj = f.rej_r + f.rej_dip;
		dm_mag_ts(&f, sp, ts);
		for (int i = 0; i < 4; i++, ts += 192000)
			dm_gyro(&f, gd, ts, 19200000.0);
		dm_mag_ts(&f, mg, ts);
		for (int i = 0; i < 50; i++, ts += 192000)
			dm_gyro(&f, gd, ts, 19200000.0);
		dm_report2(&f, ts, 0, &r);
		check("30 uT spike: heading kept", heading(&r), h0, 2);
		ok = f.rej_r + f.rej_dip == rj + 1;
		printf("%s spike gated (rej r %u dip %u)\n", ok ? "ok  " : "FAIL", f.rej_r, f.rej_dip);
		fails += !ok;
		/* turning during a gap: the gyro alone moves q, the old mag direction doesn't pull it back (> 0.25 s) */
		const float turn[3] = { 0, 0, -30 * (float)M_PI / 180 };	/* clockwise from above: heading +30 deg/s */
		dm_report2(&f, ts, 0, &r);
		h0 = heading(&r);
		for (int i = 0; i < 150; i++, ts += 192000)
			dm_gyro(&f, turn, ts, 19200000.0);
		dm_report2(&f, ts, 0, &r);
		check("turn in a 1.5 s gap: gyro only", heading(&r), fmodf(h0 + 45, 360), 1.5f);
		ts += 19200000ull * 6;	/* no mag for 6 s: heading invalid, type 3 */
		dm_gyro(&f, gd, ts, 19200000.0);
		dm_report2(&f, ts, 0, &r);
		ok = r.status == 0 && r.heading_acc == -1 && dm_type_auto(&f, ts, DM_MAG_HOLD_S) == 3;
		printf("%s 6 s without mag: status 0x%x acc %.0f\n", ok ? "ok  " : "FAIL", r.status, r.heading_acc);
		fails += !ok;
	}
	t9_tests();
	t11_tests();
	/* O13: interest from the status text (devmotion8) and the 5 s hold */
	const char *stx = "devmotion   page 0xff0c usage 0x04 clients 2 ReportInterval 0 us (set by iOS) events 0 (none yet)\n"
	    "devmotion8  page 0xff0c usage 0x08 clients 2 ReportInterval 20000 us (set by iOS) events 5\n"
	    "device: 1 writes, 0 samples, 0 rejected, last pid 1\n";
	const char *st0 = "devmotion8  page 0xff0c usage 0x08 clients 1 ReportInterval 25000 us (default) events 0\n";
	ok = dm_status_interval(stx, "devmotion8") == 20000 && dm_status_interval(stx, "devmotion") == 0 &&
	    dm_status_interval(st0, "devmotion8") == 0 && dm_status_interval("", "devmotion8") == 0 &&
	    dm_status_interval("devmotion8  not registered (sweetmotion=0x1ff)\n", "devmotion8") == 0;
	printf("%s status text: devmotion8 interval parsed, default/missing = 0\n", ok ? "ok  " : "FAIL");
	fails += !ok;
	struct dm_gate g = { 0, 0 };
	int s0 = dm_gate_step(&g, 0, 1000), s1 = dm_gate_step(&g, 1, 2000), s2 = dm_gate_step(&g, 0, 4000),
	    s3 = dm_gate_step(&g, 0, 8999), s4 = dm_gate_step(&g, 1, 9000), s5 = dm_gate_step(&g, 0, 10000),
	    s6 = dm_gate_step(&g, 0, 14999), s7 = dm_gate_step(&g, 0, 15000);
	ok = !s0 && s1 && s2 && s3 && s4 && s5 && s6 && !s7;
	printf("%s gate: off, on at once, held 5 s, re-armed, off after 5 s (%d%d%d%d%d%d%d%d)\n", ok ? "ok  " : "FAIL",
	    s0, s1, s2, s3, s4, s5, s6, s7);
	fails += !ok;
	printf("%s\n", fails ? "dm: FAILED" : "dm: all ok");
	return !!fails;
}
