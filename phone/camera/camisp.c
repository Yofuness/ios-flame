/*
 * camisp IN.raw OUT [--bayer RGGB|GRBG|GBRG|BGGR] [--full] [--fmt bgra|nv12|ppm] [--frames N] [--black B]
 *
 * Camera stage e3: one saved frame (camrx capture/stream: 2016x1512 MIPI RAW10, stride 2520, no header)
 * through ios-redfin's camera_isp.c: a redwood_camera_frame header is made for it in memory, then
 * isp_measure + isp_awb + isp_ae, and a picture: binned 2x2 preview (1008x756) by default, every pixel
 * (2016x1512, bilinear demosaic) with --full. OUT: ppm (from BGRA), raw bgra or raw nv12.
 * Prints ms per frame over N renders (default 5), the size, mean R/G/B and the ISP's AWB/AE values.
 * Builds for the phone (phone/camera/build.sh) and the PC (cc -O2 camisp.c camera_isp.c -lm).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "camera_isp.h"

#define W       2016
#define H       1512
#define STRIDE  2520

static double
now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int
usage(void)
{
	fprintf(stderr, "usage: camisp IN.raw OUT [--bayer RGGB|GRBG|GBRG|BGGR] [--full] [--fmt bgra|nv12|ppm] [--frames N] [--black B]\n");
	return 2;
}

int
main(int argc, char **argv)
{
	static const char *orders[4] = { "RGGB", "GRBG", "GBRG", "BGGR" };
	const char *fmt = "ppm";
	int bayer = 0, full = 0, frames = 5, black = 64;

	if (argc < 3)
		return usage();
	for (int i = 3; i < argc; i++) {
		if (!strcmp(argv[i], "--full")) {
			full = 1;
		} else if (i + 1 < argc && !strcmp(argv[i], "--bayer")) {
			i++;
			bayer = -1;
			for (int k = 0; k < 4; k++)
				if (!strcmp(argv[i], orders[k]))
					bayer = k;
			if (bayer < 0)
				return usage();
		} else if (i + 1 < argc && !strcmp(argv[i], "--fmt")) {
			fmt = argv[++i];
		} else if (i + 1 < argc && !strcmp(argv[i], "--frames")) {
			frames = atoi(argv[++i]);
		} else if (i + 1 < argc && !strcmp(argv[i], "--black")) {
			black = atoi(argv[++i]);
		} else {
			return usage();
		}
	}
	if (frames < 1)
		frames = 1;
	if (strcmp(fmt, "ppm") && strcmp(fmt, "bgra") && strcmp(fmt, "nv12"))
		return usage();

	size_t rawn = (size_t)STRIDE * H;
	uint8_t *raw = malloc(rawn);
	FILE *fp = fopen(argv[1], "rb");
	if (!fp || !raw || fread(raw, 1, rawn, fp) != rawn) {
		fprintf(stderr, "camisp: %s: need %zu bytes of RAW10\n", argv[1], rawn);
		return 1;
	}
	fclose(fp);

	/* the header the kernel's /dev/redwood-camera would give (IMX363 mode-8-like values for AE) */
	struct redwood_camera_frame f;
	memset(&f, 0, sizeof(f));
	memcpy(f.magic, REDWOOD_CAMERA_MAGIC, 8);
	f.header_bytes = sizeof(f);
	f.width = W;
	f.height = H;
	f.stride = STRIDE;
	f.format = 1;
	f.bayer = (uint32_t)bayer;
	f.black = (uint32_t)black;
	f.sequence = 1;
	f.exposure_lines = 1000;
	f.line_ns = 10000;
	f.gain = 0;
	f.gain_unity = 256;
	f.gain_max = 2048;
	f.frame_lines = 3300;
	f.frame_lines_min = 3300;
	f.frame_lines_max = 65535;
	f.margin = 20;
	f.sensor = 1;
	f.focus = REDWOOD_CAMERA_NO_FOCUS;

	struct isp isp;
	struct isp_stats s;
	struct redwood_camera_command c;
	isp_init(&isp);
	isp_measure(&isp, &f, raw, &s);
	/* AWB moves a little at a time: let it settle on this frame as a stream would */
	for (int i = 0; i < 30; i++) {
		isp_awb(&isp, &s);
		isp_measure(&isp, &f, raw, &s);
	}
	memset(&c, 0, sizeof(c));
	bool asked = isp_ae(&isp, &f, &s, &c);

	uint32_t ow = full ? W : W / 2, oh = full ? H : H / 2;
	int nv12 = !strcmp(fmt, "nv12");
	size_t outn = nv12 ? (size_t)ow * oh * 3 / 2 : (size_t)ow * oh * 4;
	uint8_t *out = malloc(outn);
	if (!out)
		return 1;
	double t0 = now_ms();
	for (int n = 0; n < frames; n++) {
		if (full && nv12)
			isp_render_full_nv12(&isp, &f, raw, out, ow, out + (size_t)ow * oh, ow, 0, oh, false);
		else if (full)
			isp_render_still_nv12(&isp, &f, raw, out, ow, out + (size_t)ow * oh, ow, ow, oh, 0, oh, false);
		else if (nv12)
			isp_render_nv12(&isp, &f, raw, out, ow, out + (size_t)ow * oh, ow, ow, oh, 0, oh, false);
		else
			isp_render_bgra(&isp, &f, raw, out, (size_t)ow * 4, ow, oh, 0, oh);
	}
	double ms = (now_ms() - t0) / frames;

	/* --full ppm/bgra: camera_isp.c's full demosaic is NV12 only; convert to BGRA (full range BT.601) */
	if (full && !nv12) {
		uint8_t *bgra = malloc((size_t)ow * oh * 4);
		const uint8_t *uv = out + (size_t)ow * oh;
		if (!bgra)
			return 1;
		for (uint32_t y = 0; y < oh; y++)
			for (uint32_t x = 0; x < ow; x++) {
				double Y = out[(size_t)y * ow + x];
				double u = uv[(size_t)(y / 2) * ow + (x & ~1u)] - 128.0, v = uv[(size_t)(y / 2) * ow + (x | 1u)] - 128.0;
				double rgb[3] = { Y + 1.402 * v, Y - 0.344136 * u - 0.714136 * v, Y + 1.772 * u };
				uint8_t *p = bgra + ((size_t)y * ow + x) * 4;
				for (int k = 0; k < 3; k++) {
					double q = rgb[k] < 0 ? 0 : rgb[k] > 255 ? 255 : rgb[k];
					p[2 - k] = (uint8_t)(q + 0.5);
				}
				p[3] = 255;
			}
		free(out);
		out = bgra;
		outn = (size_t)ow * oh * 4;
	}

	fp = fopen(argv[2], "wb");
	if (!fp) {
		perror(argv[2]);
		return 1;
	}
	if (!strcmp(fmt, "ppm")) {
		fprintf(fp, "P6\n%u %u\n255\n", ow, oh);
		for (size_t i = 0; i < (size_t)ow * oh; i++) {
			uint8_t px[3] = { out[i * 4 + 2], out[i * 4 + 1], out[i * 4] };
			fwrite(px, 1, 3, fp);
		}
	} else {
		fwrite(out, 1, outn, fp);
	}
	fclose(fp);

	printf("camisp: %ux%u %s %s bayer %s black %d: %.2f ms/frame over %d\n", ow, oh, full ? "full" : "binned", fmt,
	    orders[bayer], black, ms, frames);
	printf("mean R/G/B %.1f %.1f %.1f (10-bit above black), bright %.1f, cells %u, grey %u neutral %u\n",
	    s.mean[0], s.mean[1], s.mean[2], s.bright, s.cells, s.grey_cells, s.neutral_cells);
	printf("AWB gains R/G/B %.3f %.3f %.3f, digital %.3f\n", isp.wb[0], isp.wb[1], isp.wb[2], isp.digital);
	if (asked)
		printf("AE asks: lines %u gain %u frame_lines %u dgain %u\n", c.a, c.b, c.c, c.d);
	else
		printf("AE: no change (stable %d)\n", isp.ae_stable);
	free(out);
	free(raw);
	return 0;
}
