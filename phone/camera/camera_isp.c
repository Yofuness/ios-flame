/* from ios-redfin (MIT), phone/audio/camera_isp.c */
/*
 * The cameras' frames made into pictures by the processor (see camera_isp.h): one pixel per 2x2
 * cell, the nearest cell for each pixel of the picture (previews), or every pixel by bilinear
 * demosaic (stills); per pixel, in this order: the black level off, lens shading (a grid of gains per
 * colour, when there is one), the white balance's and AE's gains, the colour matrix (identity until
 * one is set), sRGB's curve. Grey-world white balance; no noise reduction yet. The colour matrix and
 * the lens shading can come from the camera's own tuning (isp_load_tuning(), isp_tune()).
 */
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#include <sys/sysctl.h>
#else
#include <pthread.h>
#endif
#include "camera_isp.h"

#define WHITE           959             /* 1023 less the black level of 64 */
#define LUT_WHITE       3836            /* WHITE in the gamma table's steps */

void
isp_init(struct isp *isp)
{
	memset(isp, 0, sizeof(*isp));
	isp->wb[0] = isp->wb[1] = isp->wb[2] = 1;
	isp->digital = 1;
	isp->sensor_digital = 1;
	isp->zoom = 1;
	for (int i = 0; i < 3; i++) {
		isp->ccm[i][i] = 1;
	}
	for (int i = 0; i < 4096; i++) {
		double v = i >= LUT_WHITE ? 1 : (double)i / LUT_WHITE;

		v = v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1 / 2.4) - 0.055;
		isp->gamma[i] = (uint8_t)(v * 255 + 0.5);
	}
}

/* where red and blue are in a cell (0 top left, 1 top right, 2 bottom left, 3 bottom right) */
static void
cell_places(uint32_t bayer, int *r, int *b)
{
	static const int red[4] = { 0, 1, 2, 3 }, blue[4] = { 3, 2, 1, 0 };

	*r = red[bayer & 3];
	*b = blue[bayer & 3];
}

/* the four values of the cell at cx of the pair of lines from `top' (above black, 0 and up) */
static inline void
cell(const uint8_t *top, size_t stride, uint32_t cx, int v[4])
{
	const uint8_t *g0 = top + (size_t)(cx >> 1) * 5, *g1 = g0 + stride;
	unsigned o = (cx & 1) * 2;

	v[0] = (g0[o] << 2 | (g0[4] >> (o * 2) & 3)) - 64;
	v[1] = (g0[o + 1] << 2 | (g0[4] >> (o * 2 + 2) & 3)) - 64;
	v[2] = (g1[o] << 2 | (g1[4] >> (o * 2) & 3)) - 64;
	v[3] = (g1[o + 1] << 2 | (g1[4] >> (o * 2 + 2) & 3)) - 64;
	for (int i = 0; i < 4; i++) {
		v[i] = v[i] < 0 ? 0 : v[i];
	}
}

float
isp_sharpness(const struct redwood_camera_frame *f, const uint8_t *raw, float x0, float y0, float x1, float y1, float *level)
{
	enum { BLOCK = 4, MAX_BLOCKS = 512 };
	uint32_t cells_w = f->width / 2, cells_h = f->height / 2;
	uint32_t cx0 = (uint32_t)(x0 * cells_w), cx1 = (uint32_t)(x1 * cells_w), cy0 = (uint32_t)(y0 * cells_h),
	    cy1 = (uint32_t)(y1 * cells_h);
	float above[MAX_BLOCKS], here[MAX_BLOCKS];
	double steps = 0, sum = 0;
	uint32_t blocks_w, rows = 0, n = 0;
	int r, b;

	cell_places(f->bayer, &r, &b);
	cx1 = cx1 > cells_w ? cells_w : cx1;
	cy1 = cy1 > cells_h ? cells_h : cy1;
	blocks_w = cx1 > cx0 ? (cx1 - cx0) / BLOCK : 0;
	blocks_w = blocks_w > MAX_BLOCKS ? MAX_BLOCKS : blocks_w;
	/*
	 * Green's mean over blocks of 4x4 cells, and the squares of the steps between neighbouring
	 * blocks: a cell's own noise is a quarter of itself in a block's mean, and the picture's edges are
	 * not -- in the dark, at 32x and more, the steps from cell to cell were the noise's alone and the
	 * focus went wherever it liked.
	 */
	for (uint32_t by = cy0; by + BLOCK <= cy1; by += BLOCK, rows++) {
		for (uint32_t bx = 0; bx < blocks_w; bx++) {
			uint32_t g = 0;

			for (uint32_t cy = by; cy < by + BLOCK; cy++) {
				const uint8_t *top = raw + (size_t)cy * 2 * f->stride;

				for (uint32_t cx = cx0 + bx * BLOCK; cx < cx0 + bx * BLOCK + BLOCK; cx++) {
					int v[4];

					cell(top, f->stride, cx, v);
					for (int i = 0; i < 4; i++) {
						g += i != r && i != b ? (uint32_t)v[i] : 0;
					}
				}
			}
			here[bx] = g / (2.0f * BLOCK * BLOCK);
			sum += here[bx];
			n++;
			if (bx > 0) {
				steps += (here[bx] - here[bx - 1]) * (here[bx] - here[bx - 1]);
			}
			if (rows > 0) {
				steps += (here[bx] - above[bx]) * (here[bx] - above[bx]);
			}
		}
		memcpy(above, here, sizeof(float) * blocks_w);
	}
	double mean = n ? sum / n : 0;

	if (level != NULL) {
		*level = (float)mean;
	}
	return mean > 0 ? (float)(steps / n / (mean * mean)) : 0;
}

static inline void lsc_at(const struct isp *isp, float u, float v, float g[3]);

void
isp_measure(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, struct isp_stats *s)
{
	uint32_t cells_w = f->width / 2, cells_h = f->height / 2;
	double sum[3] = { 0, 0, 0 }, grey[2] = { 0, 0 }, neutral[2] = { 0, 0 }, roi_sum[3] = { 0, 0, 0 };
	float gains[3] = { 1, 1, 1 };
	uint32_t roi_hist[64], rx0 = 0, rx1 = 0, ry0 = 0, ry1 = 0;
	bool roi = isp != NULL && isp->ae_roi[2] > isp->ae_roi[0] && isp->ae_roi[3] > isp->ae_roi[1];
	int r, b;

	memset(s, 0, sizeof(*s));
	memset(roi_hist, 0, sizeof(roi_hist));
	if (roi) {
		/* (redfin: AE's metering window, in cells; at least a 16th of the frame each way) */
		rx0 = (uint32_t)(isp->ae_roi[0] * cells_w);
		rx1 = (uint32_t)(isp->ae_roi[2] * cells_w);
		ry0 = (uint32_t)(isp->ae_roi[1] * cells_h);
		ry1 = (uint32_t)(isp->ae_roi[3] * cells_h);
		rx1 = rx1 < rx0 + cells_w / 16 ? rx0 + cells_w / 16 : rx1;
		ry1 = ry1 < ry0 + cells_h / 16 ? ry0 + cells_h / 16 : ry1;
	}
	cell_places(f->bayer, &r, &b);
	for (uint32_t cy = 0; cy < cells_h; cy += 4) {
		const uint8_t *top = raw + (size_t)cy * 2 * f->stride;
		float v_pos = cells_h > 1 ? (float)cy / (cells_h - 1) : 0;

		for (uint32_t cx = 0; cx < cells_w; cx += 4) {
			int v[4], g = 0, top_v = 0;

			cell(top, f->stride, cx, v);
			for (int i = 0; i < 4; i++) {
				g += i != r && i != b ? v[i] : 0;
				top_v = v[i] > top_v ? v[i] : top_v;
			}
			g /= 2;
			sum[0] += v[r];
			sum[1] += g;
			sum[2] += v[b];
			if (roi && cx >= rx0 && cx < rx1 && cy >= ry0 && cy < ry1) {
				roi_sum[0] += v[r];
				roi_sum[1] += g;
				roi_sum[2] += v[b];
				roi_hist[g >> 4 > 63 ? 63 : g >> 4]++;
				s->roi_cells++;
			}
			/*
			 * The white balance leaves out a cell with a colour near white -- a lamp, a screen:
			 * green, the most sensitive, stops there first and the cell would count as magenta --
			 * and one near black, where noise cut off at the black level is all there is.
			 */
			if (top_v < WHITE * 9 / 10 && g >= 24) {
				float cr = (float)v[r], cg = (float)g, cb = (float)v[b];

				/*
				 * As the picture will have it: the lens shading raises the corners several times, and
				 * the light there counts for that much more than in the frame as it comes.
				 */
				if (isp != NULL && isp->lsc.on) {
					lsc_at(isp, cells_w > 1 ? (float)cx / (cells_w - 1) : 0, v_pos, gains);
					cr *= gains[0];
					cg *= gains[1];
					cb *= gains[2];
				}
				/*
				 * Each cell's colour, not its light: summed, a lamp or a screen in the picture -- tens of
				 * times brighter than the room it is in -- would have the white balance to itself, and
				 * the room would be left the colour of its light.
				 */
				grey[0] += cr / cg;
				grey[1] += cb / cg;
				s->grey_cells++;
				/*
				 * Grey, or within a third of it in red and blue, with the white balance as it is: what
				 * the white balance should make grey, without the coloured things around it (the
				 * colour matrix makes any mistake in it twice as plain).
				 */
				if (isp != NULL) {
					float wr = cr * isp->wb[0] / cg, wb = cb * isp->wb[2] / cg;

					if (wr > 0.75f && wr < 1.33f && wb > 0.75f && wb < 1.33f) {
						neutral[0] += cr / cg;
						neutral[1] += cb / cg;
						s->neutral_cells++;
					}
				}
			}
			s->hist[g >> 4 > 63 ? 63 : g >> 4]++;
			s->cells++;
		}
	}
	for (int c = 0; c < 3; c++) {
		s->mean[c] = s->cells ? sum[c] / s->cells : 0;
	}
	for (int c = 0; c < 2; c++) {
		s->grey[c] = s->grey_cells ? grey[c] / s->grey_cells : 0;
		s->neutral[c] = s->neutral_cells ? neutral[c] / s->neutral_cells : 0;
	}
	for (int i = 63, above = 0; i >= 0; i--) {
		above += s->hist[i];
		if (above * 100 >= (int)s->cells) {
			s->bright = i * 16 + 8;
			break;
		}
	}
	for (int c = 0; c < 3 && s->roi_cells; c++) {
		s->roi_mean[c] = roi_sum[c] / s->roi_cells;
	}
	for (int i = 63, above = 0; s->roi_cells && i >= 0; i--) {
		above += roi_hist[i];
		if (above * 100 >= (int)s->roi_cells) {
			s->roi_bright = i * 16 + 8;
			break;
		}
	}
}

/*
 * Green's mean towards 15 % of white (times 2^ExposureBias), unless that would put 1 % of the picture into white;
 * metered mostly on AE's window when there is one (a tap: RegionOfInterestForCameraControls). The exposure,
 * longest first: the frame's lines at the shortest frame, then gain to the knee (16x, or the sensor's most if less),
 * then frames up to max_frame_lines, then gain to the most; beyond the sensor's analog gain, its own digital gain
 * (where it has one, EXPOSURE.d), then the processor's (up to 4x). Within the limits the capture stack set: the
 * longest exposure (MaxIntegrationTime), the most gain (AEMaxGain), the frame rate (min/max_frame_lines). Locked,
 * nothing changes; custom (manual), the exposure asked for is the one set.
 *
 * redfin: the kernel clamps what it cannot do (the IMX363's analog gain at 8x, where this used to ask up to 16x) and
 * reports what it set: AE waited for ever for a frame with the gain it asked for (`asked' never cleared: the
 * exposure froze). It takes the clamped values as the answer now, and after AE_PATIENCE frames whatever the
 * frames say.
 */
static bool
ae_got(const struct isp *isp, const struct redwood_camera_frame *f)
{
	uint32_t lines = isp->asked_lines, gain = isp->asked_gain;

	if (f->frame_lines_max > f->margin && lines > f->frame_lines_max - f->margin) {
		lines = f->frame_lines_max - f->margin;
	}
	gain = gain > f->gain_max && f->gain_max != 0 ? f->gain_max : gain;
	gain = gain < f->gain_unity ? f->gain_unity : gain;
	return (f->exposure_lines + 1 >= lines && f->exposure_lines <= lines + 1) &&
	    (uint64_t)f->gain * 100 >= (uint64_t)gain * 99 && (uint64_t)f->gain * 99 <= (uint64_t)gain * 100;
}

bool
isp_ae(struct isp *isp, const struct redwood_camera_frame *f, const struct isp_stats *s, struct redwood_camera_command *c)
{
	double unity = f->gain_unity ? f->gain_unity : 256, analog_max = f->gain_max > f->gain_unity ? f->gain_max / unity : 1;
	uint32_t dg_unity = f->reserved[REDWOOD_CAMERA_R_DGAIN] & 0xffff, dg_top = f->reserved[REDWOOD_CAMERA_R_DGAIN] >> 16;
	double digital_max = dg_unity != 0 && dg_top > dg_unity ? (double)dg_top / dg_unity : 1;
	double now, ratio, want, gain_total, target, mean, bright, line_us = f->line_ns ? f->line_ns / 1000.0 : 10;
	uint32_t floor_lines = f->frame_lines_min > isp->min_frame_lines ? f->frame_lines_min : isp->min_frame_lines;
	uint32_t ceiling = isp->max_frame_lines ? isp->max_frame_lines : 2 * f->frame_lines_min;
	uint32_t fast, longest, cap, lines, frame_lines = 0;
	double knee, analog, sensor_digital = 1, digital = 1;

	if (isp->asked) {
		if (!ae_got(isp, f) && ++isp->asked_frames < AE_PATIENCE) {
			isp->ae_stable = false;
			return false;           /* (the last change not in the frames yet) */
		}
		isp->asked = false;
		if (isp->asked_digital != 0) {
			isp->sensor_digital = isp->asked_digital / 256.0f;
		}
	}
	if (f->frame_lines_max != 0 && ceiling > f->frame_lines_max) {
		ceiling = f->frame_lines_max;
	}
	ceiling = ceiling < floor_lines ? floor_lines : ceiling;
	fast = floor_lines > f->margin + 4 ? floor_lines - f->margin : 4;
	longest = ceiling > f->margin + 4 ? ceiling - f->margin : 4;
	cap = isp->ae_max_exposure_us ? (uint32_t)(isp->ae_max_exposure_us / line_us) : longest;
	cap = cap < 4 ? 4 : cap;
	fast = fast > cap ? cap : fast;
	longest = longest > cap ? cap : longest;
	longest = longest < fast ? fast : longest;
	if (isp->ae_locked) {
		isp->ae_stable = true;
		return false;
	}
	if (isp->manual) {
		/* a custom exposure: as asked, within what the sensor and the frame rate can do */
		uint32_t most = (f->frame_lines_max > ceiling ? f->frame_lines_max : ceiling);

		most = most > f->margin + 4 ? most - f->margin : 4;
		lines = (uint32_t)(isp->manual_us / line_us);
		lines = lines < 4 ? 4 : lines > most ? most : lines;
		gain_total = isp->manual_gain < 1 ? 1 : isp->manual_gain;
		goto split;
	}
	now = (double)f->exposure_lines * f->gain / unity * isp->sensor_digital * isp->digital;
	target = 0.15 * WHITE * pow(2, isp->ae_bias < -8 ? -8 : isp->ae_bias > 8 ? 8 : isp->ae_bias);
	target = target > 0.8 * WHITE ? 0.8 * WHITE : target;
	/* (the measured levels are the sensor's, its digital gain in them: the processor's is applied to the picture only) */
	mean = s->mean[1];
	bright = s->bright;
	if (s->roi_cells != 0) {
		mean = 0.75 * s->roi_mean[1] + 0.25 * s->mean[1];
		bright = s->roi_bright;
	}
	mean = mean * isp->digital > 0.5 ? mean * isp->digital : 0.5;
	bright *= isp->digital;
	ratio = target / mean;
	if (isp->ae_bias <= 0.5f && bright * ratio > 0.9 * WHITE && bright > 0) {
		ratio = 0.9 * WHITE / bright > target / mean * 0.5 ? 0.9 * WHITE / bright : target / mean * 0.5;
	}
	ratio = ratio < 0.25 ? 0.25 : ratio > 4 ? 4 : ratio;
	if (ratio > 0.94 && ratio < 1.06) {
		isp->ae_stable = true;
		return false;
	}
	isp->ae_stable = false;
	want = now * pow(ratio, 0.7);
	knee = analog_max < 16 ? analog_max : 16;
	if (want <= fast) {
		lines = want < 4 ? 4 : (uint32_t)want;
		gain_total = 1;
	} else if (want <= (double)fast * knee) {
		lines = fast;
		gain_total = want / fast;
	} else {
		lines = (uint32_t)(want / knee);
		lines = lines > longest ? longest : lines < fast ? fast : lines;
		gain_total = want / lines;
	}
split:
	if (isp->ae_max_gain >= 1 && gain_total > isp->ae_max_gain) {
		gain_total = isp->ae_max_gain;
	}
	/* the gain: the sensor's analog, then its digital, then the processor's (at most 4x) */
	analog = gain_total < analog_max ? gain_total : analog_max;
	analog = analog < 1 ? 1 : analog;
	if (gain_total > analog && digital_max > 1) {
		sensor_digital = gain_total / analog < digital_max ? gain_total / analog : digital_max;
	}
	digital = gain_total / analog / sensor_digital;
	digital = digital < 1 ? 1 : digital > 4 ? 4 : digital;
	if (lines + f->margin > floor_lines || isp->min_frame_lines > f->frame_lines_min) {
		frame_lines = lines + f->margin > floor_lines ? lines + f->margin : floor_lines;
	}
	if (isp->manual && f->exposure_lines + 1 >= lines && f->exposure_lines <= lines + 1 &&
	    fabs(f->gain / unity - analog) < 0.01 * analog && fabsf(isp->digital - (float)digital) < 0.01f) {
		isp->ae_stable = true;
		return false;           /* (the custom exposure is there already) */
	}
	isp->digital = (float)digital;
	isp->asked_lines = lines;
	isp->asked_gain = (uint32_t)(analog * unity + 0.5);
	isp->asked_frame_lines = frame_lines;
	isp->asked_digital = digital_max > 1 ? (uint32_t)(sensor_digital * 256 + 0.5) : 0;
	isp->asked = true;
	isp->asked_frames = 0;
	memset(c, 0, sizeof(*c));
	c->command = REDWOOD_CAMERA_EXPOSURE;
	c->a = lines;
	c->b = isp->asked_gain;
	c->c = frame_lines;
	c->d = isp->asked_digital;
	return true;
}

/*
 * The point of the reference lights' line nearest a light seen as red over green rg and blue over
 * green bg, on their logarithms: where along it (segment i and how far into it, 0 to 1).
 */
static void
on_reference_line(const struct isp_tuning *t, float rg, float bg, uint32_t *segment, float *along)
{
	float x = logf(rg), y = logf(bg), best = INFINITY;

	*segment = 0;
	*along = 0;
	for (uint32_t i = 0; i + 1 < t->refs; i++) {
		float x0 = logf(t->ref[i][0]), y0 = logf(t->ref[i][1]), dx = logf(t->ref[i + 1][0]) - x0,
		    dy = logf(t->ref[i + 1][1]) - y0, length = dx * dx + dy * dy;
		float u = length > 0 ? ((x - x0) * dx + (y - y0) * dy) / length : 0, d;

		u = u < 0 ? 0 : u > 1 ? 1 : u;
		d = (x0 + u * dx - x) * (x0 + u * dx - x) + (y0 + u * dy - y) * (y0 + u * dy - y);
		if (d < best) {
			best = d;
			*segment = i;
			*along = u;
		}
	}
}

float
isp_cct(const struct isp *isp)
{
	const struct isp_tuning *t = isp->tuning;
	uint32_t i;
	float u;

	if (t == NULL || t->refs < 2 || isp->wb[0] <= 0 || isp->wb[2] <= 0) {
		return 0;
	}
	on_reference_line(t, 1 / isp->wb[0], 1 / isp->wb[2], &i, &u);
	return t->ref[i][2] + u * (t->ref[i + 1][2] - t->ref[i][2]);
}

void
isp_awb(struct isp *isp, const struct isp_stats *s)
{
	/*
	 * The nearly grey cells, when there are enough of them (1 in 32); else every cell neither near
	 * white nor near black, when there are enough of those (1 in 16); else every cell's light.
	 */
	float r, b;

	/* redfin: locked, it stays; manual (the capture stack's gains), it is those */
	if (isp->awb_manual) {
		isp->wb[0] = isp->wb_manual[0] / (isp->wb_manual[1] > 0 ? isp->wb_manual[1] : 1);
		isp->wb[1] = 1;
		isp->wb[2] = isp->wb_manual[2] / (isp->wb_manual[1] > 0 ? isp->wb_manual[1] : 1);
		isp->awb_stable = true;
		return;
	}
	if (isp->awb_locked) {
		isp->awb_stable = true;
		return;
	}
	if (s->neutral_cells * 32 >= s->cells && s->neutral[0] > 0.01 && s->neutral[1] > 0.01) {
		r = (float)(1 / s->neutral[0]);
		b = (float)(1 / s->neutral[1]);
	} else if (s->grey_cells * 16 >= s->cells && s->grey[0] > 0.01 && s->grey[1] > 0.01) {
		r = (float)(1 / s->grey[0]);
		b = (float)(1 / s->grey[1]);
	} else if (s->mean[0] >= 2 && s->mean[1] >= 2 && s->mean[2] >= 2) {
		r = (float)(s->mean[1] / s->mean[0]);
		b = (float)(s->mean[1] / s->mean[2]);
	} else {
		return;
	}

	/*
	 * Onto the line of the reference lights: a scene's own colours pull grey world off it -- a pink
	 * screen, a green wall -- and off it is what shows as green or magenta, while along it is a
	 * warmer or colder white. (The wide camera under a warm white LED: grey world of the room with a
	 * screen in it 15 % short in both red and blue, the light measured on a white sheet; kept to the
	 * line, 2 % off.)
	 */
	if (isp->tuning != NULL && isp->tuning->refs >= 2) {
		const struct isp_tuning *t = isp->tuning;
		uint32_t i;
		float u;

		on_reference_line(t, 1 / r, 1 / b, &i, &u);
		r = 1 / expf(logf(t->ref[i][0]) + u * (logf(t->ref[i + 1][0]) - logf(t->ref[i][0])));
		b = 1 / expf(logf(t->ref[i][1]) + u * (logf(t->ref[i + 1][1]) - logf(t->ref[i][1])));
	}
	r = r < 0.5f ? 0.5f : r > 4 ? 4 : r;
	b = b < 0.5f ? 0.5f : b > 4 ? 4 : b;
	isp->awb_stable = fabsf(r - isp->wb[0]) < 0.03f * isp->wb[0] && fabsf(b - isp->wb[2]) < 0.03f * isp->wb[2];
	isp->wb[0] = isp->wb[0] * 0.9f + r * 0.1f;
	isp->wb[2] = isp->wb[2] * 0.9f + b * 0.1f;
}

/* the part of the sensor's cells the picture shows: its middle, in the picture's proportions, 1/zoom of it each way */
static void
crop(const struct isp *isp, const struct redwood_camera_frame *f, uint32_t width, uint32_t height, uint32_t *x0, uint32_t *y0,
    uint32_t *cw, uint32_t *ch)
{
	uint32_t w = f->width / 2, h = f->height / 2;
	float zoom = isp->zoom < 1 ? 1 : isp->zoom;

	if ((uint64_t)width * h > (uint64_t)height * w) {
		*cw = w;
		*ch = (uint32_t)((uint64_t)w * height / width);
	} else {
		*ch = h;
		*cw = (uint32_t)((uint64_t)h * width / height);
	}
	*cw = (uint32_t)(*cw / zoom) < 2 ? 2 : (uint32_t)(*cw / zoom);
	*ch = (uint32_t)(*ch / zoom) < 2 ? 2 : (uint32_t)(*ch / zoom);
	*x0 = (w - *cw) / 2;
	*y0 = (h - *ch) / 2;
}

void
isp_set_ccm(struct isp *isp, const float m[9])
{
	for (int i = 0; i < 9; i++) {
		isp->ccm[i / 3][i % 3] = m[i];
	}
}

/*
 * The colour matrix with the white balance's and AE's gains folded in, in the gamma table's steps
 * (per 10-bit step above black): out[c] = sum over j of m[c][j] * value[j] (each value lens-shaded).
 */
struct pipe {
	float           m[3][3];
	bool            identity;
};

static void
pipe_init(const struct isp *isp, struct pipe *p)
{
	p->identity = true;
	for (int c = 0; c < 3; c++) {
		for (int j = 0; j < 3; j++) {
			p->m[c][j] = isp->ccm[c][j] * isp->wb[j] * isp->digital * (LUT_WHITE / (float)WHITE);
			p->identity = p->identity && (c == j ? isp->ccm[c][j] == 1 : isp->ccm[c][j] == 0);
		}
	}
}

static inline uint8_t
curve(const struct isp *isp, float v)
{
	int i = (int)(v + 0.5f);

	return isp->gamma[i < 0 ? 0 : i > 4095 ? 4095 : i];
}

static inline void
pixel(const struct isp *isp, const struct pipe *p, float r, float g, float b, uint8_t *rgb)
{
	if (p->identity) {
		rgb[0] = curve(isp, r * p->m[0][0]);
		rgb[1] = curve(isp, g * p->m[1][1]);
		rgb[2] = curve(isp, b * p->m[2][2]);
		return;
	}
	rgb[0] = curve(isp, p->m[0][0] * r + p->m[0][1] * g + p->m[0][2] * b);
	rgb[1] = curve(isp, p->m[1][0] * r + p->m[1][1] * g + p->m[1][2] * b);
	rgb[2] = curve(isp, p->m[2][0] * r + p->m[2][1] * g + p->m[2][2] * b);
}

/* the lens shading's gains at (u, v), 0 to 1 across the sensor */
static inline void
lsc_at(const struct isp *isp, float u, float v, float g[3])
{
	float gx = u * (LSC_W - 1), gy = v * (LSC_H - 1);
	int x = (int)gx, y = (int)gy;

	x = x < 0 ? 0 : x > LSC_W - 2 ? LSC_W - 2 : x;
	y = y < 0 ? 0 : y > LSC_H - 2 ? LSC_H - 2 : y;
	float fx = gx - x, fy = gy - y;

	for (int c = 0; c < 3; c++) {
		const float (*t)[LSC_W] = isp->lsc.gain[c];

		g[c] = (t[y][x] * (1 - fx) + t[y][x + 1] * fx) * (1 - fy) + (t[y + 1][x] * (1 - fx) + t[y + 1][x + 1] * fx) * fy;
	}
}

/*
 * redfin (plan B8): the same arithmetic in integers for the pictures' inner loops (the float code above stays for the
 * magnified picture and as the reference, isp->reference): values above black in 1/16ths, the lens shading's gains
 * in 1/4096ths (a row's grid columns interpolated once a row, then along it), the colour matrix in 1/1024ths, 64-bit
 * sums; rounded where the float code rounds (isp_hosttest compares the two).
 */
struct ipipe {
	int64_t         m[3][3];
};

static void
ipipe_init(const struct pipe *p, struct ipipe *q)
{
	for (int c = 0; c < 3; c++) {
		for (int j = 0; j < 3; j++) {
			q->m[c][j] = (int64_t)llroundf(p->m[c][j] * 1024);
		}
	}
}

/* v: R, G, B in 1/16ths of a 10-bit step */
static inline void
ipixel(const struct isp *isp, const struct ipipe *q, int64_t r, int64_t g, int64_t b, uint8_t *rgb)
{
	for (int c = 0; c < 3; c++) {
		int64_t o = (q->m[c][0] * r + q->m[c][1] * g + q->m[c][2] * b + (1 << 13)) >> 14;

		rgb[c] = isp->gamma[o < 0 ? 0 : o > 4095 ? 4095 : o];
	}
}

/* a row's lens shading: each colour's gain at each grid column (1/4096ths), at v (0 to 1 down the sensor) */
struct lsc_row {
	int32_t         g[3][LSC_W];
	uint64_t        step;           /* grid columns a pixel, in 1/2^32ths */
};

static void
lsc_row_init(const struct isp *isp, float v, uint32_t width, struct lsc_row *r)
{
	float gy = v * (LSC_H - 1);
	int y = (int)gy;

	y = y < 0 ? 0 : y > LSC_H - 2 ? LSC_H - 2 : y;
	float fy = gy - y;

	for (int c = 0; c < 3; c++) {
		for (int x = 0; x < LSC_W; x++) {
			float g = isp->lsc.on ? isp->lsc.gain[c][y][x] * (1 - fy) + isp->lsc.gain[c][y + 1][x] * fy : 1;

			r->g[c][x] = (int32_t)lroundf(g * 4096);
		}
	}
	r->step = ((uint64_t)(LSC_W - 1) << 32) / (width > 1 ? width - 1 : 1);
}

/* the gains at x (of the row's width) */
static inline void
lsc_row_at(const struct lsc_row *r, uint32_t x, int32_t g[3])
{
	uint64_t at = ((uint64_t)x * r->step + 32768) >> 16;
	uint32_t gx = (uint32_t)(at >> 16);
	int64_t t = (int64_t)(at & 0xffff);

	if (gx > LSC_W - 2) {
		gx = LSC_W - 2;
		t = 65536;
	}
	for (int c = 0; c < 3; c++) {
		g[c] = r->g[c][gx] + (int32_t)(((int64_t)(r->g[c][gx + 1] - r->g[c][gx]) * t + 32768) >> 16);
	}
}

/* one line of the picture as 8-bit R, G, B */
static void
render_line(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint32_t width, uint32_t height,
    uint32_t row, uint8_t *rgb)
{
	uint32_t x0, y0, cw, ch, cells_w = f->width / 2, cells_h = f->height / 2;
	int r, b;
	struct pipe p;

	crop(isp, f, width, height, &x0, &y0, &cw, &ch);
	cell_places(f->bayer, &r, &b);
	pipe_init(isp, &p);
	if (isp->turned) {
		row = height - 1 - row;         /* (and each line from its end, below) */
	}
	if (cw < width && ch < height) {
		/*
		 * Magnified (the zoom): each pixel between the four cells around it. The nearest cell blown up
		 * to a square of pixels showed every cell's own colour, its noise and the colours a 2x2 cell
		 * gets wrong at an edge, as coloured dots.
		 */
		float fy = y0 + (row + 0.5f) * ch / height - 0.5f, ty, v_at;
		uint32_t cy0 = fy < 0 ? 0 : (uint32_t)fy, cy1;

		cy0 = cy0 > cells_h - 1 ? cells_h - 1 : cy0;
		cy1 = cy0 + 1 > cells_h - 1 ? cells_h - 1 : cy0 + 1;
		ty = fy < (float)cy0 ? 0 : fy - cy0;
		v_at = cells_h > 1 ? (float)cy0 / (cells_h - 1) : 0;
		const uint8_t *t0 = raw + (size_t)cy0 * 2 * f->stride, *t1 = raw + (size_t)cy1 * 2 * f->stride;
		float gains[3] = { 1, 1, 1 };

		for (uint32_t x = 0; x < width; x++) {
			float fx = x0 + (x + 0.5f) * cw / width - 0.5f, tx, c[3] = { 0, 0, 0 };
			uint32_t cx0 = fx < 0 ? 0 : (uint32_t)fx, cx1;

			cx0 = cx0 > cells_w - 1 ? cells_w - 1 : cx0;
			cx1 = cx0 + 1 > cells_w - 1 ? cells_w - 1 : cx0 + 1;
			tx = fx < (float)cx0 ? 0 : fx - cx0;
			for (int k = 0; k < 4; k++) {
				int v[4];
				float w = (k & 1 ? tx : 1 - tx) * (k & 2 ? ty : 1 - ty);

				cell(k & 2 ? t1 : t0, f->stride, k & 1 ? cx1 : cx0, v);
				c[0] += w * v[r];
				c[2] += w * v[b];
				for (int j = 0; j < 4; j++) {
					c[1] += j != r && j != b ? w * v[j] * 0.5f : 0;
				}
			}
			if (isp->lsc.on) {
				lsc_at(isp, cells_w > 1 ? (float)cx0 / (cells_w - 1) : 0, v_at, gains);
			}
			pixel(isp, &p, c[0] * gains[0], c[1] * gains[1], c[2] * gains[2], rgb + 3 * (isp->turned ? width - 1 - x : x));
		}
		return;
	}
	uint32_t cy = y0 + (uint32_t)((uint64_t)row * ch / height);
	const uint8_t *top = raw + (size_t)cy * 2 * f->stride;
	uint64_t step = ((uint64_t)cw << 16) / width, at = (uint64_t)x0 << 16;
	float v_pos = cells_h > 1 ? (float)cy / (cells_h - 1) : 0, gains[3] = { 1, 1, 1 };

	if (!isp->reference) {
		/* (redfin, B8: integers) */
		struct ipipe q;
		struct lsc_row lr;

		ipipe_init(&p, &q);
		lsc_row_init(isp, v_pos, cells_w, &lr);
		for (uint32_t x = 0; x < width; x++, at += step) {
			uint32_t cx = (uint32_t)(at >> 16);
			int v[4], g = 0;
			int32_t gn[3];

			cell(top, f->stride, cx, v);
			for (int j = 0; j < 4; j++) {
				g += j != r && j != b ? v[j] : 0;
			}
			lsc_row_at(&lr, cx, gn);
			ipixel(isp, &q, ((int64_t)v[r] * gn[0] + 128) >> 8, ((int64_t)g * gn[1] + 256) >> 9, ((int64_t)v[b] * gn[2] + 128) >> 8,
			    rgb + 3 * (isp->turned ? width - 1 - x : x));
		}
		return;
	}
	for (uint32_t x = 0; x < width; x++, at += step) {
		uint32_t cx = (uint32_t)(at >> 16);
		int v[4], g = 0;

		cell(top, f->stride, cx, v);
		for (int j = 0; j < 4; j++) {
			g += j != r && j != b ? v[j] : 0;
		}
		if (isp->lsc.on) {
			lsc_at(isp, cells_w > 1 ? (float)cx / (cells_w - 1) : 0, v_pos, gains);
		}
		pixel(isp, &p, v[r] * gains[0], g * 0.5f * gains[1], v[b] * gains[2], rgb + 3 * (isp->turned ? width - 1 - x : x));
	}
}

/*
 * Two lines of 8-bit R, G, B as NV12 (BT.601's matrix, which CoreVideo's 420f and 420v and
 * tools/camraw.py take): each pixel's Y (y1 NULL: the second line has none, the picture's last of an
 * odd height), each 2x2's Cb and Cr from the sum of its four. The coefficients in 1/65536ths, video
 * range's scaled to its 219 steps of luma (16 to 235) and 224 of chroma (16 to 240); every value
 * rounded to the nearest step, so that the primaries come back within one step of 0 and 255 (red's and
 * blue's cannot be exact: no 8-bit Y, Cb, Cr is both).
 */
static void
nv12_pair(const uint8_t *rgb0, const uint8_t *rgb1, uint32_t width, uint8_t *y0, uint8_t *y1, uint8_t *uv, bool video_range)
{
	static const int32_t full[9] = { 19595, 38470, 7471, -11058, -21710, 32768, 32768, -27439, -5329 };
	static const int32_t video[9] = { 16829, 33039, 6416, -9714, -19070, 28784, 28784, -24103, -4681 };
	const int32_t *k = video_range ? video : full;
	int32_t oy = (video_range ? 16 << 16 : 0) + (1 << 15);

	for (int l = 0; l < 2; l++) {
		const uint8_t *p = l ? rgb1 : rgb0;
		uint8_t *out = l ? y1 : y0;

		for (uint32_t x = 0; out != NULL && x < width; x++, p += 3) {
			out[x] = (uint8_t)((k[0] * p[0] + k[1] * p[1] + k[2] * p[2] + oy) >> 16);
		}
	}
	for (uint32_t x = 0; x < width; x += 2) {
		uint32_t x1 = x + 1 < width ? x + 1 : x;
		int32_t R = rgb0[3 * x] + rgb0[3 * x1] + rgb1[3 * x] + rgb1[3 * x1];
		int32_t G = rgb0[3 * x + 1] + rgb0[3 * x1 + 1] + rgb1[3 * x + 1] + rgb1[3 * x1 + 1];
		int32_t B = rgb0[3 * x + 2] + rgb0[3 * x1 + 2] + rgb1[3 * x + 2] + rgb1[3 * x1 + 2];
		/*
		 * (the sums are four pixels': 18 bits off, with 128 added and just under half a step to round;
		 * a tie goes down, so that chroma's -127.5, a full colour's, comes out 0 as its +127.5 does 255)
		 */
		int32_t cb = (k[3] * R + k[4] * G + k[5] * B + (128 << 18) + (1 << 17) - 1) >> 18;
		int32_t cr = (k[6] * R + k[7] * G + k[8] * B + (128 << 18) + (1 << 17) - 1) >> 18;

		uv[x] = (uint8_t)(cb < 0 ? 0 : cb > 255 ? 255 : cb);
		uv[x + 1] = (uint8_t)(cr < 0 ? 0 : cr > 255 ? 255 : cr);
	}
}

static void
isp_render_nv12_rows(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row, bool video_range)
{
	uint8_t line[2][3 * 8192];

	if (width > 8192) {
		return;
	}
	for (uint32_t row = first_row & ~1u; row < end_row && row < height; row += 2) {
		bool second = row + 1 < height;

		render_line(isp, f, raw, width, height, row, line[0]);
		render_line(isp, f, raw, width, height, second ? row + 1 : row, line[1]);
		nv12_pair(line[0], line[1], width, y + (size_t)row * y_stride, second ? y + (size_t)(row + 1) * y_stride : NULL,
		    uv + (size_t)(row / 2) * uv_stride, video_range);
	}
}

static void
isp_render_bgra_rows(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint8_t *out, size_t stride,
    uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row)
{
	uint8_t line[3 * 8192];

	if (width > 8192) {
		return;
	}
	for (uint32_t row = first_row; row < end_row && row < height; row++) {
		uint8_t *o = out + (size_t)row * stride;

		render_line(isp, f, raw, width, height, row, line);
		for (uint32_t x = 0; x < width; x++) {
			o[4 * x] = line[3 * x + 2];
			o[4 * x + 1] = line[3 * x + 1];
			o[4 * x + 2] = line[3 * x];
			o[4 * x + 3] = 255;
		}
	}
}

/* ---- every pixel: bilinear demosaic -------------------------------------------------------------------- */

/* the colour of each site of a 2x2 cell (0 R, 1 G, 2 B), position y * 2 + x */
static void
cfa(uint32_t bayer, int colour[4])
{
	int r, b;

	cell_places(bayer, &r, &b);
	for (int i = 0; i < 4; i++) {
		colour[i] = i == r ? 0 : i == b ? 2 : 1;
	}
}

/* line y of the frame as values above black (y clamped into the frame) */
static void
unpack_line(const struct redwood_camera_frame *f, const uint8_t *raw, int y, int *out)
{
	y = y < 0 ? 0 : y >= (int)f->height ? (int)f->height - 1 : y;
	const uint8_t *line = raw + (size_t)y * f->stride;

	for (uint32_t x = 0; x + 3 < f->width + 3 && x < f->width; x += 4) {
		const uint8_t *g = line + (x >> 2) * 5;

		for (int i = 0; i < 4 && x + i < f->width; i++) {
			int v = (g[i] << 2 | (g[4] >> (i * 2) & 3)) - (int)f->black;

			out[x + i] = v < 0 ? 0 : v;
		}
	}
}

/* one line (y) at full size as 8-bit R, G, B, from the lines y - 1, y, y + 1 (l[0..2]) */
static void
full_line(const struct isp *isp, const struct redwood_camera_frame *f, const int *const l[3], uint32_t y, const int colour[4],
    const struct pipe *p, uint8_t *rgb)
{
	int w = (int)f->width;
	float v_pos = f->height > 1 ? (float)y / (f->height - 1) : 0, gains[3] = { 1, 1, 1 };

	if (!isp->reference) {
		/*
		 * (redfin, B8: integers; inside the frame the same means written out -- at a red or blue site green from the
		 * four beside it and the other colour from the four corners, at a green site each colour from the two of it
		 * beside it -- and the general loop only at the frame's edges, where it counts clamped neighbours as it does)
		 */
		struct ipipe q;
		struct lsc_row lr;
		bool inner = y >= 1 && y + 1 < f->height;
		const int *l0 = l[0], *l1 = l[1], *l2 = l[2];

		ipipe_init(p, &q);
		lsc_row_init(isp, v_pos, (uint32_t)w, &lr);
		for (int x = 0; x < w; x++) {
			int64_t val[3];
			int32_t gn[3];

			if (inner && x > 0 && x + 1 < w) {
				int c0 = colour[(y & 1) * 2 + (x & 1)];

				if (c0 == 1) {
					val[1] = (int64_t)l1[x] * 4;
					val[colour[(y & 1) * 2 + ((x + 1) & 1)]] = (int64_t)(l1[x - 1] + l1[x + 1]) * 2;
					val[colour[((y + 1) & 1) * 2 + (x & 1)]] = (int64_t)(l0[x] + l2[x]) * 2;
				} else {
					val[c0] = (int64_t)l1[x] * 4;
					val[1] = l0[x] + l2[x] + l1[x - 1] + l1[x + 1];
					val[2 - c0] = l0[x - 1] + l0[x + 1] + l2[x - 1] + l2[x + 1];
				}
			} else {
				float sum[3] = { 0, 0, 0 };
				int n[3] = { 0, 0, 0 };

				for (int dy = -1; dy <= 1; dy++) {
					const int *line = l[dy + 1];
					int yy = (int)y + dy;

					yy = yy < 0 ? 0 : yy >= (int)f->height ? (int)f->height - 1 : yy;
					for (int dx = -1; dx <= 1; dx++) {
						int xx = x + dx < 0 ? 0 : x + dx >= w ? w - 1 : x + dx;
						int c = colour[(yy & 1) * 2 + (xx & 1)];

						if (dx == 0 && dy == 0) {
							sum[c] = (float)line[xx] * 64;
							n[c] = 64;
						} else if (n[c] < 64) {
							sum[c] += (float)line[xx];
							n[c]++;
						}
					}
				}
				for (int c = 0; c < 3; c++) {
					val[c] = (int64_t)lroundf(sum[c] / (n[c] ? n[c] : 1) * 4);
				}
			}
			lsc_row_at(&lr, (uint32_t)x, gn);
			ipixel(isp, &q, (val[0] * gn[0] + 512) >> 10, (val[1] * gn[1] + 512) >> 10, (val[2] * gn[2] + 512) >> 10, rgb + 3 * x);
		}
		return;
	}
	for (int x = 0; x < w; x++) {
		float sum[3] = { 0, 0, 0 };
		int n[3] = { 0, 0, 0 };

		/* each colour: its own value where the site has it, else the mean of its sites in the 3x3 around */
		for (int dy = -1; dy <= 1; dy++) {
			const int *line = l[dy + 1];
			int yy = (int)y + dy;

			yy = yy < 0 ? 0 : yy >= (int)f->height ? (int)f->height - 1 : yy;
			for (int dx = -1; dx <= 1; dx++) {
				int xx = x + dx < 0 ? 0 : x + dx >= w ? w - 1 : x + dx;
				int c = colour[(yy & 1) * 2 + (xx & 1)];

				if (dx == 0 && dy == 0) {
					sum[c] = (float)line[xx] * 64;          /* (its own value wins over the neighbours') */
					n[c] = 64;
				} else if (n[c] < 64) {
					sum[c] += (float)line[xx];
					n[c]++;
				}
			}
		}
		if (isp->lsc.on) {
			lsc_at(isp, w > 1 ? (float)x / (w - 1) : 0, v_pos, gains);
		}
		pixel(isp, p, sum[0] / (n[0] ? n[0] : 1) * gains[0], sum[1] / (n[1] ? n[1] : 1) * gains[1],
		    sum[2] / (n[2] ? n[2] : 1) * gains[2], rgb + 3 * x);
	}
}

static void
isp_render_full_nv12_rows(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t first_row, uint32_t end_row, bool video_range)
{
	uint32_t width = f->width;
	int colour[4];
	struct pipe p;
	static __thread int *lines[4];
	static __thread uint8_t *rgb[2];
	static __thread uint32_t have;

	if (width > have) {
		for (int i = 0; i < 4; i++) {
			free(lines[i]);
			lines[i] = malloc(sizeof(int) * width);
		}
		for (int i = 0; i < 2; i++) {
			free(rgb[i]);
			rgb[i] = malloc(3 * (size_t)width);
		}
		have = width;
	}
	cfa(f->bayer, colour);
	pipe_init(isp, &p);
	for (uint32_t row = first_row & ~1u; row < end_row && row < f->height; row += 2) {
		for (int i = 0; i < 4; i++) {
			unpack_line(f, raw, (int)row - 1 + i, lines[i]);
		}
		const int *const above[3] = { lines[0], lines[1], lines[2] }, *const below[3] = { lines[1], lines[2], lines[3] };

		bool second = row + 1 < f->height;

		full_line(isp, f, above, row, colour, &p, rgb[0]);
		full_line(isp, f, below, second ? row + 1 : row, colour, &p, rgb[1]);
		nv12_pair(rgb[0], rgb[1], width, y + (size_t)row * y_stride, second ? y + (size_t)(row + 1) * y_stride : NULL,
		    uv + (size_t)(row / 2) * uv_stride, video_range);
	}
}

/* one line (y) of the sensor demosaiced, as 8-bit R, G, B (the unpacked lines around it in this thread's l) */
static void
demosaic_row(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint32_t row, const int colour[4],
    const struct pipe *p, int *const l[3], uint8_t *rgb)
{
	for (int i = 0; i < 3; i++) {
		unpack_line(f, raw, (int)row - 1 + i, l[i]);
	}
	full_line(isp, f, (const int *const *)l, row, colour, p, rgb);
}

static void
isp_render_still_nv12_rows(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row, bool video_range)
{
	uint32_t x0, y0, cw, ch, fw = f->width;
	int colour[4];
	struct pipe p;
	static __thread int *lines[3];
	static __thread uint8_t *rows[2], *out[2];
	static __thread uint32_t have_fw, have_width;
	int32_t cached[2] = { -1, -1 };

	if (fw > have_fw) {
		for (int i = 0; i < 3; i++) {
			free(lines[i]);
			lines[i] = malloc(sizeof(int) * fw);
		}
		for (int i = 0; i < 2; i++) {
			free(rows[i]);
			rows[i] = malloc(3 * (size_t)fw);
		}
		have_fw = fw;
	}
	if (width > have_width) {
		for (int i = 0; i < 2; i++) {
			free(out[i]);
			out[i] = malloc(3 * (size_t)width);
		}
		have_width = width;
	}
	if (lines[2] == NULL || rows[1] == NULL || out[1] == NULL) {
		return;
	}
	crop(isp, f, width, height, &x0, &y0, &cw, &ch);
	cfa(f->bayer, colour);
	pipe_init(isp, &p);
	float sx_step = 2.0f * cw / width, sy_step = 2.0f * ch / height;

	for (uint32_t row = first_row & ~1u; row < end_row && row < height; row += 2) {
		for (int k = 0; k < 2; k++) {
			uint32_t r = row + k < height ? row + k : row, at = isp->turned ? height - 1 - r : r;
			float sy = 2.0f * y0 + (at + 0.5f) * sy_step - 0.5f, ty;
			uint32_t sy0 = sy < 0 ? 0 : (uint32_t)sy, sy1, slot[2];

			sy0 = sy0 > f->height - 1 ? f->height - 1 : sy0;
			sy1 = sy0 + 1 > f->height - 1 ? f->height - 1 : sy0 + 1;
			ty = sy < (float)sy0 ? 0 : sy - sy0;
			/* the two lines of the sensor it is between, demosaiced (kept from the line before where they are the same) */
			for (int n = 0; n < 2; n++) {
				uint32_t want = n ? sy1 : sy0;

				if (cached[0] == (int32_t)want || cached[1] == (int32_t)want) {
					slot[n] = cached[0] == (int32_t)want ? 0 : 1;
					continue;
				}
				/* (into the slot the other line does not need: sy0's for sy1, and sy1's kept for sy0) */
				slot[n] = n == 1 ? slot[0] ^ 1 : cached[0] == (int32_t)sy1 ? 1 : 0;
				demosaic_row(isp, f, raw, want, colour, &p, lines, rows[slot[n]]);
				cached[slot[n]] = (int32_t)want;
			}
			const uint8_t *a = rows[slot[0]], *b = rows[slot[1]];
			uint8_t *o = out[k];

			for (uint32_t x = 0; x < width; x++) {
				float sx = 2.0f * x0 + (x + 0.5f) * sx_step - 0.5f, tx;
				uint32_t sx0 = sx < 0 ? 0 : (uint32_t)sx, sx1;

				sx0 = sx0 > fw - 1 ? fw - 1 : sx0;
				sx1 = sx0 + 1 > fw - 1 ? fw - 1 : sx0 + 1;
				tx = sx < (float)sx0 ? 0 : sx - sx0;
				uint8_t *px = o + 3 * (isp->turned ? width - 1 - x : x);

				for (int c = 0; c < 3; c++) {
					float top = a[3 * sx0 + c] + tx * (a[3 * sx1 + c] - a[3 * sx0 + c]);
					float bottom = b[3 * sx0 + c] + tx * (b[3 * sx1 + c] - b[3 * sx0 + c]);

					px[c] = (uint8_t)(top + ty * (bottom - top) + 0.5f);
				}
			}
		}
		bool second = row + 1 < height;

		nv12_pair(out[0], out[1], width, y + (size_t)row * y_stride, second ? y + (size_t)(row + 1) * y_stride : NULL,
		    uv + (size_t)(row / 2) * uv_stride, video_range);
	}
}

/*
 * ---- O8: the renders in bands of rows on several cores ---------------------------------------------------
 * A call for every row (first_row 0, end_row past the last) is cut into isp_threads bands of even rows, each
 * rendered as the single-threaded path does it (every row depends on the frame only: the output is the same
 * byte for byte). A call for part of the rows (camera_device.c's own pieces) stays on its thread. isp_threads
 * (or env ISP_THREADS): 0 or 1 = off, the old path; -1 = hw.ncpu, at most 8. libdispatch on iOS, pthreads on the PC.
 */
int isp_threads = -1;

int
isp_thread_count(void)
{
	static int resolved = -1;
	int n = isp_threads;

	if (n < 0) {
		if (resolved < 0) {
			const char *env = getenv("ISP_THREADS");
			int cpus = 1;

			if (env != NULL && *env != '\0') {
				cpus = atoi(env);
			} else {
#if defined(__APPLE__)
				size_t size = sizeof(cpus);

				if (sysctlbyname("hw.ncpu", &cpus, &size, NULL, 0) != 0) {
					cpus = 1;
				}
#else
				cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
			}
			resolved = cpus < 1 ? 1 : cpus > 8 ? 8 : cpus;
		}
		n = resolved;
	}
	return n < 1 ? 1 : n > 8 ? 8 : n;
}

enum { BAND_NV12, BAND_BGRA, BAND_FULL, BAND_STILL };

struct band_job {
	int kind;
	const struct isp *isp;
	const struct redwood_camera_frame *f;
	const uint8_t *raw;
	uint8_t *y, *uv;
	size_t y_stride, uv_stride;
	uint32_t width, height, bands;
	bool video_range;
};

static void
band(void *context, size_t i)
{
	const struct band_job *j = context;
	uint32_t rows = ((j->height + j->bands - 1) / j->bands + 1) & ~1u, first = (uint32_t)i * rows, end = first + rows;

	end = end > j->height ? j->height : end;
	if (first >= end) {
		return;
	}
	switch (j->kind) {
	case BAND_NV12:
		isp_render_nv12_rows(j->isp, j->f, j->raw, j->y, j->y_stride, j->uv, j->uv_stride, j->width, j->height, first, end,
		    j->video_range);
		break;
	case BAND_BGRA:
		isp_render_bgra_rows(j->isp, j->f, j->raw, j->y, j->y_stride, j->width, j->height, first, end);
		break;
	case BAND_FULL:
		isp_render_full_nv12_rows(j->isp, j->f, j->raw, j->y, j->y_stride, j->uv, j->uv_stride, first, end, j->video_range);
		break;
	default:
		isp_render_still_nv12_rows(j->isp, j->f, j->raw, j->y, j->y_stride, j->uv, j->uv_stride, j->width, j->height, first, end,
		    j->video_range);
		break;
	}
}

#if !defined(__APPLE__)
struct band_thread {
	struct band_job *job;
	size_t i;
};

static void *
band_main(void *context)
{
	struct band_thread *t = context;

	band(t->job, t->i);
	return NULL;
}
#endif

/* true when the job was done in bands; false: the caller renders it on its own thread */
static bool
in_bands(struct band_job *j, uint32_t first_row, uint32_t end_row)
{
	int n = isp_thread_count();

	if (n < 2 || first_row != 0 || end_row < j->height || j->height < 4 * (uint32_t)n) {
		return false;
	}
	j->bands = (uint32_t)n;
#if defined(__APPLE__)
	dispatch_apply_f(j->bands, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), j, band);
#else
	pthread_t th[8];
	struct band_thread bt[8];
	bool started[8];

	for (uint32_t i = 0; i < j->bands; i++) {
		bt[i] = (struct band_thread){ j, i };
		started[i] = i > 0 && pthread_create(&th[i], NULL, band_main, &bt[i]) == 0;
	}
	band(j, 0);
	for (uint32_t i = 1; i < j->bands; i++) {
		if (started[i]) {
			pthread_join(th[i], NULL);
		} else {
			band(j, i);
		}
	}
#endif
	return true;
}

void
isp_render_nv12(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row, bool video_range)
{
	struct band_job j = { BAND_NV12, isp, f, raw, y, uv, y_stride, uv_stride, width, height, 0, video_range };

	if (!in_bands(&j, first_row, end_row)) {
		isp_render_nv12_rows(isp, f, raw, y, y_stride, uv, uv_stride, width, height, first_row, end_row, video_range);
	}
}

void
isp_render_bgra(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint8_t *out, size_t stride,
    uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row)
{
	struct band_job j = { BAND_BGRA, isp, f, raw, out, NULL, stride, 0, width, height, 0, false };

	if (!in_bands(&j, first_row, end_row)) {
		isp_render_bgra_rows(isp, f, raw, out, stride, width, height, first_row, end_row);
	}
}

void
isp_render_full_nv12(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t first_row, uint32_t end_row, bool video_range)
{
	struct band_job j = { BAND_FULL, isp, f, raw, y, uv, y_stride, uv_stride, f->width, f->height, 0, video_range };

	if (!in_bands(&j, first_row, end_row)) {
		isp_render_full_nv12_rows(isp, f, raw, y, y_stride, uv, uv_stride, first_row, end_row, video_range);
	}
}

void
isp_render_still_nv12(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row, bool video_range)
{
	struct band_job j = { BAND_STILL, isp, f, raw, y, uv, y_stride, uv_stride, width, height, 0, video_range };

	if (!in_bands(&j, first_row, end_row)) {
		isp_render_still_nv12_rows(isp, f, raw, y, y_stride, uv, uv_stride, width, height, first_row, end_row, video_range);
	}
}

/* ---- lens shading from a flat field --------------------------------------------------------------------- */

bool
isp_lsc_from_flat(struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, float *min_ratio)
{
	uint32_t cells_w = f->width / 2, cells_h = f->height / 2, half_w = cells_w / (LSC_W - 1) / 2, half_h = cells_h / (LSC_H - 1) / 2;
	float level[3][LSC_H][LSC_W], top[3] = { 0, 0, 0 };
	uint32_t clipped = 0, seen = 0;
	int r, b;

	cell_places(f->bayer, &r, &b);
	for (int gy = 0; gy < LSC_H; gy++) {
		for (int gx = 0; gx < LSC_W; gx++) {
			int cx0 = (int)((uint64_t)gx * (cells_w - 1) / (LSC_W - 1)) - (int)half_w;
			int cy0 = (int)((uint64_t)gy * (cells_h - 1) / (LSC_H - 1)) - (int)half_h;
			double sum[3] = { 0, 0, 0 };
			uint32_t n = 0;

			for (int cy = cy0; cy <= cy0 + 2 * (int)half_h; cy += 2) {
				for (int cx = cx0; cx <= cx0 + 2 * (int)half_w; cx += 2) {
					int v[4];

					if (cx < 0 || cy < 0 || cx >= (int)cells_w || cy >= (int)cells_h) {
						continue;
					}
					cell(raw + (size_t)cy * 2 * f->stride, f->stride, (uint32_t)cx, v);
					sum[0] += v[r];
					sum[2] += v[b];
					for (int j = 0; j < 4; j++) {
						sum[1] += j != r && j != b ? v[j] / 2.0 : 0;
						clipped += v[j] > WHITE - 30;
					}
					seen += 4;
					n++;
				}
			}
			for (int c = 0; c < 3; c++) {
				level[c][gy][gx] = n ? (float)(sum[c] / n) : 0;
				top[c] = level[c][gy][gx] > top[c] ? level[c][gy][gx] : top[c];
			}
		}
	}
	/* bright enough to tell shading from noise, and not clipped anywhere that counts */
	if (top[1] < 100 || clipped * 1000 > seen) {
		return false;
	}
	float darkest = 1;

	for (int c = 0; c < 3; c++) {
		for (int gy = 0; gy < LSC_H; gy++) {
			for (int gx = 0; gx < LSC_W; gx++) {
				float l = level[c][gy][gx] > 1 ? level[c][gy][gx] : 1;

				isp->lsc.gain[c][gy][gx] = top[c] / l;
				if (c == 1 && l / top[c] < darkest) {
					darkest = l / top[c];
				}
			}
		}
	}
	isp->lsc.on = true;
	if (min_ratio != NULL) {
		*min_ratio = darkest;
	}
	return true;
}

/* one module's part of a tuning file (tools/redfin-camtune.py): its ranges, and where each range's values are */
static bool
tune_set(const uint8_t **p, const uint8_t *end, struct isp_tune_set *set, size_t floats)
{
	uint32_t n;

	if (end - *p < 4) {
		return false;
	}
	memcpy(&n, *p, 4);
	*p += 4;
	if (n == 0 || n > TUNE_LIGHTS) {
		return false;
	}
	set->lights = n;
	for (uint32_t i = 0; i < n; i++) {
		if (end - *p < 12) {
			return false;
		}
		memcpy(&set->light[i].lux_from, *p, 4);
		memcpy(&set->light[i].lux_to, *p + 4, 4);
		memcpy(&set->light[i].ccts, *p + 8, 4);
		*p += 12;
		if (set->light[i].ccts == 0 || set->light[i].ccts > TUNE_CCTS) {
			return false;
		}
		for (uint32_t j = 0; j < set->light[i].ccts; j++) {
			if ((size_t)(end - *p) < (2 + floats) * 4) {
				return false;
			}
			memcpy(&set->light[i].cct_from[j], *p, 4);
			memcpy(&set->light[i].cct_to[j], *p + 4, 4);
			set->light[i].v[j] = (const float *)(const void *)(*p + 8);     /* (every field is 4 bytes) */
			*p += (2 + floats) * 4;
		}
	}
	return true;
}

bool
isp_load_tuning(struct isp *isp, const char *path)
{
	struct isp_tuning *t = calloc(1, sizeof(*t));
	int fd = open(path, O_RDONLY);
	struct stat st;
	const uint8_t *p, *end;
	bool good = false;

	if (isp->tuning != NULL) {
		free(isp->tuning->file);
		free(isp->tuning);
		isp->tuning = NULL;
	}
	if (t != NULL && fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 8 && st.st_size < (16 << 20) &&
	    (t->file = malloc((size_t)st.st_size)) != NULL && read(fd, t->file, (size_t)st.st_size) == st.st_size) {
		p = t->file;
		end = p + st.st_size;
		good = memcmp(p, "RWCISP02", 8) == 0;
		p += 8;
		good = good && tune_set(&p, end, &t->ccm, 9) && tune_set(&p, end, &t->lsc, 4 * LSC_W * LSC_H) && end - p >= 4;
		if (good) {
			memcpy(&t->refs, p, 4);
			p += 4;
			good = t->refs <= 16 && (size_t)(end - p) >= t->refs * 12;
		}
		if (good) {
			memcpy(t->ref, p, t->refs * 12);
		}
	}
	if (fd >= 0) {
		close(fd);
	}
	if (!good) {
		if (t != NULL) {
			free(t->file);
		}
		free(t);
		return false;
	}
	isp->tuning = t;
	return true;
}

/*
 * Where x falls among ascending ranges [from[i], to[i]]: in one (a = b), or between two (a, b and how
 * far from a's end to b's start, 0 to 1); before the first, the first; after the last, the last.
 */
static void
pick(const float *from, const float *to, uint32_t n, float x, uint32_t *a, uint32_t *b, float *t)
{
	*a = *b = n - 1;
	*t = 0;
	for (uint32_t i = 0; i < n; i++) {
		if (x <= to[i]) {
			*a = *b = i;
			if (i > 0 && x < from[i]) {
				*a = i - 1;
				*t = (x - to[i - 1]) / (from[i] - to[i - 1]);
			}
			return;
		}
	}
}

/* a module's values for this light and temperature: those of the ranges they fall in, or between */
static void
tune_blend(const struct isp_tune_set *set, float lux_index, float cct, size_t floats, float *out)
{
	float from[TUNE_LIGHTS], to[TUNE_LIGHTS], tl;
	uint32_t la, lb;

	for (uint32_t i = 0; i < set->lights; i++) {
		from[i] = set->light[i].lux_from;
		to[i] = set->light[i].lux_to;
	}
	pick(from, to, set->lights, lux_index, &la, &lb, &tl);
	memset(out, 0, floats * sizeof(float));
	for (int k = 0; k < 2; k++) {
		uint32_t l = k ? lb : la, ca, cb;
		float w = k ? tl : 1 - tl, tc;

		if (w == 0) {
			continue;
		}
		pick(set->light[l].cct_from, set->light[l].cct_to, set->light[l].ccts, cct, &ca, &cb, &tc);
		for (size_t i = 0; i < floats; i++) {
			out[i] += w * ((1 - tc) * set->light[l].v[ca][i] + tc * set->light[l].v[cb][i]);
		}
	}
}

void
isp_tune(struct isp *isp, float lux_index, float cct)
{
	const struct isp_tuning *t = isp->tuning;
	float m[9], g[4 * LSC_H * LSC_W];
	const int grid = LSC_H * LSC_W;

	if (t == NULL) {
		return;
	}
	tune_blend(&t->ccm, lux_index, cct, 9, m);
	isp_set_ccm(isp, m);
	tune_blend(&t->lsc, lux_index, cct, 4 * (size_t)grid, g);
	for (int gy = 0; gy < LSC_H; gy++) {
		for (int gx = 0; gx < LSC_W; gx++) {
			int at = gy * LSC_W + gx;

			isp->lsc.gain[0][gy][gx] = g[at];
			isp->lsc.gain[1][gy][gx] = (g[grid + at] + g[2 * grid + at]) / 2;
			isp->lsc.gain[2][gy][gx] = g[3 * grid + at];
		}
	}
	isp->lsc.on = true;
}
