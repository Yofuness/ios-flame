/*
 * apr_tone: audio M1b (docs/features/audio.md), a 1 kHz beep on the bottom speaker. Run after adspboot and a
 * successful aprtest; it never boots or resets the ADSP.
 *
 *   apr_tone [--dry-run] [--amp L|R|both] [--secs S] [--vol-db N] [--boost] [--no-amp] [--rt-port P]
 *            [--hwvote] [--lpi-pins] [-r]
 *
 *   --probe     amp IDs over I2C only (read-only), then exit; no ADSP needed
 *   --dry-run   print the planned APR packets and CS35L41 writes; no /dev/pmem, no glink, no I2C
 *   --amp       which CS35L41 (0x40 = L, 0x41 = R; default L). --no-amp: AFE only, amps untouched
 *   --secs      tone length, default 2, capped so the amp is on at most 3 s
 *   --vol-db    amp digital volume in dB down, at least (and default) 20; the PCM itself is at -20 dBFS
 *   --boost     internal boost converter on (peak 3700 mA, the DT value); default: bypass, VBST = VP
 *   --rt-port   RT proxy port the PCM is written to, default 0x2001 (RT_PROXY_PORT_001_TX)
 *   --hwvote    AFE LPASS core hw vote first (needed by --lpi-pins)
 *   --lpi-pins  set LPI GPIO 8/9/10 (sec MI2S SCK/WS/SD0) to their DT functions, restored at exit
 *
 * Tone path, the smallest found that needs no ASM stream or ADM/routing matrix: our PCM in a DRAM buffer is
 * mapped with AFE_SERVICE_CMD_SHARED_MEM_MAP_REGIONS, written to an RT proxy port with AFE_SERVICE_CMD_RTPORT_WR,
 * and an AFE loopback (AFE_MODULE_LOOPBACK) routes that port to SECONDARY_MI2S_RX (0x1002), where the amps sit
 * (DT: qcom,msm-dai-q6-mi2s-sec, rx-lines 0x1 = SD0; I2S 48 kHz, 16 bit, stereo -> BCLK 1.536 MHz, the IBIT
 * clock set with AFE_PARAM_ID_CLOCK_SET, afe-clk-ver 2). The AFE has no tone generator we know of. Fallback if
 * the loopback is refused: ASM open-write + ADM copp + matrix map (more commands, same memory map).
 * Opcodes and structs are from memory of Linux q6afe.c / downstream apr_audio-v2.h; confidence in the table.
 * Unknown: whether the ADSP reaches our physical buffer without an LPASS SMMU mapping.
 *
 * Order: amp probe (I2C, read-only) -> AFE clock, I2S config, MI2S start -> buffer map, RT proxy config and
 * start, loopback -> amp config + power-up -> stream -> amp power-down -> AFE teardown. Every exit path
 * powers the amp down first. Exit 0 = the tone ran to the end without an amp error.
 */
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "pmem.h"
#include "apr.h"
#include "geni_i2c.h"
#include "cs35l41.h"

/* AFE opcodes (confidence) */
#define AFE_PORT_CMD_DEVICE_START		0x000100e5	/* 85% */
#define AFE_PORT_CMD_DEVICE_STOP		0x000100e6	/* 85% */
#define AFE_PORT_CMD_SET_PARAM_V2		0x000100ef	/* 80% */
#define AFE_SVC_CMD_SET_PARAM			0x000100fc	/* 65% */
#define AFE_SERVICE_CMD_SHARED_MEM_MAP_REGIONS	0x000100ea	/* 75% */
#define AFE_SERVICE_CMDRSP_SHARED_MEM_MAP_REGIONS 0x000100eb	/* 75% */
#define AFE_SERVICE_CMD_SHARED_MEM_UNMAP_REGIONS 0x000100ec	/* 70% */
#define AFE_SERVICE_CMD_RTPORT_WR		0x000100ed	/* 65% */
#define AFE_CMD_REMOTE_LPASS_CORE_HW_VOTE_REQUEST 0x0001027c	/* 35% */
#define AFE_CMD_RSP_REMOTE_LPASS_CORE_HW_VOTE_REQUEST 0x0001027d /* 35% */
#define AFE_CMD_REMOTE_LPASS_CORE_HW_DEVOTE_REQUEST 0x0001027e	/* 35% */
/* modules / params */
#define AFE_MODULE_AUDIO_DEV_INTERFACE	0x0001020c	/* 75% */
#define AFE_PARAM_ID_I2S_CONFIG		0x0001020d	/* 75% */
#define AFE_PARAM_ID_RT_PROXY_CONFIG	0x00010213	/* 55% */
#define AFE_MODULE_LOOPBACK		0x00010205	/* 50% */
#define AFE_PARAM_ID_LOOPBACK_CONFIG	0x0001020b	/* 50% */
#define AFE_MODULE_CLOCK_SET		0x0001028f	/* 60% */
#define AFE_PARAM_ID_CLOCK_SET		0x00010290	/* 60% */
#define CLK_ID_SEC_MI2S_IBIT		0x102		/* 70% */
#define SECONDARY_MI2S_RX		0x1002		/* 85% */
#define ADSP_MEMORY_MAP_SHMEM8_4K_POOL	3		/* 65% */

#define RATE		48000
#define CH		2
#define BCLK		(RATE * 16 * CH)	/* 1.536 MHz */
#define CHUNK_FR	480			/* 10 ms */
#define CHUNK_B		(CHUNK_FR * CH * 2)
#define AMP_MAX_MS	3000
#define LPI_PA		0x62b40000u

static volatile sig_atomic_t stop;
static int dry, n_amp, hw_handle = -1, probe_only;
static struct cs_amp amps[2];
static uint16_t rt_port = 0x2001;
static uint32_t map_handle;
static uint64_t buf_pa;
static uint32_t lpi_saved[3];
static int lpi_set;

static void on_sig(int s)
{
	(void)s;
	stop = 1;
}

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* little payload builder */
struct pl {
	uint8_t b[512];
	size_t n;
};
static void p16(struct pl *p, uint16_t v) { memcpy(p->b + p->n, &v, 2); p->n += 2; }
static void p32(struct pl *p, uint32_t v) { memcpy(p->b + p->n, &v, 4); p->n += 4; }

static int step(const char *what, int st)
{
	printf("apr_tone: %-28s %s (status %d)\n", what, st ? "FAILED" : "ok", st);
	return st;
}

/* AFE_PORT_CMD_SET_PARAM_V2, in-band: port, payload size, addr lsw/msw 0, handle 0, then the param. */
static int port_param(const char *what, uint16_t port, uint32_t module, uint32_t param, const struct pl *d)
{
	struct pl p = { .n = 0 };
	p16(&p, port);
	p16(&p, (uint16_t)(12 + d->n));
	p32(&p, 0); p32(&p, 0); p32(&p, 0);
	p32(&p, module); p32(&p, param);
	p16(&p, (uint16_t)d->n); p16(&p, 0);
	memcpy(p.b + p.n, d->b, d->n);
	p.n += d->n;
	return step(what, apr_cmd(APR_SVC_AFE, AFE_PORT_CMD_SET_PARAM_V2, p.b, p.n, 0, 1000));
}

static int port_cmd(const char *what, uint32_t op, uint16_t port)
{
	struct pl p = { .n = 0 };
	p16(&p, port); p16(&p, 0);
	return step(what, apr_cmd(APR_SVC_AFE, op, p.b, p.n, 0, 1000));
}

/* AFE_SVC_CMD_SET_PARAM with AFE_PARAM_ID_CLOCK_SET (v1 struct): the sec MI2S bit clock. */
static int clock_set(int on)
{
	struct pl p = { .n = 0 };
	p32(&p, 12 + 24); p32(&p, 0); p32(&p, 0); p32(&p, 0);
	p32(&p, AFE_MODULE_CLOCK_SET); p32(&p, AFE_PARAM_ID_CLOCK_SET); p16(&p, 24); p16(&p, 0);
	p32(&p, 1);			/* clk_set_minor_version */
	p32(&p, CLK_ID_SEC_MI2S_IBIT);
	p32(&p, on ? BCLK : 0);
	p16(&p, 1);			/* clk_attri: COUPLE_NO */
	p16(&p, 0);			/* clk_root: default */
	p32(&p, on ? 1 : 0);
	return step(on ? "clock set IBIT 1.536 MHz" : "clock IBIT off",
		    apr_cmd(APR_SVC_AFE, AFE_SVC_CMD_SET_PARAM, p.b, p.n, 0, 1000));
}

static int i2s_config(void)
{
	struct pl d = { .n = 0 };
	p32(&d, 1);		/* i2s_cfg_minor_version */
	p16(&d, 16);		/* bit_width */
	p16(&d, 1);		/* channel_mode: AFE_PORT_I2S_SD0 (DT rx-lines 0x1) */
	p16(&d, 1);		/* mono_stereo: stereo */
	p16(&d, 1);		/* ws_src: internal (we are master, qcom,msm-mi2s-master) */
	p32(&d, RATE);
	p16(&d, 0);		/* data_format: linear PCM */
	p16(&d, 0);
	return port_param("I2S config sec MI2S RX", SECONDARY_MI2S_RX, AFE_MODULE_AUDIO_DEV_INTERFACE,
			  AFE_PARAM_ID_I2S_CONFIG, &d);
}

static int rt_config(void)
{
	struct pl d = { .n = 0 };
	p32(&d, 1);		/* minor version */
	p16(&d, 16);		/* bit_width */
	p16(&d, 1);		/* interleaved */
	p16(&d, CHUNK_B);	/* frame_size (bytes) */
	p16(&d, CHUNK_B / 2);	/* jitter_allowance */
	p16(&d, 0);		/* low_water_mark */
	p16(&d, 0);		/* high_water_mark */
	p32(&d, RATE);
	p16(&d, CH);
	p16(&d, 0);
	return port_param("RT proxy config", rt_port, AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_RT_PROXY_CONFIG, &d);
}

static int loopback(int on)
{
	struct pl d = { .n = 0 };
	p32(&d, 1);		/* loopback_cfg_minor_version */
	p16(&d, SECONDARY_MI2S_RX);	/* dst_port_id */
	p16(&d, 1);		/* routing_mode: LB_MODE_DEFAULT */
	p16(&d, on ? 1 : 0);
	p16(&d, 0);
	return port_param(on ? "loopback RT proxy -> MI2S" : "loopback off", rt_port, AFE_MODULE_LOOPBACK,
			  AFE_PARAM_ID_LOOPBACK_CONFIG, &d);
}

static int mem_map(uint64_t pa, uint32_t size)
{
	struct pl p = { .n = 0 };
	p16(&p, ADSP_MEMORY_MAP_SHMEM8_4K_POOL); p16(&p, 1); p32(&p, 0);	/* pool, 1 region, physical */
	p32(&p, (uint32_t)pa); p32(&p, (uint32_t)(pa >> 32)); p32(&p, size);
	int st = apr_cmd(APR_SVC_AFE, AFE_SERVICE_CMD_SHARED_MEM_MAP_REGIONS, p.b, p.n,
			 AFE_SERVICE_CMDRSP_SHARED_MEM_MAP_REGIONS, 1000);
	if (!st)
		map_handle = dry ? 1 : apr_rsp[0];
	printf("apr_tone: buffer %#llx + %u mapped, handle %#x\n", (unsigned long long)pa, size, map_handle);
	return step("shared mem map", st || (!dry && !map_handle));
}

static int hw_vote(void)
{
	struct pl p = { .n = 0 };
	p32(&p, 1);			/* hw_block_id: AFE_LPASS_CORE_HW_MACRO_BLOCK? (35%) */
	memcpy(p.b + p.n, "aprtone", 8);
	p.n += 8;
	int st = apr_cmd(APR_SVC_AFE, AFE_CMD_REMOTE_LPASS_CORE_HW_VOTE_REQUEST, p.b, p.n,
			 AFE_CMD_RSP_REMOTE_LPASS_CORE_HW_VOTE_REQUEST, 1000);
	if (!st)
		hw_handle = dry ? 1 : (int)apr_rsp[0];
	return step("LPASS core hw vote", st);
}

static void hw_devote(void)
{
	struct pl p = { .n = 0 };
	p32(&p, 1);
	p32(&p, (uint32_t)hw_handle);
	step("LPASS core hw devote", apr_cmd(APR_SVC_AFE, AFE_CMD_REMOTE_LPASS_CORE_HW_DEVOTE_REQUEST, p.b, p.n, 0, 1000));
}

/* LPI TLMM CFG (pinctrl-lpass-lpi.c, 50%): pull [1:0], func [5:2], drive [8:6] = mA/2 - 1. */
static void lpi_pins(int on)
{
	static const uint32_t func[3] = { 3, 3, 4 };	/* gpio8 SCK, gpio9 WS, gpio10 SD0 (live.dts:23261-23335) */
	for (int i = 0; i < 3; i++) {
		uint64_t pa = LPI_PA + 0x1000u * (8 + i);
		uint32_t v = on ? (func[i] << 2 | 3u << 6) : lpi_saved[i];
		if (on && !dry)
			lpi_saved[i] = rd32(pa);
		printf("apr_tone: %slpi gpio%d cfg %#llx: %#x -> %#x\n", dry ? "[dry] " : "", 8 + i,
		       (unsigned long long)pa, on ? lpi_saved[i] : 0, v);
		if (!dry)
			wr32(pa, v);
	}
	lpi_set = on;
}

static void amps_down(void)
{
	for (int i = 0; i < n_amp; i++)
		cs_power_down(&amps[i]);	/* sp1: also restores a configured, never powered amp */
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	const char *which = "L";
	double secs = 2;
	int vol = 20, boost = 0, no_amp = 0, reset = 0, want_vote = 0, want_lpi = 0, rc = 1;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(a, "--dry-run")) dry = 1;
		else if (!strcmp(a, "--probe")) probe_only = 1;
		else if (!strcmp(a, "--amp") && v) which = argv[++i];
		else if (!strcmp(a, "--secs") && v) secs = atof(argv[++i]);
		else if (!strcmp(a, "--vol-db") && v) vol = atoi(argv[++i]);
		else if (!strcmp(a, "--rt-port") && v) rt_port = (uint16_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(a, "--boost")) boost = 1;
		else if (!strcmp(a, "--no-amp")) no_amp = 1;
		else if (!strcmp(a, "--hwvote")) want_vote = 1;
		else if (!strcmp(a, "--lpi-pins")) want_vote = want_lpi = 1;
		else if (!strcmp(a, "-r")) reset = 1;
		else {
			fprintf(stderr, "usage: apr_tone [--probe] [--dry-run] [--amp L|R|both] [--secs S] [--vol-db N] [--boost]"
				" [--no-amp] [--rt-port P] [--hwvote] [--lpi-pins] [-r]\n");
			return 2;
		}
	}
	if (vol < 20)
		vol = 20;
	if (secs <= 0 || secs > 2.5)
		secs = secs <= 0 ? 2 : 2.5;	/* + ramp margin stays under AMP_MAX_MS */
	printf("apr_tone: %s, amp %s, %.1f s, -%d dB amp + -20 dBFS PCM, boost %s, rt port %#x\n",
	       dry ? "DRY RUN" : "live", no_amp ? "none" : which, secs, vol, boost ? "on" : "bypass", rt_port);
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	apr_set_dry(dry);
	cs_set_dry(dry);
	if (!dry)
		pm_init();

	/* 1. amps: answer on I2C, read-only */
	if (!no_amp) {
		if (strchr(which, 'L') || !strcmp(which, "both"))
			amps[n_amp++] = (struct cs_amp){ .addr = 0x40, .right = 0 };
		if (strchr(which, 'R') || !strcmp(which, "both"))
			amps[n_amp++] = (struct cs_amp){ .addr = 0x41, .right = 1 };
		if (!dry && gi2c_init())
			return 1;
		for (int i = 0; i < n_amp; i++)
			if (cs_probe(&amps[i]))
				return 1;
	}
	if (probe_only) {
		printf("apr_tone: probe done, %d amp(s) answer\n", n_amp);
		return 0;
	}

	/* 2. PCM: 1 kHz sine, -20 dBFS, both channels; 48 samples per period */
	uint32_t nfr = (uint32_t)(secs * RATE) / CHUNK_FR * CHUNK_FR, bytes = nfr * CH * 2;
	uint32_t map_sz = (bytes + 4095) & ~4095u;
	int16_t *pcm = malloc(bytes);
	if (!pcm)
		return 1;
	for (uint32_t i = 0; i < nfr; i++)
		pcm[2 * i] = pcm[2 * i + 1] = (int16_t)lrint(3277.0 * sin(2 * M_PI * (i % 48) / 48.0));
	if (!dry) {
		buf_pa = pm_alloc(map_sz, 4096);
		if (!buf_pa || pm_wr(buf_pa, pcm, bytes))
			return 1;
	} else {
		buf_pa = 0x80000000ull;
	}

	/* 3. AFE */
	if (apr_open(reset))
		return 1;
	int clk = 0, mi2s = 0, rt = 0, lb = 0;
	if (want_vote && hw_vote())
		goto out;
	if (want_lpi)
		lpi_pins(1);
	if (clock_set(1))
		goto out;
	clk = 1;
	if (i2s_config() || port_cmd("start sec MI2S RX", AFE_PORT_CMD_DEVICE_START, SECONDARY_MI2S_RX))
		goto out;
	mi2s = 1;
	if (mem_map(buf_pa, map_sz) || rt_config() || port_cmd("start RT proxy", AFE_PORT_CMD_DEVICE_START, rt_port))
		goto out;
	rt = 1;
	if (loopback(1))
		goto out;
	lb = 1;

	/* 4. amps on */
	for (int i = 0; i < n_amp; i++)
		if (cs_config(&amps[i], -vol, boost) || cs_verify(&amps[i]))	/* sp1: dB, read-back */
			goto out;
	uint64_t t0 = now_ms();
	for (int i = 0; i < n_amp; i++)
		if (cs_power_up(&amps[i]))
			goto out;

	/* 5. stream 10 ms chunks in real time; amp IRQ check every 100 ms; hard stop at AMP_MAX_MS */
	uint32_t sent = 0, nchunk = nfr / CHUNK_FR;
	int amp_err = 0;
	for (uint32_t c = 0; c < nchunk && !stop && !amp_err; c++) {
		if (now_ms() - t0 > AMP_MAX_MS - 200)
			break;
		struct pl p = { .n = 0 };
		uint64_t pa = buf_pa + (uint64_t)c * CHUNK_B;
		p16(&p, rt_port); p16(&p, 0);
		p32(&p, (uint32_t)pa); p32(&p, (uint32_t)(pa >> 32)); p32(&p, map_handle); p32(&p, CHUNK_B);
		if (dry && c >= 2) {
			sent++;
			continue;	/* print the first two only */
		}
		if (apr_send_nowait(APR_SVC_AFE, AFE_SERVICE_CMD_RTPORT_WR, p.b, p.n))
			break;
		sent++;
		while (!dry && now_ms() < t0 + 10ull * (c + 1) && !stop)
			apr_pump(1);
		if (c % 10 == 9)
			for (int i = 0; i < n_amp; i++)
				if (cs_check(&amps[i]))
					amp_err = 1;
	}
	printf("apr_tone: %u of %u chunks written in %llu ms%s\n", sent, nchunk,
	       (unsigned long long)(now_ms() - t0), amp_err ? ", AMP ERROR" : stop ? ", stopped" : "");
	rc = amp_err || stop || sent < nchunk;

out:
	/* 6. teardown, amp first */
	amps_down();
	if (lb)
		loopback(0);
	if (rt)
		port_cmd("stop RT proxy", AFE_PORT_CMD_DEVICE_STOP, rt_port);
	if (map_handle) {
		struct pl p = { .n = 0 };
		p32(&p, map_handle);
		step("shared mem unmap", apr_cmd(APR_SVC_AFE, AFE_SERVICE_CMD_SHARED_MEM_UNMAP_REGIONS, p.b, p.n, 0, 1000));
	}
	if (mi2s)
		port_cmd("stop sec MI2S RX", AFE_PORT_CMD_DEVICE_STOP, SECONDARY_MI2S_RX);
	if (clk)
		clock_set(0);
	if (lpi_set)
		lpi_pins(0);
	if (hw_handle >= 0)
		hw_devote();
	apr_close();
	if (!dry && buf_pa)
		pm_free(buf_pa);
	printf("apr_tone: done, exit %d\n", rc);
	return rc;
}
