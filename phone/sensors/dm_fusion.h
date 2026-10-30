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
	uint16_t status;	/* @0x02, 0 */
	uint64_t seq_or_ts;	/* @0x04: mach time */
	uint32_t unk;		/* @0x0c, 0 */
	float mag_raw[3];	/* @0x10: uT */
	float q[4];		/* @0x1c: x y z w, device -> (x magnetic north, z up) */
	float rot_rate[3];	/* @0x2c */
	float user_accel[3];	/* @0x38 */
	float f44, f48;		/* @0x44, @0x48: 0 */
	float mag_bias[3];	/* @0x4c: uT */
};
_Static_assert(sizeof(struct dm_report2) == 88, "dm_report2 size");
_Static_assert(offsetof(struct dm_report2, mag_raw) == 0x10, "dm_report2 mag_raw");
_Static_assert(offsetof(struct dm_report2, q) == 0x1c, "dm_report2 q");
_Static_assert(offsetof(struct dm_report2, rot_rate) == 0x2c, "dm_report2 rot_rate");
_Static_assert(offsetof(struct dm_report2, user_accel) == 0x38, "dm_report2 user_accel");
_Static_assert(offsetof(struct dm_report2, f44) == 0x44, "dm_report2 f44");
_Static_assert(offsetof(struct dm_report2, f48) == 0x48, "dm_report2 f48");
_Static_assert(offsetof(struct dm_report2, mag_bias) == 0x4c, "dm_report2 mag_bias");

/* dm2: hard-iron calibration accuracy from the per-axis span of the min/max box (every axis must reach it) */
#define DM_MAG_SPAN_LOW 40.0f	/* uT: accuracy 0 */
#define DM_MAG_SPAN_MED 60.0f	/* 1 */
#define DM_MAG_SPAN_HIGH 80.0f	/* 2 */
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
	float mag_min[3], mag_max[3];	/* hard-iron box (valid when have_cal) */
	int have_mag, have_cal, mag_seeded;
};

void dm_init(struct dm_fusion *f, float beta);
void dm_accel(struct dm_fusion *f, const float a[3]);
/* One filter step with this gyro sample; ts in ticks of ticks_per_s (0 = no step, only stored). */
void dm_gyro(struct dm_fusion *f, const float g[3], uint64_t ts, double ticks_per_s);
/* yaw_deg: test knob, extra rotation about the reference z axis (+ = heading increases, clockwise from above) */
void dm_report3(const struct dm_fusion *f, uint64_t ts, float yaw_deg, struct dm_report3 *r);
/* dm2: one raw mag sample (uT); updates the hard-iron box (outliers rejected). Returns 0 kept, -1 rejected. */
int dm_mag(struct dm_fusion *f, const float m[3]);
int dm_mag_accuracy(const struct dm_fusion *f);	/* -1 uncalibrated, 0..2 */
void dm_mag_bias(const struct dm_fusion *f, float b[3], float span[3]);
int dm_magcal_load(struct dm_fusion *f, const char *path);
int dm_magcal_save(const struct dm_fusion *f, const char *path);
void dm_report2(const struct dm_fusion *f, uint64_t ts, float yaw_deg, struct dm_report2 *r);

#endif
