/*
 * bt_lext: legacy LE -> extended LE translation for btbridge's H4 shim (bt7). Ported from iOSPlus10 xnu 0338
 * (negroni_bt_shim.c, MIT; notice in bt_lext.c). No phone code: tools/bt-shim-test runs it on the host.
 * The caller provides the three hooks below.
 */
#ifndef BT_LEXT_H
#define BT_LEXT_H
#include <stdint.h>

int lext_to_chip(const uint8_t *p, unsigned n);	/* one H4 command to the chip; 0 = sent */
void lext_to_host(const uint8_t *p, unsigned n);	/* one H4 event to bluetoothd */
uint64_t lext_us(void);				/* monotonic-enough clock in us (stale chain check) */

void lext_reset(void);				/* new session: forget set 0, the random address, a running chain */
int lext_tx(uint8_t *p, unsigned n);		/* host command; 1 = consumed (sent translated or answered here) */
int lext_rx(uint8_t *p, unsigned n);		/* chip event; 1 = dropped (a chain step or a converted report) */

#endif
