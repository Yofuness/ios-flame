/* camrx_lib: camrx's register code as a library (camrx.c = main + usage, camd.c = camera f1 daemon) */
#ifndef CAMRX_LIB_H
#define CAMRX_LIB_H
#include <signal.h>
#include <stdint.h>
#include <sys/types.h>

/* cam34: the WM writes RAW10 lines padded to 16 B (rear 2520 -> 2528, front 2050 -> 2064); CAM_STRIDE=N overrides */
#define RAW10_STRIDE(w)	((((w) * 10u / 8u) + 15u) & ~15u)
#define REAR_FRAME_BYTES (1512u * RAW10_STRIDE(2016u))	/* 3822336: 2016 x 1512 RAW10, stride 2528 (CSID counts 3810240) */
#define FRONT_FRAME_BYTES (924u * RAW10_STRIDE(1640u))	/* 1907136: IMX355 mode 2, 1640 x 924 RAW10, stride 2064 */
#define FRAME_BYTES_MAX	REAR_FRAME_BYTES
#define CAP_CO_PA	0x9cb00000ull	/* display carveout zero-copy slot 1 (SweetSurface SWEET_ZC_SLOT_PA(1)) */
#define CAP_CO_LEN	0xB00000u	/* frame + guard */
#define RING_N		2

/*
 * cam-f1: the sensor path, picked by cam_select() before anything runs (default rear). Rear: IMX363 mode 8 on
 * CSIPHY0; front (--front): IMX355 mode 2 on CSIPHY2 (DT csiphy@ace4000, cell-index 2), its own phytimer RCG and
 * csiphy/phytimer branches (camcc-sm7150.c csi2phytimer 0x604c, branches 0x6064/0x6068), CSID RX_CFG0 phy 2.
 * CSID-lite 0, RDI0, IFE-lite WM0 and the SMMU SIDs are the same for both.
 */
struct cam_sensor {
	const char *name;
	int sensor;			/* camd frame header: 1 rear, 2 front */
	unsigned w, h, stride, bytes;	/* RAW10 packed */
	unsigned extra;			/* bytes the WM writes past the frame (embedded lines) */
	unsigned mbps;			/* lane rate (settle count) */
	unsigned phy;			/* CSIPHY index = CSID RX_CFG0 phy select */
	uint32_t phy_base, timer_rcg, timer_cbcr, phy_cbcr;
	const char *stream_cmd;		/* camrx's default sensor command */
};
extern const struct cam_sensor *cam;
void cam_select(int front);

#define FRAME_BYTES	(cam->bytes)
#define FRAME_W		(cam->w)
#define FRAME_H		(cam->h)
#define FRAME_STRIDE	(cam->stride)
#define FRAME_EXTRA	(cam->extra)
#define STREAM_CMD	(cam->stream_cmd)
#define PLACE_BYTES	((FRAME_BYTES + FRAME_EXTRA + 0xffffu) & ~0xffffu)
#define STREAM_MAX_S	600

extern int dry;
extern volatile sig_atomic_t stop;	/* the stream loop's end flag (SIGINT/SIGTERM in camrx) */
extern uint16_t sid_mask;
extern uint32_t addr_off;
extern int sid2;
extern double stage_t0;	/* B43: stage() lines get "+s" since this (CLOCK_MONOTONIC), 0 = off */

/*
 * camd (camera f1): stream_run with hooks. NULL (camrx) = the e1 behaviour. With hooks: SECONDS 0 = until tick()
 * says stop, no signal handlers set (the caller's), PREFIX NULL = no frame saved at the end.
 *   spawn      starts the sensor command instead of run_cmd (0 = ok)
 *   busy       the ring slot being read out, or -1: the next SOF does not hand it to the WM (frames are dropped
 *              instead, the WM rewrites its own slot)
 *   frame      a slot done that the WM will not write next (seq = WM0 done count, from 1; sof = its SOF, s)
 *   tick       every 1 ms poll; nonzero ends the loop
 *   ended      after the loop, WM0 off: stop the sensor command (it is then waited for)
 */
struct cam_stream_hooks {
	int (*spawn)(pid_t *pid);
	int (*busy)(void);
	void (*frame)(int slot, uint64_t seq, double sof);
	int (*tick)(void);
	void (*ended)(void);
};
extern const struct cam_stream_hooks *stream_hooks;

/* cam36: why stream_run ended (it returns 2 for FAULT, 1 for the others but OK). CAM36_OFF=1 in the environment: the
 * pre-cam36 behaviour (carveout zeroed once per process, no stall watchdog, no SIGKILL, any failure = 1) */
enum { CAM_END_OK, CAM_END_SETUP, CAM_END_FAULT, CAM_END_PAST_RING, CAM_END_NO_FRAMES, CAM_END_STOPPED, CAM_END_SAVE };
extern int stream_end;
int cam36_off(void);

int precheck(void);
int status(void);
int smmu_report(int n, char **sids);
int clocks_on(void);
int clocks_off(void);
int tpg(int secs);
int phy_on(int settle_arg);
int phy_off(void);
int rx_arm(void);
int rx_report(void);
int rx_stop(void);
int capture(const char *out, const char *cmd, int alloc32, int trap, int sid, int xlate);
int stream_run(int secs, const char *prefix, const char *cmd);

#endif
