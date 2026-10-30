/*
 * radio: the WLAN radio after WLFW mission mode, from userland. A port of ios-topaz's TopazModem
 * (patches 0123 and 0125: ath10k snoc.c/ce.c/htc.c/wmi-tlv.c/htt.c for WCN3990) to /dev/pmem:
 *  - apps SMMU: the WLAN stream 0x640 (mask 1) gets a context bank with translation off, the way
 *    ABL's own streams and our USB stream run (sweet_usb.c ud_smmu_setup): the WLAN DMA then uses
 *    physical addresses. Must be in place before the WLAN firmware touches host memory.
 *  - copy engines at 0x18800000 + 0x240000, polled (no interrupts); rings and buffers in RAM below
 *    4 GB that XNU does not manage (0xE0000000: /dev/pmem maps it as device memory, so no cache
 *    maintenance is needed; WMI host memory chunks only take 32-bit addresses).
 *  - HTC: READY on ep0, connect HTT_DATA and WMI_CONTROL, SETUP_COMPLETE_EX.
 *  - WMI (TLV): SERVICE_READY -> INIT -> READY; HTT version, frag bank, rx ring, aggr (without the
 *    host rx ring no beacon reaches the scan); then a channel list, a station vdev and a passive
 *    scan (nothing is transmitted). Every beacon/probe response heard is kept; radio_summary()
 *    prints them.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonKeyDerivation.h>
#include <CommonCrypto/CommonSymmetricKeywrap.h>
#include "pmem.h"
#include "link.h"
#include "radio.h"
#include "smmu.h"

#define LOG(fmt, ...)	printf("[%6llu] radio: " fmt "\n", (unsigned long long)link_ms(), ##__VA_ARGS__)
#define MIN(a, b)	((a) < (b) ? (a) : (b))

const uint8_t radio_mac[6] = { 0x02, 0x50, 0x49, 0x58, 0x45, 0x4c };	/* what WLFW was given (wlfw.c) */

/* ---------------------------------------------------------------- host memory */

#define POOL_PA		0xE0000000ull
#define POOL_SIZE	0x01000000ull
static uint64_t pool_next = POOL_PA;

static void pool_zero(uint64_t pa, uint64_t len)
{
	static const uint8_t z[65536];
	for (uint64_t o = 0; o < len; o += sizeof(z))
		pm_wr(pa + o, z, MIN(len - o, (uint64_t)sizeof(z)));
}

/* zeroed, page aligned; 0 when the pool is used up */
static uint64_t pool_alloc(uint64_t size)
{
	size = (size + 0xfff) & ~0xfffull;
	if (pool_next + size > POOL_PA + POOL_SIZE)
		return 0;
	uint64_t pa = pool_next;
	pool_next += size;
	pool_zero(pa, size);
	return pa;
}

/* pmem device accesses are 32-bit: lengths round up to 4 */
static void mem_rd(uint64_t pa, void *buf, uint32_t len)
{
	pm_rd(pa, buf, (len + 3) & ~3u);
}

static void mem_wr(uint64_t pa, const void *buf, uint32_t len)
{
	pm_wr(pa, buf, (len + 3) & ~3u);
}

/* ---------------------------------------------------------------- apps SMMU */

#define WLAN_SID	0x240	/* sunfish icnss iommus (flame: 0x640) */
#define WLAN_SID_MASK	0x1

static int smmu_done;

int smmu_wlan_identity(void)
{
	smmu_done = smmu_identity("WLAN", WLAN_SID, WLAN_SID_MASK) == 0;
	return smmu_done ? 0 : -1;
}

/* ---------------------------------------------------------------- copy engines */

#define WLAN_PA		0x18800000ull
#define CE_PA		(WLAN_PA + 0x240000)
#define SHADOW_PA	(WLAN_PA + 0x32000)
#define CE_COUNT	12
#define SR_BASE_LO	0x00
#define SR_BASE_HI	0x04
#define SR_SIZE		0x08
#define DR_BASE_LO	0x0c
#define DR_BASE_HI	0x10
#define DR_SIZE		0x14
#define CTRL1		0x18
#define SR_WR_IDX	0x3c
#define DR_WR_IDX	0x40
#define CUR_DRRI	0x48
#define SRC_WM		0x4c
#define DST_WM		0x50
#define WRAP_RRI_LO	0xc004		/* ce wrapper 0x24c000 + 4, from CE0 */
#define WRAP_RRI_HI	0xc008
#define CTRL1_DMAX	0x0000ffffu
#define CTRL1_SRC_SWAP	(1u << 17)
#define CTRL1_DST_SWAP	(1u << 18)
#define CTRL1_RRI_UPD	(1u << 19)
#define ADDR_HI_MASK	0x1fu
#define ATTR_DIS_INTR	0x08
#define RX_BUF_SIZE	2048
#define RX_POST		32
#define TX_SLOTS_MAX	64
#define DESC		16		/* u64 addr, u16 nbytes, u16 flags, u32 toeplitz */

struct ce_attr { uint16_t src_n, src_max, dst_n, flags; };

/* host_ce_config_wlan (ath10k snoc.c) */
static const struct ce_attr ce_attr[CE_COUNT] = {
	{ 16, 2048, 0, 0 }, { 0, 2048, 512, 0 }, { 0, 2048, 64, 0 }, { 32, 2048, 0, 0 },
	{ 2048, 256, 0, ATTR_DIS_INTR }, { 0, 512, 512, 0 }, { 0, 0, 0, 0 }, { 2, 2048, 2, 0 },
	{ 0, 2048, 128, 0 }, { 0, 2048, 512, 0 }, { 0, 2048, 512, 0 }, { 0, 2048, 512, 0 },
};

struct ce_state {
	uint64_t src_pa, dst_pa, rx_pa, tx_pa;
	uint32_t src_n, dst_n, src_wr, dst_wr, dst_sw;
	uint32_t rx, tx, tx_full;
};
static struct ce_state ce[CE_COUNT];
static uint64_t rri_pa;
static int ce_up;

static int ce_host_rx(uint32_t c) { return ce_attr[c].dst_n != 0 && c != 7; }
static int ce_host_tx(uint32_t c) { return c == 0 || c == 3 || c == 4; }
static uint32_t ce_tx_slots(uint32_t c) { return MIN(ce[c].src_n, (uint32_t)TX_SLOTS_MAX); }
static uint32_t ce_tx_sz(uint32_t c) { return ce_attr[c].src_max; }
static uint32_t pow2(uint32_t n) { uint32_t p = 1; while (p < n) p <<= 1; return p; }
static uint32_t ce_rd(uint32_t c, uint32_t o) { return rd32(CE_PA + c * 0x1000ull + o); }
static void ce_wr(uint32_t c, uint32_t o, uint32_t v) { wr32(CE_PA + c * 0x1000ull + o, v); }

static void desc_put(uint64_t pa, uint64_t addr, uint16_t nbytes, uint16_t flags)
{
	uint32_t w[4] = { (uint32_t)addr, (uint32_t)(addr >> 32) & ADDR_HI_MASK, nbytes | (uint32_t)flags << 16, 0 };
	pm_wr(pa, w, sizeof(w));
}

static void ce_post(uint32_t c)
{
	struct ce_state *s = &ce[c];
	uint32_t slot = s->dst_wr;
	desc_put(s->dst_pa + (uint64_t)slot * DESC, s->rx_pa + (uint64_t)(slot % RX_POST) * RX_BUF_SIZE, 0, 0);
	s->dst_wr = (slot + 1) & (s->dst_n - 1);
}

static int ce_start(void)
{
	if (ce_up)
		return 0;
	if (!smmu_done) {
		LOG("ce: the WLAN SMMU stream is not set up: no DMA, not starting");
		return -1;
	}
	rri_pa = pool_alloc(0x1000);
	for (uint32_t c = 0; c < CE_COUNT; c++) {
		struct ce_state *s = &ce[c];
		s->src_n = ce_attr[c].src_n ? pow2(ce_attr[c].src_n) : 0;
		s->dst_n = ce_attr[c].dst_n ? pow2(ce_attr[c].dst_n) : 0;
		if (s->src_n)
			s->src_pa = pool_alloc((uint64_t)s->src_n * DESC);
		if (s->dst_n)
			s->dst_pa = pool_alloc((uint64_t)s->dst_n * DESC);
		if (ce_host_rx(c))
			s->rx_pa = pool_alloc(RX_POST * RX_BUF_SIZE);
		if (ce_host_tx(c))
			s->tx_pa = pool_alloc((uint64_t)ce_tx_slots(c) * ce_tx_sz(c));
		if ((s->src_n && !s->src_pa) || (s->dst_n && !s->dst_pa) || (ce_host_rx(c) && !s->rx_pa) ||
		    (ce_host_tx(c) && !s->tx_pa) || !rri_pa) {
			LOG("ce: out of host memory");
			return -1;
		}
	}
	LOG("ce: host memory %llx..%llx", (unsigned long long)POOL_PA, (unsigned long long)pool_next);
	/* the CE writes its read indices to rri[ce] (source [15:0], destination [31:16]) */
	wr32(CE_PA + WRAP_RRI_LO, (uint32_t)rri_pa);
	wr32(CE_PA + WRAP_RRI_HI, (uint32_t)(rri_pa >> 32) & ADDR_HI_MASK);
	for (uint32_t c = 0; c < CE_COUNT; c++)
		ce_wr(c, CTRL1, ce_rd(c, CTRL1) | CTRL1_RRI_UPD);
	for (uint32_t c = 0; c < CE_COUNT; c++) {
		struct ce_state *s = &ce[c];
		if (s->src_n) {				/* ath10k_ce_init_src_ring */
			s->src_wr = ce_rd(c, SR_WR_IDX) & (s->src_n - 1);
			ce_wr(c, SR_BASE_LO, (uint32_t)s->src_pa);
			ce_wr(c, SR_BASE_HI, (uint32_t)(s->src_pa >> 32) & ADDR_HI_MASK);
			ce_wr(c, SR_SIZE, s->src_n);
			ce_wr(c, CTRL1, (ce_rd(c, CTRL1) & ~(CTRL1_DMAX | CTRL1_SRC_SWAP)) | ce_attr[c].src_max);
			ce_wr(c, SRC_WM, s->src_n);
		}
		if (s->dst_n) {				/* ath10k_ce_init_dest_ring */
			s->dst_sw = ce_rd(c, CUR_DRRI) & (s->dst_n - 1);
			s->dst_wr = ce_rd(c, DR_WR_IDX) & (s->dst_n - 1);
			ce_wr(c, DR_BASE_LO, (uint32_t)s->dst_pa);
			ce_wr(c, DR_BASE_HI, (ce_rd(c, DR_BASE_HI) & ~ADDR_HI_MASK) | ((uint32_t)(s->dst_pa >> 32) & ADDR_HI_MASK));
			ce_wr(c, DR_SIZE, s->dst_n);
			ce_wr(c, CTRL1, ce_rd(c, CTRL1) & ~CTRL1_DST_SWAP);
			ce_wr(c, DST_WM, s->dst_n);
		}
	}
	for (uint32_t c = 0; c < CE_COUNT; c++) {	/* ath10k_snoc_rx_post */
		if (!ce_host_rx(c))
			continue;
		for (uint32_t i = 0; i < RX_POST; i++)
			ce_post(c);
		ce_wr(c, DR_WR_IDX, ce[c].dst_wr);
	}
	ce_up = 1;
	LOG("ce: %u rings programmed, rx buffers posted", CE_COUNT);
	return 0;
}

static void htc_rx(uint32_t c, const uint8_t *d, uint32_t len);

static void ce_poll(void)
{
	uint32_t rri[CE_COUNT];
	static uint8_t buf[RX_BUF_SIZE];
	if (!ce_up)
		return;
	mem_rd(rri_pa, rri, sizeof(rri));
	for (uint32_t c = 0; c < CE_COUNT; c++) {
		struct ce_state *s = &ce[c];
		if (!ce_host_rx(c) || ((rri[c] >> 16) & (s->dst_n - 1)) == s->dst_sw)
			continue;
		int posted = 0;
		for (int k = 0; k < 16; k++) {
			uint32_t slot = s->dst_sw, w = rd32(s->dst_pa + (uint64_t)slot * DESC + 8), n = w & 0xffff;
			if (n == 0)
				break;			/* index ahead of the descriptor: next round */
			if (n > RX_BUF_SIZE)
				n = RX_BUF_SIZE;
			mem_rd(s->rx_pa + (uint64_t)(slot % RX_POST) * RX_BUF_SIZE, buf, n);
			wr32(s->dst_pa + (uint64_t)slot * DESC + 8, 0);
			s->dst_sw = (slot + 1) & (s->dst_n - 1);
			s->rx++;
			htc_rx(c, buf, n);
			ce_post(c);
			posted = 1;
		}
		if (posted) {
			ce_wr(c, DR_WR_IDX, s->dst_wr);
			link_traffic++;	/* O12 */
		}
	}
}

static int ce_send(uint32_t c, const void *data, uint32_t len, uint32_t transfer_id)
{
	if (!ce_up || c >= CE_COUNT || !ce_host_tx(c) || len > ce_tx_sz(c))
		return -1;
	struct ce_state *s = &ce[c];
	uint32_t mask = s->src_n - 1, sw = rd32(rri_pa + 4 * c) & 0xffff;
	if (((sw - 1 - s->src_wr) & mask) == 0 || ((s->src_wr - sw) & mask) >= ce_tx_slots(c) - 1) {
		s->tx_full++;
		return -1;
	}
	uint32_t slot = s->src_wr;
	uint64_t bpa = s->tx_pa + (uint64_t)(slot % ce_tx_slots(c)) * ce_tx_sz(c);
	mem_wr(bpa, data, len);
	desc_put(s->src_pa + (uint64_t)slot * DESC, bpa, (uint16_t)len, (uint16_t)((transfer_id << 4) & 0xfff0));
	s->src_wr = (slot + 1) & mask;
	s->tx++;
	wr32(SHADOW_PA + 4 * c, s->src_wr);		/* shadow SR write index of CE c */
	return 0;
}

/* ---------------------------------------------------------------- HTC (ath10k htc.c) */

#define HTC_MSG_READY		1
#define HTC_MSG_CONNECT		2
#define HTC_MSG_CONNECT_RESP	3
#define HTC_MSG_SETUP_COMPLETE_EX 5
#define HTC_FLAG_NEED_CREDIT_UPDATE 0x01
#define HTC_FLAG_TRAILER	0x02
#define HTC_CONN_NO_CREDIT_FLOW	0x0008
#define SVC_RSVD_CTRL		0x0001
#define SVC_WMI_CONTROL		0x0100
#define SVC_HTT_DATA		0x0300
#define EP_MAX			8

struct htc_hdr { uint8_t eid, flags; uint16_t len; uint8_t trailer, seq; uint16_t pad; } __attribute__((packed));
struct htc_ep { uint16_t svc, max_msg; uint8_t ul, dl, seq, credits, up, credit_flow; uint32_t rx; };
static struct htc_ep ep[EP_MAX];
static uint8_t wmi_eid = 0xff, htt_eid = 0xff;
static int htc_ready, htc_setup;

static int htc_send(uint8_t eid, const void *payload, uint32_t len)
{
	static uint8_t b[2048];
	struct htc_hdr *h = (struct htc_hdr *)b;
	if (eid >= EP_MAX || len + sizeof(*h) > sizeof(b))
		return -1;
	struct htc_ep *e = &ep[eid];
	if (e->credit_flow) {
		if (e->credits == 0)
			return -1;
		e->credits--;
	}
	memset(h, 0, sizeof(*h));
	h->eid = eid;
	h->len = (uint16_t)len;
	h->flags = e->credit_flow ? HTC_FLAG_NEED_CREDIT_UPDATE : 0;
	h->seq = e->seq++;
	memcpy(b + sizeof(*h), payload, len);
	if (ce_send(e->ul, b, (uint32_t)sizeof(*h) + len, eid)) {
		LOG("htc: send on ep %u (ce%u) FAILED", eid, e->ul);
		return -1;
	}
	return 0;
}

static void htc_connect(uint16_t svc)
{
	uint8_t m[8] = { 0 };
	uint16_t id = HTC_MSG_CONNECT, flags = svc == SVC_WMI_CONTROL ? (uint16_t)(1u << 8) : HTC_CONN_NO_CREDIT_FLOW;
	memcpy(m, &id, 2);
	memcpy(m + 2, &svc, 2);
	memcpy(m + 4, &flags, 2);
	htc_send(0, m, sizeof(m));
}

/* WMI waits here while its endpoint has no credit */
#define WMIQ_LEN	32
#define WMIQ_MAX	2040
static uint8_t wmiq[WMIQ_LEN][WMIQ_MAX];
static uint16_t wmiq_len[WMIQ_LEN];
static uint32_t wmiq_head, wmiq_tail;

static void wmiq_pump(void)
{
	while (wmiq_head != wmiq_tail && wmi_eid != 0xff && ep[wmi_eid].credits != 0) {
		uint32_t i = wmiq_head % WMIQ_LEN;
		if (htc_send(wmi_eid, wmiq[i], wmiq_len[i]))
			break;
		wmiq_head++;
	}
}

static int htc_wmi_send(const void *data, uint32_t len)
{
	if (wmi_eid == 0xff || len > WMIQ_MAX)
		return -1;
	if (wmiq_head == wmiq_tail && ep[wmi_eid].credits != 0)
		return htc_send(wmi_eid, data, len);
	if (wmiq_tail - wmiq_head >= WMIQ_LEN) {
		LOG("htc: wmi queue full, message dropped");
		return -1;
	}
	uint32_t i = wmiq_tail++ % WMIQ_LEN;
	memcpy(wmiq[i], data, len);
	wmiq_len[i] = (uint16_t)len;
	return 0;
}

static void wmi_rx(const uint8_t *d, uint32_t len);
static void htt_rx(const uint8_t *p, uint32_t len);

static void htc_rx(uint32_t c, const uint8_t *d, uint32_t len)
{
	struct htc_hdr h;
	if (len < sizeof(h))
		return;
	memcpy(&h, d, sizeof(h));
	const uint8_t *p = d + sizeof(h);
	uint32_t plen = MIN((uint32_t)h.len, len - (uint32_t)sizeof(h));
	if ((h.flags & HTC_FLAG_TRAILER) && h.trailer <= plen) {
		const uint8_t *t = p + plen - h.trailer;	/* records {id, len, data}; id 1 = credits */
		for (uint32_t o = 0; o + 2 <= h.trailer && o + 2 + t[o + 1] <= h.trailer; o += 2 + t[o + 1]) {
			if (t[o] != 1)
				continue;
			for (uint32_t i = 0; i + 1 < t[o + 1]; i += 2)
				if (t[o + 2 + i] < EP_MAX)
					ep[t[o + 2 + i]].credits = (uint8_t)(ep[t[o + 2 + i]].credits + t[o + 3 + i]);
		}
		plen -= h.trailer;
		wmiq_pump();
	}
	if (h.eid < EP_MAX)
		ep[h.eid].rx++;
	if (h.eid == 0) {
		uint16_t msg = 0;
		if (plen >= 2)
			memcpy(&msg, p, 2);
		if (msg == HTC_MSG_READY && plen >= 8) {
			uint16_t cc, cs;
			memcpy(&cc, p + 2, 2);
			memcpy(&cs, p + 4, 2);
			htc_ready = 1;
			ep[0].svc = SVC_RSVD_CTRL;
			ep[0].ul = 0;
			ep[0].dl = 2;
			ep[0].up = 1;
			LOG("*** HTC READY on ce%u: credits %u x %u, %u endpoints ***", c, cc, cs, p[6]);
			htc_connect(SVC_HTT_DATA);		/* ath10k_core_start: htt, then wmi */
		} else if (msg == HTC_MSG_CONNECT_RESP && plen >= 8) {
			uint16_t svc, max;
			memcpy(&svc, p + 2, 2);
			memcpy(&max, p + 6, 2);
			uint8_t status = p[4], eid = p[5];
			LOG("htc: connect resp svc %#x status %u eid %u max_msg %u", svc, status, eid, max);
			if (status != 0 || eid >= EP_MAX)
				return;
			ep[eid].svc = svc;
			ep[eid].max_msg = max;
			ep[eid].ul = svc == SVC_WMI_CONTROL ? 3 : 4;
			ep[eid].dl = svc == SVC_WMI_CONTROL ? 2 : 1;
			ep[eid].up = 1;
			ep[eid].credit_flow = svc == SVC_WMI_CONTROL;
			ep[eid].credits = ep[eid].credit_flow ? 1 : 0;
			if (svc == SVC_HTT_DATA) {
				htt_eid = eid;
				htc_connect(SVC_WMI_CONTROL);
			} else if (svc == SVC_WMI_CONTROL) {
				wmi_eid = eid;
				uint8_t m[12] = { 0 };
				uint16_t id = HTC_MSG_SETUP_COMPLETE_EX;
				memcpy(m, &id, 2);
				htc_setup = !htc_send(0, m, sizeof(m));
				LOG("htc: SETUP_COMPLETE_EX %s", htc_setup ? "sent" : "FAILED");
			}
		} else if (plen) {				/* (empty ones only carry credit trailers) */
			LOG("htc: ep0 message %u (%u bytes)", msg, plen);
		}
	} else if (h.eid == wmi_eid) {
		wmi_rx(p, plen);
	} else if (h.eid == htt_eid) {
		htt_rx(p, plen);
	}
}

/* ---------------------------------------------------------------- WMI (TLV, ath10k wmi-tlv.c) */

#define WMI_EV_SERVICE_READY	0x1
#define WMI_EV_READY		0x2
#define WMI_EV_SERVICE_AVAILABLE 0x3
#define WMI_EV_SCAN		0x3001
#define WMI_EV_MGMT_RX		0x7001
#define WMI_CMD_INIT		0x1
#define CMD_START_SCAN		0x3001
#define CMD_SCAN_CHAN_LIST	0x3003
#define CMD_VDEV_CREATE		0x5001
#define TAG_ARRAY_UINT32	0x10
#define TAG_ARRAY_BYTE		0x11
#define TAG_ARRAY_STRUCT	0x12
#define TAG_ARRAY_FIXED_STRUCT	0x13
#define TAG_SERVICE_READY	0x20
#define TAG_HOST_MEM_REQ	0x22
#define TAG_READY_EVENT		0x23
#define TAG_SCAN_EVENT		0x24
#define TAG_MGMT_RX_HDR		0x2c
#define TAG_INIT_CMD		0x4a
#define TAG_RESOURCE_CONFIG	0x4b
#define TAG_HOST_MEM_CHUNK	0x4c
#define TAG_START_SCAN_CMD	0x4d
#define TAG_SCAN_CHAN_LIST_CMD	0x4f
#define TAG_CHANNEL		0x50
#define TAG_VDEV_CREATE_CMD	0x56
#define TAG_SERVICE_AVAILABLE	0x22f
#define ABI_VER0		0x01000000u
#define ABI_VER1		53
#define ABI_NS0			0x5f414351u	/* "QCA_" */
#define ABI_NS1			0x00004c4du	/* "ML" */
#define SVC_RX_FULL_REORDER	65
#define SVC_EXTEND_ADDRESS	179
#define NUM_PEERS		33	/* ath10k wcn3990: TARGET_HL_TLV_NUM_PEERS */
#define NUM_VDEVS		4
#define MAX_MEM_REQS		8
#define MAX_MEM_TOTAL		(4u << 20)

struct wmi_mem_req { uint32_t req_id, unit_size, num_unit_info, num_units; };
static uint32_t wmi_svc_map[32], wmi_svc_ext[4], wmi_nreq;
static struct wmi_mem_req wmi_req[MAX_MEM_REQS];
static int wmi_init_sent, wmi_ready, wmi_seen_bmap, wmi_seen_reqs;
static uint32_t wmi_eeprom_rd = 0xffffffff;	/* HAL_REG_CAPABILITIES.eeprom_rd */
static uint8_t wmi_mac[6];
static uint32_t wmi_rf_chains;
/*
 * Test hook: /var/local/wlan-steps.conf, a hex mask of ath10k station steps we used to skip (default all on):
 * bit 0 keepalive off, bit 1 sta ps params, bit 2 rts threshold, bit 3 vdev start in HT20 mode;
 * bits 4-6 (ours, before vdev start): sta ps mode off, pdev tx power limits, vdev wmm;
 * probes: bit 7 vdev start on 2437 MHz instead of a scan, bit 8 no HTT setup.
 */
static uint32_t w_steps = 0x7f;
static int w_steps_read;
static uint32_t steps(void)
{
	if (!w_steps_read) {
		w_steps_read = 1;
		FILE *f = fopen("/var/local/wlan-steps.conf", "r");
		unsigned v;
		if (f && fscanf(f, "%x", &v) == 1)
			w_steps = v;
		if (f)
			fclose(f);
		printf("radio: steps %#x\n", w_steps);
	}
	return w_steps;
}
static int w_pdev_param(uint32_t param, uint32_t value);
static int w_vdev_start(uint32_t freq, uint32_t bcn, uint32_t dtim);

/* ath10k WMI_SERVICE_IS_ENABLED: 4 service bits per 32-bit word (sizeof(u32)) */
static int wmi_svc(uint32_t bit)
{
	if (bit < 128)
		return (wmi_svc_map[bit / 4] & (1u << (bit % 4))) != 0;
	bit -= 128;
	return bit < 128 && (wmi_svc_ext[bit / 32] & (1u << (bit % 32))) != 0;
}

static uint8_t *put_tlv(uint8_t *p, uint16_t tag, uint32_t len)
{
	uint16_t l = (uint16_t)len;
	memcpy(p, &l, 2);
	memcpy(p + 2, &tag, 2);
	return p + 4;
}

static uint8_t *put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); return p + 2; }
static uint8_t *put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); return p + 4; }
static uint8_t *put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); return p + 8; }

static int wmi_send(uint32_t cmd, const void *tlvs, uint32_t len)
{
	static uint8_t m[2048];
	if (len + 4 > sizeof(m))
		return -1;
	put32(m, cmd & 0xffffff);
	memcpy(m + 4, tlvs, len);
	return htc_wmi_send(m, len + 4);
}

/* the TLVs of SERVICE_READY (ath10k_wmi_tlv_svc_rdy_parse) */
static void wmi_svc_ready(const uint8_t *p, uint32_t len)
{
	for (uint32_t o = 0; o + 4 <= len;) {
		uint16_t l, tag;
		memcpy(&l, p + o, 2);
		memcpy(&tag, p + o + 2, 2);
		if (o + 4 + l > len)
			break;
		const uint8_t *v = p + o + 4;
		if (tag == TAG_SERVICE_READY && l >= 40) {
			uint32_t fw, a0, rf;
			memcpy(&fw, v, 4);
			memcpy(&a0, v + 4, 4);
			memcpy(&rf, v + 36, 4);
			wmi_rf_chains = rf;
			LOG("wmi: fw %08x abi %08x rf chains %u", fw, a0, rf);
		} else if (tag == 0x21 && l >= 4) {	/* WMI_TLV_TAG_STRUCT_HAL_REG_CAPABILITIES */
			memcpy(&wmi_eeprom_rd, v, 4);
			LOG("wmi: eeprom regdomain %#x", wmi_eeprom_rd);
		} else if (tag == TAG_ARRAY_UINT32 && !wmi_seen_bmap) {
			wmi_seen_bmap = 1;
			memcpy(wmi_svc_map, v, MIN((uint32_t)l, (uint32_t)sizeof(wmi_svc_map)));
		} else if (tag == TAG_ARRAY_STRUCT && !wmi_seen_reqs) {
			wmi_seen_reqs = 1;
			for (uint32_t q = 0; q + 4 <= l;) {
				uint16_t ql, qt;
				memcpy(&ql, v + q, 2);
				memcpy(&qt, v + q + 2, 2);
				if (q + 4 + ql > l)
					break;
				if (qt == TAG_HOST_MEM_REQ && ql >= sizeof(struct wmi_mem_req) && wmi_nreq < MAX_MEM_REQS) {
					memcpy(&wmi_req[wmi_nreq], v + q + 4, sizeof(struct wmi_mem_req));
					LOG("wmi: mem_req %u: id %u unit %u info %#x units %u", wmi_nreq, wmi_req[wmi_nreq].req_id,
					    wmi_req[wmi_nreq].unit_size, wmi_req[wmi_nreq].num_unit_info, wmi_req[wmi_nreq].num_units);
					wmi_nreq++;
				}
				q += 4 + ql;
			}
		}
		o += 4 + l;
	}
}

static void wmi_send_init(void)
{
	static uint8_t m[1024];
	uint8_t *p = m;
	uint32_t total = 0, n = wmi_nreq, len[MAX_MEM_REQS];
	uint64_t pa = 0;

	for (uint32_t i = 0; i < n; i++) {
		uint32_t units = wmi_req[i].num_units;
		if (wmi_req[i].num_unit_info & 6)
			units = NUM_PEERS + 1;
		else if (wmi_req[i].num_unit_info & 1)
			units = NUM_VDEVS + 1;
		len[i] = units * ((wmi_req[i].unit_size + 3) & ~3u);
		total += (len[i] + 0xfff) & ~0xfffu;
	}
	if (total && (total > MAX_MEM_TOTAL || !(pa = pool_alloc(total)))) {
		LOG("wmi: host memory %u bytes not given", total);
		n = 0;
	}
	memset(m, 0, sizeof(m));
	p = put_tlv(p, TAG_INIT_CMD, 24 + 4);
	p = put32(p, ABI_VER0);
	p = put32(p, ABI_VER1);
	p = put32(p, ABI_NS0);
	p = put32(p, ABI_NS1);
	p = put32(p, 0);
	p = put32(p, 0);
	p = put32(p, n);
	/* wmi_tlv_resource_config, 44 x u32, ath10k's values for wcn3990 */
	uint32_t rc[44] = { 0 };
	rc[0] = NUM_VDEVS;
	rc[1] = NUM_PEERS;
	if (wmi_svc(SVC_RX_FULL_REORDER)) {
		rc[2] = NUM_VDEVS;
		rc[3] = NUM_VDEVS;
	}
	rc[4] = 2;					/* peer keys */
	rc[5] = 33 * 2;					/* tids: TARGET_TLV_NUM_TIDS */
	rc[6] = 16;					/* ast skid limit: TARGET_HL_TLV_AST_SKID_LIMIT */
	rc[7] = 0x7;					/* tx chain mask */
	rc[8] = 0x7;					/* rx chain mask */
	rc[9] = 0x64; rc[10] = 0x64; rc[11] = 0x64; rc[12] = 0x28;
	rc[13] = 1;					/* rx decap: native wifi */
	rc[14] = 4;					/* scan max pending */
	rc[15] = NUM_VDEVS;				/* bmiss offload */
	rc[16] = NUM_VDEVS;				/* roam offload */
	rc[17] = 8;					/* roam ap profiles */
	rc[21] = 0x400;					/* tx dbglog size */
	rc[22] = 2;					/* wds entries */
	rc[27] = 2;					/* gtk offload */
	rc[28] = 1024 + 32;				/* msdu descs */
	rc[29] = 2;					/* frag entries */
	rc[30] = 1;					/* tdls vdevs */
	rc[31] = 0x20;					/* tdls conn table */
	rc[32] = 2;					/* beacon tx offload */
	rc[33] = 5;					/* mcast filter entries */
	rc[34] = 22;					/* wow filters */
	rc[35] = 6;					/* keepalive patterns */
	rc[37] = 1;
	rc[38] = 1;
	rc[43] = 1u << 9;				/* host_capab: WMI_TLV_FLAG_MGMT_BUNDLE_TX_COMPL as ath10k */
	p = put_tlv(p, TAG_RESOURCE_CONFIG, sizeof(rc));
	memcpy(p, rc, sizeof(rc));
	p += sizeof(rc);
	p = put_tlv(p, TAG_ARRAY_STRUCT, n * 20);
	for (uint32_t i = 0, off = 0; i < n; i++) {
		uint64_t cpa = pa + off;
		off += (len[i] + 0xfff) & ~0xfffu;
		uint8_t *c = put_tlv(p, TAG_HOST_MEM_CHUNK, 16);
		c = put32(c, wmi_req[i].req_id);
		c = put32(c, (uint32_t)cpa);
		c = put32(c, len[i]);
		put32(c, wmi_svc(SVC_EXTEND_ADDRESS) ? (uint32_t)(cpa >> 32) : 0);
		p += 20;
	}
	wmi_init_sent = !wmi_send(WMI_CMD_INIT, m, (uint32_t)(p - m));
	LOG("wmi: INIT (%u chunks, %u bytes host memory) %s", n, total, wmi_init_sent ? "sent" : "FAILED");
}

/* ---------------------------------------------------------------- the passive scan */

/* 2.4 GHz 1-13, 5 GHz UNII-1/2/2e/3 */
static const uint16_t scan_freqs[] = {
	2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462, 2467, 2472,
	5180, 5200, 5220, 5240, 5260, 5280, 5300, 5320,
	5500, 5520, 5540, 5560, 5580, 5600, 5620, 5640, 5660, 5680, 5700,
	5745, 5765, 5785, 5805, 5825,
};
#define NFREQ ((uint32_t)(sizeof(scan_freqs) / sizeof(scan_freqs[0])))
#define NFREQ_MAX 64
#define NBSS 64

struct bss {
	uint8_t bssid[6];
	char ssid[33];				/* printable */
	uint8_t raw_ssid[32], raw_ssid_len;
	uint8_t chan;
	int8_t rssi;
	uint16_t seen, capab, freq, bintval, ie_len;
	char sec[12];
	uint8_t ie[768];			/* the last beacon's elements */
	uint64_t last_ms;			/* link_ms() it was last heard */
};
static struct bss bss[NBSS];
static uint32_t nbss, scans, scan_frames;
static int scan_started, scanning;
static uint64_t next_scan_ms;
static int scan_asked;

static int scan_send_start(void)
{
	static uint8_t m[512];
	uint8_t *p = m, *c;
	/* test hook: /var/local/wlan-scan.conf lists the only frequencies to scan (MHz, one per line) */
	static uint32_t sf[NFREQ_MAX], nsf;
	static int sf_read;
	if (!sf_read) {
		sf_read = 1;
		FILE *f = fopen("/var/local/wlan-scan.conf", "r");
		unsigned v;
		while (f && nsf < NFREQ_MAX && fscanf(f, "%u", &v) == 1)
			sf[nsf++] = v;
		if (f)
			fclose(f);
		if (!nsf)
			for (; nsf < NFREQ; nsf++)
				sf[nsf] = scan_freqs[nsf];
		LOG("scan: %u frequencies%s", nsf, nsf == NFREQ ? "" : " (wlan-scan.conf)");
	}
	memset(m, 0, sizeof(m));
	c = put_tlv(p, TAG_START_SCAN_CMD, 100);
	c = put32(c, 0xa000 | (scans + 1));		/* scan_id */
	c = put32(c, 0xa000 | 1);			/* scan_req_id */
	c = put32(c, 0);				/* vdev */
	c = put32(c, 1);				/* priority low */
	c = put32(c, 0x001 | 0x002 | 0x004 | 0x008 | 0x100 | 0x010);	/* events */
	c = put32(c, 50);				/* dwell active */
	c = put32(c, 150);				/* dwell passive */
	c = put32(c, 50);				/* min rest */
	c = put32(c, 500);				/* max rest */
	c = put32(c, 0);
	c = put32(c, 0);
	c = put32(c, 0);
	c = put32(c, 20000);				/* max scan time */
	c = put32(c, 5);				/* probe delay */
	c = put32(c, (0x01 | 0x10) ^ 0x20);		/* passive, chan stat; probe-req filter inverted on TLV */
	c = put32(c, 0);
	c = put32(c, nsf);
	c = put32(c, 1);				/* bssids */
	c = put32(c, 0);				/* ssids */
	c = put32(c, 0);				/* ie_len */
	c = put32(c, 3);				/* num_probes */
	p = c + 16;					/* mac addr + mask: zero */
	c = put_tlv(p, TAG_ARRAY_UINT32, nsf * 4);
	for (uint32_t i = 0; i < nsf; i++)
		c = put32(c, sf[i]);
	p = put_tlv(c, TAG_ARRAY_FIXED_STRUCT, 0);
	c = put_tlv(p, TAG_ARRAY_FIXED_STRUCT, 8);	/* bssid: broadcast */
	memset(c, 0xff, 6);
	p = put_tlv(c + 8, TAG_ARRAY_BYTE, 0);
	return wmi_send(CMD_START_SCAN, m, (uint32_t)(p - m));
}

static void scan_start(void)
{
	static uint8_t m[1536];
	uint8_t *p = m, *c;

	if (scan_started)
		return;
	scan_started = 1;
	if (steps() & 0x200) {			/* probe: no regdomain, pdev params, dbglog or channel list */
		LOG("probe: regdomain/pdev/dbglog/chan list skipped");
		goto vdev_create;
	}
	/*
	 * sunfish: WMI_PDEV_SET_REGDOMAIN first (ath10k_regd_update; tag 0x51, cmd 0x4001), with the
	 * firmware's own eeprom code, FCC conformance limits. The HL3.2.2 firmware asserted in vdev
	 * start (first TX) without it and with every channel passive.
	 */
	{
		uint8_t rd[4 + 24] = { 0 }, *q = put_tlv(rd, 0x51, 24);
		uint32_t code = wmi_eeprom_rd != 0xffffffff ? (wmi_eeprom_rd & 0xffff) : 0;
		q = put32(q, 0);	/* pdev */
		q = put32(q, code);
		q = put32(q, code);
		q = put32(q, code);
		q = put32(q, 0x10);	/* CTL_FCC 2 GHz */
		put32(q, 0x10);		/* CTL_FCC 5 GHz */
		int rc = wmi_send(0x4001, rd, sizeof(rd));
		LOG("regdomain: %#x -> %d", code, rc);
	}
	/*
	 * ath10k_start's pdev setup. Above all the chain masks: INIT says 0x7 (3 chains), the firmware has 2 and
	 * ath10k narrows it here ((1 << num_rf_chains) - 1); without it the fw indexes per-chain state past 2.
	 */
	{
		uint32_t mask = (wmi_rf_chains >= 1 && wmi_rf_chains <= 3) ? (1u << wmi_rf_chains) - 1 : 3;
		static const uint32_t pp[][2] = { { 0x22, 1 }, { 0x0a, 1 }, { 0x2c, 1 }, { 0x23, 0 }, { 0x25, 1 } };
		int e = 0;
		for (uint32_t i = 0; i < sizeof(pp) / sizeof(pp[0]); i++)
			e |= w_pdev_param(pp[i][0], pp[i][1]);
		e |= w_pdev_param(1, mask) | w_pdev_param(2, mask);
		/* fw debug log: all modules verbose (ath10k_wmi_tlv_op_gen_dbglog_cfg; cmd 0x1d003, tag 0xbe) */
		uint8_t dl[4 + 8 + 4], *q = put_tlv(dl, 0xbe, 8);
		q = put32(q, 1);
		q = put32(q, 0xffffu << 16);
		put_tlv(q, TAG_ARRAY_UINT32, 0);
		LOG("dbglog: verbose %s", wmi_send(0x1d003, dl, sizeof(dl)) ? "FAILED" : "on");
		LOG("pdev: chain mask %#x (rf chains %u), pmf/bw/idle-ps/ani %s", mask, wmi_rf_chains, e ? "FAILED" : "ok");
	}
	/* channel list: 2.4 GHz 1-11 and 5 GHz 36-48 may transmit, the rest passive */
	p = put32(put_tlv(p, TAG_SCAN_CHAN_LIST_CMD, 4), NFREQ);
	p = put_tlv(p, TAG_ARRAY_STRUCT, NFREQ * (4 + 24));
	for (uint32_t i = 0; i < NFREQ; i++) {
		uint32_t mode = scan_freqs[i] < 4000 ? 1 : 0;
		int tx_ok = (scan_freqs[i] >= 2412 && scan_freqs[i] <= 2462) || (scan_freqs[i] >= 5180 && scan_freqs[i] <= 5240);
		uint32_t flags = mode | (tx_ok ? 0 : (1u << 7)) | (1u << 11) | (scan_freqs[i] < 4000 ? 0 : (1u << 12));
		c = put_tlv(p, TAG_CHANNEL, 24);
		c = put32(c, scan_freqs[i]);
		c = put32(c, scan_freqs[i]);
		c = put32(c, 0);
		c = put32(c, flags);
		c = put32(c, (40u << 8) | (40u << 16));
		p = put32(c, 40u << 8);
	}
	int a = wmi_send(CMD_SCAN_CHAN_LIST, m, (uint32_t)(p - m));
	if (0) {
vdev_create:
		a = 0;
	}
	uint8_t v[4 + 20] = { 0 };
	c = put_tlv(v, TAG_VDEV_CREATE_CMD, 20);
	c = put32(c, 0);				/* vdev 0 */
	c = put32(c, 2);				/* station */
	c = put32(c, 0);
	memcpy(c, radio_mac, 6);
	int b = wmi_send(CMD_VDEV_CREATE, v, sizeof(v));
	/* ath10k_add_interface: vdev NSS from the chain mask (2 streams), WMI_TLV_VDEV_PARAM_NSS 0x22 */
	{
		uint32_t nss = wmi_rf_chains >= 1 && wmi_rf_chains <= 3 ? wmi_rf_chains : 2;
		uint8_t vp[4 + 12], *q = put_tlv(vp, 0x5f, 12);
		q = put32(q, 0);
		q = put32(q, 0x22);
		put32(q, nss);
		LOG("vdev: nss %u %s", nss, wmi_send(0x5008, vp, sizeof(vp)) ? "FAILED" : "ok");
	}
	/* ath10k_add_interface for a station, after NSS */
	if (steps() & 1) {			/* ath10k_mac_vif_disable_keepalive: STA_KEEPALIVE 0x1c005, interval 0 */
		uint8_t ka[4 + 16 + 4 + 16] = { 0 }, *q = put_tlv(ka, 0x7f, 16);
		q = put32(q, 0);
		q = put32(q, 1);		/* enabled */
		q = put32(q, 1);		/* method: null frame */
		q = put32(q, 0);		/* interval 0: off */
		put_tlv(q, 0x80, 16);		/* arp response: zero */
		LOG("vdev: keepalive off %s", wmi_send(0x1c005, ka, sizeof(ka)) ? "FAILED" : "ok");
	}
	if (steps() & 2) {			/* sta ps: rx wake policy WAKE, tx wake threshold ALWAYS, pspoll count NO_MAX */
		static const uint32_t ps[3][2] = { { 0, 0 }, { 1, 1 }, { 2, 0 } };
		int e = 0;
		for (int i = 0; i < 3; i++) {
			uint8_t m[4 + 12], *q = put_tlv(m, 0x6d, 12);
			q = put32(q, 0);
			q = put32(q, ps[i][0]);
			put32(q, ps[i][1]);
			e |= wmi_send(0x9002, m, sizeof(m));
		}
		LOG("vdev: sta ps params %s", e ? "FAILED" : "ok");
	}
	if (steps() & 4) {			/* ath10k_mac_set_rts: vdev param 1, wiphy default (u32)-1 */
		uint8_t m[4 + 12], *q = put_tlv(m, 0x5f, 12);
		q = put32(q, 0);
		q = put32(q, 1);
		put32(q, 0xffffffffu);
		LOG("vdev: rts threshold %s", wmi_send(0x5008, m, sizeof(m)) ? "FAILED" : "ok");
	}
	if (steps() & 0x80) {			/* probe: vdev start on 2437 MHz, no scan at all */
		fflush(stdout);
		LOG("probe: vdev start 2437 MHz without a scan: %s", w_vdev_start(2437, 100, 1) ? "FAILED" : "sent");
		fflush(stdout);
		return;
	}
	scanning = !scan_send_start();
	LOG("scan: channel list (%u) %s, vdev %s, passive scan %s", NFREQ, a ? "FAILED" : "ok", b ? "FAILED" : "ok",
	    scanning ? "requested" : "FAILED");
}

static void scan_event(const uint8_t *t, uint32_t len)
{
	uint16_t tag;
	uint32_t e[6];
	if (len < 4 + sizeof(e))
		return;
	memcpy(&tag, t + 2, 2);
	if (tag != TAG_SCAN_EVENT)
		return;
	memcpy(e, t + 4, sizeof(e));
	if (e[0] == 0x008 || e[0] == 0x100 || e[0] == 0x004)
		return;					/* foreign channel / exit / bss channel */
	if (e[0] == 0x001)
		LOG("scan: STARTED");
	if (e[0] == 0x002 || e[0] == 0x040 || e[0] == 0x010) {
		scanning = 0;
		scans++;
		LOG("*** scan %u done (event %#x): %u networks, %u beacons/probe responses ***", scans, e[0], nbss,
		    scan_frames);
		radio_summary();
		next_scan_ms = scans < 3 ? link_ms() + 10000 : 0;
	}
}

/* what the beacon says about protection: RSN (AKM 8 = SAE), the WPA vendor IE, the privacy bit */
static void scan_security(const uint8_t *ie, uint32_t len, uint16_t capab, char *out, size_t n)
{
	int rsn = 0, wpa = 0, sae = 0, psk = 0, eap = 0;
	for (uint32_t o = 0; o + 2 <= len && o + 2 + ie[o + 1] <= len; o += 2 + ie[o + 1]) {
		const uint8_t *v = ie + o + 2;
		uint8_t l = ie[o + 1];
		if (ie[o] == 48 && l >= 8) {
			rsn = 1;
			uint32_t q = 2 + 4;			/* version, group cipher */
			uint16_t np = 0, na = 0;
			if (q + 2 <= l) {
				memcpy(&np, v + q, 2);
				q += 2 + 4u * np;
			}
			if (q + 2 <= l) {
				memcpy(&na, v + q, 2);
				q += 2;
				for (uint16_t i = 0; i < na && q + 4 <= l; i++, q += 4) {
					sae |= v[q + 3] == 8;
					psk |= v[q + 3] == 2 || v[q + 3] == 6;
					eap |= v[q + 3] == 1 || v[q + 3] == 5;
				}
			}
		} else if (ie[o] == 221 && l >= 4 && v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xf2 && v[3] == 1) {
			wpa = 1;
		}
	}
	if (rsn)
		snprintf(out, n, "%s%s", sae ? (psk ? "WPA2/3" : "WPA3") : eap ? "WPA2-EAP" : "WPA2", wpa ? "+WPA" : "");
	else if (wpa)
		snprintf(out, n, "WPA");
	else
		snprintf(out, n, (capab & 0x10) ? "WEP" : "open");
}

static void sta_mgmt_rx(const uint8_t *f, uint32_t len);

static void scan_mgmt_rx(const uint8_t *t, uint32_t len)
{
	uint32_t hdr[6];
	const uint8_t *f = NULL;
	uint32_t flen = 0;
	int have_hdr = 0;

	for (uint32_t o = 0; o + 4 <= len;) {
		uint16_t l, tag;
		memcpy(&l, t + o, 2);
		memcpy(&tag, t + o + 2, 2);
		if (o + 4 + l > len)
			break;
		if (tag == TAG_MGMT_RX_HDR && l >= 24 && !have_hdr) {
			memcpy(hdr, t + o + 4, sizeof(hdr));
			have_hdr = 1;
		} else if (tag == TAG_ARRAY_BYTE && !f) {
			f = t + o + 4;
			flen = l;
		}
		o += 4 + l;
	}
	if (!have_hdr || !f)
		return;
	flen = MIN(hdr[4], flen);
	if (flen >= 24 && (f[0] & 0x0c) == 0 && (f[0] >> 4) != 8 && (f[0] >> 4) != 5) {
		sta_mgmt_rx(f, flen);			/* auth, assoc response, deauth, disassoc */
		return;
	}
	if (flen < 36 || (f[0] & 0x0c) != 0 || ((f[0] >> 4) != 8 && (f[0] >> 4) != 5))
		return;					/* beacons and probe responses only */
	scan_frames++;
	int32_t rssi = (int32_t)hdr[1] - 95;		/* snr over ath10k's default noise floor */
	uint32_t chan = hdr[0];
	char ssid[33] = "";
	for (uint32_t o = 36; o + 2 <= flen && o + 2 + f[o + 1] <= flen; o += 2 + f[o + 1]) {
		if (f[o] == 0) {
			uint32_t n = MIN((uint32_t)f[o + 1], 32u);
			for (uint32_t i = 0; i < n; i++) {
				uint8_t ch = f[o + 2 + i];
				ssid[i] = (ch >= 0x20 && ch != 0x7f) ? (char)ch : '?';
			}
			ssid[n] = 0;
			if (n == 0 || f[o + 2] == 0)
				snprintf(ssid, sizeof(ssid), "<hidden>");
		} else if (f[o] == 3 && f[o + 1] == 1) {
			chan = f[o + 2];
		}
	}
	struct bss *b = NULL;
	int fresh = 0;
	for (uint32_t i = 0; i < nbss && !b; i++)
		if (!memcmp(bss[i].bssid, f + 16, 6))
			b = &bss[i];
	if (!b) {
		if (nbss >= NBSS)
			return;
		b = &bss[nbss++];
		memset(b, 0, sizeof(*b));
		memcpy(b->bssid, f + 16, 6);
		fresh = 1;
	}
	b->seen++;
	b->last_ms = link_ms();
	b->rssi = (int8_t)rssi;
	b->chan = (uint8_t)chan;
	b->freq = (uint16_t)(chan == 14 ? 2484 : chan < 14 ? 2407 + 5 * chan : 5000 + 5 * chan);
	memcpy(&b->bintval, f + 32, 2);
	memcpy(&b->capab, f + 34, 2);
	b->ie_len = (uint16_t)MIN(flen - 36, (uint32_t)sizeof(b->ie));
	memcpy(b->ie, f + 36, b->ie_len);
	snprintf(b->ssid, sizeof(b->ssid), "%s", ssid);
	for (uint32_t o = 36; o + 2 <= flen && o + 2 + f[o + 1] <= flen; o += 2 + f[o + 1]) {
		if (f[o] == 0) {
			b->raw_ssid_len = (uint8_t)MIN((uint32_t)f[o + 1], 32u);
			memcpy(b->raw_ssid, f + o + 2, b->raw_ssid_len);
			break;
		}
	}
	scan_security(b->ie, b->ie_len, b->capab, b->sec, sizeof(b->sec));
	if (fresh)
		LOG("net %2u: %02x:%02x:%02x:**:**:** ch %3u %4d dBm %-8s '%s'", nbss, b->bssid[0], b->bssid[1],
		    b->bssid[2], b->chan, b->rssi, b->sec, b->ssid);
}

void radio_summary(void)
{
	printf("radio: %u networks heard (%u scans, %u frames)\n", nbss, scans, scan_frames);
	for (uint32_t i = 0; i < nbss; i++) {
		const struct bss *b = &bss[i];
		printf("  %-32s ch %3u %4d dBm %-9s %02x:%02x:%02x:**:**:** (%u frames)\n", b->ssid, b->chan, b->rssi, b->sec,
		       b->bssid[0], b->bssid[1], b->bssid[2], b->seen);
	}
}

/* ---------------------------------------------------------------- HTT setup (ath10k htt.c, 64-bit, WCN3990) */

/*
 * Without the host rx ring the radio receives but no frame reaches the firmware's rx path: no
 * beacon, no scan result (topaz). So after WMI READY and before the scan: VERSION_REQ ->
 * VERSION_CONF (or 2 s), FRAG_DESC_BANK_CFG, RX_RING_CFG (host ring of buffer addresses + a
 * host-written alloc index), AGGR_CFG. All on the HTT endpoint (CE4).
 */
#define H2T_VERSION_REQ		0
#define H2T_RX_RING_CFG		2
#define H2T_AGGR_CFG		5
#define H2T_FRAG_DESC_BANK_CFG	6
#define T2H_VERSION_CONF	0x00
#define T2H_TX_COMPL_IND	0x07
#define T2H_RX_IN_ORD_PADDR_IND	0x12
#define HTT_RING_SIZE		256
#define HTT_BUF_SIZE		2048
#define HTT_MAX_PENDING_TX	(1024 + 32)
#define HTT_EXT_DESC_SIZE	72
/* wcn3990 htt_rx_desc_v2 offsets in 4-byte words */
#define OFF_ATTENTION		1
#define OFF_FRAG_INFO		2
#define OFF_MPDU_START		4
#define OFF_MSDU_START		7
#define OFF_MSDU_END		12
#define OFF_MPDU_END		26
#define OFF_PPDU_START		27
#define OFF_PPDU_END		37
#define OFF_HDR_STATUS		74
#define OFF_MSDU_PAYLOAD	90

static uint64_t htt_ring_pa, htt_idx_pa, htt_bank_pa, htt_buf_pa;
static uint32_t htt_alloc, htt_state, htt_msgs, htt_msdus;
static uint64_t htt_req_ms;

static int htt_send(const void *d, uint32_t len) { return htt_eid == 0xff ? -1 : htc_send(htt_eid, d, len); }

static void htt_post(uint64_t pa)
{
	wr32(pa + 4 * OFF_ATTENTION, 0);
	mem_wr(htt_ring_pa + 8ull * htt_alloc, &pa, 8);
	htt_alloc = (htt_alloc + 1) & (HTT_RING_SIZE - 1);
}

static void htt_publish(void)
{
	wr32(htt_idx_pa, htt_alloc);			/* device memory: ordered after the entries */
}

static void htt_configure(const char *why)
{
	uint8_t m[64] = { 0 }, *p = m;

	htt_state = 2;
	/* ath10k_htt_send_frag_desc_bank_cfg_64 */
	*p++ = H2T_FRAG_DESC_BANK_CFG;
	*p++ = 0;
	*p++ = 1;					/* num_banks */
	*p++ = HTT_EXT_DESC_SIZE;
	p = put64(p, htt_bank_pa);
	p += 3 * 8;
	p = put16(p, 0);
	p = put16(p, HTT_MAX_PENDING_TX - 1);
	p += 3 * 4;
	p += 4 + 2 + 2;
	*p++ = 1;
	*p++ = 0;
	int a = htt_send(m, sizeof(m));
	/* ath10k_htt_send_rx_ring_cfg_64 */
	for (uint32_t i = 0; i < HTT_RING_SIZE - 1; i++)
		htt_post(htt_buf_pa + (uint64_t)i * HTT_BUF_SIZE);
	uint8_t r[48] = { 0 };
	p = r;
	*p++ = H2T_RX_RING_CFG;
	*p++ = 1;
	p = put16(p, 0);
	p = put64(p, htt_idx_pa);
	p = put64(p, htt_ring_pa);
	p = put16(p, HTT_RING_SIZE);
	p = put16(p, HTT_BUF_SIZE);
	p = put16(p, 0xffff);				/* every frame type, management included */
	p = put16(p, 0);				/* fw_idx_init_val */
	p = put16(p, OFF_HDR_STATUS);
	p = put16(p, OFF_MSDU_PAYLOAD);
	p = put16(p, OFF_PPDU_START);
	p = put16(p, OFF_PPDU_END);
	p = put16(p, OFF_MPDU_START);
	p = put16(p, OFF_MPDU_END);
	p = put16(p, OFF_MSDU_START);
	p = put16(p, OFF_MSDU_END);
	p = put16(p, OFF_ATTENTION);
	p = put16(p, OFF_FRAG_INFO);
	htt_publish();
	int b = htt_send(r, sizeof(r));
	uint8_t g[4] = { H2T_AGGR_CFG, 64, 3, 0 };
	int c = htt_send(g, sizeof(g));
	LOG("htt (%s): frag bank %s, rx ring cfg %s (%u buffers), aggr cfg %s", why, a ? "FAILED" : "ok",
	    b ? "FAILED" : "ok", HTT_RING_SIZE - 1, c ? "FAILED" : "ok");
	scan_start();
}

static void htt_start(void)
{
	uint32_t ring = (HTT_RING_SIZE * 8 + 64 + 0xfff) & ~0xfffu;
	uint32_t bank = (HTT_MAX_PENDING_TX * HTT_EXT_DESC_SIZE + 0xfff) & ~0xfffu;
	uint64_t pa;

	if (htt_state != 0)
		return;
	if (steps() & 0x100) {			/* probe: no HTT setup at all */
		LOG("probe: HTT setup skipped");
		htt_state = 2;
		scan_start();
		return;
	}
	if (!(pa = pool_alloc(ring + bank + (uint64_t)HTT_RING_SIZE * HTT_BUF_SIZE))) {
		LOG("htt: no memory, scanning without it");
		scan_start();
		return;
	}
	htt_ring_pa = pa;
	htt_idx_pa = pa + HTT_RING_SIZE * 8;
	htt_bank_pa = pa + ring;
	htt_buf_pa = pa + ring + bank;
	htt_state = 1;
	htt_req_ms = link_ms();
	uint8_t m[4] = { H2T_VERSION_REQ, 0, 0, 0 };
	LOG("htt: VERSION_REQ %s", htt_send(m, sizeof(m)) ? "FAILED" : "sent");
}

static void sta_rx_native(const uint8_t *f, uint32_t len);
static void htt_tx_compl(const uint8_t *p, uint32_t len);

static void htt_rx(const uint8_t *p, uint32_t len)
{
	static uint8_t frame[HTT_BUF_SIZE];
	uint8_t type = len ? p[0] : 0xff;
	htt_msgs++;
	if (type == T2H_TX_COMPL_IND && len >= 4) {
		htt_tx_compl(p, len);
		return;
	}
	if (type == T2H_VERSION_CONF && len >= 3) {
		LOG("*** HTT VERSION_CONF %u.%u ***", p[2], p[1]);
		if (htt_state == 1)
			htt_configure("version conf");
		return;
	}
	if (type == T2H_RX_IN_ORD_PADDR_IND && len >= 8) {
		uint16_t n;
		memcpy(&n, p + 6, 2);
		const uint8_t *d = p + 8;
		for (uint32_t i = 0; i < n && 8 + (i + 1) * 12 <= len; i++, d += 12) {
			uint64_t pa;
			memcpy(&pa, d, 8);
			if (pa < htt_buf_pa || pa >= htt_buf_pa + (uint64_t)HTT_RING_SIZE * HTT_BUF_SIZE ||
			    (pa - htt_buf_pa) % HTT_BUF_SIZE)
				continue;
			htt_msdus++;
			uint16_t mlen;
			memcpy(&mlen, d + 8, 2);
			if (mlen && mlen <= HTT_BUF_SIZE - 4 * OFF_MSDU_PAYLOAD) {
				mem_rd(pa + 4 * OFF_MSDU_PAYLOAD, frame, mlen);
				sta_rx_native(frame, mlen);
			}
			htt_post(pa);				/* recycle the same buffer */
		}
		htt_publish();
	}
}

/* ---------------------------------------------------------------- station: association, WPA2-PSK, data */

/*
 * Port of ios-topaz 0129/0130 (topaz-woa assoc.c and the TX half of htt.c, plus the 4-way
 * handshake): VDEV_START on the AP's channel, PEER_CREATE, open-system AUTH and ASSOC REQ as WMI
 * management frames, PEER_ASSOC + VDEV_UP, then EAPOL over the data path, keys by
 * VDEV_INSTALL_KEY. Frames to and from the IP side (ipnet.c) are Ethernet; the radio's are native
 * wifi (802.11 header without QoS, then LLC/SNAP). Crypto from CommonCrypto.
 */
#define CMD_VDEV_START		0x5003
#define CMD_VDEV_UP		0x5005
#define CMD_VDEV_STOP		0x5006
#define CMD_VDEV_DOWN		0x5007
#define CMD_VDEV_INSTALL_KEY	0x5009
#define CMD_PEER_CREATE		0x6001
#define CMD_PEER_DELETE		0x6002
#define CMD_PEER_SET_PARAM	0x6004
#define CMD_PEER_ASSOC		0x6005
#define CMD_MGMT_TX_SEND	0x7008
#define CMD_STA_PS_MODE		0x9001
#define EV_VDEV_START_RESP	0x5001
#define EV_PEER_STA_KICKOUT	0x6001
#define EV_ROAM			0xb001	/* {vdev, reason, rssi}; reason 2 = beacon miss */
#define TAG_VDEV_START		0x58
#define TAG_VDEV_UP		0x5c
#define TAG_VDEV_STOP		0x5d
#define TAG_VDEV_DOWN		0x5e
#define TAG_VDEV_INSTALL_KEY	0x60
#define TAG_PEER_CREATE		0x61
#define TAG_PEER_DELETE		0x62
#define TAG_PEER_SET_PARAM	0x64
#define TAG_PEER_ASSOC		0x65
#define TAG_VHT_RATE_SET	0x66
#define TAG_STA_PS_MODE		0x6c
#define TAG_MGMT_TX		0x1a6
#define PEER_FLAG_AUTH		0x1
#define PEER_FLAG_NEED_PTK_4WAY	0x4
#define PEER_PARAM_AUTHORIZE	3
#define WMI_CIPHER_TKIP		2
#define WMI_CIPHER_CCMP		4
#define FC_ASSOC_REQ		0x00
#define FC_ASSOC_RESP		0x10
#define FC_DISASSOC		0xa0
#define FC_AUTH			0xb0
#define FC_DEAUTH		0xc0
/* HTT data TX (ath10k_htt_tx_64, native wifi) */
#define TX_SLOTS		64
#define TX_SLOT_SIZE		2048
#define H2T_TX_FRM		1
#define TXF0_MAC_HDR		0x01
#define TXF0_NO_ENCRYPT		0x04
#define TXF0_NATIVE		(1u << 5)
#define TXF1_TID_NONQOS		(16u << 6)
#define TXF1_POSTPONED		(1u << 11)
#define PREFETCH_LEN		50

enum sta_state { S_IDLE, S_START, S_AUTH, S_ASSOC, S_4WAY, S_UP };
static enum sta_state s_state;
static int s_join_asked, s_result = -1;
static uint8_t s_ssid[32], s_ssid_len, s_bssid[6], s_pmk[32];
static uint8_t s_body[768];				/* the AP's beacon elements */
static uint32_t s_body_len, s_freq, s_aid, s_tries, s_desc, s_msg1s;
static uint64_t s_deadline;
static int s_rsn, s_start_resp, s_ptk_set;
static int32_t s_start_status;
static uint8_t s_assoc_req[512];
static uint32_t s_assoc_req_len;
static uint8_t s_rsn_ie[22];
static uint8_t s_ptk[48], s_anonce[32], s_snonce[32], s_group_cipher;
static uint64_t s_replay, s_mgmt_pa, tx_pool_pa, tx_busy;
static uint32_t tx_sent, tx_done, tx_fail, rx_up;

void (*radio_rx_hook)(const uint8_t *frame, uint32_t len);
void (*radio_link_hook)(int up);

static uint8_t *put_mac(uint8_t *p, const uint8_t *mac) { memset(p, 0, 8); memcpy(p, mac, 6); return p + 8; }

static uint8_t *w_channel(uint8_t *p, uint32_t freq)
{
	uint8_t *c = put_tlv(p, TAG_CHANNEL, 24);
	c = put32(c, freq);
	c = put32(c, freq);
	c = put32(c, 0);
	/* ath10k chan_to_phymode for an HT AP: 11NG_HT20 (5) / 11NA_HT20 (4); bit 3 off: legacy 11G (1) / 11A (0) */
	c = put32(c, (steps() & 8) ? (freq < 4000 ? 5 : 4) : (freq < 4000 ? 1 : 0));
	c = put32(c, (40u << 8) | (40u << 16));
	return put32(c, 40u << 8);
}

static const uint8_t *find_ie(const uint8_t *ies, uint32_t len, uint8_t id)
{
	for (uint32_t o = 0; o + 2 <= len && o + 2 + ies[o + 1] <= len; o += 2 + ies[o + 1])
		if (ies[o] == id)
			return ies + o;
	return NULL;
}

/* ---- crypto: PBKDF2/HMAC-SHA1/PRF/AES key unwrap */

static void hmac_sha1(const uint8_t *key, size_t klen, const uint8_t *d, size_t dlen, uint8_t out[20])
{
	CCHmac(kCCHmacAlgSHA1, key, klen, d, dlen, out);
}

/* IEEE 802.11 PRF-SHA1 */
static void prf(const uint8_t *key, size_t klen, const char *label, const uint8_t *data, size_t dlen, uint8_t *out,
		size_t olen)
{
	uint8_t buf[128], h[20];
	size_t ll = strlen(label);
	for (uint8_t i = 0; olen > 0; i++) {
		memcpy(buf, label, ll);
		buf[ll] = 0;
		memcpy(buf + ll + 1, data, dlen);
		buf[ll + 1 + dlen] = i;
		hmac_sha1(key, klen, buf, ll + 2 + dlen, h);
		size_t take = MIN(olen, (size_t)20);
		memcpy(out, h, take);
		out += take;
		olen -= take;
	}
}

static void pbkdf2_pmk(const char *pass, size_t plen, const uint8_t *ssid, size_t slen, uint8_t pmk[32])
{
	CCKeyDerivationPBKDF(kCCPBKDF2, pass, plen, ssid, slen, kCCPRFHmacAlgSHA1, 4096, pmk, 32);
}

static int aes_unwrap(const uint8_t kek[16], const uint8_t *in, size_t len, uint8_t *out)
{
	size_t olen = len - 8;
	return CCSymmetricKeyUnwrap(kCCWRAPAES, CCrfc3394_iv, CCrfc3394_ivLen, kek, 16, in, len, out, &olen) == 0;
}

static int crypto_selftest(void)
{
	static const uint8_t kek[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
	static const uint8_t ct[24] = { 0x1f, 0xa6, 0x8b, 0x0a, 0x81, 0x12, 0xb4, 0x47, 0xae, 0xf3, 0x4b, 0xd8,
					0xfb, 0x5a, 0x7b, 0x82, 0x9d, 0x3e, 0x86, 0x23, 0x71, 0xd2, 0xcf, 0xe5 };
	static const uint8_t pt[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
					0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
	static const uint8_t pmk_ref[4] = { 0xf4, 0x2c, 0x6f, 0xc5 };	/* "password"/"IEEE" (802.11 annex) */
	uint8_t out[16], pmk[32];
	int a = aes_unwrap(kek, ct, 24, out) && !memcmp(out, pt, 16);
	pbkdf2_pmk("password", 8, (const uint8_t *)"IEEE", 4, pmk);
	int b = !memcmp(pmk, pmk_ref, 4);
	LOG("wpa: crypto self-test: key unwrap %s, pbkdf2 %s", a ? "ok" : "FAILED", b ? "ok" : "FAILED");
	return a && b;
}

/* ---- HTT data TX */

static int htt_tx_frame(const uint8_t *f, uint32_t len, int clear)
{
	uint8_t m[20 + 52] = { 0 }, ext[HTT_EXT_DESC_SIZE] = { 0 }, *p = m;
	uint32_t id, pf = (MIN(len, (uint32_t)PREFETCH_LEN) + 3) & ~3u;
	if (!tx_pool_pa)
		tx_pool_pa = pool_alloc(TX_SLOTS * TX_SLOT_SIZE);
	if (!tx_pool_pa || !htt_bank_pa || htt_state != 2 || len > TX_SLOT_SIZE)
		return -1;
	for (id = 0; id < TX_SLOTS && (tx_busy & (1ull << id)); id++)
		;
	if (id == TX_SLOTS)
		return -1;
	uint64_t pa = tx_pool_pa + (uint64_t)id * TX_SLOT_SIZE;
	mem_wr(pa, f, len);
	uint32_t lo = (uint32_t)pa;			/* frags[0] at +24: paddr_lo, paddr_hi16, len16 */
	uint16_t hi = (uint16_t)(pa >> 32), l16 = (uint16_t)len;
	memcpy(ext + 24, &lo, 4);
	memcpy(ext + 28, &hi, 2);
	memcpy(ext + 30, &l16, 2);
	mem_wr(htt_bank_pa + (uint64_t)id * HTT_EXT_DESC_SIZE, ext, sizeof(ext));
	*p++ = H2T_TX_FRM;
	*p++ = (uint8_t)(TXF0_MAC_HDR | TXF0_NATIVE | (clear ? TXF0_NO_ENCRYPT : 0));
	p = put16(p, (uint16_t)(TXF1_TID_NONQOS | TXF1_POSTPONED));	/* vdev 0 */
	p = put16(p, (uint16_t)len);
	p = put16(p, (uint16_t)id);
	p = put64(p, htt_bank_pa + (uint64_t)id * HTT_EXT_DESC_SIZE);
	p = put32(p, 0xffff);				/* peer id: invalid */
	memcpy(p, f, MIN(len, pf));
	if (htt_send(m, 20 + pf))
		return -1;
	tx_busy |= 1ull << id;
	tx_sent++;
	return 0;
}

static void htt_tx_compl(const uint8_t *p, uint32_t len)
{
	uint32_t n = p[2], st = p[1] & 7;
	for (uint32_t i = 0; i < n && 4 + 2 * (i + 1) <= len; i++) {
		uint16_t id;
		memcpy(&id, p + 4 + 2 * i, 2);
		if (id < TX_SLOTS)
			tx_busy &= ~(1ull << id);
	}
	tx_done += n;
	if (st != 0 && (tx_fail += n) <= 8)
		LOG("htt tx: %u frame(s) status %u", n, st);
}

/* ---- WMI commands for the join (layouts: ath10k wmi-tlv) */

/* ath10k_start / ath10k_conf_tx: pdev tx power limits (0.5 dBm units) and vdev WMM (mac80211 STA defaults)
 * are set before the first vdev start; without them the firmware asserted there (RT:0x1084/0x1089). */
static int w_pdev_param(uint32_t param, uint32_t value)
{
	uint8_t m[4 + 12], *p = put_tlv(m, 0x52, 12);
	p = put32(p, 0);
	p = put32(p, param);
	put32(p, value);
	return wmi_send(0x4003, m, sizeof(m));
}

static int w_vdev_wmm(void)
{
	/* BE, BK, VI, VO: cwmin, cwmax, aifs, txop (32 us units), acm, no_ack */
	static const uint32_t ac[4][6] = { { 15, 1023, 3, 0, 0, 0 }, { 15, 1023, 7, 0, 0, 0 },
					   { 7, 15, 2, 94, 0, 0 }, { 3, 7, 2, 47, 0, 0 } };
	uint8_t m[4 + 4 + 4 * 28] = { 0 }, *p = put_tlv(m, 0xc7, 4 + 4 * 28);
	p = put32(p, 0);				/* vdev 0 */
	for (int i = 0; i < 4; i++) {
		p = put32(p, 0);			/* dummy */
		for (int j = 0; j < 6; j++)
			p = put32(p, ac[i][j]);
	}
	return wmi_send(0x500d, m, sizeof(m));
}

static int w_vdev_start(uint32_t freq, uint32_t bcn, uint32_t dtim)
{
	uint8_t m[4 + 72 + 4 + 24 + 4] = { 0 }, *p;
	p = put_tlv(m, TAG_VDEV_START, 72);
	p = put32(p, 0);
	p = put32(p, 0);
	p = put32(p, bcn);
	p = put32(p, dtim);
	p = put32(p, 0);
	p += 36 + 16;					/* ssid (empty for a station), tx rate/power, noa, hw ack */
	p = w_channel(p, freq);
	put_tlv(p, TAG_ARRAY_STRUCT, 0);
	return wmi_send(CMD_VDEV_START, m, sizeof(m));
}

static int w_vdev_simple(uint32_t cmd, uint16_t tag)
{
	uint8_t m[8], *p = put_tlv(m, tag, 4);
	put32(p, 0);
	return wmi_send(cmd, m, sizeof(m));
}

static int w_peer(uint32_t cmd, uint16_t tag, int create)
{
	uint8_t m[4 + 16] = { 0 }, *p = put_tlv(m, tag, create ? 16 : 12);
	p = put32(p, 0);
	p = put_mac(p, s_bssid);
	if (create)
		put32(p, 0);
	return wmi_send(cmd, m, create ? 20 : 16);
}

static int w_peer_param(uint32_t param, uint32_t value)
{
	uint8_t m[4 + 20], *p = put_tlv(m, TAG_PEER_SET_PARAM, 20);
	p = put32(p, 0);
	p = put_mac(p, s_bssid);
	p = put32(p, param);
	put32(p, value);
	return wmi_send(CMD_PEER_SET_PARAM, m, sizeof(m));
}

static int w_ps_off(void)
{
	uint8_t m[12], *p = put_tlv(m, TAG_STA_PS_MODE, 8);
	p = put32(p, 0);
	put32(p, 0);
	return wmi_send(CMD_STA_PS_MODE, m, sizeof(m));
}

static int w_mgmt_tx(const uint8_t *f, uint32_t len)
{
	uint8_t m[4 + 28 + 4 + 64] = { 0 }, *p;
	uint32_t buf = (MIN(len, 64u) + 3) & ~3u;
	static uint8_t copy[0x1000];
	if (!s_mgmt_pa)
		s_mgmt_pa = pool_alloc(0x1000);
	if (!s_mgmt_pa || len > sizeof(copy) - 8)
		return -1;
	memset(copy, 0, ((len + 3) & ~3u) + 4);
	memcpy(copy, f, len);
	mem_wr(s_mgmt_pa, copy, len + 4);
	p = put_tlv(m, TAG_MGMT_TX, 28);
	p = put32(p, 0);
	p = put32(p, ++s_desc);
	p = put32(p, 0);
	p = put64(p, s_mgmt_pa);
	p = put32(p, len);
	p = put32(p, buf);
	p = put_tlv(p, TAG_ARRAY_BYTE, buf);
	memcpy(p, copy, buf);
	return wmi_send(CMD_MGMT_TX_SEND, m, 4 + 28 + 4 + buf);
}

static uint32_t w_ap_rates(uint8_t *out)
{
	static const uint8_t ids[2] = { 1, 50 };
	uint32_t n = 0;
	for (int k = 0; k < 2; k++) {
		const uint8_t *ie = find_ie(s_body, s_body_len, ids[k]);
		for (uint32_t i = 0; ie && i < ie[1] && n < 16; i++)
			out[n++] = ie[2 + i];
	}
	return n;
}

static int w_peer_assoc(uint32_t caps)
{
	uint8_t m[4 + 76 + 4 + 16 + 4 + 4 + 16] = { 0 }, *p, *r, rates[16];
	uint32_t n = w_ap_rates(rates), rl = (MIN(n, 16u) + 3) & ~3u;
	p = put_tlv(m, TAG_PEER_ASSOC, 76);
	p = put_mac(p, s_bssid);
	p = put32(p, 0);
	p = put32(p, 1);				/* new assoc */
	p = put32(p, s_aid);
	p = put32(p, PEER_FLAG_AUTH | (s_rsn ? PEER_FLAG_NEED_PTK_4WAY : 0));
	p = put32(p, caps);
	p = put32(p, 1);				/* listen interval */
	p += 4 * 4;					/* ht caps, max mpdu, density, rate caps */
	p = put32(p, 1);				/* nss */
	p = put32(p, 0);				/* vht caps */
	p = put32(p, s_freq < 4000 ? 1 : 0);		/* phy mode: 11g / 11a */
	p += 8;
	p = put32(p, MIN(n, 16u));
	p = put32(p, 0);
	r = put_tlv(p, TAG_ARRAY_BYTE, rl);
	for (uint32_t i = 0; i < n && i < 16; i++) {
		uint8_t v = rates[i] & 0x7f;
		r[i] = (v == 2 || v == 4 || v == 11 || v == 22) ? (uint8_t)(v | 0x80) : v;
	}
	p = r + rl;
	p = put_tlv(p, TAG_ARRAY_BYTE, 0);
	put_tlv(p, TAG_VHT_RATE_SET, 16);
	return wmi_send(CMD_PEER_ASSOC, m, 4 + 76 + 4 + rl + 4 + 4 + 16);
}

static int w_install_key(int group, uint32_t idx, uint32_t cipher, const uint8_t *key, uint32_t klen)
{
	uint8_t m[4 + 92 + 4 + 32] = { 0 }, *p;
	uint32_t kl = (klen + 3) & ~3u;
	p = put_tlv(m, TAG_VDEV_INSTALL_KEY, 92);
	p = put32(p, 0);
	p = put_mac(p, s_bssid);
	p = put32(p, idx);
	p = put32(p, group ? 1 : 0);
	p = put32(p, cipher);
	p += 3 * 8 + 16 + 16;
	p = put32(p, klen);
	p = put32(p, cipher == WMI_CIPHER_TKIP ? 8 : 0);
	p = put32(p, cipher == WMI_CIPHER_TKIP ? 8 : 0);
	p = put_tlv(p, TAG_ARRAY_BYTE, kl);
	memcpy(p, key, klen);
	return wmi_send(CMD_VDEV_INSTALL_KEY, m, 4 + 92 + 4 + kl);
}

/* ---- 802.11 management frames */

static uint8_t *w_mgmt_hdr(uint8_t *f, uint8_t fc)
{
	memset(f, 0, 24);
	f[0] = fc;
	memcpy(f + 4, s_bssid, 6);
	memcpy(f + 10, radio_mac, 6);
	memcpy(f + 16, s_bssid, 6);
	return f + 24;
}

static int w_send_auth(void)
{
	uint8_t f[30], *b = w_mgmt_hdr(f, FC_AUTH);
	memset(b, 0, 6);
	b[2] = 1;					/* open system, transaction 1 */
	return w_mgmt_tx(f, sizeof(f));
}

static void w_build_assoc_req(void)
{
	uint8_t *f = s_assoc_req, *p = w_mgmt_hdr(f, FC_ASSOC_REQ), rates[16];
	uint32_t n = w_ap_rates(rates), cap = 0x0001 | 0x0020 | 0x0400;	/* ESS, short preamble, short slot */
	if (s_rsn)
		cap |= 0x0010;
	*p++ = (uint8_t)cap;
	*p++ = (uint8_t)(cap >> 8);
	*p++ = 10;					/* listen interval */
	*p++ = 0;
	*p++ = 0;
	*p++ = s_ssid_len;
	memcpy(p, s_ssid, s_ssid_len);
	p += s_ssid_len;
	*p++ = 1;
	*p++ = (uint8_t)MIN(n, 8u);
	for (uint32_t k = 0; k < n && k < 8; k++)
		*p++ = rates[k];
	if (n > 8) {
		*p++ = 50;
		*p++ = (uint8_t)(n - 8);
		for (uint32_t k = 8; k < n; k++)
			*p++ = rates[k];
	}
	if (s_rsn) {
		memcpy(p, s_rsn_ie, sizeof(s_rsn_ie));
		p += sizeof(s_rsn_ie);
	}
	s_assoc_req_len = (uint32_t)(p - f);
}

/* ---- Ethernet <-> native wifi */

/* an Ethernet frame (dst, src, type, payload) to the AP as a to-DS data frame */
static int w_tx_ether(const uint8_t *e, uint32_t len, int clear)
{
	static uint8_t f[TX_SLOT_SIZE];
	static const uint8_t snap[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };
	if (len < 14 || len - 14 + 32 > sizeof(f))
		return -1;
	memset(f, 0, 24);
	f[0] = 0x08;					/* data */
	f[1] = (uint8_t)(0x01 | (s_ptk_set && !clear ? 0x40 : 0));	/* to DS (+ protected once keyed) */
	memcpy(f + 4, s_bssid, 6);			/* addr1: the AP */
	memcpy(f + 10, radio_mac, 6);			/* addr2: us */
	memcpy(f + 16, e, 6);				/* addr3: the destination */
	memcpy(f + 24, snap, 6);
	memcpy(f + 30, e + 12, 2);			/* ethertype */
	memcpy(f + 32, e + 14, len - 14);
	return htt_tx_frame(f, len - 14 + 32, clear);
}

int radio_tx_ether(const uint8_t *e, uint32_t len)
{
	return s_state == S_UP ? w_tx_ether(e, len, 0) : -1;
}

/* ---- the join, step by step from the poll loop */

static void w_link(int up)
{
	if (radio_link_hook)
		radio_link_hook(up);
}

static void w_teardown(void)
{
	if (s_state >= S_START) {
		if (s_state >= S_4WAY)
			w_vdev_simple(CMD_VDEV_DOWN, TAG_VDEV_DOWN);
		if (s_state >= S_AUTH)
			w_peer(CMD_PEER_DELETE, TAG_PEER_DELETE, 0);
		w_vdev_simple(CMD_VDEV_STOP, TAG_VDEV_STOP);
	}
	if (s_state == S_UP)
		w_link(0);
	s_state = S_IDLE;
	s_ptk_set = 0;
}

static void w_join_end(int result)
{
	if (result) {
		LOG("join: failed (%s) in state %u", strerror(result), s_state);
		w_teardown();
	} else {
		s_state = S_UP;
		LOG("*** joined '%.*s' ***", (int)s_ssid_len, s_ssid);
		w_link(1);
	}
	s_result = result;
}

/* a joined link went away: down, and the result says so (ENOTCONN) so the caller can join again; rescan first */
static void w_link_lost(const char *why)
{
	LOG("join: link lost (%s)", why);
	w_teardown();
	s_result = ENOTCONN;
	next_scan_ms = link_ms() + 1000;
}

static void w_join_begin(void)
{
	/* the AP: the strongest one heard with that name */
	int best = -1;
	for (uint32_t i = 0; i < nbss; i++)
		if (bss[i].raw_ssid_len == s_ssid_len && !memcmp(bss[i].raw_ssid, s_ssid, s_ssid_len) &&
		    (best < 0 || bss[i].rssi > bss[best].rssi))
			best = (int)i;
	if (best < 0) {
		LOG("join: '%.*s' not heard", (int)s_ssid_len, s_ssid);
		w_join_end(ENOENT);
		return;
	}
	memcpy(s_bssid, bss[best].bssid, 6);
	s_freq = bss[best].freq;
	s_body_len = bss[best].ie_len;
	memcpy(s_body, bss[best].ie, s_body_len);
	const uint8_t *rsn = find_ie(s_body, s_body_len, 48);
	s_rsn = rsn != NULL;
	s_group_cipher = (rsn && rsn[1] >= 8) ? rsn[2 + 2 + 3] : 4;
	static const uint8_t hdr[4] = { 48, 20, 1, 0 };
	memcpy(s_rsn_ie, hdr, 4);
	uint8_t *q = s_rsn_ie + 4;
	*q++ = 0x00; *q++ = 0x0f; *q++ = 0xac; *q++ = s_group_cipher;
	*q++ = 1; *q++ = 0; *q++ = 0x00; *q++ = 0x0f; *q++ = 0xac; *q++ = 4;	/* pairwise CCMP */
	*q++ = 1; *q++ = 0; *q++ = 0x00; *q++ = 0x0f; *q++ = 0xac; *q++ = 2;	/* AKM PSK */
	*q++ = 0; *q++ = 0;
	uint32_t bcn = bss[best].bintval ? bss[best].bintval : 100, dtim = 1;
	const uint8_t *tim = find_ie(s_body, s_body_len, 5);
	if (tim && tim[1] >= 2 && tim[3])
		dtim = tim[3];
	s_msg1s = 0;
	s_start_resp = 0;
	LOG("join: '%.*s' %02x:%02x:%02x:**:**:** %u MHz %d dBm %s", (int)s_ssid_len, s_ssid, s_bssid[0], s_bssid[1],
	    s_bssid[2], s_freq, bss[best].rssi, s_rsn ? "WPA2" : "open");
	/* steps bits 4-6: our own pre-start commands (ath10k does not send these before vdev start) */
	if (steps() & 0x10)
		w_ps_off();
	int t2 = 0, t5 = 0, wm = 0;
	if (steps() & 0x20) {
		t2 = w_pdev_param(3, 40);
		t5 = w_pdev_param(4, 40);
	}
	if (steps() & 0x40)
		wm = w_vdev_wmm();
	LOG("join: tx power limit 2G/5G 20 dBm %s, vdev wmm %s", t2 || t5 ? "FAILED" : "ok", wm ? "FAILED" : "ok");
	fflush(stdout);
	if (w_vdev_start(s_freq, bcn, dtim)) {
		w_join_end(EIO);
		return;
	}
	s_state = S_START;
	s_deadline = link_ms() + 1500;
}

static void sta_mgmt_rx(const uint8_t *f, uint32_t len)
{
	if (len < 26 || s_state == S_IDLE || memcmp(f + 10, s_bssid, 6))
		return;
	uint8_t fc = f[0];
	uint16_t v = (uint16_t)(f[24] | f[25] << 8);
	if (fc == FC_AUTH && s_state == S_AUTH && len >= 30 && f[26] == 2) {
		v = (uint16_t)(f[28] | f[29] << 8);
		LOG("join: auth reply status %u", v);
		if (v) {
			w_join_end(ECONNREFUSED);
			return;
		}
		s_state = S_ASSOC;
		s_tries = 1;
		w_build_assoc_req();
		w_mgmt_tx(s_assoc_req, s_assoc_req_len);
		s_deadline = link_ms() + 400;
	} else if (fc == FC_ASSOC_RESP && s_state == S_ASSOC && len >= 30) {
		v = (uint16_t)(f[26] | f[27] << 8);
		s_aid = (uint32_t)(f[28] | f[29] << 8) & 0x3fff;
		LOG("join: assoc reply status %u aid %u", v, s_aid);
		if (v) {
			w_join_end(ECONNREFUSED);
			return;
		}
		w_peer_assoc((uint32_t)(f[24] | f[25] << 8));
		uint8_t m[20], *p = put_tlv(m, TAG_VDEV_UP, 16);
		p = put32(p, 0);
		p = put32(p, s_aid);
		put_mac(p, s_bssid);
		wmi_send(CMD_VDEV_UP, m, sizeof(m));
		s_state = S_4WAY;				/* (teardown order) */
		if (!s_rsn) {
			w_peer_param(PEER_PARAM_AUTHORIZE, 1);
			w_join_end(0);
		} else {
			s_deadline = link_ms() + 8000;
			LOG("join: associated, waiting for the 4-way handshake");
		}
	} else if (fc == FC_DEAUTH || fc == FC_DISASSOC) {
		LOG("join: the AP ended the link, reason %u (state %u)", v, s_state);
		if (s_state == S_UP)
			w_link_lost("deauth/disassoc");
		else
			w_join_end(s_state == S_4WAY ? EAUTH : ECONNREFUSED);
	}
}

static void sta_wmi_event(uint32_t id, const uint8_t *t, uint32_t len)
{
	uint16_t l = 0;
	uint32_t v[4] = { 0 };
	if (len >= 4)
		memcpy(&l, t, 2);
	if (len < 4u + l)
		return;
	memcpy(v, t + 4, MIN((uint32_t)l, (uint32_t)sizeof(v)));
	if (id == EV_VDEV_START_RESP && l >= 16) {	/* {vdev, req, type, status} */
		s_start_status = (int32_t)v[3];
		s_start_resp = 1;
		LOG("vdev start resp: status %d", s_start_status);
	} else if (id == EV_PEER_STA_KICKOUT && s_state == S_UP) {
		w_link_lost("peer kickout");
	} else if (id == EV_ROAM && l >= 12) {
		LOG("join: roam event, reason %u, rssi %d (state %u)", v[1], (int)v[2], s_state);
		if (v[1] == 2 && s_state == S_UP)
			w_link_lost("beacon miss");
	}
}

/* ---- WPA2 4-way handshake (EAPOL-Key, descriptor version 2: HMAC-SHA1-128 MIC, AES key wrap) */

static void w_eapol_send(uint16_t info, const uint8_t *nonce, const uint8_t *data, uint16_t dlen)
{
	uint8_t e[14 + 4 + 95 + 64] = { 0 }, *k = e + 14 + 4, mic[20];
	memcpy(e, s_bssid, 6);
	memcpy(e + 6, radio_mac, 6);
	e[12] = 0x88;
	e[13] = 0x8e;
	uint16_t body = (uint16_t)(95 + dlen);
	e[14] = 1;					/* EAPOL version */
	e[15] = 3;					/* key */
	e[16] = (uint8_t)(body >> 8);
	e[17] = (uint8_t)body;
	k[0] = 2;					/* RSN key descriptor */
	k[1] = (uint8_t)(info >> 8);
	k[2] = (uint8_t)info;
	for (int i = 0; i < 8; i++)
		k[5 + i] = (uint8_t)(s_replay >> (56 - 8 * i));
	if (nonce)
		memcpy(k + 13, nonce, 32);
	k[93] = (uint8_t)(dlen >> 8);
	k[94] = (uint8_t)dlen;
	if (dlen)
		memcpy(k + 95, data, dlen);
	hmac_sha1(s_ptk, 16, e + 14, 4 + body, mic);
	memcpy(k + 77, mic, 16);
	w_tx_ether(e, 14 + 4 + body, 1);
}

static int w_eapol_mic_ok(uint8_t *eapol, size_t len)
{
	uint8_t want[16], got[20];
	if (len < 4 + 95)
		return 0;
	memcpy(want, eapol + 4 + 77, 16);
	memset(eapol + 4 + 77, 0, 16);
	hmac_sha1(s_ptk, 16, eapol, len, got);
	memcpy(eapol + 4 + 77, want, 16);
	return !memcmp(want, got, 16);
}

static void w_eapol_rx(const uint8_t *e, size_t elen)
{
	uint8_t frame[512];
	if (s_state != S_4WAY || elen < 14 + 4 + 95 || elen - 14 > sizeof(frame) || memcmp(e + 6, s_bssid, 6))
		return;
	size_t len = elen - 14;
	memcpy(frame, e + 14, len);
	uint8_t *k = frame + 4;
	if (frame[1] != 3 || k[0] != 2)
		return;
	uint16_t info = (uint16_t)(k[1] << 8 | k[2]), dlen = (uint16_t)(k[93] << 8 | k[94]);
	uint64_t replay = 0;
	for (int i = 0; i < 8; i++)
		replay = replay << 8 | k[5 + i];
	if (95 + (size_t)dlen > len - 4)
		return;
	int pairwise = info & 0x0008, ack = info & 0x0080, has_mic = info & 0x0100, install = info & 0x0040;
	if (pairwise && ack && !has_mic) {
		/* message 1: ANonce -> PTK, answer with message 2 */
		if (++s_msg1s > 3) {
			LOG("wpa: the AP keeps sending message 1: wrong passphrase");
			w_join_end(EAUTH);
			return;
		}
		s_replay = replay;
		memcpy(s_anonce, k + 13, 32);
		arc4random_buf(s_snonce, 32);
		uint8_t data[76];
		const uint8_t *aa = s_bssid, *spa = radio_mac;
		int amin = memcmp(aa, spa, 6) < 0, nmin = memcmp(s_anonce, s_snonce, 32) < 0;
		memcpy(data, amin ? aa : spa, 6);
		memcpy(data + 6, amin ? spa : aa, 6);
		memcpy(data + 12, nmin ? s_anonce : s_snonce, 32);
		memcpy(data + 44, nmin ? s_snonce : s_anonce, 32);
		prf(s_pmk, 32, "Pairwise key expansion", data, sizeof(data), s_ptk, 48);
		w_eapol_send(0x010a, s_snonce, s_rsn_ie, sizeof(s_rsn_ie));
		LOG("wpa: message 1 -> message 2 sent");
		return;
	}
	if (pairwise && ack && has_mic && install) {
		/* message 3: check, unwrap the GTK, answer with message 4, install the keys */
		if (!w_eapol_mic_ok(frame, len)) {
			LOG("wpa: message 3 MIC wrong: wrong passphrase");
			w_join_end(EAUTH);
			return;
		}
		s_replay = replay;
		uint8_t plain[256], gtk[32], gtk_len = 0, gtk_idx = 1;
		if ((info & 0x1000) && dlen >= 24 && dlen <= sizeof(plain) + 8 && aes_unwrap(s_ptk + 16, k + 95, dlen, plain)) {
			for (uint32_t o = 0; o + 2 <= (uint32_t)dlen - 8 && o + 2 + plain[o + 1] <= (uint32_t)dlen - 8;
			     o += 2 + plain[o + 1]) {
				if (plain[o] == 0xdd && plain[o + 1] >= 6 && plain[o + 2] == 0x00 && plain[o + 3] == 0x0f &&
				    plain[o + 4] == 0xac && plain[o + 5] == 0x01) {
					gtk_idx = plain[o + 6] & 3;
					gtk_len = (uint8_t)MIN((uint32_t)plain[o + 1] - 6, 32u);
					memcpy(gtk, plain + o + 8, gtk_len);
				}
			}
		}
		w_eapol_send(0x030a, NULL, NULL, 0);
		delay_ms(5);				/* message 4 out in the clear before the keys go in */
		w_install_key(0, 0, WMI_CIPHER_CCMP, s_ptk + 32, 16);
		if (gtk_len)
			w_install_key(1, gtk_idx, s_group_cipher == 2 ? WMI_CIPHER_TKIP : WMI_CIPHER_CCMP, gtk, gtk_len);
		w_peer_param(PEER_PARAM_AUTHORIZE, 1);
		s_ptk_set = 1;
		memset(gtk, 0, sizeof(gtk));
		LOG("*** WPA2 keys installed (group key %s, idx %u) ***", gtk_len ? "yes" : "NO", gtk_idx);
		w_join_end(0);
	}
}

/* a received data frame (native wifi, from the HTT rx ring) */
static void sta_rx_native(const uint8_t *f, uint32_t len)
{
	static uint8_t e[14 + 2048];
	if (len < 24 + 8 || (f[0] & 0x0c) != 0x08)
		return;					/* data frames only */
	uint32_t hl = ((f[1] & 3) == 3) ? 30 : 24;
	const uint8_t *da = f + 4, *sa = (f[1] & 2) ? f + 16 : f + 10;	/* from DS: addr3 is the source */
	const uint8_t *llc = f + hl;
	if (len < hl + 8 || llc[0] != 0xaa || llc[1] != 0xaa || llc[2] != 0x03)
		return;
	uint32_t plen = len - hl - 8;
	if (plen > sizeof(e) - 14)
		return;
	memcpy(e, da, 6);
	memcpy(e + 6, sa, 6);
	memcpy(e + 12, llc + 6, 2);
	memcpy(e + 14, llc + 8, plen);
	if (e[12] == 0x88 && e[13] == 0x8e) {
		w_eapol_rx(e, 14 + plen);
		return;
	}
	if (s_state == S_UP && radio_rx_hook) {
		rx_up++;
		radio_rx_hook(e, 14 + plen);
	}
}

static void sta_poll(void)
{
	uint64_t now = link_ms();
	if (s_join_asked && !scanning && scan_started) {
		s_join_asked = 0;
		w_teardown();
		w_join_begin();
		return;
	}
	if (s_state == S_START) {
		if (s_start_resp) {
			if (s_start_status) {
				LOG("join: vdev start status %d", s_start_status);
				w_join_end(EIO);
				return;
			}
			s_state = S_AUTH;
			s_tries = 1;
			w_peer(CMD_PEER_CREATE, TAG_PEER_CREATE, 1);
			w_send_auth();
			s_deadline = now + 400;
		} else if (now > s_deadline) {
			w_join_end(ETIMEDOUT);
		}
	} else if ((s_state == S_AUTH || s_state == S_ASSOC) && now > s_deadline) {
		if (s_tries >= 4) {
			w_join_end(ETIMEDOUT);
		} else {
			s_tries++;
			if (s_state == S_AUTH)
				w_send_auth();
			else
				w_mgmt_tx(s_assoc_req, s_assoc_req_len);
			s_deadline = now + 400;
		}
	} else if (s_state == S_4WAY && now > s_deadline) {
		LOG("join: no 4-way handshake (%u message 1s)", s_msg1s);
		w_join_end(s_msg1s ? EAUTH : ETIMEDOUT);
	}
}

/*
 * Join SSID with a passphrase (8-63 characters), 64 hex digits (the PMK itself) or "" (open).
 * Starts once the radio has scanned; radio_link_hook says when the link is up.
 */
int radio_join(const char *ssid, const char *pass)
{
	static int tested, crypto_ok;
	size_t sl = strlen(ssid), pl = strlen(pass);
	if (!sl || sl > 32)
		return EINVAL;
	if (!tested) {
		tested = 1;
		crypto_ok = crypto_selftest();
	}
	if (pl && (!crypto_ok || pl < 8 || pl > 64))
		return crypto_ok ? EINVAL : EIO;
	memset(s_pmk, 0, sizeof(s_pmk));
	if (pl == 64) {
		for (int i = 0; i < 32; i++) {
			unsigned v;
			if (sscanf(pass + 2 * i, "%2x", &v) != 1)
				return EINVAL;
			s_pmk[i] = (uint8_t)v;
		}
	} else if (pl) {
		pbkdf2_pmk(pass, pl, (const uint8_t *)ssid, sl, s_pmk);
	}
	memcpy(s_ssid, ssid, sl);
	s_ssid_len = (uint8_t)sl;
	s_result = -1;
	s_join_asked = 1;
	return 0;
}

int radio_join_result(void)	/* -1 while it runs, 0 joined, else an errno */
{
	return s_join_asked ? -1 : s_result;
}

/* ---------------------------------------------------------------- WMI events */

static void wmi_rx(const uint8_t *d, uint32_t len)
{
	uint32_t id;
	if (len < 4)
		return;
	memcpy(&id, d, 4);
	id &= 0xffffff;
	switch (id) {
	case WMI_EV_SERVICE_AVAILABLE: {
		uint16_t tag;
		memcpy(&tag, d + 6, 2);
		if (len >= 12 + sizeof(wmi_svc_ext) && tag == TAG_SERVICE_AVAILABLE)
			memcpy(wmi_svc_ext, d + 12, sizeof(wmi_svc_ext));
		break;
	}
	case WMI_EV_SERVICE_READY:
		LOG("wmi: SERVICE_READY, %u bytes", len);
		wmi_svc_ready(d + 4, len - 4);
		if (!wmi_init_sent)
			wmi_send_init();
		break;
	case WMI_EV_READY:
		for (uint32_t o = 4; o + 4 <= len;) {
			uint16_t l, tag;
			memcpy(&l, d + o, 2);
			memcpy(&tag, d + o + 2, 2);
			if (o + 4 + l > len)
				break;
			if (tag == TAG_READY_EVENT && l >= 24 + 8 + 4) {
				memcpy(wmi_mac, d + o + 4 + 24, 6);
				wmi_ready = 1;
			}
			o += 4 + l;
		}
		LOG("*** WMI READY (fw MAC %02x:%02x:%02x:%02x:%02x:%02x) ***", wmi_mac[0], wmi_mac[1], wmi_mac[2],
		    wmi_mac[3], wmi_mac[4], wmi_mac[5]);
		if (wmi_ready)
			htt_start();				/* the host rx ring first, then the scan */
		break;
	case WMI_EV_SCAN:
		scan_event(d + 4, len - 4);
		break;
	case WMI_EV_MGMT_RX:
		scan_mgmt_rx(d + 4, len - 4);
		break;
	case EV_VDEV_START_RESP:
	case EV_PEER_STA_KICKOUT:
	case EV_ROAM:
		sta_wmi_event(id, d + 4, len - 4);
		break;
	case 0x1d003:					/* WMI_TLV_DEBUG_MESG_EVENTID: fw dbglog, raw words */
		for (uint32_t o = 4; o + 4 <= len; o += 32) {
			char line[128];
			int n = 0;
			for (uint32_t k = o; k < o + 32 && k + 4 <= len; k += 4) {
				uint32_t w;
				memcpy(&w, d + k, 4);
				n += snprintf(line + n, sizeof(line) - n, " %08x", w);
			}
			LOG("dbglog%s", line);
		}
		fflush(stdout);
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- start and poll */

static uint64_t start_ms;

int radio_start(void)
{
	if (ce_start())
		return -1;
	start_ms = link_ms();
	return 0;
}

void radio_poll(void)
{
	static uint64_t last;
	uint64_t now = link_ms();
	if (!ce_up || now - last < 2)
		return;
	last = now;
	ce_poll();
	now = link_ms();				/* what ce_poll started is stamped after the first now */
	if (htt_state == 1 && now > htt_req_ms + 2000) {
		LOG("htt: no VERSION_CONF in 2 s, configuring anyway");
		htt_configure("timeout");
	}
	if (!htc_ready && start_ms && now - start_ms > 5000) {
		start_ms = 0;
		uint32_t rri[CE_COUNT];
		mem_rd(rri_pa, rri, sizeof(rri));
		LOG("ce: no HTC READY in 5 s; rri ce1 %08x ce2 %08x, ce2 DRRI %u DR_WR %u, rx %u/%u",
		    rri[1], rri[2], ce_rd(2, CUR_DRRI), ce_rd(2, DR_WR_IDX), ce[1].rx, ce[2].rx);
	}
	if (scan_started && !scanning && next_scan_ms && now >= next_scan_ms) {
		next_scan_ms = 0;
		if (s_state == S_IDLE && !s_join_asked)	/* scans leave the channel: never while joining or joined */
			scanning = !scan_send_start();
	}
	/* asked for (wifid, bridge.c): also while joined, as a background scan (the firmware rests on the home
	 * channel between the others: min/max rest in scan_send_start), never in the middle of a join */
	if (scan_asked && scan_started && !scanning && !s_join_asked && (s_state == S_IDLE || s_state == S_UP)) {
		scan_asked = 0;
		scanning = !scan_send_start();
		if (s_state == S_UP)
			LOG("scan: in the background (joined)");
	}
	sta_poll();
}

void radio_disconnect(void)	/* test hook: drop the link as a deauth would */
{
	if (s_state == S_UP)
		w_link_lost("asked");
}

void radio_rescan(unsigned ms)
{
	uint64_t t = link_ms() + ms;
	if (!next_scan_ms || t < next_scan_ms)
		next_scan_ms = t;
}

unsigned radio_scans(void)
{
	return scans;
}

void radio_stats(void)
{
	LOG("stats: state %u, tx %u sent %u done %u failed, rx %u to the IP side, htt msdus %u, ce tx full %u/%u/%u",
	    s_state, tx_sent, tx_done, tx_fail, rx_up, htt_msdus, ce[0].tx_full, ce[3].tx_full, ce[4].tx_full);
}

/* the i-th network heard (any scan): 0, or -1 past the last */
int radio_heard(unsigned i, struct radio_bss *out)
{
	if (i >= nbss)
		return -1;
	const struct bss *b = &bss[i];
	memcpy(out->bssid, b->bssid, 6);
	memcpy(out->ssid, b->raw_ssid, b->raw_ssid_len);
	out->ssid_len = b->raw_ssid_len;
	out->freq = b->freq;
	out->rssi = b->rssi;
	out->bintval = b->bintval;
	out->capab = b->capab;
	out->age_ms = (uint32_t)MIN(link_ms() - b->last_ms, 0xffffffffull);
	out->ie_len = b->ie_len;
	memcpy(out->ie, b->ie, b->ie_len);
	return 0;
}

int radio_scanning(void)
{
	return scanning || (next_scan_ms != 0 && s_state == S_IDLE);
}

int radio_joined(void)
{
	return s_state == S_UP;
}

/* the SSID (and BSSID) of the network joined or being joined (length; 0 when idle) */
unsigned radio_ssid(uint8_t ssid[32], uint8_t bssid[6])
{
	if (s_state == S_IDLE)
		return 0;
	memcpy(ssid, s_ssid, s_ssid_len);
	if (bssid)
		memcpy(bssid, s_bssid, 6);
	return s_ssid_len;
}

/* a scan as soon as the radio can, also while joined (radio_rescan is only for an idle radio) */
void radio_scan_now(void)
{
	scan_asked = 1;
}
