/*
 * cam38 host test: isp_ae against a simulated sensor: real RAW10 frames through isp_measure, a lens shading like the
 * tuning's (1x in the middle, 4x in the corners, on a lens that falls off to match), headers made as camd makes them
 * (gain codes per sensor, clamps, frame length, a command showing CAM38_LATENCY frames later; the light one frame later
 * still in the "late" runs). 60 frames from camd's start values; it must settle within 15 % of the target (or at
 * its limits, with no more commands) and not swing. cc -O2 test_ae.c -lm -pthread && ./a.out
 */
#include <stdio.h>
#include "camera_isp.c"

#define TW	256u
#define TH	192u
#define TS	(TW * 5 / 4)
#define FRAMES	60
#define LAT	3

struct sensor {
	const char *name;
	uint32_t which, line_ns, margin, fl_min, fl_max, base, start_lines, start_code;
};
static const struct sensor rear = { "rear", 1, 10347, 40, 1608, 6443, 512, 1500, 256 };
static const struct sensor front = { "front", 2, 12750, 10, 2614, 5228, 1024, 2604, 512 };

static double
falloff(uint32_t x, uint32_t y)
{
	double u = 2.0 * x / (TW - 1) - 1, v = 2.0 * y / (TH - 1) - 1;

	return 1 / (1 + 1.5 * (u * u + v * v));         /* 4x at the corners */
}

/* a grey scene: `scene' 10-bit steps above black for 1 s at 1x in the lens's middle */
static void
make_raw(uint8_t *raw, double light)
{
	for (uint32_t y = 0; y < TH; y++) {
		for (uint32_t x = 0; x < TW; x += 4) {
			uint8_t *g = raw + y * TS + x / 4 * 5;

			g[4] = 0;
			for (uint32_t i = 0; i < 4; i++) {
				double v = 64 + light * falloff(x + i, y);
				unsigned p = v > 1023 ? 1023 : (unsigned)v;

				g[i] = (uint8_t)(p >> 2);
				g[4] |= (uint8_t)((p & 3) << (i * 2));
			}
		}
	}
}

static int
run(const struct sensor *c, const char *what, double scene, bool want_limit, bool old, int late)
{
	static uint8_t raw[TS * TH];
	static struct isp isp;
	struct { int at, fl, lines, code; } q[FRAMES * 2];
	int qn = 0, fl = (int)c->fl_min, lines = (int)c->start_lines, code = (int)c->start_code;
	int rl = lines, rc = code;      /* what the light sees */
	struct { int at, lines, code; } lq[FRAMES * 2];
	int lqn = 0, cmds = 0, last_cmd = 0, settled = -1;
	double means[FRAMES], target = 0;

	isp_init(&isp);
	isp.ae_old = old;
	isp.max_frame_lines = (uint32_t)(1e9 / (15.0 * c->line_ns));
	isp.max_frame_lines = isp.max_frame_lines > c->fl_max ? c->fl_max : isp.max_frame_lines;
	isp.lsc.on = true;
	for (int y = 0; y < LSC_H; y++) {
		for (int x = 0; x < LSC_W; x++) {
			double gain = 1 / falloff((uint32_t)(x * (TW - 1) / (LSC_W - 1)), (uint32_t)(y * (TH - 1) / (LSC_H - 1)));

			isp.lsc.gain[0][y][x] = isp.lsc.gain[1][y][x] = isp.lsc.gain[2][y][x] = (float)gain;
		}
	}
	for (int n = 0; n < FRAMES; n++) {
		struct redwood_camera_frame f;
		struct redwood_camera_command cmd;
		struct isp_stats st;

		for (int i = 0; i < qn; i++) {
			if (q[i].at == n) {
				fl = q[i].fl, lines = q[i].lines, code = q[i].code;
			}
		}
		for (int i = 0; i < lqn; i++) {
			if (lq[i].at == n) {
				rl = lq[i].lines, rc = lq[i].code;
			}
		}
		memset(&f, 0, sizeof(f));
		f.width = TW, f.height = TH, f.stride = TS, f.format = 1, f.black = 64, f.sensor = c->which;
		f.sequence = (uint64_t)n + 1;
		f.exposure_lines = (uint32_t)lines, f.line_ns = c->line_ns;
		f.gain = c->base * 256 / (c->base - (uint32_t)code), f.gain_unity = 256, f.gain_max = 2048;
		f.frame_lines = (uint32_t)fl, f.frame_lines_min = c->fl_min, f.frame_lines_max = c->fl_max, f.margin = c->margin;
		make_raw(raw, scene * rl * (double)c->line_ns / 1e9 * c->base / (c->base - rc));
		isp_measure(&isp, &f, raw, &st);
		means[n] = st.ae_mean * isp.digital;
		if (isp_ae(&isp, &f, &st, &cmd)) {
			/* camd + camid: the frame, the clamps, the truncated gain code */
			uint32_t g = cmd.b < 256 ? 256 : cmd.b > 2048 ? 2048 : cmd.b;
			int ncode = (int)(c->base - c->base * 256 / g);
			uint32_t nfl = cmd.c ? cmd.c : cmd.a + c->margin;

			nfl = nfl < c->fl_min ? c->fl_min : nfl > c->fl_max ? c->fl_max : nfl;
			uint32_t nl = cmd.a > nfl - c->margin ? nfl - c->margin : cmd.a;

			q[qn].at = n + LAT, q[qn].fl = (int)nfl, q[qn].lines = (int)nl, q[qn++].code = ncode;
			lq[lqn].at = n + LAT + late, lq[lqn].lines = (int)nl, lq[lqn++].code = ncode;
			cmds++;
			last_cmd = n;
		}
		target = isp.ae_seen_target > 0 ? isp.ae_seen_target : target;
	}
	double lo = 1e9, hi = 0;

	for (int n = FRAMES - 20; n < FRAMES; n++) {
		lo = means[n] < lo ? means[n] : lo;
		hi = means[n] > hi ? means[n] : hi;
	}
	for (int n = FRAMES - 1; n >= 0 && settled < 0; n--) {
		if (fabs(means[n] - target) > 0.15 * target) {
			settled = n + 1;
		}
	}
	settled = settled < 0 ? 0 : settled;
	bool steady = hi <= lo * 1.1, near = fabs(means[FRAMES - 1] - target) <= 0.15 * target;
	bool ok = want_limit ? steady && last_cmd < FRAMES - 20 : near && steady;

	printf("%-6s %-11s%s: settled at frame %2d, mean %5.1f, target %.0f, swing %.1f %%; %u lines, frame %d (%.1f fps), code %d, digital %.2fx; %d commands, the last at %d: %s\n",
	    c->name, what, late ? " late" : "", settled, means[FRAMES - 1], target, lo > 0 ? (hi / lo - 1) * 100 : 0, (uint32_t)lines, fl,
	    1e9 / (fl * (double)c->line_ns), code, isp.digital, cmds, last_cmd, ok ? "ok" : "FAIL");
	return ok ? 0 : 1;
}

int
main(void)
{
	int bad = 0;

	for (int late = 0; late < 2; late++) {
		bad += run(&rear, "dark", 150, false, false, late);     /* needs long frames and gain */
		bad += run(&rear, "bright", 200000, false, false, late);
		bad += run(&front, "dark", 150, false, false, late);
		bad += run(&front, "bright", 200000, false, false, late);
		bad += run(&rear, "black", 0.5, true, false, late);     /* out of reach: at its limits, quiet */
		bad += run(&front, "black", 0.5, true, false, late);
	}
	printf("test-ae: %s\n", bad ? "FAIL" : "ok");
	return bad != 0;
}
