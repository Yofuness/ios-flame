/*
 * APR client transport (see apr.h): glink channel handling as in aprtest.c (OPEN / OPEN_ACK, intents,
 * TX_DATA(_CONT), RX_DONE, RX_INTENT_REQ), APR 5-word header APPS (5) -> ADSP (4).
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "smem.h"
#include "glink.h"
#include "apr.h"

#define SMEM_HOST_ADSP	2
#define IPC_ADSP_GLINK	24
#define CHAN		"apr_audio_svc"
#define LCID		1
#define INTENT_SZ	4096
#define NINTENT		4
#define APR_HDR_FIELD	0x0150
#define APR_DOMAIN_ADSP	4
#define APR_DOMAIN_APPS	5
#define APR_MAX		1024

struct apr_hdr {
	uint16_t hdr_field, pkt_size;
	uint8_t src_svc, src_domain;
	uint16_t src_port;
	uint8_t dest_svc, dest_domain;
	uint16_t dest_port;
	uint32_t token, opcode;
} __attribute__((packed));

uint32_t apr_rsp_op, apr_rsp_token, apr_rsp[64], apr_rsp_len;

static int dry, rcid = -1, local_open, remote_open;
static uint32_t r_iid[16], r_isz[16];
static int r_n, our_iid = 100;
static uint8_t rxbuf[16384];
static size_t rx_have;
static uint32_t rx_iid, next_token = 0x100;
static int got;		/* a reply arrived since the last send */

void apr_set_dry(int d)
{
	dry = d;
}

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void learn(uint16_t cid)
{
	if (rcid < 0) {
		rcid = cid;
		printf("apr: remote cid %u\n", cid);
	}
}

static void give_intents(int n)
{
	uint32_t e[2 * NINTENT];
	for (int i = 0; i < n; i++) {
		e[2 * i] = INTENT_SZ;
		e[2 * i + 1] = (uint32_t)our_iid++;
	}
	glink_send(GLINK_CMD_INTENT, LCID, (uint32_t)n, e, 8u * n);
}

static void apr_rx(const uint8_t *p, size_t n)
{
	struct apr_hdr h;
	if (n < sizeof(h)) {
		printf("apr: rx short %zu\n", n);
		return;
	}
	memcpy(&h, p, sizeof(h));
	apr_rsp_len = (uint32_t)(n - sizeof(h)) / 4;
	if (apr_rsp_len > 64)
		apr_rsp_len = 64;
	memcpy(apr_rsp, p + sizeof(h), apr_rsp_len * 4);
	apr_rsp_op = h.opcode;
	apr_rsp_token = h.token;
	got = 1;
	if (h.opcode == APR_BASIC_RSP)
		printf("apr: rx svc %x BASIC_RSP for op %x status %x (token %x)\n", h.src_svc,
		       apr_rsp_len ? apr_rsp[0] : 0, apr_rsp_len > 1 ? apr_rsp[1] : 0, h.token);
	else
		printf("apr: rx svc %x op %x len %zu token %x p0 %x p1 %x\n", h.src_svc, h.opcode, n, h.token,
		       apr_rsp_len ? apr_rsp[0] : 0, apr_rsp_len > 1 ? apr_rsp[1] : 0);
}

static int pump_one(void)
{
	if (glink_rx_avail() < 8)
		return 0;
	uint16_t h[2];
	uint32_t p2;
	glink_rx_peek(h, 0, 4);
	glink_rx_peek(&p2, 4, 4);
	size_t n = 8;
	switch (h[0]) {
	case GLINK_CMD_VERSION:
		glink_send(GLINK_CMD_VERSION_ACK, h[1], p2 & 1, NULL, 0);
		break;
	case GLINK_CMD_OPEN: {
		n = 8 + ((p2 + 7) & ~7u);
		if (glink_rx_avail() < n)
			return 0;
		char name[64] = { 0 };
		glink_rx_peek(name, 8, p2 < 63 ? p2 : 63);
		printf("apr: remote OPEN \"%s\" (rcid %u)\n", name, h[1]);
		if (!strcmp(name, CHAN)) {
			rcid = h[1];
			remote_open = 1;
			glink_send(GLINK_CMD_OPEN_ACK, h[1], 0, NULL, 0);
		}
		break;
	}
	case GLINK_CMD_OPEN_ACK:
		if (h[1] == LCID) {
			local_open = 1;
			give_intents(NINTENT);
		}
		break;
	case GLINK_CMD_CLOSE:
		printf("apr: remote CLOSE cid %u\n", h[1]);
		glink_send(GLINK_CMD_CLOSE_ACK, h[1], 0, NULL, 0);
		if ((int)h[1] == rcid)
			remote_open = 0;
		break;
	case GLINK_CMD_INTENT:
		n = 8 + 8ull * p2;
		if (glink_rx_avail() < n)
			return 0;
		learn(h[1]);
		for (uint32_t i = 0; i < p2; i++) {
			uint32_t e[2];
			glink_rx_peek(e, 8 + 8 * i, 8);
			if ((int)h[1] == rcid && r_n < 16) {
				r_isz[r_n] = e[0];
				r_iid[r_n++] = e[1];
			}
		}
		break;
	case GLINK_CMD_RX_INTENT_REQ:
		learn(h[1]);
		glink_send(GLINK_CMD_RX_INTENT_REQ_ACK, LCID, 1, NULL, 0);
		give_intents(1);
		break;
	case GLINK_CMD_TX_DATA:
	case GLINK_CMD_TX_DATA_CONT: {
		if (glink_rx_avail() < 16)
			return 0;
		uint32_t cl[2];
		glink_rx_peek(cl, 8, 8);
		n = (16 + cl[0] + 7) & ~(size_t)7;
		if (glink_rx_avail() < n)
			return 0;
		learn(h[1]);
		if (h[0] == GLINK_CMD_TX_DATA) {
			rx_have = 0;
			rx_iid = p2;
		}
		if (rx_have + cl[0] <= sizeof(rxbuf)) {
			glink_rx_peek(rxbuf + rx_have, 16, cl[0]);
			rx_have += cl[0];
		}
		if (!cl[1]) {
			glink_send(GLINK_CMD_RX_DONE, LCID, rx_iid, NULL, 0);
			give_intents(1);
			apr_rx(rxbuf, rx_have);
		}
		break;
	}
	case GLINK_CMD_VERSION_ACK:
	case GLINK_CMD_RX_DONE:
	case GLINK_CMD_RX_DONE_W_REUSE:
	case GLINK_CMD_SIGNALS:
	case GLINK_CMD_READ_NOTIF:
	case GLINK_CMD_CLOSE_ACK:
	case GLINK_CMD_RX_INTENT_REQ_ACK:
		break;
	default:
		printf("apr: glink cmd %u (p1 %u p2 %u): skipped as 8 bytes\n", h[0], h[1], p2);
	}
	glink_rx_advance(n);
	return 1;
}

void apr_pump(int ms)
{
	if (dry)
		return;
	uint64_t end = now_ms() + (uint64_t)ms;
	do {
		while (pump_one())
			;
		if (ms)
			usleep(1000);
	} while (now_ms() < end);
}

static int wait_flag(int *flag, int ms)
{
	uint64_t end = now_ms() + (uint64_t)ms;
	while (now_ms() < end) {
		while (pump_one())
			;
		if (*flag)
			return 0;
		usleep(2000);
	}
	return -1;
}

int apr_open(int reset)
{
	if (dry) {
		printf("apr: [dry] open \"%s\" on the lpass edge\n", CHAN);
		return 0;
	}
	if (smem_init())
		return -1;
	glink_select(SMEM_HOST_ADSP, IPC_ADSP_GLINK);
	glink_keep_state(!reset);
	if (glink_init())
		return -1;
	if (reset)
		glink_send(GLINK_CMD_VERSION, 1, 1, NULL, 0);
	while (pump_one())
		;
	static const char name[] = CHAN;
	glink_send(GLINK_CMD_OPEN, LCID, sizeof(name), name, sizeof(name));
	if (wait_flag(&local_open, 3000)) {
		printf("apr: no OPEN_ACK in 3 s\n");
		return -1;
	}
	wait_flag(&remote_open, 500);
	apr_pump(300);
	printf("apr: channel up (rcid %d, remote open %d, %d remote intents)\n", rcid, remote_open, r_n);
	return 0;
}

void apr_close(void)
{
	if (dry)
		return;
	glink_send(GLINK_CMD_CLOSE, LCID, 0, NULL, 0);
	apr_pump(200);
}

static int send_pkt(uint8_t svc, uint32_t opcode, uint32_t token, const void *payload, size_t plen)
{
	uint8_t d[8 + APR_MAX];
	size_t n = sizeof(struct apr_hdr) + plen;
	if (n > APR_MAX) {
		printf("apr: tx op %x: %zu bytes too big\n", opcode, n);
		return -1;
	}
	struct apr_hdr h = {
		.hdr_field = APR_HDR_FIELD, .pkt_size = (uint16_t)n,
		.src_svc = svc, .src_domain = APR_DOMAIN_APPS, .src_port = 0,
		.dest_svc = svc, .dest_domain = APR_DOMAIN_ADSP, .dest_port = 0,
		.token = token, .opcode = opcode,
	};
	uint32_t cl[2] = { (uint32_t)n, 0 };
	memcpy(d, cl, 8);
	memcpy(d + 8, &h, sizeof(h));
	if (plen)
		memcpy(d + 8 + sizeof(h), payload, plen);
	if (dry) {
		printf("apr: [dry] tx svc %x op %08x token %x len %zu:", svc, opcode, token, n);
		for (size_t i = 0; i + 4 <= plen; i += 4) {
			uint32_t w;
			memcpy(&w, (const uint8_t *)payload + i, 4);
			printf(" %08x", w);
		}
		printf("\n");
		return 0;
	}
	if (!r_n) {
		glink_send(GLINK_CMD_RX_INTENT_REQ, LCID, (uint32_t)n, NULL, 0);
		uint64_t end = now_ms() + 1000;
		while (!r_n && now_ms() < end) {
			while (pump_one())
				;
			usleep(1000);
		}
	}
	int i;
	for (i = 0; i < r_n && r_isz[i] < n; i++)
		;
	if (i >= r_n) {
		printf("apr: tx svc %x op %x: no remote intent >= %zu bytes\n", svc, opcode, n);
		return -1;
	}
	uint32_t iid = r_iid[i];
	r_iid[i] = r_iid[--r_n];
	r_isz[i] = r_isz[r_n];
	got = 0;
	return glink_send(GLINK_CMD_TX_DATA, LCID, iid, d, 8 + n);
}

int apr_send_nowait(uint8_t svc, uint32_t opcode, const void *payload, size_t plen)
{
	return send_pkt(svc, opcode, next_token++, payload, plen);
}

int apr_cmd(uint8_t svc, uint32_t opcode, const void *payload, size_t plen, uint32_t rsp_op, int ms)
{
	uint32_t token = next_token++;
	if (!dry)
		printf("apr: tx svc %x op %08x token %x len %zu\n", svc, opcode, token, 20 + plen);
	if (send_pkt(svc, opcode, token, payload, plen))
		return -1;
	if (dry)
		return 0;
	uint64_t end = now_ms() + (uint64_t)ms;
	while (now_ms() < end) {
		while (pump_one())
			;
		if (got) {
			got = 0;
			if (apr_rsp_op == APR_BASIC_RSP && apr_rsp_len >= 2 && apr_rsp[0] == opcode) {
				printf("apr: op %08x -> status %x\n", opcode, apr_rsp[1]);
				return (int)apr_rsp[1];
			}
			if (rsp_op && apr_rsp_op == rsp_op) {
				printf("apr: op %08x -> rsp %08x\n", opcode, rsp_op);
				return 0;
			}
		}
		usleep(1000);
	}
	printf("apr: op %08x -> no answer in %d ms\n", opcode, ms);
	return -1;
}
