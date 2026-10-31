/* sp23: host test of af_ring.h: downmix, gate, cut frames, overflow, underrun, drift trim, DC guard. */
#include <math.h>
#include <stdio.h>
#include "af_ring.h"

static int fails;
static struct af_ring r;

#define CHECK(c) do { int ok_ = (c); printf("%s %s\n", ok_ ? "ok  " : "FAIL", #c); fails += !ok_; } while (0)

static void put(int16_t l, int16_t x)
{
	int16_t f[2] = { l, x };
	af_ring_put(&r, f, 4);
}

static void feed_dc(struct af_dc *g, int level, int windows)
{
	int16_t s[480];
	for (int i = 0; i < 480; i++)
		s[i] = (int16_t)level;
	for (int i = 0; i < windows * 10; i++)
		af_dc_feed(g, s, 480);
}

/* HZ sine, peak PEAK, for MS milliseconds */
static void feed_sine(struct af_dc *g, double hz, int peak, int ms)
{
	int16_t s[480];
	static long at;
	for (int k = 0; k < ms / 10; k++) {
		for (int i = 0; i < 480; i++, at++)
			s[i] = (int16_t)(peak * sin(2 * M_PI * hz * (double)at / 48000));
		af_dc_feed(g, s, 480);
	}
}

int main(void)
{
	int16_t o[480];

	/* downmix */
	af_ring_clear(&r, 0);
	put(1000, 3000);
	put(-32768, -32768);
	put(32767, -32768);
	for (int i = 0; i < AF_RING_LOW; i++)	/* above LOW: no repeat in this pull */
		put(7, 7);
	af_ring_pull(&r, o, 3);
	CHECK(o[0] == 2000 && o[1] == -32768 && o[2] == 0);

	/* gate: zeros dropped until the first non-zero frame, then zeros kept */
	af_ring_clear(&r, 1);
	for (int i = 0; i < 100; i++)
		put(0, 0);
	CHECK(r.n == 0 && r.gate == 1);
	put(0, 5);
	CHECK(r.n == 1 && r.gate == 0);
	put(0, 0);
	CHECK(r.n == 2);

	/* a frame cut across two reads */
	af_ring_clear(&r, 0);
	{
		int16_t f[2] = { 100, 300 };
		const uint8_t *b = (const uint8_t *)f;
		af_ring_put(&r, b, 1);
		CHECK(r.n == 0);
		af_ring_put(&r, b + 1, 2);
		CHECK(r.n == 0);
		af_ring_put(&r, b + 3, 1);
		CHECK(r.n == 1);
		af_ring_pull(&r, o, 1);
		CHECK(o[0] == 200);
	}

	/* empty: zeros and a counter */
	r.underrun = 0;
	memset(o, 0x55, sizeof(o));
	CHECK(af_ring_pull(&r, o, 480) == 0 && o[0] == 0 && o[479] == 0 && r.underrun == 1);
	put(10, 10);
	CHECK(af_ring_pull(&r, o, 480) == 1 && o[0] == 10 && o[1] == 0 && r.underrun == 2);

	/* overflow: the oldest goes, the count stays at the size, the order holds */
	af_ring_clear(&r, 0);
	r.overrun = 0;
	for (int i = 0; i < AF_RING_FRAMES + 7; i++)
		put((int16_t)(i & 0x7fff), (int16_t)(i & 0x7fff));
	CHECK(r.n == AF_RING_FRAMES && r.overrun == 7);
	/* the hard trim cuts to HIGH before the pull: the newest HIGH frames are left */
	r.dropped = 0;
	af_ring_pull(&r, o, 1);
	CHECK(r.dropped == AF_RING_FRAMES - AF_RING_HIGH);
	CHECK(o[0] == (int16_t)(AF_RING_FRAMES + 7 - AF_RING_HIGH));

	/* drift, writer fast by 1 frame per pull: the level stays near HIGH */
	af_ring_clear(&r, 0);
	r.dropped = 0;
	for (int i = 0; i < AF_RING_HIGH; i++)
		put(1, 1);
	for (int k = 0; k < 1000; k++) {
		for (int i = 0; i < 481; i++)
			put(1, 1);
		af_ring_pull(&r, o, 480);
	}
	CHECK(r.n >= AF_RING_HIGH - 1 && r.n <= AF_RING_HIGH + 2 && r.dropped > 900);

	/* drift, writer slow: one frame repeated per pull under LOW, no underrun, the level stays near LOW */
	af_ring_clear(&r, 0);
	r.underrun = r.repeated = 0;
	for (int i = 0; i < AF_RING_LOW + 100; i++)
		put(1, 1);
	for (int k = 0; k < 2000; k++) {
		for (int i = 0; i < 479; i++)
			put(1, 1);
		af_ring_pull(&r, o, 480);
		if (o[479] != 1)
			fails++;
	}
	CHECK(r.underrun == 0 && r.repeated > 1000 && r.n >= AF_RING_LOW - 480 - 2 && r.n <= AF_RING_LOW);	/* LOW is judged with this period's 479 in */

	/* DC guard */
	struct af_dc g;
	memset(&g, 0, sizeof(g));
	feed_dc(&g, 0, 20);
	CHECK(!g.muted);
	feed_dc(&g, 1000, 50);	/* under the limit */
	CHECK(!g.muted);
	feed_dc(&g, 3000, AF_DC_TRIP - 1);
	CHECK(!g.muted);
	feed_dc(&g, 3000, 1);
	CHECK(g.muted && g.trips == 1);
	feed_dc(&g, 3000, 100);	/* stays muted */
	CHECK(g.muted && g.trips == 1);
	feed_dc(&g, 0, AF_DC_CLEAR - 1);
	CHECK(g.muted);
	feed_dc(&g, 0, 1);
	CHECK(!g.muted);
	feed_dc(&g, -3000, AF_DC_TRIP);	/* negative too */
	CHECK(g.muted && g.trips == 2);

	/* a sign change restarts the count */
	memset(&g, 0, sizeof(g));
	for (int i = 0; i < 20; i++)
		feed_dc(&g, i & 1 ? 3000 : -3000, 1);
	CHECK(!g.muted);

	/* loud music-like tones never trip: 1 kHz, 100 Hz, 20 Hz, 5 Hz at full scale, 10 s each */
	memset(&g, 0, sizeof(g));
	feed_sine(&g, 1000, 32000, 10000);
	feed_sine(&g, 100, 32000, 10000);
	feed_sine(&g, 20, 32000, 10000);
	feed_sine(&g, 5, 32000, 10000);
	CHECK(!g.muted && g.trips == 0);

	printf("%s\n", fails ? "af_ring: FAILED" : "af_ring: all ok");
	return fails != 0;
}
