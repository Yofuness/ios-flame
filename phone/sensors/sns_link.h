/*
 * sns1: the transport under ios-redfin's sensor.c (which called it rqrtr_send/rqrtr_recv over their kernel
 * /dev/redwood-qrtr). Sunfish has no kernel QRTR: the lpass edge and QRTR live in adspboot, so adspboot installs
 * its send/receive hooks here (sns_link_set). Without hooks the calls use a datagram fd framed as
 * { u32 node; u32 port; u8 data[] } (the host tests' socketpair to a fake DSP).
 */
#ifndef SNS_LINK_H
#define SNS_LINK_H
#include <stdint.h>
#include <stddef.h>

#define RQRTR_MAX 16384		/* adspboot's glink rxbuf; the largest QRTR packet it can hand over */

struct sns_link_ops {
	/* One QMI message to node:port from the client port; 0 or -1. */
	int (*send)(uint32_t node, uint32_t port, const void *data, size_t len);
	/* Wait up to timeout_ms (0: once) for one message to the client port; length, 0 on timeout, -1 on error. */
	int (*recv)(uint32_t *node, uint32_t *port, uint8_t *data, size_t cap, int timeout_ms);
};
void sns_link_set(const struct sns_link_ops *ops);	/* NULL: back to the fd framing */

/* fd is ignored while hooks are set. */
int rqrtr_send(int fd, uint32_t node, uint32_t port, const void *data, size_t len);
int rqrtr_recv(int fd, uint32_t *node, uint32_t *port, uint8_t *data, size_t cap, int timeout_ms);

#endif
