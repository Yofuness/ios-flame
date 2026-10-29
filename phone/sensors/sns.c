/*
 * From ios-redfin (DenialOfGravity, https://github.com/DenialOfGravity/ios-redfin) phone/sensors/sns.c,
 * Copyright (c) 2026 dipa-studio, MIT License (see LICENSE.redfin). Sunfish changes are marked "sunfish".
 */
#include <string.h>
#include "pb.h"
#include "sns.h"
#include "../wifi/qmi.h"

const struct sns_suid sns_suid_lookup = { 0xababababababababull, 0xababababababababull };

size_t sns_enc_suid_req(uint8_t *out, size_t cap, const char *data_type, int register_updates, int default_only)
{
	struct pb_buf w;
	pb_init(&w, out, cap);
	pb_string(&w, 1, data_type);
	pb_bool(&w, 2, register_updates);
	pb_bool(&w, 3, default_only);
	return w.err ? 0 : w.n;
}

size_t sns_enc_sensor_config(uint8_t *out, size_t cap, float sample_rate)
{
	struct pb_buf w;
	pb_init(&w, out, cap);
	pb_float(&w, 1, sample_rate);
	return w.err ? 0 : w.n;
}

size_t sns_enc_attr_req(uint8_t *out, size_t cap, int register_updates)
{
	struct pb_buf w;
	pb_init(&w, out, cap);
	pb_bool(&w, 1, register_updates);
	return w.err ? 0 : w.n;
}

static void enc_suid(struct pb_buf *w, uint32_t field, const struct sns_suid *s)
{
	uint8_t t[24];
	struct pb_buf in;
	pb_init(&in, t, sizeof(t));
	pb_fixed64(&in, 1, s->low);
	pb_fixed64(&in, 2, s->high);
	pb_bytes(w, field, t, in.n);
}

size_t sns_enc_client_request(uint8_t *out, size_t cap, const struct sns_suid *suid, uint32_t msg_id,
    const uint8_t *payload, size_t plen, int delivery)
{
	struct pb_buf w, sc, rq;
	uint8_t scb[16], rqb[1024];

	pb_init(&sc, scb, sizeof(scb));
	pb_varint(&sc, 1, SNS_PROC_APSS);
	pb_varint(&sc, 2, (uint64_t)delivery);
	pb_init(&rq, rqb, sizeof(rqb));
	if (plen)
		pb_bytes(&rq, 2, payload, plen);
	if (rq.err)
		return 0;

	pb_init(&w, out, cap);
	enc_suid(&w, 1, suid);
	pb_fixed32(&w, 2, msg_id);
	pb_bytes(&w, 3, scb, sc.n);
	pb_bytes(&w, 4, rqb, rq.n);	/* required, even when empty */
	return w.err ? 0 : w.n;
}

size_t sns_qmi_request(uint8_t *out, size_t cap, uint16_t txn, const uint8_t *req, size_t rlen, int jumbo)
{
	static struct qmi_buf q;
	uint8_t tlv[2 + 1000];
	uint16_t l = (uint16_t)rlen;

	if (rlen > 1000)	/* SNS_CLIENT_REQ_LEN_MAX_V01 */
		return 0;
	memcpy(tlv, &l, 2);
	memcpy(tlv + 2, req, rlen);
	qmi_start(&q, QMI_REQUEST, txn, SNS_QMI_CLIENT_REQ);
	qmi_tlv(&q, 0x01, tlv, (uint16_t)(2 + rlen));
	if (jumbo)
		qmi_tlv_u8(&q, 0x10, 1);
	size_t n = qmi_finish(&q);
	if (!n || n > cap)
		return 0;
	memcpy(out, q.b, n);
	return n;
}

int sns_qmi_parse(const uint8_t *d, size_t n, struct sns_qmi *q)
{
	struct qmi_msg m;
	uint16_t l;
	const uint8_t *t;

	memset(q, 0, sizeof(*q));
	if (n > 0xffff + 7)
		n = 0xffff + 7;
	if (qmi_parse(d, (uint32_t)n, &m))
		return -1;
	q->type = m.type;
	q->txn = m.txn;
	q->msg_id = m.id;
	if (m.type == QMI_RESPONSE) {
		t = qmi_get(&m, 0x02, &l);
		if (!t || l < 4)
			return -1;
		memcpy(&q->result, t, 2);
		memcpy(&q->error, t + 2, 2);
		t = qmi_get(&m, 0x10, &l);
		if (t && l >= 8) {
			memcpy(&q->client_id, t, 8);
			q->has_client_id = 1;
		}
		return 0;
	}
	if (m.type == QMI_INDICATION) {
		t = qmi_get(&m, 0x01, &l);
		if (t && l >= 8) {
			memcpy(&q->client_id, t, 8);
			q->has_client_id = 1;
		}
		t = qmi_get(&m, 0x02, &l);
		if (!t || l < 2)
			return -1;
		uint16_t pl;
		memcpy(&pl, t, 2);
		if (pl > l - 2)
			return -1;
		q->payload = t + 2;
		q->plen = pl;
		return 0;
	}
	return 0;
}

static int dec_suid(const uint8_t *p, size_t n, struct sns_suid *s)
{
	struct pb_rd r;
	struct pb_field f;
	int rc, got = 0;
	pb_rd_init(&r, p, n);
	s->low = s->high = 0;
	while ((rc = pb_next(&r, &f)) == 1) {
		if (f.num == 1 && f.wt == PB_FIXED64) {
			s->low = f.v;
			got |= 1;
		} else if (f.num == 2 && f.wt == PB_FIXED64) {
			s->high = f.v;
			got |= 2;
		}
	}
	return rc == 0 && got == 3 ? 0 : -1;
}

int sns_parse_event_msg(const uint8_t *p, size_t n, sns_event_cb cb, void *ctx)
{
	struct pb_rd r, e;
	struct pb_field f, g;
	struct sns_suid suid = { 0, 0 };
	int rc, count = 0;

	/* The SUID comes first on the wire, but do not rely on it: find it first. */
	pb_rd_init(&r, p, n);
	while ((rc = pb_next(&r, &f)) == 1)
		if (f.num == 1 && f.wt == PB_LEN && dec_suid(f.data, f.len, &suid))
			return -1;
	if (rc < 0)
		return -1;
	pb_rd_init(&r, p, n);
	while ((rc = pb_next(&r, &f)) == 1) {
		if (f.num != 2 || f.wt != PB_LEN)
			continue;
		uint32_t msg_id = 0;
		uint64_t ts = 0;
		const uint8_t *pl = NULL;
		size_t plen = 0;
		pb_rd_init(&e, f.data, f.len);
		while ((rc = pb_next(&e, &g)) == 1) {
			if (g.num == 1 && g.wt == PB_FIXED32)
				msg_id = (uint32_t)g.v;
			else if (g.num == 2 && g.wt == PB_FIXED64)
				ts = g.v;
			else if (g.num == 3 && g.wt == PB_LEN) {
				pl = g.data;
				plen = g.len;
			}
		}
		if (rc < 0)
			return -1;
		if (cb)
			cb(ctx, &suid, msg_id, ts, pl ? pl : (const uint8_t *)"", plen);
		count++;
	}
	return rc < 0 ? -1 : count;
}

static void copy_str(char *dst, size_t cap, const struct pb_field *f)
{
	size_t l = f->len < cap - 1 ? f->len : cap - 1;
	memcpy(dst, f->data, l);
	dst[l] = 0;
}

int sns_parse_suid_event(const uint8_t *p, size_t n, char *data_type, size_t dtlen, struct sns_suid *suids, int max)
{
	struct pb_rd r;
	struct pb_field f;
	int rc, count = 0;

	if (dtlen)
		data_type[0] = 0;
	pb_rd_init(&r, p, n);
	while ((rc = pb_next(&r, &f)) == 1) {
		if (f.num == 1 && f.wt == PB_LEN && dtlen)
			copy_str(data_type, dtlen, &f);
		else if (f.num == 2 && f.wt == PB_LEN && count < max) {
			if (dec_suid(f.data, f.len, &suids[count]) == 0)
				count++;
		}
	}
	return rc < 0 ? -1 : count;
}

/* One sns_std_attr_value: the first scalar of each kind, plus all floats (for rates). */
struct attr_val {
	char str[64];
	int has_str, has_flt, has_int, has_bool;
	float flt[16];
	int nflt;
	int64_t sint;
	int boolean;
};

static int dec_attr_value(const uint8_t *p, size_t n, struct attr_val *v, int depth)
{
	struct pb_rd r, d;
	struct pb_field f, g;
	int rc;

	if (depth > 3)
		return 0;
	pb_rd_init(&r, p, n);
	while ((rc = pb_next(&r, &f)) == 1) {
		if (f.num != 1 || f.wt != PB_LEN)
			continue;
		pb_rd_init(&d, f.data, f.len);
		while ((rc = pb_next(&d, &g)) == 1) {
			if (g.num == 1 && g.wt == PB_LEN)
				dec_attr_value(g.data, g.len, v, depth + 1);	/* subtype, e.g. a range [min, max] */
			else if (g.num == 2 && g.wt == PB_LEN && !v->has_str) {
				copy_str(v->str, sizeof(v->str), &g);
				v->has_str = 1;
			} else if (g.num == 3 && (g.wt == PB_FIXED32 || g.wt == PB_LEN)) {
				v->nflt = pb_floats(&g, v->flt, v->nflt, 16);
				v->has_flt = 1;
			} else if (g.num == 4 && g.wt == PB_VARINT && !v->has_int) {
				v->sint = pb_zigzag(g.v);
				v->has_int = 1;
			} else if (g.num == 5 && g.wt == PB_VARINT && !v->has_bool) {
				v->boolean = g.v != 0;
				v->has_bool = 1;
			}
		}
		if (rc < 0)
			return -1;
	}
	return rc;
}

int sns_parse_attr_event(const uint8_t *p, size_t n, struct sns_attrs *a)
{
	struct pb_rd r, at;
	struct pb_field f, g;
	int rc;

	memset(a, 0, sizeof(*a));
	a->hw_id = -1;
	pb_rd_init(&r, p, n);
	while ((rc = pb_next(&r, &f)) == 1) {
		if (f.num != 1 || f.wt != PB_LEN)
			continue;
		int64_t id = -1;
		struct attr_val v;
		memset(&v, 0, sizeof(v));
		pb_rd_init(&at, f.data, f.len);
		while ((rc = pb_next(&at, &g)) == 1) {
			if (g.num == 1 && g.wt == PB_VARINT)
				id = (int64_t)(int32_t)g.v;
			else if (g.num == 2 && g.wt == PB_LEN)
				dec_attr_value(g.data, g.len, &v, 0);
		}
		if (rc < 0)
			return -1;
		a->nattrs++;
		switch (id) {
		case SNS_ATTR_NAME:
			if (v.has_str)
				memcpy(a->name, v.str, sizeof(a->name));
			break;
		case SNS_ATTR_VENDOR:
			if (v.has_str)
				memcpy(a->vendor, v.str, sizeof(a->vendor));
			break;
		case SNS_ATTR_TYPE:
			if (v.has_str)
				memcpy(a->type, v.str, sizeof(a->type));
			break;
		case SNS_ATTR_AVAILABLE:
			a->available = v.has_bool ? v.boolean : 0;
			break;
		case SNS_ATTR_VERSION:
			a->version = v.sint;
			break;
		case SNS_ATTR_RATES:
			memcpy(a->rates, v.flt, sizeof(a->rates));
			a->nrates = v.nflt;
			break;
		case SNS_ATTR_RANGES:
			if (v.nflt >= 2) {
				a->range_min = v.flt[0];
				a->range_max = v.flt[1];
			}
			break;
		case SNS_ATTR_RESOLUTIONS:
			if (v.nflt >= 1)
				a->resolution = v.flt[0];
			break;
		case SNS_ATTR_STREAM_TYPE:
			a->stream_type = (int)v.sint;
			a->have_stream_type = v.has_int;
			break;
		case SNS_ATTR_PHYSICAL_SENSOR:
			a->physical = v.has_bool ? v.boolean : 0;
			break;
		case SNS_ATTR_HW_ID:
			a->hw_id = v.has_int ? (int)v.sint : -1;
			break;
		}
	}
	return rc < 0 ? -1 : 0;
}

int sns_parse_sensor_event(const uint8_t *p, size_t n, float *data, int max, int *status)
{
	struct pb_rd r;
	struct pb_field f;
	int rc, count = 0;

	*status = 0;
	pb_rd_init(&r, p, n);
	while ((rc = pb_next(&r, &f)) == 1) {
		if (f.num == 1 && (f.wt == PB_FIXED32 || f.wt == PB_LEN))
			count = pb_floats(&f, data, count, max);
		else if (f.num == 1)
			return -1;	/* not a sns_std_sensor_event (e.g. a proximity event) */
		else if (f.num == 2 && f.wt == PB_VARINT)
			*status = (int)f.v;
	}
	return rc < 0 ? -1 : count;
}

int sns_parse_prox_event(const uint8_t *p, size_t n, int *near, uint32_t *raw, int *status)
{
	struct pb_rd r;
	struct pb_field f;
	int rc, got = 0;

	*near = 0;
	*raw = 0;
	*status = 0;
	pb_rd_init(&r, p, n);
	while ((rc = pb_next(&r, &f)) == 1) {
		if (f.num == 1 && f.wt == PB_VARINT) {
			*near = f.v != 0;
			got = 1;
		} else if (f.num == 1)
			return -1;
		else if (f.num == 2 && f.wt == PB_VARINT)
			*raw = (uint32_t)f.v;
		else if (f.num == 3 && f.wt == PB_VARINT)
			*status = (int)f.v;
	}
	/* proto2 omits nothing that is required, but an all-default event may arrive as raw_adc only */
	return rc == 0 && (got || n > 0) ? 0 : -1;
}

int sns_parse_error_event(const uint8_t *p, size_t n)
{
	struct pb_rd r;
	struct pb_field f;
	pb_rd_init(&r, p, n);
	while (pb_next(&r, &f) == 1)
		if (f.num == 1 && f.wt == PB_VARINT)
			return (int)f.v;
	return -1;
}
