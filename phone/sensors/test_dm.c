/*
 * B46 dm2: host test of dm_fusion's type-2 output. Heading as Compass shows it: degrees clockwise from magnetic
 * north of the device's +y (top) axis, from the report quaternion (device -> x north, y west, z up).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "dm_fusion.h"

static int fails;

static float heading(const struct dm_report2 *r)
{
	float x = r->q[0], y = r->q[1], z = r->q[2], w = r->q[3];
	/* R * (0, 1, 0): device top in the reference frame */
	float tx = 2 * (x * y - w * z), ty = 1 - 2 * (x * x + z * z);
	float h = atan2f(-ty, tx) * 180.0f / (float)M_PI;	/* east = -y */
	return h < 0 ? h + 360 : h;
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
	/* no calibration: accuracy 0xff */
	struct dm_fusion f;
	dm_init(&f, 0.04f);
	dm_mag(&f, north);
	dm_mag(&f, north);
	dm_report2(&f, 1, 0, &r);
	printf("%s uncalibrated accuracy 0x%x\n", r.mag_accuracy == 0xff ? "ok  " : "FAIL", r.mag_accuracy);
	fails += r.mag_accuracy != 0xff;
	printf("%s\n", fails ? "dm: FAILED" : "dm: all ok");
	return !!fails;
}
