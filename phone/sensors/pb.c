/*
 * From ios-redfin (DenialOfGravity, https://github.com/DenialOfGravity/ios-redfin) phone/sensors/pb.c,
 * Copyright (c) 2026 dipa-studio, MIT License (see LICENSE.redfin). Sunfish changes are marked "sunfish".
 */
#include <string.h>
#include "pb.h"

void pb_init(struct pb_buf *w, uint8_t *mem, size_t cap)
{
	w->b = mem;
	w->n = 0;
	w->cap = cap;
	w->err = 0;
}

size_t pb_put_varint(uint8_t *out, uint64_t v)
{
	size_t n = 0;
	do {
		uint8_t c = v & 0x7f;
		v >>= 7;
		out[n++] = c | (v ? 0x80 : 0);
	} while (v);
	return n;
}

static void put(struct pb_buf *w, const void *p, size_t len)
{
	if (w->err || len > w->cap - w->n) {
		w->err = 1;
		return;
	}
	memcpy(w->b + w->n, p, len);
	w->n += len;
}

static void key(struct pb_buf *w, uint32_t field, int wt)
{
	uint8_t t[10];
	put(w, t, pb_put_varint(t, ((uint64_t)field << 3) | (uint64_t)wt));
}

void pb_varint(struct pb_buf *w, uint32_t field, uint64_t v)
{
	uint8_t t[10];
	key(w, field, PB_VARINT);
	put(w, t, pb_put_varint(t, v));
}

void pb_bool(struct pb_buf *w, uint32_t field, int v) { pb_varint(w, field, v ? 1 : 0); }

void pb_sint64(struct pb_buf *w, uint32_t field, int64_t v)
{
	pb_varint(w, field, ((uint64_t)v << 1) ^ (uint64_t)(v >> 63));
}

void pb_fixed32(struct pb_buf *w, uint32_t field, uint32_t v)
{
	uint8_t t[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
	key(w, field, PB_FIXED32);
	put(w, t, 4);
}

void pb_fixed64(struct pb_buf *w, uint32_t field, uint64_t v)
{
	uint8_t t[8];
	for (int i = 0; i < 8; i++)
		t[i] = (uint8_t)(v >> (8 * i));
	key(w, field, PB_FIXED64);
	put(w, t, 8);
}

void pb_float(struct pb_buf *w, uint32_t field, float v)
{
	uint32_t u;
	memcpy(&u, &v, 4);
	pb_fixed32(w, field, u);
}

void pb_bytes(struct pb_buf *w, uint32_t field, const void *p, size_t len)
{
	uint8_t t[10];
	key(w, field, PB_LEN);
	put(w, t, pb_put_varint(t, len));
	put(w, p, len);
}

void pb_string(struct pb_buf *w, uint32_t field, const char *s) { pb_bytes(w, field, s, strlen(s)); }

void pb_rd_init(struct pb_rd *r, const void *p, size_t n)
{
	r->p = p;
	r->end = r->p + n;
}

static int get_varint(struct pb_rd *r, uint64_t *v)
{
	uint64_t x = 0;
	for (int s = 0; s < 70; s += 7) {
		if (r->p >= r->end)
			return -1;
		uint8_t c = *r->p++;
		x |= (uint64_t)(c & 0x7f) << s;
		if (!(c & 0x80)) {
			*v = x;
			return 0;
		}
	}
	return -1;
}

int pb_next(struct pb_rd *r, struct pb_field *f)
{
	uint64_t k, v;
	if (r->p >= r->end)
		return 0;
	if (get_varint(r, &k) || (k >> 3) == 0 || (k >> 3) > 0x1fffffff)
		return -1;
	memset(f, 0, sizeof(*f));
	f->num = (uint32_t)(k >> 3);
	f->wt = k & 7;
	switch (f->wt) {
	case PB_VARINT:
		return get_varint(r, &f->v) ? -1 : 1;
	case PB_FIXED64:
		if (r->end - r->p < 8)
			return -1;
		for (int i = 7; i >= 0; i--)
			f->v = (f->v << 8) | r->p[i];
		r->p += 8;
		return 1;
	case PB_FIXED32:
		if (r->end - r->p < 4)
			return -1;
		f->v = (uint32_t)r->p[0] | (uint32_t)r->p[1] << 8 | (uint32_t)r->p[2] << 16 | (uint32_t)r->p[3] << 24;
		r->p += 4;
		return 1;
	case PB_LEN:
		if (get_varint(r, &v) || v > (uint64_t)(r->end - r->p))
			return -1;
		f->data = r->p;
		f->len = (size_t)v;
		r->p += v;
		return 1;
	default:		/* groups (3/4) are not used by SEE */
		return -1;
	}
}

float pb_f32(const struct pb_field *f)
{
	uint32_t u = (uint32_t)f->v;
	float x;
	memcpy(&x, &u, 4);
	return x;
}

int64_t pb_zigzag(uint64_t v) { return (int64_t)(v >> 1) ^ -(int64_t)(v & 1); }

int pb_floats(const struct pb_field *f, float *out, int count, int max)
{
	if (f->wt == PB_FIXED32) {
		if (count < max)
			out[count++] = pb_f32(f);
	} else if (f->wt == PB_LEN) {
		for (size_t o = 0; o + 4 <= f->len && count < max; o += 4) {
			uint32_t u = (uint32_t)f->data[o] | (uint32_t)f->data[o + 1] << 8 |
			    (uint32_t)f->data[o + 2] << 16 | (uint32_t)f->data[o + 3] << 24;
			memcpy(&out[count++], &u, 4);
		}
	}
	return count;
}
