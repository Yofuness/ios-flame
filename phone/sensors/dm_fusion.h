/*
 * sunfish B46 (kernel 0243): sensd's own 6-axis fusion (Madgwick, accel + gyro) for the fake HID DeviceMotion
 * service {0xff0c, 4}: CoreMotion's SPU fusion path (CLSensorFusionService*SPU, Compass mode 0x22) reads one
 * VendorDefined report per sample. Report layout: research/notes/devicemotion.md ("Report layout").
 * Frame: the iOS device frame (x right, y up, z out of the screen; the Android frame has the same axes).
 */
#ifndef DM_FUSION_H
#define DM_FUSION_H
#include <stdint.h>
#include <stddef.h>

/* Type-3 report (6-axis SPU), 52 bytes, little-endian, packed (CLSensorFusionServiceSPU::populateDeviceMotion). */
struct __attribute__((packed)) dm_report3 {
	uint8_t type;		/* 3 */
	uint16_t status;	/* @0x01, 0 */
	uint8_t pad;
	uint64_t seq_or_ts;	/* @0x04: mach time of the sample */
	float q[4];		/* @0x0c: x y z w */
	float rot_rate[3];	/* @0x1c: rad/s */
	float user_accel[3];	/* @0x28: g, gravity removed (CMDeviceMotion.userAcceleration) */
};
_Static_assert(sizeof(struct dm_report3) == 52, "dm_report3 size");
_Static_assert(offsetof(struct dm_report3, q) == 0x0c, "dm_report3 q");
_Static_assert(offsetof(struct dm_report3, rot_rate) == 0x1c, "dm_report3 rot_rate");
_Static_assert(offsetof(struct dm_report3, user_accel) == 0x28, "dm_report3 user_accel");

/*
 * B46 dm2: type-2 report (9-axis SPU, CMAttitudeReferenceFrameXMagneticNorthZVertical), 88 bytes; CoreMotion takes
 * magneticField = magRaw - magBias. Units: uT, rad/s, g; frame as above (the kernel posts SEE's compass uT 1:1).
 */
struct __attribute__((packed)) dm_report2 {
	uint8_t type;		/* 2 */
	uint8_t mag_accuracy;	/* @0x01: CMMagneticFieldCalibrationAccuracy 0/1/2, 0xff uncalibrated */
	uint16_t status;	/* @0x02: bit 8 (0x100) = heading valid; CLCompass7 publishes only with it (dm3 RE) */
	uint64_t seq_or_ts;	/* @0x04: mach time */
	uint32_t unk;		/* @0x0c, 0 */
	float mag_raw[3];	/* @0x10: uT */
	float q[4];		/* @0x1c: x y z w, device -> (x magnetic north, z up) */
	float rot_rate[3];	/* @0x2c */
	float user_accel[3];	/* @0x38 */
	float heading;		/* @0x44: magnetic heading, degrees clockwise from north (CLCompass7 uses it as is) */
	float heading_acc;	/* @0x48: heading accuracy in degrees, -1 invalid (locationd clamps to 10..180) */
	float mag_bias[3];	/* @0x4c: uT */
};
_Static_assert(sizeof(struct dm_report2) == 88, "dm_report2 size");
_Static_assert(offsetof(struct dm_report2, mag_raw) == 0x10, "dm_report2 mag_raw");
_Static_assert(offsetof(struct dm_report2, q) == 0x1c, "dm_report2 q");
_Static_assert(offsetof(struct dm_report2, rot_rate) == 0x2c, "dm_report2 rot_rate");
_Static_assert(offsetof(struct dm_report2, user_accel) == 0x38, "dm_report2 user_accel");
_Static_assert(offsetof(struct dm_report2, heading) == 0x44, "dm_report2 heading");
_Static_assert(offsetof(struct dm_report2, heading_acc) == 0x48, "dm_report2 heading_acc");
_Static_assert(offsetof(struct dm_report2, mag_bias) == 0x4c, "dm_report2 mag_bias");

/*
 * dm7: online hard-iron calibration, a least-squares sphere fit over a ring of decimated raw samples (model: Google's
 * contexthub mag_cal.c, written anew). Replaces the dm2 min/max box, which only grew and kept a wrong seed.
 */
#define DM_MAG_RING 300		/* samples kept */
#define DM_MAG_DECIM 5.0f	/* uT: a sample enters the ring only this far from the last one that did */
#define DM_MAG_FIT_MIN 40	/* samples needed for a fit */
#define DM_MAG_FIT_EVERY 20	/* new ring samples between fits (~2 s while turning) */
#define DM_MAG_COVER 30.0f	/* uT: the ring's spread needed in every axis */
#define DM_MAG_COVER_GOOD 50.0f	/* uT: spread for accuracy 2 */
#define DM_MAG_R_MIN 25.0f	/* uT: accepted fit radius (Earth's field is 25-65 uT) */
#define DM_MAG_R_MAX 70.0f
#define DM_MAG_RES_MAX 4.0f	/* uT: RMS residual for accuracy 1 */
#define DM_MAG_RES_GOOD 2.0f	/* uT: for accuracy 2 */
#define DM_MAG_STEP 10.0f	/* uT: a bias change this big needs two consecutive fits that agree */
#define DM_HEADING_TC 0.3f	/* s: low-pass of the published heading @0x44 (SNS_HEADING_LP=0: off) */
#define DM_MAG_MAX 200.0f	/* uT: |m| above this is an outlier (Earth's field is 25-65 uT) */
#define DM_MAG_JUMP 60.0f	/* uT: a step this big from the last sample is an outlier */

struct dm_fusion {
	float q[4];		/* w x y z: device -> earth (z up) */
	float beta;
	float acc[3];		/* last accel, Android frame m/s² (specific force: +z face up) */
	float gyro[3];		/* last gyro, rad/s */
	int have_acc, have_gyro;
	uint64_t last_ts;	/* last gyro update, ticks */
	float mag[3];		/* dm2: last raw mag, uT, device frame */
	int have_mag, mag_seeded;
	/* dm7: sphere-fit calibration (survives dm_init); have_cal: fit_c is the bias */
	struct dm_magcal {
		float ring[DM_MAG_RING][3];
		int ring_n, ring_i, ring_new;
		int have_cal, acc, n;		/* acc 0..2; n: samples in the accepted fit */
		float c[3], r, res;
		int have_pend;			/* a > DM_MAG_STEP change waiting for a second fit */
		float pend[3];
		float heading_tc;		/* s, 0 = no heading low-pass */
	} cal;
	float hd_out;			/* dm7: low-passed heading */
	uint64_t hd_ts;
	int hd_have;
	/* dm5: SEE's own mag_cal/gyro_cal biases (survive dm_init); the mag one replaces the box while status >= 1 */
	float see_mag_bias[3], see_gyro_bias[3];
	int see_mag_st, see_gyro_st;	/* -1 none yet (set by dm_init), else SEE status 0-3 */
};

void dm_init(struct dm_fusion *f, float beta);
void dm_accel(struct dm_fusion *f, const float a[3]);
/* One filter step with this gyro sample; ts in ticks of ticks_per_s (0 = no step, only stored). */
void dm_gyro(struct dm_fusion *f, const float g[3], uint64_t ts, double ticks_per_s);
/* yaw_deg: test knob, extra rotation about the reference z axis (+ = heading increases, clockwise from above) */
void dm_report3(const struct dm_fusion *f, uint64_t ts, float yaw_deg, struct dm_report3 *r);
/* dm2: one raw mag sample (uT); dm7: feeds the sphere-fit ring (outliers rejected). Returns 0 kept, -1 rejected. */
int dm_mag(struct dm_fusion *f, const float m[3]);
int dm_mag_accuracy(const struct dm_fusion *f);	/* -1 uncalibrated, 0..2 */
void dm_mag_bias(const struct dm_fusion *f, float b[3]);
/* dm7: one sphere fit over the samples (exposed for tests): 0 ok (c, r, rms residual), -1 singular */
int dm_sphere_fit(const float (*m)[3], int n, float c[3], float *r, float *res);
/* dm5: SEE calibration events (bias uT or rad/s, status 0 unreliable .. 3 high) */
void dm_mag_cal_bias(struct dm_fusion *f, const float b[3], int status);
void dm_gyro_cal_bias(struct dm_fusion *f, const float b[3], int status);
int dm_mag_cal_dsp(const struct dm_fusion *f);	/* 1: the SEE bias is in use, 0: the fit */
int dm_magcal_load(struct dm_fusion *f, const char *path);
int dm_magcal_save(const struct dm_fusion *f, const char *path);
float dm_heading_deg(const float q[4]);	/* device top in the reference frame, 0..360 clockwise from north */
#define DM_STATUS_HEADING 0x0100
void dm_report2(struct dm_fusion *f, uint64_t ts, float yaw_deg, struct dm_report2 *r);

/*
 * O13: run the fusion only while a client wants device motion (backboardd keeps the services open at
 * ReportInterval 0). dm_status_interval: the ReportInterval iOS set for service NAME in /dev/redwood-motion's
 * read() text (devmotion8 has no motion_want slot), 0 when unset/default or not found.
 * dm_gate_step: on at once when interest appears, off DM_GATE_HOLD_MS after it is gone.
 */
#define DM_GATE_HOLD_MS 5000
struct dm_gate {
	int on;
	long idle_since;	/* ms; 0 = interest seen on the last step */
};
unsigned dm_status_interval(const char *text, const char *name);
int dm_gate_step(struct dm_gate *g, int interest, long now_ms);

#endif
