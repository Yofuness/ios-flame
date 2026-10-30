/* camrx_lib: camrx's register code as a library (camrx.c = main + usage, camd.c = camera f1 daemon) */
#ifndef CAMRX_LIB_H
#define CAMRX_LIB_H
#include <signal.h>
#include <stdint.h>
#include <sys/types.h>

#define FRAME_BYTES	3810240u	/* 2016 x 1512 RAW10 packed = CSID bytes ping/pong in cam8 */
#define FRAME_W		2016
#define FRAME_H		1512
#define FRAME_STRIDE	2520
#define CAP_CO_PA	0x9cb00000ull	/* display carveout zero-copy slot 1 (SweetSurface SWEET_ZC_SLOT_PA(1)) */
#define CAP_CO_LEN	0xB00000u	/* frame + guard */
#define STREAM_CMD	"/var/local/bin/camid --addr 0x1a --keep --mclk-asis --stream /var/local/cam/imx363-mode8.txt"
#define FRAME_EXTRA	12096u	/* cam21: ~4.8 lines of 2520 B past 2016x1512 */
#define RING_N		2
#define PLACE_BYTES	((FRAME_BYTES + FRAME_EXTRA + 0xffffu) & ~0xffffu)
#define STREAM_MAX_S	600

extern int dry;
extern volatile sig_atomic_t stop;	/* the stream loop's end flag (SIGINT/SIGTERM in camrx) */
extern uint16_t sid_mask;
extern uint32_t addr_off;
extern int sid2;

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
