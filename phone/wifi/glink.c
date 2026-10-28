#include <stdio.h>
#include <string.h>
#include "glink.h"
#include "pmem.h"
#include "smem.h"

#define DESC_ITEM	478
#define FIFO_TX_ITEM	479	/* ours: apps -> modem */
#define FIFO_RX_ITEM	480	/* the modem's */
#define APCS_IPC	0x17c0000cull
#define IPC_GLINK	12

static uint64_t desc, tx_fifo, rx_fifo;
static uint32_t tx_len, rx_len;
/* the edge: the modem unless glink_select (adspboot: ADSP host 2, IPC bit 24) */
static unsigned g_host = SMEM_HOST_MODEM, g_ipc = IPC_GLINK;

void glink_select(unsigned host, unsigned ipc_bit)
{
	g_host = host;
	g_ipc = ipc_bit;
}

/* descs: [0] tx tail, [1] tx head, [2] rx tail, [3] rx head (ours = tx) */
#define TX_TAIL	(desc + 0)
#define TX_HEAD	(desc + 4)
#define RX_TAIL	(desc + 8)
#define RX_HEAD	(desc + 12)

int glink_init(void)
{
	uint32_t sz;
	if (smem_alloc(g_host, DESC_ITEM, 32, &desc) ||
	    smem_alloc(g_host, FIFO_TX_ITEM, 16384, &tx_fifo) ||
	    smem_get(g_host, FIFO_TX_ITEM, &tx_fifo, &tx_len)) {
		fprintf(stderr, "glink: smem items\n");
		return -1;
	}
	if (smem_get(g_host, FIFO_RX_ITEM, &rx_fifo, &rx_len)) {
		fprintf(stderr, "glink: host %u has no rx fifo (item 480)\n", g_host);
		return -1;
	}
	(void)sz;
	/* as Linux qcom_glink_smem_register: our ends of both pipes start over */
	wr32(RX_TAIL, 0);
	wr32(TX_HEAD, 0);
	printf("glink: desc %llx tx fifo %llx (%u) rx fifo %llx (%u); tx %u/%u rx %u/%u\n",
	       (unsigned long long)desc, (unsigned long long)tx_fifo, tx_len, (unsigned long long)rx_fifo, rx_len,
	       rd32(TX_TAIL), rd32(TX_HEAD), rd32(RX_TAIL), rd32(RX_HEAD));
	return 0;
}

static uint32_t tx_write(uint32_t head, const uint8_t *data, size_t n)
{
	/* the FIFO is device memory here: whole words only (n is padded by the caller) */
	for (size_t i = 0; i < n; i += 4) {
		uint32_t w;
		memcpy(&w, data + i, 4);
		wr32(tx_fifo + head, w);
		head += 4;
		if (head >= tx_len)
			head -= tx_len;
	}
	return head;
}

int glink_send(uint16_t cmd, uint16_t p1, uint32_t p2, const void *data, size_t len)
{
	uint8_t buf[8 + 8 + 8192 + 8] = { 0 };
	size_t n = 8 + len, padded = (n + 7) & ~(size_t)7;
	if (padded > sizeof(buf))
		return -1;
	memcpy(buf, &cmd, 2);
	memcpy(buf + 2, &p1, 2);
	memcpy(buf + 4, &p2, 4);
	if (len)
		memcpy(buf + 8, data, len);
	uint32_t head = rd32(TX_HEAD), tail = rd32(TX_TAIL);
	uint32_t avail = tail <= head ? tx_len - head + tail : tail - head;
	if (avail < padded + 16)
		return -1;	/* full: the caller polls and retries */
	head = tx_write(head, buf, padded);
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(TX_HEAD, head);
	wr32(APCS_IPC, 1u << g_ipc);
	return 0;
}

size_t glink_rx_avail(void)
{
	uint32_t head = rd32(RX_HEAD), tail = rd32(RX_TAIL);
	return head < tail ? rx_len - tail + head : head - tail;
}

void glink_rx_peek(void *buf, size_t off, size_t len)
{
	uint32_t t = rd32(RX_TAIL) + (uint32_t)off;
	if (t >= rx_len)
		t -= rx_len;
	uint8_t *b = buf;
	for (size_t i = 0; i < len; i++) {
		uint32_t w = rd32(rx_fifo + (t & ~3u));
		b[i] = (uint8_t)(w >> (8 * (t & 3)));
		if (++t >= rx_len)
			t = 0;
	}
}

void glink_rx_advance(size_t len)
{
	uint32_t t = rd32(RX_TAIL) + (uint32_t)len;
	if (t >= rx_len)
		t -= rx_len;
	wr32(RX_TAIL, t);
}
