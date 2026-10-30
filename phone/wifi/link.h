#ifndef LINK_H
#define LINK_H
#include <stdint.h>

#define QRTR_NODE_APPS		1
#define QRTR_NODE_MODEM		0
#define QRTR_NODE_BCAST		0xffffffffu
#define QRTR_PORT_CTRL		0xfffffffeu

enum {
	QRTR_TYPE_DATA = 1, QRTR_TYPE_HELLO, QRTR_TYPE_BYE, QRTR_TYPE_NEW_SERVER,
	QRTR_TYPE_DEL_SERVER, QRTR_TYPE_DEL_CLIENT, QRTR_TYPE_RESUME_TX, QRTR_TYPE_EXIT,
	QRTR_TYPE_PING, QRTR_TYPE_NEW_LOOKUP, QRTR_TYPE_DEL_LOOKUP,
};

struct qrtr_hdr {
	uint32_t version, type, src_node, src_port, confirm_rx, size, dst_node, dst_port;
};

typedef void (*qrtr_cb_t)(const struct qrtr_hdr *h, const uint8_t *data, uint32_t len);

extern int verbose_link;
extern unsigned link_stats[4];	/* remote RX_DONE, RX_DONE_W_REUSE, our intent requests */
extern unsigned link_traffic;	/* bumped on any RX/TX work: link_poll naps short while it moves (O12) */
extern void (*link_idle)(void);	/* called every poll round */
uint64_t link_ms(void);		/* since the first call */
int link_up(qrtr_cb_t cb, unsigned timeout_ms);
int link_poll(unsigned ms);		/* handle what arrived; for ms (0 = just once) */
int link_send(const void *data, uint32_t len);
int qrtr_send(uint32_t type, uint32_t src_port, uint32_t dst_node, uint32_t dst_port,
	      const void *payload, uint32_t len);
int qrtr_new_server(uint32_t service, uint32_t version, uint32_t instance, uint32_t port);
int qrtr_resume_tx(const struct qrtr_hdr *h);

#endif
