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
/*
 * dm8: mag gaps and transients. The MARG correction skips a mag older than DM_MAG_STALE_S (IMU-only meanwhile, the
 * heading still comes from q); the report keeps heading valid until no mag for DM_MAG_HOLD_S (SNS_MAG_HOLD=0: old 1 s
 * type-3 fallback, no skip). Gate (SNS_MAG_GATE=0: off): a sample whose calibrated |m| is > DM_MAG_GATE_R off the fit
 * radius, or whose dip vs gravity is > DM_MAG_GATE_DIP deg off the running median, is not used for the correction.
 */
#define DM_MAG_STALE_S 0.25
#define DM_MAG_HOLD_S 5.0
#define DM_MAG_GATE_R 0.25f
#define DM_MAG_GATE_DIP 10.0f
#define DM_MAG_DIP_N 15		/* dips in the running median */
#define DM_MAG_GATE_STREAK 50	/* this many rejects in a row: accept anyway (the field really changed) */

/*
 * dm9: gyro bias and published heading. Still = every raw gyro axis < DM_STILL_GYRO and |accel| within DM_STILL_ACC of
 * its mean, for DM_STILL_S: the window's average seeds the bias, then a DM_GBIAS_TC low-pass (SNS_DM_GBIAS=0: off; SEE
 * gyro_cal wins with status >= 1). @0x44 comes from a complementary filter (SNS_HEADING_CF=0: q's yaw and beta fixed):
 * gyro about world z plus DM_HC_K_* toward the tilt-compensated mag heading; beta is DM_BETA_STILL while still.
 */
#define DM_STILL_GYRO 0.03f	/* rad/s */
#define DM_STILL_ACC 0.02f
#define DM_STILL_S 2.0f
#define DM_GBIAS_TC 5.0f	/* s */
#define DM_GRAV_TC 0.2f		/* s: low-pass of the accel used as gravity */
#define DM_GRAV_DEV 0.10f	/* |accel| further than this from 1 g: gravity from q */
#define DM_HC_K_SLOW 0.7f	/* 1/s */
#define DM_HC_K_FAST 0.2f	/* 1/s, |gyro| >= DM_HC_FAST */
#define DM_HC_FAST 0.5f		/* rad/s */
#define DM_HC_UPRIGHT 20.0f	/* deg: top this close to vertical: the back of the device gives the heading */
#define DM_BETA_STILL 0.2f
/*
 * dm11: gyro-aided hard iron (SNS_MAG_GYROFIT=0: off), independent of the field strength, which differs between places.
 * Pairs of mag samples ~2 s apart with the gyro's rotation R (j -> i): (I - R) b = m_i - R m_j. Normal equations with a
 * forgetting per pair (a still phone forgets nothing). Source: SEE status >= 1 > this (see gm_ok) > the sphere fit.
 */
#define DM_GM_HIST 64		/* history entries, ~2.5 s */
#define DM_GM_HIST_S 0.035	/* s: at least this between entries (~25 Hz at 50 Hz mag) */
#define DM_GM_PAIR_EVERY 5	/* entries per pair (~5 Hz) */
#define DM_GM_LAG_S 2.0		/* s, +- DM_GM_LAG_TOL */
#define DM_GM_LAG_TOL 0.5
#define DM_GM_MIN_DEG 25.0f	/* smaller rotations carry mostly noise */
#define DM_GM_MAX_RATE 5.0f	/* rad/s: faster in the window: integration error */
#define DM_GM_JUMP 10.0f	/* uT: | |m_i - b| - |m_j - b| | above this: a disturbance */
#define DM_GM_JUMP_STREAK 50	/* this many in a row: the bias is off, not the field; accept */
#define DM_GM_HALF_PAIRS 3000.0	/* half-life, ~10 min of pairs */
#define DM_GM_SOLVE_S 5.0
#define DM_GM_LAMBDA 0.5	/* Tikhonov weight toward the prior; a 25 deg pair adds ~0.2 */
#define DM_GM_EIG_MIN 20.0	/* AtA eigenvalue of an observable direction */
#define DM_GM_RES_GOOD 4.0f	/* uT: RMS pair residual for accuracy 2 */
/*
 * dm12 sphere guard (SNS_MAG_GUARD=0: off): flat turning leaves z and r degenerate (live: r 43 -> 31, z -64 -> -83). A fit
 * counts only with every axis's spread >= DM_MAG_GUARD_SPREAD x r and r >= DM_MAG_GUARD_R_MIN (also for a saved fit at load).
 */
#define DM_MAG_GUARD_SPREAD 1.1f
#define DM_MAG_GUARD_R_MIN 35.0f
/*
 * dm12 live path (SNS_GM_LIVE=0: off): mag samples wait for the gyro to pass their time, then take the orientation from a
 * ring (batches of any size up to the rings), z stays at the prior while unobserved, a streak of jumps stays accepted
 * until the pairs agree again, and the normal equations are saved before they reach gm_ok.
 */
#define DM_GM_GQ_N 256		/* gyro orientations, ~2.5 s at 100 Hz */
#define DM_GM_PEND_N 128	/* mag samples waiting for the gyro, ~2.5 s at 50 Hz */
#define DM_GM_PRIOR_W 1000.0	/* weight of the prior along the unobserved direction */
enum { DM_GMR_OFF, DM_GMR_NOTS, DM_GMR_NOGB, DM_GMR_LAG, DM_GMR_SOON, DM_GMR_OLD, DM_GMR_FAST, DM_GMR_ANGLE, DM_GMR_JUMP, DM_GMR_N };
/*
 * dm12 heading correction (SNS_HC_JUMP=0: off): the median innovation over ~0.3 s, k x DM_HC_K_X, at most DM_HC_RATE deg/s
 * unless still (SNS_HC_RATE overrides), x DM_HC_TUMBLE_X while tumbling; none while |accel| is near 1 g and the tilt from q
 * is > DM_HC_TILT deg off it. Tuned on rec1 alone.
 */
#define DM_HC_MED_N 30		/* innovations, 0.3 s at 100 Hz */
#define DM_HC_RATE 15.0f	/* deg/s: <= 7.5 deg per 0.5 s */
#define DM_HC_TUMBLE 0.3f	/* rad/s off the vertical */
#define DM_HC_TUMBLE_X 0.033f
#define DM_HC_K_X 2.2f
#define DM_HC_ACC_DEV 0.05f
#define DM_HC_TILT 3.0f		/* deg */

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
		int hold_off, gate_off;		/* dm8: env switches (SNS_MAG_HOLD=0, SNS_MAG_GATE=0) */
		int gbias_off, hc_off;		/* dm9: env switches (SNS_DM_GBIAS=0, SNS_HEADING_CF=0) */
		int grav_q;			/* dm11: tilt from q (default); SNS_HEADING_GRAVITY=lp: the low-passed accel */
		float hc_k_slow, hc_k_fast;	/* dm10: DM_HC_K_* unless SNS_HC_K_SLOW / SNS_HC_K_FAST */
		/* dm11: AtA (xx xy xz yy yz zz), Atb, y.y and the weight sum; persisted in magcal.txt */
		double gm_ata[6], gm_atb[3], gm_yty, gm_w, gm_eig[3];	/* gm_eig: ascending */
		float gm_c[3], gm_r, gm_res;
		int gm_ok, gm_acc, gm_off;	/* gm_ok: two directions observable, gm_c is the bias; gm_off: SNS_MAG_GYROFIT=0 */
		unsigned gm_pairs;
		/* dm12 */
		int guard_off, gm_live_off, hcj_off;	/* SNS_MAG_GUARD=0, SNS_GM_LIVE=0, SNS_HC_JUMP=0 */
		float hc_rate;
		unsigned fit_rej, gm_solves, gm_rej[DM_GMR_N];	/* survive a fusion restart: "why" for the status line */
	} cal;
	/* dm8: mag used for the correction (gated), its time and the raw sample's time (ticks; 0 = untimed) */
	float mag_use[3];
	int have_mag_use, mag_rej;	/* dm9: mag_rej: the newest sample failed the gate */
	uint64_t mag_use_ts, mag_raw_ts;
	float dip[DM_MAG_DIP_N];
	int dip_n, dip_i, rej_streak;
	unsigned rej_r, rej_dip;	/* gate rejects (since dm_init) */
	float hd_out;			/* dm7: low-passed heading */
	uint64_t hd_ts;
	int hd_have;
	/* dm5: SEE's own mag_cal/gyro_cal biases (survive dm_init); the mag one replaces the box while status >= 1 */
	float see_mag_bias[3], see_gyro_bias[3];
	int see_mag_st, see_gyro_st;	/* -1 none yet (set by dm_init), else SEE status 0-3 */
	/* dm9: own gyro bias (survives dm_init, memory only), the still window and the complementary heading */
	float gbias[3], still_sum[3], still_acc, still_t;
	int gbias_have, still;
	float grav[3];			/* unit, device frame, up */
	int grav_have;
	float hc_h, hc_hmag;		/* deg, 0..360 */
	int hc_have, hc_hmag_ok;
	/* dm11: gyro-only orientation (device -> a free reference) and the mag history; restart with the fusion */
	float gq[4];
	struct dm_gm_hist {
		uint64_t ts;
		float m[3], q[4];
	} gm_h[DM_GM_HIST];
	int gm_hn, gm_hi, gm_tick, gm_jumps;
	uint64_t gm_fast_ts, gm_solve_ts;
	unsigned gm_solved_at;
	/* dm12: the gyro orientation ring, the mag samples waiting for the gyro, the heading innovations */
	struct dm_gm_gq {
		uint64_t ts;
		float q[4];
	} gm_gq[DM_GM_GQ_N];
	int gm_gq_n, gm_gq_i;
	struct dm_gm_pend {
		uint64_t ts;
		float m[3];
	} gm_pend[DM_GM_PEND_N];
	int gm_pend_n, gm_pend_i;
	float hc_d[DM_HC_MED_N], hc_k_used, hc_tilt;
	int hc_d_n, hc_d_i, hc_tilt_skip;
};

void dm_init(struct dm_fusion *f, float beta);
void dm_accel(struct dm_fusion *f, const float a[3]);
/* One filter step with this gyro sample; ts in ticks of ticks_per_s (0 = no step, only stored). */
void dm_gyro(struct dm_fusion *f, const float g[3], uint64_t ts, double ticks_per_s);
/* yaw_deg: test knob, extra rotation about the reference z axis (+ = heading increases, clockwise from above) */
void dm_report3(const struct dm_fusion *f, uint64_t ts, float yaw_deg, struct dm_report3 *r);
/* dm2: one raw mag sample (uT); dm7: feeds the sphere-fit ring (outliers rejected). Returns 0 kept, -1 rejected. */
int dm_mag(struct dm_fusion *f, const float m[3]);
/* dm8: the same with the sample time (ticks, 19.2 MHz) for the staleness checks */
int dm_mag_ts(struct dm_fusion *f, const float m[3], uint64_t ts);
/* dm8: report type in auto mode: 2 while a mag sample came within hold_s, else 3 */
int dm_type_auto(const struct dm_fusion *f, uint64_t ts, double hold_s);
int dm_mag_accuracy(const struct dm_fusion *f);	/* -1 uncalibrated, 0..2 */
void dm_mag_bias(const struct dm_fusion *f, float b[3]);
/* dm7: one sphere fit over the samples (exposed for tests): 0 ok (c, r, rms residual), -1 singular */
int dm_sphere_fit(const float (*m)[3], int n, float c[3], float *r, float *res);
/* dm5: SEE calibration events (bias uT or rad/s, status 0 unreliable .. 3 high) */
void dm_mag_cal_bias(struct dm_fusion *f, const float b[3], int status);
void dm_gyro_cal_bias(struct dm_fusion *f, const float b[3], int status);
int dm_mag_cal_dsp(const struct dm_fusion *f);	/* 1: the SEE bias is in use, 0: the fit */
const char *dm_mag_src(const struct dm_fusion *f);	/* dm11: "dsp", "gyro", "fit", "none" */
/* dm12: "gm pairs P solves S eig a,b,c rej off/nots/nogb/lag/soon/old/fast/angle/jump fitrej F" */
void dm_gm_status(const struct dm_fusion *f, char *buf, size_t n);
int dm_magcal_load(struct dm_fusion *f, const char *path);
int dm_magcal_save(const struct dm_fusion *f, const char *path);
float dm_heading_deg(const float q[4]);	/* device top in the reference frame, 0..360 clockwise from north */
/* dm9: tilt-compensated heading of mag m (any scale) with up u (unit), device frame. 0 ok, -1 m along u */
int dm_heading_mag(const float u[3], const float m[3], float *deg);
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
