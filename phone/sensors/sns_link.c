/* sns1: see sns_link.h. */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "sns_link.h"

static const struct sns_link_ops *link_ops;
static uint8_t frame[8 + RQRTR_MAX];

/* sns2: one queue per client port (was one queue in adspboot for the single port 0x40). */
#define QLEN 128
static struct q {
	struct { uint8_t *d; uint32_t n, node, port; } m[QLEN];
	int head, n;
	uint64_t drop;
} qs[SNS_LINK_NPORTS];

void sns_link_set(const struct sns_link_ops *ops)
{
	link_ops = ops;
}

int sns_link_is_port(uint32_t lport)
{
	return lport >= SNS_LINK_PORT0 && lport < SNS_LINK_PORT0 + SNS_LINK_NPORTS;
}

int sns_link_push(uint32_t lport, uint32_t node, uint32_t port, const uint8_t *d, size_t n)
{
	if (!sns_link_is_port(lport))
		return -1;
	struct q *q = &qs[lport - SNS_LINK_PORT0];
	uint8_t *c = malloc(n ? n : 1);
	if (!c)
		return -1;
	if (q->n == QLEN) {	/* full: drop the oldest */
		free(q->m[q->head].d);
		q->head = (q->head + 1) % QLEN;
		q->n--;
		if (q->drop++ < 5)
			printf("sns: port %x queue full: oldest dropped\n", lport);
	}
	int i = (q->head + q->n++) % QLEN;
	memcpy(c, d, n);
	q->m[i].d = c;
	q->m[i].n = (uint32_t)n;
	q->m[i].node = node;
	q->m[i].port = port;
	return 0;
}

static long mono_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

int rqrtr_send(int fd, uint32_t node, uint32_t port, const void *data, size_t len)
{
	if (link_ops)
		return fd >= 0 && fd < SNS_LINK_NPORTS ? link_ops->send(SNS_LINK_PORT0 + (uint32_t)fd, node, port, data, len)
						       : -1;
	if (len > RQRTR_MAX)
		return -1;
	memcpy(frame, &node, 4);
	memcpy(frame + 4, &port, 4);
	memcpy(frame + 8, data, len);
	return write(fd, frame, 8 + len) == (ssize_t)(8 + len) ? 0 : -1;
}

/* Take the oldest message of stream fd's queue: length, or 0 when empty. */
static int take(int fd, uint32_t *node, uint32_t *port, uint8_t *data, size_t cap)
{
	struct q *q = &qs[fd];
	if (!q->n)
		return 0;
	int i = q->head;
	size_t n = q->m[i].n < cap ? q->m[i].n : cap;
	memcpy(data, q->m[i].d, n);
	*node = q->m[i].node;
	*port = q->m[i].port;
	free(q->m[i].d);
	q->head = (q->head + 1) % QLEN;
	q->n--;
	return (int)n;
}

int rqrtr_recv(int fd, uint32_t *node, uint32_t *port, uint8_t *data, size_t cap, int timeout_ms)
{
	if (link_ops) {
		/* Pump the transport until this stream has a message; others' messages wait in their own queues. */
		if (fd < 0 || fd >= SNS_LINK_NPORTS)
			return -1;
		long end = mono_ms() + (timeout_ms > 0 ? timeout_ms : 0);
		for (;;) {
			int n = take(fd, node, port, data, cap);
			if (n)
				return n;
			int r;
			while ((r = link_ops->pump()) > 0 && !qs[fd].n)
				;
			if (r < 0)
				return -1;
			if (qs[fd].n)
				continue;
			if (mono_ms() >= end)
				return 0;
			usleep(2000);
		}
	}
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
