/*
 * bt_uart: WCN3990 Bluetooth over the polled SE3 GENI UART (bt_uart.c), shared by btup and btbridge.
 * Call pm_init() first. The first error sets `failed` and prints "btup: MSG: stop".
 */
#ifndef BT_UART_H
#define BT_UART_H
#include <stdint.h>

extern int failed;
extern unsigned rx_overruns, rx_dropped, acl_pkts, acl_bytes;

double now_s(void);
void hex(const char *what, const uint8_t *b, unsigned n);
int fail(const char *msg);

void status(void);			/* read-only register dump */
int uart_loop(void);			/* SE internal loopback test */
int power(void);			/* ldoa10, clocks, pins, UART, QCA power pulses; RX on, flow control on */
int hci_reset(void);
int version(void);			/* EDL patch version request */
int firmware(const char *dir);		/* patch + NVM, HCI reset, drain, Read_BD_ADDR, Read_Local_Version */
int off(void);

int tx(const uint8_t *b, unsigned n);	/* raw bytes to the chip (an H4 packet with its type byte) */
/* next complete H4 packet (02 ACL / 04 event) within ms; copies up to max bytes, returns its length, 0 on timeout */
unsigned pkt(uint8_t *p, unsigned max, unsigned ms);
unsigned event(uint8_t *ev, unsigned max, unsigned ms);	/* next event, ACL skipped */
unsigned cmd(uint16_t op, const uint8_t *p, uint8_t n, uint8_t *ev, unsigned max, unsigned ms);
void decode(const uint8_t *ev, unsigned n);
void drain(unsigned ms);
#endif
