/*
 * The IPCRTR channel over GLINK, and QRTR on it: enough of Linux
 * qcom_glink_native.c and net/qrtr to talk QMI to the modem. Polled, one
 * channel. The modem is QRTR node 0; we are node 1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "glink.h"
#include "link.h"
#include "pmem.h"

#define LCID		1
#define N_INTENTS	8
#define INTENT_SIZE	4096
#define FEAT_INTENT_REUSE 1

static uint32_t rcid;
static int open_tx, open_acked, opened_by_remote, version_done;
#define MAX_RINTENTS 256
static struct { uint32_t size, iid; int busy; } rintent[MAX_RINTENTS];
static int nrintent;
static volatile int intent_req_acked;

/* reassembly of a TX_DATA spread over chunks */
static uint8_t rxbuf[65536];
static uint32_t rxoff;

static qrtr_cb_t qrtr_cb;

int verbose_link = 1;
unsigned link_stats[4];
void (*link_idle)(void);
#define LOG(...) do { if (verbose_link) { printf("[%6llu] ", (unsigned long long)link_ms()); printf(__VA_ARGS__); } } while (0)

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t t0;
uint64_t link_ms(void)
{
	if (!t0)
		t0 = now_ms();
	return now_ms() - t0;
}

static void send_open(void)
{
	const char name[] = "IPCRTR";
	glink_send(GLINK_CMD_OPEN, LCID, sizeof(name), name, sizeof(name));
	open_tx = 1;
}

static void advertise_intents(void)
{
	uint32_t p[N_INTENTS * 2];
	for (int i = 0; i < N_INTENTS; i++) {
		p[2 * i] = INTENT_SIZE;
		p[2 * i + 1] = i + 1;
	}
	glink_send(GLINK_CMD_INTENT, LCID, N_INTENTS, p, sizeof(p));
}

static void handle_data(const uint8_t *d, uint32_t n);

/* one command from the FIFO; returns 0 if incomplete (wait for more) */
static int rx_one(void)
{
	size_t avail = glink_rx_avail();
	if (avail < 8)
		return 0;
	uint8_t h[16];
	glink_rx_peek(h, 0, 8);
	uint16_t cmd, p1;
	uint32_t p2;
	memcpy(&cmd, h, 2);
	memcpy(&p1, h + 2, 2);
	memcpy(&p2, h + 4, 4);
	size_t adv = 8;

	switch (cmd) {
	case GLINK_CMD_VERSION:
		LOG("glink: < VERSION %u features %#x\n", p1, p2);
		glink_send(GLINK_CMD_VERSION_ACK, 1, p2 & FEAT_INTENT_REUSE, NULL, 0);
		if (!version_done) {
			glink_send(GLINK_CMD_VERSION, 1, FEAT_INTENT_REUSE, NULL, 0);
		}
		break;
	case GLINK_CMD_VERSION_ACK:
		LOG("glink: < VERSION_ACK %u features %#x\n", p1, p2);
		version_done = 1;
		break;
	case GLINK_CMD_OPEN: {
		size_t nl = (p2 + 7) & ~7u;
		if (avail < 8 + nl)
			return 0;
		char name[64] = { 0 };
		glink_rx_peek(name, 8, p2 < 63 ? p2 : 63);
		adv += nl;
		LOG("glink: < OPEN rcid %u \"%s\"\n", p1, name);
		/* Linux acks every remote open; only IPCRTR gets opened from our side */
		glink_send(GLINK_CMD_OPEN_ACK, p1, 0, NULL, 0);
		if (!strcmp(name, "IPCRTR")) {
			rcid = p1;
			opened_by_remote = 1;
			if (!open_tx)
				send_open();
		}
		break;
	}
	case GLINK_CMD_OPEN_ACK:
		LOG("glink: < OPEN_ACK lcid %u\n", p1);
		if (p1 == LCID && !open_acked) {
			open_acked = 1;
			advertise_intents();
		}
		break;
	case GLINK_CMD_CLOSE:
		LOG("glink: < CLOSE rcid %u\n", p1);
		glink_send(GLINK_CMD_CLOSE_ACK, p1, 0, NULL, 0);
		break;
	case GLINK_CMD_CLOSE_ACK:
		LOG("glink: < CLOSE_ACK %u\n", p1);
		break;
	case GLINK_CMD_INTENT: {
		size_t len = 8 + (size_t)p2 * 8;
		if (avail < len)
			return 0;
		for (uint32_t i = 0; i < p2; i++) {
			uint32_t pr[2];
			glink_rx_peek(pr, 8 + i * 8, 8);
			if (verbose_link > 1)
				LOG("glink: < INTENT rcid %u size %u iid %u\n", p1, pr[0], pr[1]);
			if (p1 == rcid && opened_by_remote && nrintent < MAX_RINTENTS) {	/* intent ids are per channel */
				rintent[nrintent].size = pr[0];
				rintent[nrintent].iid = pr[1];
				rintent[nrintent].busy = 0;
				nrintent++;
			}
		}
		adv = (len + 7) & ~7u;
		break;
	}
	case GLINK_CMD_RX_DONE:
	case GLINK_CMD_RX_DONE_W_REUSE:
		if (p1 != rcid)
			break;
		if (verbose_link > 1)
			LOG("glink: < RX_DONE%s iid %u\n", cmd == GLINK_CMD_RX_DONE ? "" : "_W_REUSE", p2);
		link_stats[cmd == GLINK_CMD_RX_DONE ? 0 : 1]++;
		for (int i = 0; i < nrintent; i++)
			if (rintent[i].iid == p2) {
				if (cmd == GLINK_CMD_RX_DONE) {
					rintent[i] = rintent[--nrintent];
				} else {
					rintent[i].busy = 0;
				}
				break;
			}
		break;
	case GLINK_CMD_RX_INTENT_REQ:
		/* we take any data in place, so every request is granted with a fresh intent id */
		if (p1 == rcid) {
			static uint32_t next_iid = 100;
			uint32_t p[2] = { p2 > INTENT_SIZE ? p2 : INTENT_SIZE, next_iid++ };
			glink_send(GLINK_CMD_INTENT, LCID, 1, p, sizeof(p));
			glink_send(GLINK_CMD_RX_INTENT_REQ_ACK, LCID, 1, NULL, 0);
		} else {
			glink_send(GLINK_CMD_RX_INTENT_REQ_ACK, 0, 0, NULL, 0);
		}
		break;
	case GLINK_CMD_RX_INTENT_REQ_ACK:
		intent_req_acked = 1;
		if (!p2)
			LOG("glink: < RX_INTENT_REQ_ACK: refused\n");
		break;
	case GLINK_CMD_TX_DATA:
	case GLINK_CMD_TX_DATA_CONT: {
		if (avail < 16)
			return 0;
		glink_rx_peek(h, 0, 16);
		uint32_t chunk, left;
		memcpy(&chunk, h + 8, 4);
		memcpy(&left, h + 12, 4);
		size_t len = 16 + chunk;
		if (avail < len)
			return 0;
		if (rxoff + chunk <= sizeof(rxbuf))
			glink_rx_peek(rxbuf + rxoff, 16, chunk);
		rxoff += chunk;
		adv = (len + 7) & ~7u;
		if (p1 != rcid) {
			LOG("glink: < TX_DATA on rcid %u (%u bytes)\n", p1, chunk);
			rxoff = 0;
			break;
		}
		if (!left) {
			glink_rx_advance(adv);
			uint32_t n = rxoff;
			rxoff = 0;
			glink_send(GLINK_CMD_RX_DONE_W_REUSE, LCID, p2, NULL, 0);
			handle_data(rxbuf, n);
			return 1;
		}
		break;
	}
	case GLINK_CMD_READ_NOTIF:
		wr32(0x17c0000cull, 1u << 12);
		break;
	case GLINK_CMD_SIGNALS:
		LOG("glink: < SIGNALS %#x\n", p2);
		break;
	default:
		LOG("glink: < unknown cmd %u (%u %#x); dropping the FIFO\n", cmd, p1, p2);
		adv = avail;
		break;
	}
	glink_rx_advance(adv);
	return 1;
}

/* O12: the FIFOs are shared memory (no fd to poll): sleep 1 ms right after traffic (ours or what link_idle's
 * pollers count in link_traffic), doubling to 10 ms after 50 ms without; WLAND_POLL1MS=1 = the fixed 1 ms */
unsigned link_traffic;

int link_poll(unsigned ms)
{
	static int fixed = -1;
	static uint64_t last_traffic;
	static unsigned seen, nap = 1;
	uint64_t end = now_ms() + ms;
	int n = 0;
	if (fixed < 0)
		fixed = getenv("WLAND_POLL1MS") != NULL;
	do {
		while (rx_one())
			n++, link_traffic++;
		if (link_idle)
			link_idle();
		if (!ms)
			break;
		uint64_t now = now_ms();
		if (fixed || link_traffic != seen) {
			seen = link_traffic;
			last_traffic = now;
			nap = 1;
		} else if (now - last_traffic >= 50 && nap < 10) {
			nap = nap * 2 > 10 ? 10 : nap * 2;
		}
		unsigned d = nap;
		if (now + d > end)
			d = now < end ? (unsigned)(end - now) : 1;
		delay_ms(d);
	} while (now_ms() < end);
	return n;
}

int link_up(qrtr_cb_t cb, unsigned timeout_ms)
{
	qrtr_cb = cb;
	if (glink_init())
		return -1;
	glink_send(GLINK_CMD_VERSION, 1, FEAT_INTENT_REUSE, NULL, 0);
	uint64_t end = now_ms() + timeout_ms;
	while (now_ms() < end && !(open_acked && opened_by_remote))
		link_poll(10);
	if (!(open_acked && opened_by_remote)) {
		printf("glink: IPCRTR not open (version %d, remote open %d, ack %d)\n",
		       version_done, opened_by_remote, open_acked);
		return -1;
	}
	printf("glink: IPCRTR open (rcid %u)\n", rcid);
	return 0;
}

static int find_intent(uint32_t len)
{
	int best = -1;
	for (int i = 0; i < nrintent; i++)
		if (!rintent[i].busy && rintent[i].size >= len && (best < 0 || rintent[i].size < rintent[best].size))
			best = i;
	return best;
}

/* One packet into one of the modem's intents (requested if none fits), in chunks of 8K as Linux does. */
int link_send(const void *data, uint32_t len)
{
	uint64_t start = now_ms(), end = start + 3000;
	int i, asked = 0;
	while ((i = find_intent(len)) < 0) {
		if (!asked || (intent_req_acked && asked)) {
			if (asked > 3)
				goto fail;
			intent_req_acked = 0;
			link_stats[2]++;
			glink_send(GLINK_CMD_RX_INTENT_REQ, LCID, len, NULL, 0);
			asked++;
		}
		if (now_ms() > end)
			goto fail;
		link_poll(1);
	}
	link_stats[3] += (unsigned)(now_ms() - start);
	rintent[i].busy = 1;
	if (verbose_link > 1)
		LOG("glink: > TX %u bytes in iid %u (size %u)\n", len, rintent[i].iid, rintent[i].size);
	const uint8_t *d = data;
	for (uint32_t off = 0; off < len;) {
		uint32_t chunk = len - off > 8192 ? 8192 : len - off;
		uint8_t b[8 + 8192];
		uint32_t hdr[2] = { chunk, len - off - chunk };
		memcpy(b, hdr, 8);
		memcpy(b + 8, d + off, chunk);
		while (glink_send(off ? GLINK_CMD_TX_DATA_CONT : GLINK_CMD_TX_DATA, LCID, rintent[i].iid, b, 8 + chunk)) {
			if (now_ms() > end)
				goto fail;
			link_poll(1);
		}
		off += chunk;
	}
	return 0;
fail:
	printf("glink: cannot send %u bytes (intents %d)\n", len, nrintent);
	return -1;
}

/* ---- QRTR ---- */

static void handle_data(const uint8_t *d, uint32_t n)
{
	struct qrtr_hdr hd;
	if (n < sizeof(hd)) {
		LOG("qrtr: short packet %u\n", n);
		return;
	}
	memcpy(&hd, d, sizeof(hd));
	if (hd.version != 1) {
		LOG("qrtr: version %u\n", hd.version);
		return;
	}
	if (qrtr_cb)
		qrtr_cb(&hd, d + sizeof(hd), n - sizeof(hd) < hd.size ? n - sizeof(hd) : hd.size);
}

int qrtr_send(uint32_t type, uint32_t src_port, uint32_t dst_node, uint32_t dst_port,
	      const void *payload, uint32_t len)
{
	static uint8_t b[65536 + 64];
	struct qrtr_hdr hd = {
		.version = 1, .type = type, .src_node = QRTR_NODE_APPS, .src_port = src_port,
		.confirm_rx = 0, .size = len, .dst_node = dst_node, .dst_port = dst_port,
	};
	uint32_t padded = (len + 3) & ~3u;
	if (sizeof(hd) + padded > sizeof(b))
		return -1;
	memset(b + sizeof(hd) + len, 0, padded - len);
	memcpy(b, &hd, sizeof(hd));
	memcpy(b + sizeof(hd), payload, len);
	return link_send(b, sizeof(hd) + padded);
}

int qrtr_new_server(uint32_t service, uint32_t version, uint32_t instance, uint32_t port)
{
	uint32_t pkt[5] = { QRTR_TYPE_NEW_SERVER, service, (version & 0xff) | (instance << 8), QRTR_NODE_APPS, port };
	return qrtr_send(QRTR_TYPE_NEW_SERVER, QRTR_PORT_CTRL, QRTR_NODE_MODEM, QRTR_PORT_CTRL, pkt, sizeof(pkt));
}

/* After a packet with confirm_rx: tell the sender it may go on (Linux qrtr_port_post: RESUME_TX). */
int qrtr_resume_tx(const struct qrtr_hdr *h)
{
	/* struct qrtr_ctrl_pkt: cmd, then client { node, port } (the union's first two words) */
	uint32_t pkt[5] = { QRTR_TYPE_RESUME_TX, h->dst_node, h->dst_port, 0, 0 };
	return qrtr_send(QRTR_TYPE_RESUME_TX, h->dst_port, h->src_node, h->src_port, pkt, sizeof(pkt));
}
