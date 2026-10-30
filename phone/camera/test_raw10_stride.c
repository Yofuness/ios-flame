/*
 * cam34 host test: a synthetic 2016x4 RAW10 frame with 16 B-padded lines (stride 2528, data 2520) must unpack to the
 * pixels written; the padding bytes (0xee) must never show up. cc -O2 test_raw10_stride.c -lm -pthread && ./a.out
 */
#include <stdio.h>
#include "camera_isp.c"

#define TW	2016u
#define TH	4u
#define TS	((TW * 10 / 8 + 15) & ~15u)

int
main(void)
{
	static uint8_t raw[TS * TH];
	static int out[TW];
	struct redwood_camera_frame f;
	int bad = 0;

	_Static_assert(TS == 2528, "stride");
	memset(raw, 0xee, sizeof raw);
	memset(&f, 0, sizeof f);
	f.width = TW, f.height = TH, f.stride = TS, f.black = 0;
	for (uint32_t y = 0; y < TH; y++)
		for (uint32_t x = 0; x < TW; x += 4) {
			uint8_t *g = raw + y * TS + x / 4 * 5;
			g[4] = 0;
			for (int i = 0; i < 4; i++) {
				unsigned v = (y * 7919u + (x + i) * 31u) & 1023u;
				g[i] = (uint8_t)(v >> 2), g[4] |= (uint8_t)((v & 3) << (i * 2));
			}
		}
	for (uint32_t y = 0; y < TH; y++) {
		unpack_line(&f, raw, (int)y, out);
		for (uint32_t x = 0; x < TW; x++)
			if (out[x] != (int)((y * 7919u + x * 31u) & 1023u) && bad++ < 5)
				printf("raw10 stride: y %u x %u got %d\n", y, x, out[x]);
	}
	printf("raw10 stride %u: %s\n", TS, bad ? "FAIL" : "ok");
	return bad != 0;
}
