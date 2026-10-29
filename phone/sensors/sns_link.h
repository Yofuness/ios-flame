/*
 * sns1: the transport under ios-redfin's sensor.c (which called it rqrtr_send/rqrtr_recv over their kernel
 * /dev/redwood-qrtr). Sunfish has no kernel QRTR: the lpass edge and QRTR live in adspboot, so adspboot installs
 * its send/pump hooks here (sns_link_set). Without hooks the calls use a datagram fd framed as
 * { u32 node; u32 port; u8 data[] } (the host tests' socketpair to a fake DSP).
 *
 * sns2: redfin binds one QRTR socket per sensor (rqrtr.c), so the SNS service's indications for each sensor come
 * back to that sensor's own port. With hooks, the fd argument is the sensor's stream index: its QRTR client port
 * is SNS_LINK_PORT0 + fd, and adspboot hands every DATA packet to sns_link_push by destination port, so each
 * sensor's queue holds only its own messages.
 */
#ifndef SNS_LINK_H
#define SNS_LINK_H
#include <stdint.h>
#include <stddef.h>

#define RQRTR_MAX 16384		/* adspboot's glink rxbuf; the largest QRTR packet it can hand over */
#define SNS_LINK_PORT0	0x40	/* sns2: client port of stream 0; stream i uses 0x40 + i */
#define SNS_LINK_NPORTS	8

struct sns_link_ops {
	/* One QMI message from client port lport to node:port; 0 or -1. */
	int (*send)(uint32_t lport, uint32_t node, uint32_t port, const void *data, size_t len);
	/* Move the transport on once (may call sns_link_push); >0 progressed, 0 idle, -1 error. */
	int (*pump)(void);
};
void sns_link_set(const struct sns_link_ops *ops);	/* NULL: back to the fd framing */
/* sns2: a message for client port lport arrived (adspboot's qrtr_rx); 0 queued, -1 not one of our ports. */
int sns_link_push(uint32_t lport, uint32_t node, uint32_t port, const uint8_t *data, size_t len);
int sns_link_is_port(uint32_t lport);

/* With hooks, fd is the stream index (0 .. SNS_LINK_NPORTS-1). */
int rqrtr_send(int fd, uint32_t node, uint32_t port, const void *data, size_t len);
int rqrtr_recv(int fd, uint32_t *node, uint32_t *port, uint8_t *data, size_t cap, int timeout_ms);

#endif
