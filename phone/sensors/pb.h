/*
 * From ios-redfin (DenialOfGravity, https://github.com/DenialOfGravity/ios-redfin) phone/sensors/pb.h,
 * Copyright (c) 2026 dipa-studio, MIT License (see LICENSE.redfin). Sunfish changes are marked "sunfish".
 */
/*
 * A small protobuf (proto2 wire format) coder for the Qualcomm SEE messages sensd speaks: varint, zigzag
 * sint64, fixed32/fixed64/float, length-delimited bytes/strings/nested messages. No schema: callers write
 * fields by number and walk received messages field by field.
 */
#ifndef PB_H
#define PB_H
#include <stdint.h>
#include <stddef.h>

enum { PB_VARINT = 0, PB_FIXED64 = 1, PB_LEN = 2, PB_FIXED32 = 5 };

/* Writer over a caller's buffer; err is set (and further writes ignored) when it would overflow. */
struct pb_buf {
	uint8_t *b;
	size_t n, cap;
	int err;
};
void pb_init(struct pb_buf *w, uint8_t *mem, size_t cap);
void pb_varint(struct pb_buf *w, uint32_t field, uint64_t v);
void pb_bool(struct pb_buf *w, uint32_t field, int v);
void pb_sint64(struct pb_buf *w, uint32_t field, int64_t v);	/* zigzag */
void pb_fixed32(struct pb_buf *w, uint32_t field, uint32_t v);
void pb_fixed64(struct pb_buf *w, uint32_t field, uint64_t v);
void pb_float(struct pb_buf *w, uint32_t field, float v);
void pb_bytes(struct pb_buf *w, uint32_t field, const void *p, size_t len);	/* also nested messages */
void pb_string(struct pb_buf *w, uint32_t field, const char *s);
/* Raw helpers (exposed for the tests). */
size_t pb_put_varint(uint8_t *out, uint64_t v);		/* out needs 10 bytes */

/* Reader. */
struct pb_rd {
	const uint8_t *p, *end;
};
struct pb_field {
	uint32_t num;
	uint8_t wt;
	uint64_t v;		/* PB_VARINT, PB_FIXED64, PB_FIXED32 */
	const uint8_t *data;	/* PB_LEN */
	size_t len;
};
void pb_rd_init(struct pb_rd *r, const void *p, size_t n);
/* 1: a field was read, 0: end of message, -1: malformed. */
int pb_next(struct pb_rd *r, struct pb_field *f);
float pb_f32(const struct pb_field *f);		/* PB_FIXED32 as float */
int64_t pb_zigzag(uint64_t v);
/*
 * Repeated floats: either one PB_FIXED32 field per value or one PB_LEN "packed" field. Appends the value(s)
 * of f to out (at most max in total); returns the new count.
 */
int pb_floats(const struct pb_field *f, float *out, int count, int max);

#endif
