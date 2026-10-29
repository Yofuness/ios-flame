/*
 * bt_lext (bt7): legacy LE -> extended LE translation for btbridge's H4 shim (docs/features/bluetooth.md, bt6).
 * Our WCN3990 answers 0x200b/0x200c with 0x0c Command Disallowed: bluetoothd sends 0x204a (LE Read Maximum
 * Advertising Data Length, an extended-set command) at init, and a controller that has seen an extended LE command
 * refuses the legacy set. bluetoothd itself only uses the legacy set on this chip, so the shim translates:
 *   0x200b Set_Scan_Parameters  -> 0x2041 Set_Extended_Scan_Parameters (1M PHY), answer op rewritten
 *   0x200c Set_Scan_Enable      -> 0x2042 Set_Extended_Scan_Enable (duration 0, period 0), answer op rewritten
 *   0x200d Create_Connection    -> 0x2043 Extended_Create_Connection (1M PHY), Command Status op rewritten
 *   0x2006 Set_Advertising_Parameters -> 0x2036 (set 0, legacy PDU properties) [+ 0x2035 set random address]
 *   0x2008 / 0x2009 Set_Advertising_Data / Scan_Response_Data -> 0x2037 / 0x2038 (set 0, complete data)
 *   0x200a Set_Advertising_Enable -> 0x2039 (set 0)
 *   0x2005 Set_Random_Address: sent as is; when set 0 exists, 0x2035 for set 0 follows (status only logged)
 *   events: LE Extended Advertising Report (0x3e sub 0x0d) -> one LE Advertising Report (sub 0x02) per legacy-PDU
 *   report, plen 12 + data (their review F1); LE Advertising Set Terminated (0x12) / Scan Timeout (0x11) dropped;
 *   0x2001 LE event mask gets bit 12 (F7).
 * Advertising commands run as a chain: one command in flight, the next after the chip's answer, then one legacy
 * Command Complete with the first failing status. Single-threaded here (btbridge's pump), so their F5 (deferred send
 * to a sleeping controller) and F8 (locking) do not apply. Off: btbridge -L.
 *
 * Ported from iOSPlus10 patches/xnu/0338-negroni-bt-le-legacy-to-extended.patch (bsd/dev/negroni_bt_shim.c:
 * mkcmd, mk_ext_adv_param, chain_*, lext_tx, lext_report, lext_rx; review fixes F1-F4, F6, F7), adapted to userland:
 *
 *   MIT License
 *   Copyright (c) 2026 iOSPlus10 contributors
 *   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 *   documentation files (the "Software"), to deal in the Software without restriction, including without limitation
 *   the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
 *   to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *   The above copyright notice and this permission notice shall be included in all copies or substantial portions of
 *   the Software.
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
 *   THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 *   CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *   IN THE SOFTWARE.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "bt_lext.h"

#define OP_RESET		0x0c03u
#define OP_LE_EVENT_MASK	0x2001u
#define OP_LE_RAND_ADDR		0x2005u
#define OP_LE_ADV_PARAM		0x2006u
#define OP_LE_ADV_DATA		0x2008u
#define OP_LE_SCAN_RSP		0x2009u
#define OP_LE_ADV_ENABLE	0x200au
#define OP_LE_SCAN_PARAM	0x200bu
#define OP_LE_SCAN_ENABLE	0x200cu
#define OP_LE_CREATE_CONN	0x200du
#define OP_LE_SET_RAND		0x2035u
#define OP_LE_EXT_ADV_PARAM	0x2036u
#define OP_LE_EXT_ADV_DATA	0x2037u
#define OP_LE_EXT_SCAN_RSP	0x2038u
#define OP_LE_EXT_ADV_EN	0x2039u
#define OP_LE_EXT_SCAN_PARAM	0x2041u
#define OP_LE_EXT_SCAN_EN	0x2042u
#define OP_LE_EXT_CONN		0x2043u
#define MIN(a, b)		((a) < (b) ? (a) : (b))

static struct {
	bool adv_set, rnd_known;
	uint8_t rnd[6];
	unsigned n_xlat, n_rep_in, n_rep_out, n_rep_drop, n_ev_drop, n_rep_logged, n_chain_stale;
	uint8_t adv_type;		/* legacy Advertising_Type of the last accepted 0x2006 */
	uint8_t pend_scan_p, pend_scan_e, pend_conn;
	struct {
		bool active;
		uint16_t legacy;
		uint8_t n, i, crit;	/* crit: bit i = a failure of step i is the legacy status */
		uint8_t want_type;
		uint64_t at_us;
		uint8_t len[3];
		uint8_t cmd[3][48];
	} ch;
} s;

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static void answer(uint16_t op, uint8_t status)
{
	const uint8_t ev[7] = { 0x04, 0x0e, 4, 1, (uint8_t)op, (uint8_t)(op >> 8), status };
	lext_to_host(ev, sizeof(ev));
}

void lext_reset(void)
{
	s.adv_set = s.rnd_known = false;
	s.adv_type = 0;
	s.pend_scan_p = s.pend_scan_e = s.pend_conn = 0;
	memset(&s.ch, 0, sizeof(s.ch));
}

static uint8_t mkcmd(uint8_t *b, uint16_t op, const uint8_t *par, uint8_t plen)
{
	b[0] = 0x01;
	b[1] = (uint8_t)op;
	b[2] = (uint8_t)(op >> 8);
	b[3] = plen;
	memcpy(b + 4, par, plen);
	return (uint8_t)(4u + plen);
}

/* 0x2036 for set 0 from the 15 legacy parameter bytes (NULL: legacy defaults, ADV_IND 1.28 s, public) */
static uint8_t mk_ext_adv_param(uint8_t *b, const uint8_t *lp)
{
	static const uint8_t def[15] = { 0x00, 0x08, 0x00, 0x08, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0x07, 0x00 };
	const uint8_t *l = lp ? lp : def;
	uint16_t props;
	switch (l[4]) {
	case 0x01: props = 0x001d; break;	/* ADV_DIRECT_IND high duty */
	case 0x02: props = 0x0012; break;	/* ADV_SCAN_IND */
	case 0x03: props = 0x0010; break;	/* ADV_NONCONN_IND */
	case 0x04: props = 0x0015; break;	/* ADV_DIRECT_IND low duty */
	default:   props = 0x0013; break;	/* ADV_IND: legacy, connectable, scannable */
	}
	const uint8_t p[25] = {
		0x00, (uint8_t)props, (uint8_t)(props >> 8),
		l[0], l[1], 0x00,			/* primary interval min (3 bytes) */
		l[2], l[3], 0x00,			/* primary interval max */
		l[13], l[5], l[6],			/* channel map, own address type, peer address type */
		l[7], l[8], l[9], l[10], l[11], l[12],	/* peer address */
		l[14], 0x7f, 0x01, 0x00, 0x01, 0x00, 0x00 /* filter, TX power any, 1M, skip 0, 1M, SID 0, no notify */
	};
	return mkcmd(b, OP_LE_EXT_ADV_PARAM, p, sizeof(p));
}

static void chain_answer(uint8_t status)
{
	s.ch.active = false;
	if (status || s.n_xlat < 16)
		printf("lext: legacy %#06x answered with status %#x after %u extended command(s)\n", s.ch.legacy, status,
		       s.ch.i + 1u);
	answer(s.ch.legacy, status);
}

/* send step s.ch.i; a failed UART send answers the legacy command with 0x03 Hardware Failure */
static void chain_send(void)
{
	s.ch.at_us = lext_us();
	if (lext_to_chip(s.ch.cmd[s.ch.i], s.ch.len[s.ch.i]))
		chain_answer(0x03);
}

static int chain_start(uint16_t legacy, uint8_t crit)
{
	s.ch.active = true;
	s.ch.legacy = legacy;
	s.ch.i = 0;
	s.ch.crit = crit;
	s.n_xlat++;
	printf("lext: legacy %#06x -> %u extended command(s), first %#06x\n", legacy, s.ch.n, le16(s.ch.cmd[0] + 1));
	chain_send();
	return 1;
}

/* the chip answered a command; 1 = it was a chain step (the event is not delivered) */
static int chain_rx(uint16_t op, uint8_t status)
{
	if (!s.ch.active || s.ch.len[s.ch.i] < 4 || op != le16(s.ch.cmd[s.ch.i] + 1))
		return 0;
	if (op == OP_LE_EXT_ADV_PARAM && !status) {
		s.adv_set = true;		/* review F2: set 0 exists only after the chip took 0x2036 */
		s.adv_type = s.ch.want_type;
	}
	if (status) {
		if (s.ch.crit & (1u << s.ch.i)) {
			chain_answer(status);
			return 1;
		}
		printf("lext: %#06x (step %u of legacy %#06x) status %#x: only logged\n", op, s.ch.i + 1u, s.ch.legacy,
		       status);
	}
	if (s.ch.i + 1u < s.ch.n) {
		s.ch.i++;
		chain_send();
		return 1;
	}
	chain_answer(0);
	return 1;
}

static void send_or_fail(uint16_t legacy, uint16_t ext, const uint8_t *p, uint8_t n, uint8_t *pend)
{
	uint8_t b[48];
	(*pend)++;
	s.n_xlat++;
	if (s.n_xlat <= 16)
		printf("lext: legacy %#06x -> %#06x\n", legacy, ext);
	if (!lext_to_chip(b, mkcmd(b, ext, p, n)))
		return;
	(*pend)--;
	if (legacy == OP_LE_CREATE_CONN) {
		const uint8_t st[7] = { 0x04, 0x0f, 4, 0x03, 1, (uint8_t)legacy, (uint8_t)(legacy >> 8) };
		lext_to_host(st, sizeof(st));
	} else {
		answer(legacy, 0x03);
	}
}

int lext_tx(uint8_t *pkt, unsigned len)
{
	if (len < 4 || len < 4u + pkt[3])
		return 0;
	uint16_t op = le16(pkt + 1);
	uint8_t plen = pkt[3];
	const uint8_t *l = pkt + 4;
	if (op == OP_RESET) {
		lext_reset();
		return 0;
	}
	if (op >> 10 != 0x08)
		return 0;
	if (op == OP_LE_EVENT_MASK && plen >= 8 && !(pkt[5] & 0x10u)) {
		/* review F7: extended reports need bit 12 */
		printf("lext: LE event mask byte 1 %#x -> %#x (Extended Advertising Report on)\n", pkt[5], pkt[5] | 0x10u);
		pkt[5] |= 0x10u;
		return 0;
	}
	bool adv = op == OP_LE_ADV_PARAM || op == OP_LE_ADV_DATA || op == OP_LE_SCAN_RSP || op == OP_LE_ADV_ENABLE ||
	    (op == OP_LE_RAND_ADDR && s.adv_set);
	if (adv && s.ch.active && lext_us() - s.ch.at_us > 2000000u) {
		/* review F6: a step answer never came */
		s.n_chain_stale++;
		printf("lext: chain of legacy %#06x had no answer for step %u in 2 s: dropped (#%u)\n", s.ch.legacy,
		       s.ch.i + 1u, s.n_chain_stale);
		s.ch.active = false;
	}
	if (adv && s.ch.active) {
		printf("lext: %#06x while legacy %#06x is still running: answered with 0x0c Command Disallowed\n", op,
		       s.ch.legacy);
		answer(op, 0x0c);
		return 1;
	}
	switch (op) {
	case OP_LE_SCAN_PARAM:
		if (plen >= 7) {
			/* own type, filter, PHYs 1M, [type, interval, window] */
			const uint8_t p[8] = { l[5], l[6], 0x01, l[0], l[1], l[2], l[3], l[4] };
			send_or_fail(op, OP_LE_EXT_SCAN_PARAM, p, sizeof(p), &s.pend_scan_p);
			return 1;
		}
		break;
	case OP_LE_SCAN_ENABLE:
		if (plen >= 2) {
			const uint8_t p[6] = { l[0], l[1], 0, 0, 0, 0 };
			send_or_fail(op, OP_LE_EXT_SCAN_EN, p, sizeof(p), &s.pend_scan_e);
			return 1;
		}
		break;
	case OP_LE_CREATE_CONN:
		if (plen >= 25) {
			/* filter, own type, peer type, peer address, PHYs 1M, scan interval/window, conn interval min/max,
			 * latency, supervision timeout, CE length min/max */
			const uint8_t p[26] = { l[4], l[12], l[5], l[6], l[7], l[8], l[9], l[10], l[11], 0x01,
				l[0], l[1], l[2], l[3], l[13], l[14], l[15], l[16], l[17], l[18], l[19], l[20],
				l[21], l[22], l[23], l[24] };
			send_or_fail(op, OP_LE_EXT_CONN, p, sizeof(p), &s.pend_conn);
			return 1;
		}
		break;
	case OP_LE_ADV_PARAM:
		if (plen < 15)
			break;
		s.ch.n = 1;
		s.ch.len[0] = mk_ext_adv_param(s.ch.cmd[0], l);
		s.ch.want_type = l[4];
		if ((l[5] == 0x01 || l[5] == 0x03) && s.rnd_known) {
			const uint8_t p[7] = { 0x00, s.rnd[0], s.rnd[1], s.rnd[2], s.rnd[3], s.rnd[4], s.rnd[5] };
			s.ch.len[1] = mkcmd(s.ch.cmd[1], OP_LE_SET_RAND, p, sizeof(p));
			s.ch.n = 2;
		}
		return chain_start(op, 0x01);
	case OP_LE_ADV_DATA:
	case OP_LE_SCAN_RSP:
		if (plen < 1)
			break;
		if (s.adv_set && ((op == OP_LE_ADV_DATA && (s.adv_type == 0x01 || s.adv_type == 0x04)) ||
		    (op == OP_LE_SCAN_RSP && l[0] && s.adv_type != 0x00 && s.adv_type != 0x02))) {
			/* review F4: legacy takes and ignores data the PDU type cannot carry; 0x2037/0x2038 answer 0x12 */
			printf("lext: %#06x for legacy type %u: the PDU carries no such data; local status 0\n", op,
			       s.adv_type);
			answer(op, 0);
			return 1;
		} else {
			uint8_t dl = (uint8_t)MIN(MIN(l[0], 31u), plen - 1u);
			uint8_t p[4 + 31] = { 0x00, 0x03, 0x01, dl };	/* set 0, complete data, no fragmentation pref */
			memcpy(p + 4, l + 1, dl);
			uint8_t k = 0;
			if (!s.adv_set) {
				s.ch.len[k++] = mk_ext_adv_param(s.ch.cmd[0], NULL);
				s.ch.want_type = 0x00;
			}
			s.ch.len[k] = mkcmd(s.ch.cmd[k], op == OP_LE_ADV_DATA ? OP_LE_EXT_ADV_DATA : OP_LE_EXT_SCAN_RSP, p,
			    (uint8_t)(4u + dl));
			s.ch.n = (uint8_t)(k + 1u);
			return chain_start(op, 0x03);
		}
	case OP_LE_ADV_ENABLE:
		if (plen < 1)
			break;
		if (!s.adv_set && !l[0]) {
			answer(op, 0);		/* nothing advertises: a legacy disable succeeds */
			return 1;
		} else {
			/* review F3: high duty directed needs a duration (1.28 s), as the legacy timeout */
			bool hd = s.adv_set && s.adv_type == 0x01 && l[0];
			const uint8_t p[6] = { l[0] ? 0x01 : 0x00, 0x01, 0x00, hd ? 0x80 : 0x00, 0x00, 0x00 };
			uint8_t k = 0;
			if (!s.adv_set) {
				s.ch.len[k++] = mk_ext_adv_param(s.ch.cmd[0], NULL);
				s.ch.want_type = 0x00;
			}
			s.ch.len[k] = mkcmd(s.ch.cmd[k], OP_LE_EXT_ADV_EN, p, sizeof(p));
			s.ch.n = (uint8_t)(k + 1u);
			return chain_start(op, 0x03);
		}
	case OP_LE_RAND_ADDR:
		if (plen < 6)
			break;
		memcpy(s.rnd, l, 6);
		s.rnd_known = true;
		if (!s.adv_set)
			return 0;
		{
			const uint8_t p[7] = { 0x00, l[0], l[1], l[2], l[3], l[4], l[5] };
			s.ch.len[0] = mkcmd(s.ch.cmd[0], OP_LE_RAND_ADDR, l, 6);
			s.ch.len[1] = mkcmd(s.ch.cmd[1], OP_LE_SET_RAND, p, sizeof(p));
			s.ch.n = 2;
			return chain_start(op, 0x01);
		}
	default:
		break;
	}
	return 0;
}

/* LE Extended Advertising Report -> LE Advertising Report(s); the original is always dropped */
static int lext_report(const uint8_t *pkt, unsigned len)
{
	unsigned end = MIN(len, 3u + pkt[2]);
	unsigned at = 5;			/* 04 3e plen 0d num */
	uint8_t num = len > 4 ? pkt[4] : 0;
	for (uint8_t r = 0; r < num && at + 24u <= end; r++) {
		const uint8_t *e = pkt + at;
		uint16_t et = le16(e);
		uint8_t dl = e[23];
		if (at + 24u + dl > end)
			break;
		at += 24u + dl;
		s.n_rep_in++;
		if (!(et & 0x10u) || dl > 31 || e[2] == 0xff) {
			s.n_rep_drop++;		/* extended PDU (a legacy scan does not see it) or anonymous */
			continue;
		}
		uint8_t lt;
		if (et & 0x08u)
			lt = 0x04;		/* SCAN_RSP */
		else if (et & 0x04u)
			lt = 0x01;		/* ADV_DIRECT_IND */
		else if ((et & 0x03u) == 0x03u)
			lt = 0x00;		/* ADV_IND */
		else if (et & 0x02u)
			lt = 0x02;		/* ADV_SCAN_IND */
		else
			lt = 0x03;		/* ADV_NONCONN_IND */
		/* review F1: plen = sub, num, type, addr type, addr(6), len, data, RSSI = 12 + dl */
		uint8_t ev[3 + 12 + 31] = { 0x04, 0x3e, (uint8_t)(12u + dl), 0x02, 1, lt, e[2] };
		memcpy(ev + 7, e + 3, 6);
		ev[13] = dl;
		memcpy(ev + 14, e + 24, dl);
		ev[14 + dl] = e[13];		/* RSSI */
		s.n_rep_out++;
		lext_to_host(ev, 15u + dl);
	}
	if (s.n_rep_in && s.n_rep_in != s.n_rep_logged && (!s.n_rep_logged || s.n_rep_in - s.n_rep_logged >= 1024u)) {
		s.n_rep_logged = s.n_rep_in;
		printf("lext: extended advertising reports %u in, %u legacy reports out, %u dropped (extended PDU)\n",
		       s.n_rep_in, s.n_rep_out, s.n_rep_drop);
	}
	return 1;
}

int lext_rx(uint8_t *pkt, unsigned len)
{
	if (len < 4 || pkt[0] != 0x04)
		return 0;
	if (pkt[1] == 0x3e) {
		if (pkt[3] == 0x0d)
			return lext_report(pkt, len);
		if (pkt[3] == 0x12 || pkt[3] == 0x11) {
			if (s.n_ev_drop++ < 8)
				printf("lext: LE Meta sub %#x (%s) not delivered (bluetoothd runs legacy adv/scan)\n", pkt[3],
				       pkt[3] == 0x12 ? "Advertising Set Terminated" : "Scan Timeout");
			return 1;
		}
		return 0;
	}
	bool cc = pkt[1] == 0x0e && pkt[2] >= 4 && len >= 7;
	bool cs = pkt[1] == 0x0f && pkt[2] >= 4 && len >= 7;
	if (!cc && !cs)
		return 0;
	uint8_t *opp = cc ? pkt + 4 : pkt + 5;
	uint16_t op = le16(opp);
	uint8_t st = cc ? pkt[6] : pkt[3];
	if (chain_rx(op, st))
		return 1;
	uint16_t to = 0;
	if (op == OP_LE_EXT_SCAN_PARAM && s.pend_scan_p) {
		s.pend_scan_p--;
		to = OP_LE_SCAN_PARAM;
	} else if (op == OP_LE_EXT_SCAN_EN && s.pend_scan_e) {
		s.pend_scan_e--;
		to = OP_LE_SCAN_ENABLE;
	} else if (op == OP_LE_EXT_CONN && s.pend_conn) {
		s.pend_conn--;
		to = OP_LE_CREATE_CONN;
	}
	if (to) {
		opp[0] = (uint8_t)to;
		opp[1] = (uint8_t)(to >> 8);
		if (st || s.n_xlat < 16)
			printf("lext: %#06x answer (status %#x) delivered as legacy %#06x\n", op, st, to);
	}
	return 0;
}
