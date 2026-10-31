/*
 * sp23: adspboot --audio-fifo: the ring between the FIFO and pcm_fill, and the DC guard. Header only, no phone calls, so
 * phone/wifi/test_af_ring.c can run it on the host (phone/wifi/test.sh). research/notes/speaker-daemon.md section 2.
 *
 * Input: s16le stereo at 48 kHz, downmixed (L+R)/2 here since the amp's RX is slot 0 (cs35l41.c SP_RX_SLOT).
 * Drift (HAL clock against the AFE's): above AF_RING_HIGH one frame is dropped per pull, below AF_RING_LOW one is
 * repeated; a burst above AF_RING_HARD (a stall in the glink loop) is cut back to HIGH at once. Values are guesses.
 */
#ifndef AF_RING_H
#define AF_RING_H

#include <stdint.h>
#include <string.h>

#define AF_RING_FRAMES	9600	/* 200 ms */
#define AF_RING_HIGH	4800	/* 100 ms */
#define AF_RING_HARD	7200
#define AF_RING_LOW	960	/* 20 ms */

struct af_ring {
	int16_t d[AF_RING_FRAMES];
	uint32_t rd, n;		/* oldest frame, frames held */
	uint8_t carry[4];	/* a frame cut by a read */
	uint32_t ncarry;
	int gate;		/* 1: zeros are dropped until the first non-zero sample (no stream yet) */
	uint64_t underrun, overrun, dropped, repeated;	/* underrun: pulls cut short; the others in frames */
};

static inline void af_ring_clear(struct af_ring *r, int gate)
{
	r->rd = r->n = r->ncarry = 0;
	r->gate = gate;
}

static inline void af_ring_frame(struct af_ring *r, const uint8_t *b)
{
	int16_t lr[2];
	memcpy(lr, b, 4);
	if (r->gate) {
		if (!lr[0] && !lr[1])
			return;
		r->gate = 0;
	}
	if (r->n == AF_RING_FRAMES) {	/* full: the oldest goes */
		r->rd = (r->rd + 1) % AF_RING_FRAMES;
		r->n--;
		r->overrun++;
	}
	r->d[(r->rd + r->n++) % AF_RING_FRAMES] = (int16_t)(((int)lr[0] + lr[1]) / 2);
}

static inline void af_ring_put(struct af_ring *r, const void *data, size_t len)
{
	const uint8_t *p = data;
	while (len && r->ncarry) {
		r->carry[r->ncarry++] = *p++;
		len--;
		if (r->ncarry == 4) {
			af_ring_frame(r, r->carry);
			r->ncarry = 0;
		}
	}
	for (; len >= 4; p += 4, len -= 4)
		af_ring_frame(r, p);
	while (len--)
		r->carry[r->ncarry++] = *p++;
}

/* N mono frames into OUT, zeros after what the ring held; returns the frames that were real */
static inline uint32_t af_ring_pull(struct af_ring *r, int16_t *out, uint32_t n)
{
	uint32_t want = n, got = 0, drop = 0;
	if (r->n > AF_RING_HARD)
		drop = r->n - AF_RING_HIGH;
	else if (r->n > AF_RING_HIGH)
		drop = 1;
	r->rd = (r->rd + drop) % AF_RING_FRAMES;
	r->n -= drop;
	r->dropped += drop;
	int rep = n > 1 && r->n >= n && r->n < AF_RING_LOW;
	if (rep)
		want = n - 1;
	for (; got < want && r->n; got++, r->n--) {
		out[got] = r->d[r->rd];
		r->rd = (r->rd + 1) % AF_RING_FRAMES;
	}
	if (rep) {
		out[got] = out[got - 1];
		got++;
		r->repeated++;
	}
	if (got < n) {
		memset(out + got, 0, (n - got) * sizeof(*out));
		r->underrun++;
	}
	return got;
}

/*
 * DC guard: the mean of each 100 ms window; AF_DC_TRIP windows in a row of one sign at or above AF_DC_LIMIT = muted,
 * AF_DC_CLEAR windows below it = unmuted. One sign, so that a loud low tone (under 10 Hz the windows swing) doesn't
 * trip. Not reset between streams: DC that goes on stays muted. Limit (-26 dBFS) and windows are guesses.
 */
#define AF_DC_WINDOW	4800
#define AF_DC_LIMIT	1638
#define AF_DC_TRIP	5
#define AF_DC_CLEAR	10

struct af_dc {
	int64_t sum;
	uint32_t n;
	int run, clean, muted;	/* run: windows over the limit, its sign is the sign of run */
	uint32_t trips;
	int last_mean;
};

/* Feed samples; 1 = muted now */
static inline int af_dc_feed(struct af_dc *g, const int16_t *s, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		g->sum += s[i];
		if (++g->n < AF_DC_WINDOW)
			continue;
		int mean = (int)(g->sum / (int64_t)g->n);
		g->sum = 0;
		g->n = 0;
		g->last_mean = mean;
		if (mean >= AF_DC_LIMIT || mean <= -AF_DC_LIMIT) {
			int sign = mean > 0 ? 1 : -1;
			g->run = g->run * sign > 0 ? g->run + sign : sign;
			g->clean = 0;
			if ((g->run < 0 ? -g->run : g->run) >= AF_DC_TRIP && !g->muted) {
				g->muted = 1;
				g->trips++;
			}
		} else {
			g->run = 0;
			if (g->muted && ++g->clean >= AF_DC_CLEAR)
				g->muted = 0;
		}
	}
	return g->muted;
}

#endif
