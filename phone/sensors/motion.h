/*
 * From ios-redfin (DenialOfGravity, https://github.com/DenialOfGravity/ios-redfin) phone/sensors/motion.h,
 * Copyright (c) 2026 dipa-studio, MIT License (see LICENSE.redfin). Sunfish changes are marked "sunfish".
 */
/*
 * /dev/redwood-motion (kernel 0972, WS-S; ABI in docs/redfin/qol-plan.md §3.3): sensd writes whole
 * struct motion_samples; the kernel posts each as an IOHIDEvent from the matching SweetHID* sensor service
 * (iokit/Kernel/arm/hid/SweetHID.cpp). Values are in the Android/SEE device frame and units listed below;
 * the kernel converts to what iOS expects (accel: g, sign flipped).
 *
 *   type 13 accel   m/s² x y z            type 20 gyro     rad/s x y z
 *   type 12 als     lux, [1] colour temp  type 14 prox     [0] 1 near / 0 far, [1] raw
 *   type 21 compass µT x y z              type 31 pressure kPa
 *   type 10 orientation [0] UIDeviceOrientation 1-6
 *   flags: bits 0-1 accuracy (SEE status 0 unreliable .. 3 high); bit 31 synthetic (poke m fake)
 *
 * Additions (not in §3.3, nothing there changes):
 *   read()                   text status, one line per sensor service (clients, ReportInterval, events)
 *   ioctl MOTION_GET_WANT    what iOS asked each service for, so sensd streams only what is wanted
 */
#ifndef MOTION_H
#define MOTION_H
#include <stdint.h>
#include <sys/ioctl.h>

#define MOTION_DEV "/dev/redwood-motion"

struct motion_sample {
	uint32_t type;
	uint32_t flags;
	uint64_t ts_mach;
	float v[4];
};

enum {
	MOTION_TYPE_ORIENTATION = 10, MOTION_TYPE_ALS = 12, MOTION_TYPE_ACCEL = 13, MOTION_TYPE_PROX = 14,
	MOTION_TYPE_GYRO = 20, MOTION_TYPE_COMPASS = 21, MOTION_TYPE_PRESSURE = 31,
};
#define MOTION_FLAG_ACCURACY	0x3u
#define MOTION_FLAG_SYNTHETIC	0x80000000u

/* Sensor service index in struct motion_want. */
enum { MOTION_ACCEL, MOTION_GYRO, MOTION_ALS, MOTION_PROX, MOTION_COMPASS, MOTION_PRESSURE, MOTION_ORIENT,
	MOTION_NSENSORS };

struct motion_want {
	uint32_t version;		/* 1 */
	uint32_t present;		/* bit i: service i registered */
	uint32_t clients[8];		/* open clients */
	uint32_t interval_us[8];	/* last ReportInterval set by iOS */
	uint32_t interval_set[8];	/* 1 once iOS set one (0 us then means "off") */
};
#define MOTION_GET_WANT _IOR('m', 1, struct motion_want)

#endif
