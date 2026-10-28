/*
 * aprtest: audio M1a (docs/features/audio.md). Talk to the ADSP's APR core service over the glink "lpass" edge
 * that adspboot brought up. Run after adspboot; it never boots or resets the ADSP.
 *
 *   aprtest [-r]   -r: reset our FIFO ends and redo VERSION (only if adspboot never got the edge up)
 *
 * Channels: the ADSP on sm6150-era firmware (downstream apr_tal_glink / upstream qcom,apr over rpmsg) opens
 * "apr_audio_svc" on the lpass edge; other names seen downstream on this edge are "adsp_apps" (adsprpc),
 * "LPASS_IPCRTR" (QRTR) and "DATA_*"/"loopback" test channels. adspboot prints the real list. APR uses
 * "apr_audio_svc": each TX_DATA carries one raw APR packet.
 *
 * glink (Linux qcom_glink_native.c): both sides send OPEN(lcid, name) and each answers the other with
 * OPEN_ACK(the opener's lcid). Data needs an intent from the receiver: INTENT(cid, count){size, iid}; the
 * sender uses it in TX_DATA(cid, iid){chunk, left} and the receiver returns it with RX_DONE(cid, iid).
 * RX_INTENT_REQ(cid, size) asks for one; it is answered by RX_INTENT_REQ_ACK + INTENT.
 * adspboot consumed the ADSP's OPEN, so its rcid may be unknown here: we learn it from the first INTENT,
 * TX_DATA, OPEN or RX_INTENT_REQ the ADSP sends after our OPEN (only one channel is ours).
 *
 * APR header (Linux include/linux/soc/qcom/apr.h), 20 bytes: hdr_field (msg type << 8 | header words << 4 |
 * version) = 0x0150 for a 5-word SEQ_CMD, pkt_size, src_svc, src_domain, src_port, dest_svc, dest_domain,
 * dest_port, token, opcode. Exit 0 = GET_STATE answered "ready".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "pmem.h"
#include "smem.h"
#include "glink.h"

#define SMEM_HOST_ADSP	2
#define IPC_ADSP_GLINK	24
#define CHAN		"apr_audio_svc"
#define LCID		1		/* ours; Linux starts at 1 */
#define INTENT_SZ	4096
#define NINTENT		4

#define APR_HDR_FIELD	0x0150		/* SEQ_CMD (1) << 8 | 5 words << 4 | version 0 */
#define APR_DOMAIN_ADSP	4
#define APR_DOMAIN_APPS	5
#define APR_SVC_CORE	0x3
#define APR_BASIC_RSP	0x000110e8	/* payload: opcode, status */
#define AVCS_GET_STATE		0x0001326c
#define AVCS_GET_STATE_RSP	0x0001326d
#define AVCS_GET_FWK_VERSION	0x0001292c
#define AVCS_GET_FWK_VERSION_RSP 0x0001292d
#define AVCS_GET_VERSIONS	0x00012905
#define AVCS_GET_VERSIONS_RSP	0x00012906

struct apr_hdr {
	uint16_t hdr_field, pkt_size;
	uint8_t src_svc, src_domain;
	uint16_t src_port;
	uint8_t dest_svc, dest_domain;
	uint16_t dest_port;
	uint32_t token, opcode;
} __attribute__((packed));

static int rcid = -1, local_open, remote_open;
static uint32_t r_iid[16], r_isz[16];	/* the ADSP's free intents */
static int r_n, our_iid = 100;
static uint8_t rxbuf[16384];
static size_t rx_have;
static uint32_t rx_iid;

/* last APR reply */
static uint32_t got_op, got_payload[64], got_len;

static void learn(uint16_t cid)
{
	if (rcid < 0) {
		rcid = cid;
		printf("aprtest: remote cid %u\n", cid);
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
	got_len = (uint32_t)(n - sizeof(h)) / 4;
	if (got_len > 64)
		got_len = 64;
	memcpy(got_payload, p + sizeof(h), got_len * 4);
	uint32_t status = got_len ? got_payload[h.opcode == APR_BASIC_RSP && got_len > 1 ? 1 : 0] : 0;
	printf("apr: rx svc %x op %x status %x (len %zu, from %u/%u port %u token %u%s%x)\n", h.src_svc, h.opcode,
	       status, n, h.src_domain, h.src_svc, h.src_port, h.token,
	       h.opcode == APR_BASIC_RSP ? ", for op " : "", h.opcode == APR_BASIC_RSP ? got_payload[0] : 0);
	got_op = h.opcode;
}

/* Parse what the ADSP sent; 0 = nothing (more) complete. */
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
		printf("aprtest: glink version %u features %x (acked)\n", h[1], p2);
		break;
	case GLINK_CMD_VERSION_ACK:
		printf("aprtest: glink version acked (%u, %x)\n", h[1], p2);
		break;
	case GLINK_CMD_OPEN: {
		n = 8 + ((p2 + 7) & ~7u);
		if (glink_rx_avail() < n)
			return 0;
		char name[64] = { 0 };
		glink_rx_peek(name, 8, p2 < 63 ? p2 : 63);
		printf("aprtest: remote OPEN \"%s\" (rcid %u)\n", name, h[1]);
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
			printf("aprtest: OPEN_ACK for our lcid %u\n", LCID);
			give_intents(NINTENT);
		}
		break;
	case GLINK_CMD_CLOSE:
		printf("aprtest: remote CLOSE cid %u\n", h[1]);
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
		printf("aprtest: remote intents +%u (%d free)\n", p2, r_n);
		break;
	case GLINK_CMD_RX_INTENT_REQ:
		learn(h[1]);
		printf("aprtest: remote asks for an intent of %u bytes\n", p2);
		glink_send(GLINK_CMD_RX_INTENT_REQ_ACK, LCID, 1, NULL, 0);
		give_intents(1);
		break;
	case GLINK_CMD_RX_DONE:
	case GLINK_CMD_RX_DONE_W_REUSE:
		break;	/* reuse: not tracked; the ADSP grants more intents or we ask (RX_INTENT_REQ) */
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
	case GLINK_CMD_SIGNALS:
	case GLINK_CMD_READ_NOTIF:
	case GLINK_CMD_CLOSE_ACK:
	case GLINK_CMD_RX_INTENT_REQ_ACK:
		break;
	default:
		printf("aprtest: glink cmd %u (p1 %u p2 %u): skipped as 8 bytes\n", h[0], h[1], p2);
	}
	glink_rx_advance(n);
	return 1;
}

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Pump until *flag (or got_op == want, if flag is NULL) or ms pass; 0 = met. */
static int wait_for(int *flag, uint32_t want, int ms)
{
	uint64_t end = now_ms() + (uint64_t)ms;
	while (now_ms() < end) {
		while (pump_one())
			;
		if (flag ? *flag : (got_op == want || got_op == APR_BASIC_RSP))
			return 0;
		usleep(2000);
	}
	return -1;
}

static int apr_send(uint8_t svc, uint32_t opcode, uint32_t token, const void *payload, size_t plen)
{
	uint8_t pkt[256];
	struct apr_hdr h = {
		.hdr_field = APR_HDR_FIELD, .pkt_size = (uint16_t)(sizeof(h) + plen),
		.src_svc = svc, .src_domain = APR_DOMAIN_APPS, .src_port = 0,
		.dest_svc = svc, .dest_domain = APR_DOMAIN_ADSP, .dest_port = 0,
		.token = token, .opcode = opcode,
	};
	memcpy(pkt, &h, sizeof(h));
	if (plen)
		memcpy(pkt + sizeof(h), payload, plen);
	size_t n = sizeof(h) + plen;
	int i;
	if (!r_n) {
		glink_send(GLINK_CMD_RX_INTENT_REQ, LCID, (uint32_t)n, NULL, 0);
		uint64_t end = now_ms() + 1000;
		while (!r_n && now_ms() < end) {
			while (pump_one())
				;
			usleep(2000);
		}
	}
	for (i = 0; i < r_n && r_isz[i] < n; i++)
		;
	if (i >= r_n) {
		printf("apr: tx svc %x op %x: no remote intent >= %zu bytes\n", svc, opcode, n);
		return -1;
	}
	uint32_t iid = r_iid[i];
	r_iid[i] = r_iid[--r_n];
	r_isz[i] = r_isz[r_n];
	uint8_t d[8 + 256];
	uint32_t cl[2] = { (uint32_t)n, 0 };
	memcpy(d, cl, 8);
	memcpy(d + 8, pkt, n);
	got_op = 0;
	printf("apr: tx svc %x op %x len %zu (intent %u)\n", svc, opcode, n, iid);
	return glink_send(GLINK_CMD_TX_DATA, LCID, iid, d, 8 + n);
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	int reset = argc > 1 && !strcmp(argv[1], "-r");
	if (smem_init())
		return 1;
	glink_select(SMEM_HOST_ADSP, IPC_ADSP_GLINK);
	glink_keep_state(!reset);
	if (glink_init())
		return 1;
	if (reset)
		glink_send(GLINK_CMD_VERSION, 1, 1, NULL, 0);
	/* leftovers adspboot left unread (maybe the ADSP's OPEN) */
	while (pump_one())
		;

	static const char name[] = CHAN;
	glink_send(GLINK_CMD_OPEN, LCID, sizeof(name), name, sizeof(name));
	printf("aprtest: OPEN \"%s\" lcid %u sent\n", CHAN, LCID);
	if (wait_for(&local_open, 0, 3000)) {
		printf("aprtest: no OPEN_ACK in 3 s\n");
		return 1;
	}
	wait_for(&remote_open, 0, 500);	/* the ADSP may (re)send its OPEN; fine if not */
	wait_for(NULL, ~0u, 300);	/* collect its intents */
	printf("aprtest: channel up (rcid %d, remote open %d, %d remote intents)\n", rcid, remote_open, r_n);

	int ready = 0;
	if (apr_send(APR_SVC_CORE, AVCS_GET_STATE, 1, NULL, 0) == 0 && wait_for(NULL, AVCS_GET_STATE_RSP, 3000) == 0) {
		if (got_op == AVCS_GET_STATE_RSP) {
			ready = got_len && got_payload[0] == 1;
			printf("aprtest: adsp state %u (%s)\n", got_len ? got_payload[0] : 0, ready ? "ready" : "not ready");
		}
	} else {
		printf("aprtest: GET_STATE: no reply in 3 s\n");
	}

	if (apr_send(APR_SVC_CORE, AVCS_GET_FWK_VERSION, 2, NULL, 0) == 0 &&
	    wait_for(NULL, AVCS_GET_FWK_VERSION_RSP, 3000) == 0 && got_op == AVCS_GET_FWK_VERSION_RSP && got_len >= 5) {
		/* build major, minor, branch, subbranch, num_services; then {service id, api version, api branch} */
		printf("aprtest: fwk %u.%u.%u.%u, %u services\n", got_payload[0], got_payload[1], got_payload[2],
		       got_payload[3], got_payload[4]);
		for (uint32_t i = 0; i < got_payload[4] && 5 + 3 * i + 2 < got_len; i++)
			printf("aprtest:   svc %x api %u branch %u\n", got_payload[5 + 3 * i], got_payload[6 + 3 * i],
			       got_payload[7 + 3 * i]);
	} else if (apr_send(APR_SVC_CORE, AVCS_GET_VERSIONS, 3, NULL, 0) == 0 &&
		   wait_for(NULL, AVCS_GET_VERSIONS_RSP, 3000) == 0 && got_op == AVCS_GET_VERSIONS_RSP && got_len) {
		/* older firmware: num_services, then {service id, version} */
		printf("aprtest: %u services\n", got_payload[0]);
		for (uint32_t i = 0; i < got_payload[0] && 1 + 2 * i + 1 < got_len; i++)
			printf("aprtest:   svc %x version %x\n", got_payload[1 + 2 * i], got_payload[2 + 2 * i]);
	} else {
		printf("aprtest: no version reply\n");
	}

	/* close ours so a rerun starts clean (the ADSP answers CLOSE_ACK and may re-OPEN) */
	glink_send(GLINK_CMD_CLOSE, LCID, 0, NULL, 0);
	wait_for(NULL, ~0u, 200);
	return ready ? 0 : 1;
}
