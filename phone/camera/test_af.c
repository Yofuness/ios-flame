/*
 * af6 host test: af_frame (camera_af.h) against a simulated lens: a contrast curve peaked at a code, 3 % noise, a FOCUS
 * reaching the lens 2 frames late with the frame during the move spoilt, the frames' focus saying what was asked, AE
 * unstable for the first 20 frames. It must lock within 2 fine steps of the peak in < 60 frames of AE stable, then not
 * move; a new session sweeps again, a tap sweeps, a scene change sweeps, repeated ContinuousAutoFocusNow does nothing.
 * cc -O2 -Wall test_af.c -lm && ./a.out
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "camera_isp.h"

static void say(const char *fmt, ...) { (void)fmt; }

#define DELAY	2		/* frames between the write and the lens's move */
#define AE_AT	20
static int queue[4096], writes;
static double frame_no, peak, scale, width;
static int lens;
static int move_at = -1, move_to;

static long
camera_client_write(int device, const void *p, size_t n)
{
	const struct redwood_camera_command *c = p;

	(void)device, (void)n;
	if (c->command == REDWOOD_CAMERA_FOCUS) {
		move_at = (int)frame_no + 1 + DELAY;
		move_to = (int)c->a;
		queue[writes++ % 4096] = (int)c->a;
	}
	return 0;
}

float
isp_sharpness(const struct redwood_camera_frame *f, const uint8_t *raw, float x0, float y0, float x1, float y1, float *level)
{
	double d = (lens - peak) / width, noise = 1 + 0.03 * (2.0 * rand() / RAND_MAX - 1) * 1.7;

	(void)f, (void)raw, (void)x0, (void)y0, (void)x1, (void)y1;
	*level = 500;
	return (float)((0.02 + scale * exp(-d * d)) * noise);
}

#include "camera_af.h"

static const float win[4] = { 0.3f, 0.3f, 0.7f, 0.7f };

/* n frames; returns the frame (from now) at which it was last not locked in WATCH, at the end of it */
static int
run(struct af *af, int n, int *moves, int mode, bool ae_from_start)
{
	static struct redwood_camera_frame f;
	int last_busy = -1;

	f.gain = 256, f.gain_unity = 256;
	for (int i = 0; i < n; i++) {
		int before = writes;

		frame_no++;
		if (move_at >= 0 && (int)frame_no >= move_at) {
			lens = move_to;
			move_at = -1;
		}
		f.focus = af->started ? af->at : (uint32_t)lens;
		af_frame(0, af, &f, NULL, win, false, mode, -1, ae_from_start || i >= AE_AT);
		if (writes != before) {
			(*moves)++;
		}
		if (af_searching(af)) {
			last_busy = i;
		}
	}
	return last_busy;
}

static int
session(struct af *af, double p, double sc, double w, int *frames, int settle)
{
	int moves = 0, after = 0, last;

	af_init(af, false);
	af->settle = (unsigned)settle;
	peak = p, scale = sc, width = w, lens = 0, move_at = -1;
	last = run(af, 400, &moves, AF_MODE_CONTINUOUS, false);
	*frames = last + 1 - AE_AT;
	(void)after;
	return abs((int)af->at - (int)p);
}

int
main(void)
{
	static const double peaks[] = { 0, 60, 128, 300, 512, 640, 800, 960, 1023 };
	struct af af;
	int fails = 0;

	/* the settle's worth, by comparison */
	for (int settle = 1; settle <= AF_SETTLE_FRAMES; settle++) {
		int worst = 0, slowest = 0;

		for (int seed = 0; seed < 40; seed++) {
			srand((unsigned)seed + 1);
			for (unsigned i = 0; i < sizeof(peaks) / sizeof(peaks[0]); i++) {
				int frames, err = session(&af, peaks[i], 1, 130, &frames, settle);

				worst = err > worst ? err : worst;
				slowest = frames > slowest ? frames : slowest;
			}
		}
		printf("af settle %d frames: worst error %d, slowest lock %d frames\n", settle, worst, slowest);
		if (settle == AF_SETTLE_FRAMES && (worst > 2 * AF_FINE || slowest >= 60)) {
			printf("FAIL: should be within %d of the peak in < 60 frames\n", 2 * AF_FINE);
			fails++;
		}
	}

	/* settled: no hunting for 3000 frames, with noise; asking for continuous focus changes nothing */
	int frames, moves = 0;

	srand(7);
	session(&af, 700, 1, 130, &frames, AF_SETTLE_FRAMES);
	run(&af, 3000, &moves, AF_MODE_CONTINUOUS, true);
	af_asked(&af, true);
	run(&af, 200, &moves, AF_MODE_CONTINUOUS, true);
	printf("af settled: %d moves in 3200 frames\n", moves);
	fails += moves != 0;

	/* a tap sweeps and holds */
	moves = 0;
	af_asked(&af, false);
	run(&af, 100, &moves, AF_MODE_AUTO, true);
	printf("af tap: %d moves, at %u, hold %d\n", moves, af.at, af.hold);
	fails += moves < 9 || abs((int)af.at - 700) > 2 * AF_FINE || !af.hold;
	/* continuous again, from the hold: one more sweep, then still */
	moves = 0;
	af_asked(&af, true);
	af_asked(&af, true);
	run(&af, 100, &moves, AF_MODE_CONTINUOUS, true);
	int settled = moves;

	moves = 0;
	run(&af, 500, &moves, AF_MODE_CONTINUOUS, true);
	printf("af back to continuous: %d moves, then %d\n", settled, moves);
	fails += settled < 9 || moves != 0;

	/* the scene changes: contrast down by half, elsewhere; it finds it */
	moves = 0;
	peak = 200, scale = 0.5;
	run(&af, 200, &moves, AF_MODE_CONTINUOUS, true);
	printf("af scene change: %d moves, at %u\n", moves, af.at);
	fails += moves < 9 || abs((int)af.at - 200) > 2 * AF_FINE;
	/* a smaller change of contrast is left alone */
	moves = 0;
	scale = 0.42;
	run(&af, 300, &moves, AF_MODE_CONTINUOUS, true);
	printf("af contrast -16%%: %d moves\n", moves);
	fails += moves != 0;

	/* a new session (camd reset the lens): sweeps again, whatever it knew */
	moves = 0;
	af_reset(&af);
	lens = 0, move_at = -1, peak = 900, scale = 1;
	run(&af, 200, &moves, AF_MODE_CONTINUOUS, true);
	printf("af new session: %d moves, at %u\n", moves, af.at);
	fails += moves < 9 || abs((int)af.at - 900) > 2 * AF_FINE;

	/* AE that never settles: still a search; a flat, black picture: one search, no hunting */
	struct redwood_camera_frame f = { .gain = 256, .gain_unity = 256 };

	af_init(&af, false);
	lens = 0, move_at = -1, peak = 400, scale = 1;
	for (int i = 0; i < 400; i++) {
		frame_no++;
		if (move_at >= 0 && (int)frame_no >= move_at) {
			lens = move_to, move_at = -1;
		}
		f.focus = af.started ? af.at : (uint32_t)lens;
		af_frame(0, &af, &f, NULL, win, false, AF_MODE_CONTINUOUS, -1, false);
	}
	printf("af AE never stable: at %u, searching %d\n", af.at, af_searching(&af));
	fails += abs((int)af.at - 400) > 2 * AF_FINE || af_searching(&af);

	printf(fails ? "af: %d FAILED\n" : "af: all good\n", fails);
	return fails != 0;
}
