/*
 * Host test of btbridge's legacy -> extended LE translation (phone/wifi/bt_lext.c, bt7). Ported from iOSPlus10
 * tools/bt-shim-test/bt_shim_test.c (MIT, Copyright (c) 2026 iOSPlus10 contributors; full notice in bt_lext.c):
 * their 0338 cases unchanged, plus a 31-byte report (F1) and a two-report event. Run: tools/bt-shim-test/run.sh
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "bt_lext.h"

static uint8_t sent[8][64], inj[8][300];
static uint32_t sentn[8], injn[8];
static int nsent, ninj, fails;
static uint64_t now;

uint64_t lext_us(void) { return now; }
void lext_to_host(const uint8_t *p, unsigned n) { if (ninj < 8) { memcpy(inj[ninj], p, n); injn[ninj] = n; } ninj++; }
int lext_to_chip(const uint8_t *p, unsigned n) { if (nsent < 8) { memcpy(sent[nsent], p, n); sentn[nsent] = n; } nsent++; return 0; }

static void check(const char *what, const uint8_t *got, uint32_t gn, const uint8_t *exp, uint32_t en)
{
	if (gn != en || memcmp(got, exp, en)) {
		fails++;
		printf("FAIL %s: got", what);
		for (uint32_t i = 0; i < gn; i++) printf(" %02x", got[i]);
		printf("\n     expected");
		for (uint32_t i = 0; i < en; i++) printf(" %02x", exp[i]);
		printf("\n");
	} else {
		printf("ok   %s\n", what);
	}
}

#define B(...) (const uint8_t[]){ __VA_ARGS__ }, sizeof((const uint8_t[]){ __VA_ARGS__ })

/* host command; expect: consumed (1) or passed (0), and the one packet to the chip (or NULL) */
static void tx(const char *what, const uint8_t *p, uint32_t n, int r_exp, const uint8_t *chip, uint32_t cn)
{
	uint8_t b[300];
	memcpy(b, p, n);
	nsent = ninj = 0;
	int r = lext_tx(b, n);
	if (r != r_exp) { fails++; printf("FAIL %s: result %d, expected %d\n", what, r, r_exp); }
	if (chip) {
		if (nsent != 1) { fails++; printf("FAIL %s: %d packets to the chip\n", what, nsent); return; }
		check(what, sent[0], sentn[0], chip, cn);
	} else if (!r) {
		check(what, b, n, p, n);
	}
}

/* chip event; expect: the delivered packet (rewritten in place or injected), and a chip packet */
static void rx(const char *what, const uint8_t *p, uint32_t n, const uint8_t *host, uint32_t hn, const uint8_t *chip, uint32_t cn)
{
	uint8_t b[300];
	memcpy(b, p, n);
	nsent = ninj = 0;
	int r = lext_rx(b, n);
	const uint8_t *got = !r ? b : ninj ? inj[0] : NULL;
	uint32_t gn = !r ? n : ninj ? injn[0] : 0;
	if (host)
		check(what, got ? got : (const uint8_t *)"", gn, host, hn);
	else if (got) { fails++; printf("FAIL %s: delivered to the host, expected nothing\n", what); }
	if (chip) {
		if (nsent != 1) { fails++; printf("FAIL %s: %d packets to the chip\n", what, nsent); return; }
		check(what, sent[0], sentn[0], chip, cn);
	}
}

int main(void)
{
	lext_reset();
	printf("---- 0338 translation\n");
	tx("0x200b -> 0x2041", B(1, 0x0b, 0x20, 7, 0x00, 0xe0, 0x01, 0x30, 0x00, 0x01, 0x00), 1,
	    B(1, 0x41, 0x20, 8, 0x01, 0x00, 0x01, 0x00, 0xe0, 0x01, 0x30, 0x00));
	rx("0x2041 CC -> 0x200b CC", B(4, 0x0e, 4, 1, 0x41, 0x20, 0), B(4, 0x0e, 4, 1, 0x0b, 0x20, 0), NULL, 0);
	tx("0x200c -> 0x2042", B(1, 0x0c, 0x20, 2, 1, 1), 1, B(1, 0x42, 0x20, 6, 1, 1, 0, 0, 0, 0));
	rx("0x2042 CC -> 0x200c CC", B(4, 0x0e, 4, 1, 0x42, 0x20, 0x0c), B(4, 0x0e, 4, 1, 0x0c, 0x20, 0x0c), NULL, 0);
	tx("0x2005 passes (no set yet)", B(1, 0x05, 0x20, 6, 1, 2, 3, 4, 5, 0xc6), 0, NULL, 0);
	rx("0x2005 CC passes", B(4, 0x0e, 4, 1, 0x05, 0x20, 0), B(4, 0x0e, 4, 1, 0x05, 0x20, 0), NULL, 0);
	tx("0x2006 ADV_IND random -> 0x2036", B(1, 0x06, 0x20, 15, 0xa0, 0, 0xa0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 7, 0),
	    1, B(1, 0x36, 0x20, 25, 0, 0x13, 0, 0xa0, 0, 0, 0xa0, 0, 0, 7, 1, 0, 0, 0, 0, 0, 0, 0, 0,
	    0x7f, 1, 0, 1, 0, 0));
	rx("0x2036 CC -> 0x2035 to the chip", B(4, 0x0e, 5, 1, 0x36, 0x20, 0, 5), NULL, 0,
	    B(1, 0x35, 0x20, 7, 0, 1, 2, 3, 4, 5, 0xc6));
	rx("0x2035 CC -> 0x2006 CC", B(4, 0x0e, 4, 1, 0x35, 0x20, 0), B(4, 0x0e, 4, 1, 0x06, 0x20, 0), NULL, 0);
	tx("0x2008 -> 0x2037", B(1, 0x08, 0x20, 32, 3, 2, 1, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	    0, 0, 0, 0, 0, 0, 0, 0), 1, B(1, 0x37, 0x20, 7, 0, 3, 1, 3, 2, 1, 6));
	rx("0x2037 CC -> 0x2008 CC", B(4, 0x0e, 4, 1, 0x37, 0x20, 0), B(4, 0x0e, 4, 1, 0x08, 0x20, 0), NULL, 0);
	tx("0x200a -> 0x2039", B(1, 0x0a, 0x20, 1, 1), 1, B(1, 0x39, 0x20, 6, 1, 1, 0, 0, 0, 0));
	rx("0x2039 error -> 0x200a CC with it", B(4, 0x0e, 4, 1, 0x39, 0x20, 0x12), B(4, 0x0e, 4, 1, 0x0a, 0x20, 0x12),
	    NULL, 0);
	tx("0x2005 with set 0 -> 0x2005 first", B(1, 0x05, 0x20, 6, 9, 8, 7, 6, 5, 0xc4), 1,
	    B(1, 0x05, 0x20, 6, 9, 8, 7, 6, 5, 0xc4));
	rx("0x2005 CC -> 0x2035 to the chip", B(4, 0x0e, 4, 1, 0x05, 0x20, 0), NULL, 0,
	    B(1, 0x35, 0x20, 7, 0, 9, 8, 7, 6, 5, 0xc4));
	rx("0x2035 error only logged -> 0x2005 CC 0", B(4, 0x0e, 4, 1, 0x35, 0x20, 0x0c), B(4, 0x0e, 4, 1, 0x05, 0x20, 0),
	    NULL, 0);
	rx("ext report ADV_IND -> legacy report", B(4, 0x3e, 29, 0x0d, 1, 0x13, 0x00, 0x01, 1, 2, 3, 4, 5, 6, 1, 0, 0xff,
	    0x7f, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 2, 1, 6),
	    B(4, 0x3e, 15, 0x02, 1, 0x00, 0x01, 1, 2, 3, 4, 5, 6, 3, 2, 1, 6, 0xc0), NULL, 0);
	rx("ext report SCAN_RSP to ADV_IND -> type 4", B(4, 0x3e, 26, 0x0d, 1, 0x1b, 0x00, 0x00, 1, 2, 3, 4, 5, 6, 1, 0,
	    0xff, 0x7f, 0xb0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
	    B(4, 0x3e, 12, 0x02, 1, 0x04, 0x00, 1, 2, 3, 4, 5, 6, 0, 0xb0), NULL, 0);
	rx("ext report extended PDU -> dropped", B(4, 0x3e, 26, 0x0d, 1, 0x01, 0x00, 0x00, 1, 2, 3, 4, 5, 6, 1, 2,
	    0x01, 0x7f, 0xb0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0), NULL, 0, NULL, 0);
	rx("Advertising Set Terminated -> dropped", B(4, 0x3e, 6, 0x12, 0, 0, 0x40, 0, 0), NULL, 0, NULL, 0);
	tx("0x200d -> 0x2043", B(1, 0x0d, 0x20, 25, 0x60, 0, 0x30, 0, 0, 1, 1, 2, 3, 4, 5, 6, 1, 0x18, 0, 0x28, 0, 0, 0,
	    0x48, 0, 0, 0, 0, 0), 1, B(1, 0x43, 0x20, 26, 0, 1, 1, 1, 2, 3, 4, 5, 6, 1, 0x60, 0, 0x30,
	    0, 0x18, 0, 0x28, 0, 0, 0, 0x48, 0, 0, 0, 0, 0));
	rx("0x2043 CS -> 0x200d CS", B(4, 0x0f, 4, 0, 1, 0x43, 0x20), B(4, 0x0f, 4, 0, 1, 0x0d, 0x20), NULL, 0);
	{
		uint8_t b[] = { 1, 0x01, 0x20, 8, 0x7f, 0x0a, 0x86, 0x3f, 7, 0, 0, 0 };
		(void)lext_tx(b, sizeof(b));
		check("0x2001 byte 1 0x0a -> 0x1a", b + 5, 1, (const uint8_t[]){ 0x1a }, 1);
	}
	tx("0x0c03 resets the set state", B(1, 0x03, 0x0c, 0), 0, NULL, 0);
	tx("0x200a disable with no set -> local 0", B(1, 0x0a, 0x20, 1, 0), 1, NULL, 0);
	check("  its answer", ninj ? inj[0] : (const uint8_t *)"", ninj ? injn[0] : 0, B(4, 0x0e, 4, 1, 0x0a, 0x20, 0));
	tx("0x2009 with no set -> 0x2036 defaults first", B(1, 0x09, 0x20, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0), 1, B(1, 0x36, 0x20, 25, 0, 0x13, 0,
	    0, 8, 0, 0, 8, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x7f, 1, 0, 1, 0, 0));
	rx("0x2036 CC -> 0x2038 to the chip", B(4, 0x0e, 5, 1, 0x36, 0x20, 0, 5), NULL, 0, B(1, 0x38, 0x20, 4, 0, 3, 1, 0));
	rx("0x2038 CC -> 0x2009 CC", B(4, 0x0e, 4, 1, 0x38, 0x20, 0), B(4, 0x0e, 4, 1, 0x09, 0x20, 0), NULL, 0);
	printf("---- 0338 review fixes\n");
	tx("reset", B(1, 0x03, 0x0c, 0), 0, NULL, 0);
	tx("0x2006 NONCONN -> 0x2036 props 0x10", B(1, 0x06, 0x20, 15, 0xa0, 0, 0xa0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 7, 0),
	    1, B(1, 0x36, 0x20, 25, 0, 0x10, 0, 0xa0, 0, 0, 0xa0, 0, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	    0x7f, 1, 0, 1, 0, 0));
	rx("F2: 0x2036 error -> 0x2006 CC with it", B(4, 0x0e, 5, 1, 0x36, 0x20, 0x12, 0),
	    B(4, 0x0e, 4, 1, 0x06, 0x20, 0x12), NULL, 0);
	tx("F2: then 0x200a disable -> local 0 (no set)", B(1, 0x0a, 0x20, 1, 0), 1, NULL, 0);
	check("  its answer", ninj ? inj[0] : (const uint8_t *)"", ninj ? injn[0] : 0, B(4, 0x0e, 4, 1, 0x0a, 0x20, 0));
	tx("0x2006 high duty directed public -> props 0x1d", B(1, 0x06, 0x20, 15, 0, 0, 0, 0, 1, 0, 0, 1, 2, 3, 4, 5, 6,
	    7, 0), 1, B(1, 0x36, 0x20, 25, 0, 0x1d, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 1, 2, 3, 4, 5, 6, 0,
	    0x7f, 1, 0, 1, 0, 0));
	rx("0x2036 CC (public: no 0x2035) -> 0x2006 CC", B(4, 0x0e, 5, 1, 0x36, 0x20, 0, 0),
	    B(4, 0x0e, 4, 1, 0x06, 0x20, 0), NULL, 0);
	tx("F4: 0x2008 on directed -> local 0", B(1, 0x08, 0x20, 32, 3, 2, 1, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0), 1, NULL, 0);
	check("  its answer", ninj ? inj[0] : (const uint8_t *)"", ninj ? injn[0] : 0, B(4, 0x0e, 4, 1, 0x08, 0x20, 0));
	tx("F3: 0x200a enable high duty -> duration 0x0080", B(1, 0x0a, 0x20, 1, 1), 1,
	    B(1, 0x39, 0x20, 6, 1, 1, 0, 0x80, 0, 0));
	now += 3000000u;
	tx("F6: answer lost, 3 s later 0x200a disable -> stale chain dropped, sent", B(1, 0x0a, 0x20, 1, 0),
	    1, B(1, 0x39, 0x20, 6, 0, 1, 0, 0, 0, 0));
	rx("0x2039 CC -> 0x200a CC", B(4, 0x0e, 4, 1, 0x39, 0x20, 0), B(4, 0x0e, 4, 1, 0x0a, 0x20, 0), NULL, 0);
	printf("---- btbridge additions\n");
	{
		/* F1: a 31-byte legacy report is plen 43, 46 bytes in all */
		uint8_t e[5 + 24 + 31] = { 4, 0x3e, 2 + 24 + 31, 0x0d, 1, 0x10, 0, 0x01, 1, 2, 3, 4, 5, 6, 1, 0, 0xff, 0x7f, 0xc5 };
		e[5 + 23] = 31;
		for (int i = 0; i < 31; i++) e[5 + 24 + i] = (uint8_t)(0x80 + i);
		uint8_t x[46] = { 4, 0x3e, 43, 0x02, 1, 0x03, 0x01, 1, 2, 3, 4, 5, 6, 31 };
		for (int i = 0; i < 31; i++) x[14 + i] = (uint8_t)(0x80 + i);
		x[45] = 0xc5;
		rx("F1: 31-byte NONCONN report -> plen 43", e, sizeof(e), x, sizeof(x), NULL, 0);
	}
	{
		/* two reports in one event: two legacy events */
		uint8_t b[] = { 4, 0x3e, 2 + 2 * 24 + 1, 0x0d, 2,
			0x13, 0, 0, 1, 2, 3, 4, 5, 6, 1, 0, 0xff, 0x7f, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			0x12, 0, 0, 7, 8, 9, 10, 11, 12, 1, 0, 0xff, 0x7f, 0xb0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xaa };
		nsent = ninj = 0;
		int r = lext_rx(b, sizeof(b));
		if (r != 1 || ninj != 2) { fails++; printf("FAIL two reports: r %d, %d events\n", r, ninj); }
		else {
			check("two reports: first ADV_IND", inj[0], injn[0], B(4, 0x3e, 12, 2, 1, 0, 0, 1, 2, 3, 4, 5, 6, 0, 0xc0));
			check("two reports: second ADV_SCAN_IND", inj[1], injn[1], B(4, 0x3e, 13, 2, 1, 2, 0, 7, 8, 9, 10, 11, 12, 1, 0xaa, 0xb0));
		}
	}
	rx("truncated ext report -> dropped, nothing out", B(4, 0x3e, 40, 0x0d, 1, 0x13, 0), NULL, 0, NULL, 0);
	rx("unrelated CC passes", B(4, 0x0e, 4, 1, 0x4a, 0x20, 0), B(4, 0x0e, 4, 1, 0x4a, 0x20, 0), NULL, 0);
	tx("non-LE command passes", B(1, 0x01, 0x0c, 8, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3f), 0, NULL, 0);
	printf(fails ? "bt lext test: %d FAILED\n" : "bt lext test: all ok\n", fails);
	return fails ? 1 : 0;
}
