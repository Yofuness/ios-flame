/* GLINK over SMEM with the modem, polled (Linux qcom_glink_native.c + qcom_glink_smem.c). */
#ifndef GLINK_H
#define GLINK_H
#include <stdint.h>
#include <stddef.h>

#define GLINK_CMD_VERSION	0
#define GLINK_CMD_VERSION_ACK	1
#define GLINK_CMD_OPEN		2
#define GLINK_CMD_CLOSE		3
#define GLINK_CMD_OPEN_ACK	4
#define GLINK_CMD_INTENT	5
#define GLINK_CMD_RX_DONE	6
#define GLINK_CMD_RX_INTENT_REQ	7
#define GLINK_CMD_RX_INTENT_REQ_ACK 8
#define GLINK_CMD_TX_DATA	9
#define GLINK_CMD_CLOSE_ACK	11
#define GLINK_CMD_TX_DATA_CONT	12
#define GLINK_CMD_READ_NOTIF	13
#define GLINK_CMD_RX_DONE_W_REUSE 14
#define GLINK_CMD_SIGNALS	15

/* Edge to use (default: SMEM_HOST_MODEM, IPC bit 12); call before glink_init. */
void glink_select(unsigned host, unsigned ipc_bit);
int glink_init(void);
/* Queue one command (8-byte header + data, padded to 8) and ring the modem. */
int glink_send(uint16_t cmd, uint16_t p1, uint32_t p2, const void *data, size_t len);
/* Bytes available from the modem, and copying/consuming them. */
size_t glink_rx_avail(void);
void glink_rx_peek(void *buf, size_t off, size_t len);
void glink_rx_advance(size_t len);

#endif
