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

struct dm_fusion {
	float q[4];		/* w x y z: device -> earth (z up) */
	float beta;
	float acc[3];		/* last accel, Android frame m/s² (specific force: +z face up) */
	float gyro[3];		/* last gyro, rad/s */
	int have_acc, have_gyro;
	uint64_t last_ts;	/* last gyro update, ticks */
};

void dm_init(struct dm_fusion *f, float beta);
void dm_accel(struct dm_fusion *f, const float a[3]);
/* One filter step with this gyro sample; ts in ticks of ticks_per_s (0 = no step, only stored). */
void dm_gyro(struct dm_fusion *f, const float g[3], uint64_t ts, double ticks_per_s);
void dm_report3(const struct dm_fusion *f, uint64_t ts, struct dm_report3 *r);

#endif
