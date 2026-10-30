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
	f.have_cal = 1;		/* as if loaded from magcal.txt: a 90 uT box around the bias */
	for (int i = 0; i < 3; i++)
		f.mag_min[i] = bias[i] - 45, f.mag_max[i] = bias[i] + 45;
	dm_accel(&f, acc);
	float m[3] = { earth_dev[0] + bias[0], earth_dev[1] + bias[1], earth_dev[2] + bias[2] };
	for (int i = 0; i < 500; i++) {
		dm_mag(&f, m);
		dm_gyro(&f, g, 1000000ull + i * 192000ull, 19200000.0);
	}
	dm_report2(&f, 1, yaw, r);
	return heading(r);
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
