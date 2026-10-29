/* sns1: see sns_link.h. */
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include "sns_link.h"

static const struct sns_link_ops *link_ops;
static uint8_t frame[8 + RQRTR_MAX];

void sns_link_set(const struct sns_link_ops *ops)
{
	link_ops = ops;
}

int rqrtr_send(int fd, uint32_t node, uint32_t port, const void *data, size_t len)
{
	if (link_ops)
		return link_ops->send(node, port, data, len);
	if (len > RQRTR_MAX)
		return -1;
	memcpy(frame, &node, 4);
	memcpy(frame + 4, &port, 4);
	memcpy(frame + 8, data, len);
	return write(fd, frame, 8 + len) == (ssize_t)(8 + len) ? 0 : -1;
}

int rqrtr_recv(int fd, uint32_t *node, uint32_t *port, uint8_t *data, size_t cap, int timeout_ms)
{
	if (link_ops)
		return link_ops->recv(node, port, data, cap, timeout_ms);
	struct pollfd p = { fd, POLLIN, 0 };
	int r = poll(&p, 1, timeout_ms);
	if (r < 0)
		return errno == EINTR ? 0 : -1;
	if (r == 0)
		return 0;
	ssize_t n = read(fd, frame, sizeof(frame));
	if (n < 0)
		return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
	if (n < 8)
		return -1;	/* runt, or the fake DSP's end of the socketpair closed */
	size_t len = (size_t)n - 8;
	memcpy(node, frame, 4);
	memcpy(port, frame + 4, 4);
	if (len > cap)
		len = cap;
	memcpy(data, frame + 8, len);
	return (int)len;
}
