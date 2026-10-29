/*
 * bt_uart: the WCN3990 Bluetooth transport shared by btup and btbridge (split out of btup.c for btbridge,
 * 2026-10-30): QUP wrapper 0 SE3 (0x88c000, hsuart0) as a polled GENI UART, the BT rails over RPMh, the QCA power
 * pulses, HCI over H4 with an event/ACL reassembler, the QCA version, TLV patch + NVM download. Messages go to stdout.
 *
 * Facts (register numbers, sequences, chip replies) from xorbidden/hakios-sm7150 hakios.bt_wcn3990.c (no licence
 * file: used as a reference only, no code taken; research/notes/hackios-survey.md) and Linux qcom_geni_serial.c,
 * geni-se.h, gcc-sm7150.c, hci_qca.c, btqca.c. DT: ../recon/live.dts:884-897 (bt_wcn3990), :9299 (uart), :14526 (pins).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include "pmem.h"
#include "bt_uart.h"

/* GCC (gcc-sm7150.c): wrapper-0 branch vote register, its bits, SE3 branch and RCG */
#define GCC_PA		0x00100000u
#define GCC_VOTE	(GCC_PA + 0x5200c)	/* APCS_CLOCK_BRANCH_ENA_VOTE (geni_i2c.c reads it for wrapper 1) */
#define V_M_AHB		(1u << 6)	/* gcc_qupv3_wrap_0_m_ahb_clk (DT 0x67) */
#define V_S_AHB		(1u << 7)	/* gcc_qupv3_wrap_0_s_ahb_clk (DT 0x68) */
#define V_CORE		(1u << 8)	/* gcc_qupv3_wrap0_core_clk */
#define V_CORE2X	(1u << 9)	/* gcc_qupv3_wrap0_core_2x_clk */
#define V_S3		(1u << 13)	/* gcc_qupv3_wrap0_s3_clk (DT 0x4a) */
#define S3_CBCR		(GCC_PA + 0x173c0)
#define S3_RCG		(GCC_PA + 0x173c4)	/* CMD, CFG +4, M +8, N +0xc, D +0x10 */
#define CBCR_OFF	(1u << 31)

/* SE3 (geni-se.h, qcom_geni_serial.c) */
#define SE		0x0088c000u
#define WRAP0_HW_VER	0x008c0004u	/* qupv3_0 wrapper hardware version */
#define FORCE_DEFAULT	0x20
#define OUTPUT_CTRL	0x24
#define CGC_CTRL	0x28
#define GENI_STATUS	0x40	/* M_CMD_ACTIVE bit 0, S_CMD_ACTIVE bit 12 */
#define M_CLK_CFG	0x48
#define S_CLK_CFG	0x4c
#define FW_REV		0x68	/* [15:8] protocol: 1 SPI, 2 UART, 3 I2C */
#define LOOPBACK_CFG	0x22c
#define BYTE_GRAN	0x254
#define DMA_MODE_EN	0x258
#define TX_TRANS_CFG	0x25c	/* bit 1: CTS ignored */
#define TX_PACK0	0x260
#define TX_PACK1	0x264
#define TX_WORD_LEN	0x268
#define TX_STOP_LEN	0x26c
#define TX_TRANS_LEN	0x270
#define RX_TRANS_CFG	0x280
#define RX_PACK0	0x284
#define RX_PACK1	0x288
#define RX_WORD_LEN	0x28c
#define RX_STALE	0x294
#define TX_PARITY	0x2a4
#define RX_PARITY	0x2a8
#define MANUAL_RFR	0x2ac
#define M_CMD0		0x600
#define M_CMD_CTRL	0x604
#define M_IRQ_STATUS	0x610
#define M_IRQ_EN	0x614
#define M_IRQ_CLEAR	0x618
#define S_CMD0		0x630
#define S_CMD_CTRL	0x634
#define S_IRQ_STATUS	0x640
#define S_IRQ_EN	0x644
#define S_IRQ_CLEAR	0x648
#define TX_FIFO		0x700
#define RX_FIFO		0x780
#define TX_FIFO_ST	0x800
#define RX_FIFO_ST	0x804
#define RX_WM		0x810
#define RX_RFR_WM	0x814
#define DMA_TX_IRQ_STAT	0xc40
#define DMA_TX_IRQ_CLR	0xc44
#define DMA_TX_FSM_RST	0xc58
#define DMA_RX_IRQ_STAT	0xd40
#define DMA_RX_IRQ_CLR	0xd44
#define DMA_RX_FSM_RST	0xd58
#define GSI_EVENT_EN	0xe18
#define SE_IRQ_EN	0xe1c
#define DMA_GEN_CFG	0xe30
#define TX_FIFO_DEPTH_R	0xe24
#define RX_FIFO_DEPTH_R	0xe28
#define CMD_ABORT	2u
#define CMD_CANCEL	4u
#define START_TX	(1u << 27)
#define START_READ	(1u << 27)
#define SE_CLK_HZ	7372800u	/* RCG: GPLL0 even 300 MHz x 384/15625 (Linux freq table) */

/* TLMM (pinctrl-sm7150.c): gpio N config at 0x3d00000 + 0x1000*N (south tile), in/out +4 */
#define TLMM_PA		0x03d00000u
#define GPIO_CFG(n)	(TLMM_PA + 0x1000u * (n))
#define GPIO_IO(n)	(GPIO_CFG(n) + 4)

static unsigned sampling = 32;
int failed;

double now_s(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

static void sw(uint32_t off, uint32_t v)
{
	wr32(SE + off, v);
}

static uint32_t sr(uint32_t off)
{
	return rd32(SE + off);
}

void hex(const char *what, const uint8_t *b, unsigned n)
{
	printf("%s (%u bytes):", what, n);
	for (unsigned i = 0; i < n && i < 96; i++)
		printf(" %02x", b[i]);
	printf("%s\n", n > 96 ? " ..." : n ? "" : " none");
}

int fail(const char *msg)
{
	printf("btup: %s: stop\n", msg);
	failed = 1;
	return -1;
}

/* ---- status (read-only) ---- */

static int clocks_run(void)
{
	return !(rd32(S3_CBCR) & CBCR_OFF) && !(rd32(GCC_PA + 0x17004) & CBCR_OFF) &&
	       !(rd32(GCC_PA + 0x17008) & CBCR_OFF);
}

static void se_show(void)
{
	uint32_t fw = sr(FW_REV);
	printf("se3: fw rev %#x (protocol %u%s) status %#x m_clk %#x s_clk %#x m_irq %#x s_irq %#x tx %#x rx %#x "
	       "depth tx %#x rx %#x dma %#x loopback %#x\n", fw, (fw >> 8) & 0xff, ((fw >> 8) & 0xff) == 2 ? " UART" : "",
	       sr(GENI_STATUS), sr(M_CLK_CFG), sr(S_CLK_CFG), sr(M_IRQ_STATUS), sr(S_IRQ_STATUS), sr(TX_FIFO_ST),
	       sr(RX_FIFO_ST), sr(TX_FIFO_DEPTH_R), sr(RX_FIFO_DEPTH_R), sr(DMA_MODE_EN), sr(LOOPBACK_CFG));
}

void status(void)
{
	printf("gcc: vote %#x (m_ahb %u s_ahb %u core %u core2x %u s3 %u)\n", rd32(GCC_VOTE),
	       !!(rd32(GCC_VOTE) & V_M_AHB), !!(rd32(GCC_VOTE) & V_S_AHB), !!(rd32(GCC_VOTE) & V_CORE),
	       !!(rd32(GCC_VOTE) & V_CORE2X), !!(rd32(GCC_VOTE) & V_S3));
	printf("gcc: s3 cbcr %#x, m_ahb cbcr %#x, s_ahb cbcr %#x, rcg cmd %#x cfg %#x m %#x n %#x d %#x\n",
	       rd32(S3_CBCR), rd32(GCC_PA + 0x17004), rd32(GCC_PA + 0x17008), rd32(S3_RCG), rd32(S3_RCG + 4),
	       rd32(S3_RCG + 8), rd32(S3_RCG + 12), rd32(S3_RCG + 16));
	if (clocks_run())
		se_show();
	else
		printf("se3: clocks off, registers not read (an unclocked SE can hang the bus)\n");
	for (unsigned n = 38; n <= 41; n++) {
		uint32_t c = rd32(GPIO_CFG(n));
		printf("gpio%u: cfg %#x (func %u%s pull %u drive %u %s) level %u\n", n, c, (c >> 2) & 0xf,
		       ((c >> 2) & 0xf) == 1 ? " qup03," : ",", c & 3, (c >> 6) & 7, c & (1u << 9) ? "out" : "in",
		       rd32(GPIO_IO(n)) & 1);
	}
	printf("regulators: RPMh votes are write-only from here (ldoa10 state not readable)\n");
}

/* ---- clocks, pins, rails ---- */

static int clocks_on(void)
{
	/* RCG to 7.3728 MHz (clk-rcg2 MND: M, N as ~(n-m), D as ~n; cfg src 6 = GPLL0 even, div 1, mode dual edge) */
	const uint32_t m = 384, n = 15625, cfg = (2u << 12) | (6u << 8) | 1u;
	if ((rd32(S3_RCG + 4) & 0x37ffu) != cfg || rd32(S3_RCG + 8) != m) {
		printf("gcc: s3 rcg %#x -> %#x m %u n %u\n", rd32(S3_RCG + 4), cfg, m, n);
		wr32(S3_RCG + 8, m);
		wr32(S3_RCG + 12, ~(n - m) & 0xffffu);
		wr32(S3_RCG + 16, ~n & 0xffffu);
		wr32(S3_RCG + 4, cfg);
		wr32(S3_RCG, rd32(S3_RCG) | 1u);
		if (wait_bit(S3_RCG, 1u, 0, 100))
			return fail("s3 rcg update not taken");
	}
	uint32_t v = V_S3 | V_M_AHB | V_S_AHB | V_CORE | V_CORE2X;
	printf("gcc: vote %#x |= %#x\n", rd32(GCC_VOTE), v);
	wr32(GCC_VOTE, rd32(GCC_VOTE) | v);
	if (wait_bit(S3_CBCR, CBCR_OFF, 0, 100))
		return fail("s3 clock does not run");
	printf("gcc: s3 cbcr %#x rcg cmd %#x\n", rd32(S3_CBCR), rd32(S3_RCG));
	return 0;
}

static int pins_on(void)
{
	/* function 1 (qup03), 2 mA, DT pulls: CTS 38 none, RTS 39 down, TX 40 up, RX 41 none */
	static const unsigned pull[4] = { 0, 1, 3, 0 };
	for (unsigned i = 0; i < 4; i++) {
		uint32_t c = (1u << 2) | pull[i];
		printf("gpio%u: cfg %#x -> %#x\n", 38 + i, rd32(GPIO_CFG(38 + i)), c);
		wr32(GPIO_CFG(38 + i), c);
	}
	return 0;
}

static int rail(int on)
{
	/* vdd-io pm6150_l10 = ldoa10, 1800 mV (DT 1.704-1.904 V); VRM: voltage 0, mode 8 (HPM 7), enable 4 */
	uint32_t off[3] = { 0, 8, 4 }, val[3] = { 1800, 7, 1 };
	int rc = on ? pm_rpmh("ldoa10", 3, off, val) : pm_rpmh("ldoa10", 1, &off[2], (uint32_t[]){ 0 });
	printf("rail: ldoa10 %s -> %d\n", on ? "1800 mV on" : "off", rc);
	return rc ? fail("ldoa10 vote") : 0;
}

/* ---- the UART (polled FIFO) ---- */

static void seq_stop(uint32_t ctrl, uint32_t active)
{
	if (!(sr(GENI_STATUS) & active))
		return;
	sw(ctrl, CMD_CANCEL);
	for (unsigned i = 0; i < 20 && (sr(GENI_STATUS) & active); i++)
		delay_ms(1);
	if (sr(GENI_STATUS) & active) {
		sw(ctrl, CMD_ABORT);
		for (unsigned i = 0; i < 20 && (sr(GENI_STATUS) & active); i++)
			delay_ms(1);
	}
	printf("uart: %s sequencer %s\n", ctrl == S_CMD_CTRL ? "rx" : "tx",
	       sr(GENI_STATUS) & active ? "STILL ACTIVE" : "stopped");
}

static void baud(uint32_t b)
{
	uint32_t div = (SE_CLK_HZ + b * sampling - 1) / (b * sampling), cfg = (div << 4) | 1u;
	int reading = !!(sr(GENI_STATUS) & (1u << 12));
	/* the rx sequencer keeps the clock its command started with: stop, set, restart (set_termios) */
	if (reading) {
		seq_stop(S_CMD_CTRL, 1u << 12);
		sw(S_IRQ_CLEAR, 0xffffffffu);
	}
	sw(M_CLK_CFG, cfg);
	sw(S_CLK_CFG, cfg);
	if (reading)
		sw(S_CMD0, START_READ);
	printf("uart: %u baud, div %u, clk cfg %#x\n", b, div, cfg);
}

static void flow(int on)
{
	uint32_t c = sr(TX_TRANS_CFG);
	sw(TX_TRANS_CFG, on ? c & ~2u : c | 2u);
}

static int uart_setup(void)
{
	uint32_t fw = sr(FW_REV), ver = rd32(WRAP0_HW_VER);
	sampling = ver >= 0x20050000u ? 16 : 32;
	printf("uart: wrapper version %#x (%u samples per bit)\n", ver, sampling);
	se_show();
	if (((fw >> 8) & 0xff) != 2)
		return fail("SE3 has no UART firmware");
	seq_stop(S_CMD_CTRL, 1u << 12);
	seq_stop(M_CMD_CTRL, 1u);
	for (unsigned k = 0, w; (w = sr(RX_FIFO_ST) & 0x1ffffffu) && k < 4096; k += w)
		for (unsigned i = 0; i < w; i++)
			(void)sr(RX_FIFO);
	sw(MANUAL_RFR, 0);
	/* Android leaves SE3 in DMA mode: reset both DMA state machines, then FIFO mode, no interrupts (we poll) */
	sw(DMA_TX_FSM_RST, 1);
	if (wait_bit(SE + DMA_TX_IRQ_STAT, 1u << 3, 1, 20))
		printf("uart: tx dma reset not done\n");
	sw(DMA_TX_IRQ_CLR, 0xffffffffu);
	sw(DMA_RX_FSM_RST, 1);
	if (wait_bit(SE + DMA_RX_IRQ_STAT, 1u << 3, 1, 20))
		printf("uart: rx dma reset not done\n");
	sw(DMA_RX_IRQ_CLR, 0xffffffffu);
	sw(M_IRQ_EN, 0); sw(S_IRQ_EN, 0);
	sw(CGC_CTRL, sr(CGC_CTRL) | 0x7fu);
	sw(DMA_GEN_CFG, sr(DMA_GEN_CFG) | 0x0fu);
	sw(OUTPUT_CTRL, 0x7f); sw(FORCE_DEFAULT, 1);
	sw(GSI_EVENT_EN, 0); sw(SE_IRQ_EN, 0);
	sw(M_IRQ_CLEAR, 0xffffffffu); sw(S_IRQ_CLEAR, 0xffffffffu);
	sw(DMA_MODE_EN, sr(DMA_MODE_EN) & ~1u);
	/* 8-bit words, 4 per FIFO entry, LSB first (geni_se_config_packing) */
	sw(TX_PACK0, 0x4380e); sw(TX_PACK1, 0xc3e0e);
	sw(RX_PACK0, 0x4380e); sw(RX_PACK1, 0xc3e0e);
	sw(BYTE_GRAN, 0);
	sw(RX_WM, 2); sw(RX_RFR_WM, 14);
	sw(RX_STALE, 10 * 16);
	sw(TX_WORD_LEN, 8); sw(RX_WORD_LEN, 8);
	sw(TX_STOP_LEN, 0);			/* 1 stop bit */
	sw(TX_PARITY, 0); sw(RX_PARITY, 0);
	sw(RX_TRANS_CFG, 0);
	sw(TX_TRANS_CFG, 2u);			/* CTS ignored until flow(1) */
	baud(115200);
	sw(S_CMD0, START_READ);
	return 0;
}

int tx(const uint8_t *b, unsigned n)
{
	if (wait_bit(SE + GENI_STATUS, 1u, 0, 100))
		return fail("tx sequencer busy");
	sw(M_IRQ_CLEAR, 0xffffffffu);
	sw(TX_TRANS_LEN, n);
	sw(M_CMD0, START_TX);
	for (unsigned j = 0; j < n; j += 4) {
		uint32_t w = 0;
		for (unsigned k = 0; k < 4 && j + k < n; k++)
			w |= (uint32_t)b[j + k] << (8 * k);
		unsigned spin = 0;
		while ((sr(TX_FIFO_ST) & 0xfffffffu) >= 15)
			if (++spin == 200000)
				return fail("tx fifo stuck");
		sw(TX_FIFO, w);
	}
	if (wait_bit(SE + M_IRQ_STATUS, 1u, 1, 2000)) {
		printf("uart: tx of %u not done (m_irq %#x status %#x)\n", n, sr(M_IRQ_STATUS), sr(GENI_STATUS));
		return fail("tx timeout");
	}
	sw(M_IRQ_CLEAR, 1u);
	return 0;
}

unsigned rx_overruns;

/* bytes received within ms (stops early when max is reached) */
static unsigned rx(uint8_t *b, unsigned max, unsigned ms)
{
	unsigned got = 0;
	for (unsigned waited = 0; waited <= ms && got < max;) {
		uint32_t st = sr(RX_FIFO_ST), w = st & 0x1ffffffu;
		if (!w) {
			delay_ms(1);
			waited++;
			continue;
		}
		for (uint32_t i = 0; i < w; i++) {
			uint32_t v = sr(RX_FIFO), valid = 4;
			if (i == w - 1 && (st & (1u << 31)) && ((st >> 28) & 7))
				valid = (st >> 28) & 7;
			for (uint32_t k = 0; k < valid && got < max; k++)
				b[got++] = (uint8_t)(v >> (8 * k));
		}
		uint32_t s = sr(S_IRQ_STATUS);
		if (s & (1u << 25))		/* S_RX_FIFO_WR_ERR: the FIFO overflowed, bytes lost (bt3) */
			rx_overruns++;
		sw(S_IRQ_CLEAR, s);
	}
	return got;
}

static int bring_up(void)
{
	if (clocks_on() || pins_on())
		return -1;
	return uart_setup();
}

/* ---- modes ---- */

int uart_loop(void)
{
	static const uint8_t t[] = { 0x55, 0xaa, 0x01, 0x02, 0x03, 0xfe };
	uint8_t in[64];
	if (bring_up())
		return -1;
	uint32_t was = sr(LOOPBACK_CFG);
	sw(LOOPBACK_CFG, 1);			/* TX -> RX inside the SE */
	int rc = tx(t, sizeof(t));
	unsigned got = rc ? 0 : rx(in, sizeof(in), 100);
	sw(LOOPBACK_CFG, was);
	seq_stop(S_CMD_CTRL, 1u << 12);
	hex("loopback rx", in, got);
	if (rc || got != sizeof(t) || memcmp(in, t, sizeof(t)))
		return fail("loopback mismatch");
	printf("loopback: OK\n");
	return 0;
}

static int pulse(uint8_t p, uint32_t b)
{
	baud(b);
	flow(0);
	printf("pulse: %#x at %u baud\n", p, b);
	return tx(&p, 1);
}

int power(void)
{
	uint8_t in[256];
	if (rail(1))
		return -1;
	delay_ms(10);
	if (bring_up())
		return -1;
	sw(MANUAL_RFR, (1u << 31) | 1u);	/* our RTS held ready: the chip only sends while it is */
	if (pulse(0xc0, 2400))
		return -1;
	delay_ms(10);
	if (pulse(0xfc, 115200))
		return -1;
	delay_ms(100);
	unsigned got = rx(in, sizeof(in), 500);
	hex("after power-on pulse", in, got);
	flow(1);
	printf("gpio: cts %u rts %u tx %u rx %u\n", rd32(GPIO_IO(38)) & 1, rd32(GPIO_IO(39)) & 1,
	       rd32(GPIO_IO(40)) & 1, rd32(GPIO_IO(41)) & 1);
	return 0;
}

/* H4 reassembly (bt3): the chip also sends ACL packets (QCA debug log, handle 0x2edc, ~485 bytes) between events;
 * the old reader skipped to the next 0x04 byte and so framed "events" out of ACL payload. Now: 0x04 event and 0x02 ACL
 * are parsed by their headers, any other byte is dropped one at a time (counted) until a type byte lines up. */
static uint8_t rxb[8192];
static unsigned rxn;
unsigned rx_dropped, acl_pkts, acl_bytes;

static void pump(unsigned ms)
{
	if (sizeof(rxb) - rxn < 64) {
		printf("h4: buffer full, %u bytes discarded\n", rxn);
		rx_dropped += rxn;
		rxn = 0;
	}
	rxn += rx(rxb + rxn, sizeof(rxb) - rxn, ms);
}

/* next complete H4 packet (type 02/04) within ms; copies up to max bytes, returns its full length, 0 on timeout */
unsigned pkt(uint8_t *p, unsigned max, unsigned ms)
{
	double end = now_s() + ms / 1000.0;
	unsigned drop = 0;
	for (;;) {
		unsigned need = 0;
		while (rxn) {
			if (rxb[0] == 0x04 && rxn >= 3)
				need = 3u + rxb[2];
			else if (rxb[0] == 0x02 && rxn >= 5)
				need = 5u + (rxb[3] | rxb[4] << 8);
			else if (rxb[0] == 0x04 || rxb[0] == 0x02)
				break;			/* header still incomplete */
			if (need > 2048 || (rxb[0] != 0x04 && rxb[0] != 0x02)) {
				drop++;
				rx_dropped++;
				memmove(rxb, rxb + 1, --rxn);
				need = 0;
				continue;
			}
			break;
		}
		if (drop) {
			printf("h4: resync, %u bytes dropped (%u total)\n", drop, rx_dropped);
			drop = 0;
		}
		if (need && rxn >= need) {
			memcpy(p, rxb, need < max ? need : max);
			memmove(rxb, rxb + need, rxn -= need);
			return need;
		}
		if (now_s() >= end)
			return 0;
		pump(2);
	}
}

/* next event; ACL packets are counted and skipped */
unsigned event(uint8_t *ev, unsigned max, unsigned ms)
{
	static uint8_t p[2100];
	double end = now_s() + ms / 1000.0;
	for (;;) {
		double left = (end - now_s()) * 1000;
		unsigned n = pkt(p, sizeof(p), left > 0 ? (unsigned)left : 0);
		if (!n)
			return 0;
		if (p[0] == 0x02) {
			if (!acl_pkts++)
				printf("acl: handle %#x len %u (first; QCA debug log, skipped)\n",
				       (p[1] | p[2] << 8) & 0xfff, n - 5);
			acl_bytes += n;
			continue;
		}
		memcpy(ev, p, n < max ? n : max);
		return n;
	}
}

void decode(const uint8_t *ev, unsigned n)
{
	if (n >= 7 && ev[1] == 0x0e)
		printf("event: Command Complete op %#06x status %#x (%u bytes)\n", ev[4] | ev[5] << 8, ev[6], n);
	else if (n >= 7 && ev[1] == 0x0f)
		printf("event: Command Status op %#06x status %#x\n", ev[5] | ev[6] << 8, ev[3]);
	else if (n >= 3 && ev[1] == 0xff)
		hex("event: vendor", ev, n);
	else
		hex("event", ev, n);
}

void drain(unsigned ms)
{
	uint8_t ev[260];
	unsigned k = 0;
	for (unsigned n; (n = event(ev, sizeof(ev), ms)); k++)
		decode(ev, n);
	printf("drain: %u events in %u ms; acl %u packets %u bytes, dropped %u, fifo overruns %u\n", k, ms, acl_pkts,
	       acl_bytes, rx_dropped, rx_overruns);
}

/* command; the reply is the Command Complete/Status for this opcode (others printed), or any event for 0xfc00 */
unsigned cmd(uint16_t op, const uint8_t *p, uint8_t n, uint8_t *ev, unsigned max, unsigned ms)
{
	uint8_t out[4 + 255] = { 0x01, (uint8_t)op, (uint8_t)(op >> 8), n };
	if (n)
		memcpy(out + 4, p, n);
	if (tx(out, 4u + n) || !ms)
		return 0;
	double end = now_s() + ms / 1000.0;
	for (;;) {
		double left = (end - now_s()) * 1000;
		unsigned got = event(ev, max, left > 0 ? (unsigned)left : 0);
		uint16_t of = got >= 6 && ev[1] == 0x0e ? (uint16_t)(ev[4] | ev[5] << 8) :
			      got >= 7 && ev[1] == 0x0f ? (uint16_t)(ev[5] | ev[6] << 8) : 0xffff;
		if (!got || op == 0xfc00 || of == op)
			return got;
		decode(ev, got);
	}
}

int hci_reset(void)
{
	uint8_t ev[260];
	unsigned got = cmd(0x0c03, NULL, 0, ev, sizeof(ev), 1000);
	hex("HCI_Reset reply", ev, got);
	if (got < 7 || ev[1] != 0x0e || ev[6] != 0)
		return fail("HCI_Reset not answered with success");
	printf("hci-reset: OK\n");
	return 0;
}

int version(void)
{
	static const uint8_t p[] = { 0x19 };	/* EDL_PATCH_VER_REQ_CMD */
	uint8_t ev[260];
	unsigned got = cmd(0xfc00, p, 1, ev, sizeof(ev), 1000);
	hex("version reply", ev, got);
	/* 04 ff len, 00 (EDL resp), 02 (app version), product u32, patch u16, rom u16, soc u32 */
	if (got < 17 || ev[1] != 0xff || ev[4] != 0x02)
		return fail("no EDL version reply");
	const uint8_t *v = ev + 5;
	printf("version: product %#x patch %#x rom %#x soc %#x\n",
	       v[0] | v[1] << 8 | v[2] << 16 | (uint32_t)v[3] << 24, v[4] | v[5] << 8, v[6] | v[7] << 8,
	       v[8] | v[9] << 8 | v[10] << 16 | (uint32_t)v[11] << 24);
	return 0;
}

static uint8_t *slurp(const char *dir, const char *name, unsigned *size)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *f = fopen(path, "rb");
	if (!f) {
		printf("fw: %s: %s\n", path, strerror(errno));
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = n > 16 && n < (1 << 22) ? malloc((size_t)n) : NULL;
	if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(b);
		printf("fw: %s: bad size %ld\n", path, n);
		return NULL;
	}
	fclose(f);
	*size = (unsigned)n;
	printf("fw: %s %u bytes\n", path, *size);
	return b;
}

/* btqca: 243-byte segments of EDL_PATCH_TLV_REQ_CMD (0x1e); skip = only the last segment is answered */
static int download(const char *name, const uint8_t *d, unsigned size, int skip)
{
	uint8_t seg[2 + 243], ev[260];
	unsigned at = 0, segs = 0, pct = 0;
	double t0 = now_s();
	while (at < size) {
		unsigned n = size - at < 243 ? size - at : 243;
		int last = at + n == size, want = !skip || last;
		seg[0] = 0x1e;
		seg[1] = (uint8_t)n;
		memcpy(seg + 2, d + at, n);
		unsigned got = cmd(0xfc00, seg, (uint8_t)(n + 2), ev, sizeof(ev), want ? 2000 : 0);
		if (failed)
			return -1;
		segs++;
		/* command complete for opcode 0 = a credit, not the answer */
		for (unsigned k = 0; k < 3 && got >= 6 && ev[1] == 0x0e && !ev[4] && !ev[5]; k++)
			got = event(ev, sizeof(ev), 2000);
		if (want && (got < 6 || ev[1] != 0xff || ev[4] != 0x04 || ev[5] != 0)) {
			printf("%s: segment %u at %u/%u\n", name, segs, at, size);
			hex("reply", ev, got);
			return fail("segment not taken");
		}
		at += n;
		while (at * 10ull / size > pct)
			printf("%s: %u0 %%\n", name, ++pct);
	}
	if (skip)
		event(ev, sizeof(ev), 100);
	printf("%s: %u bytes, %u segments, %.1f s\n", name, size, segs, now_s() - t0);
	return 0;
}

static int info(void)
{
	uint8_t ev[260];
	unsigned got = cmd(0x1009, NULL, 0, ev, sizeof(ev), 2000);
	if (got < 13 || ev[1] != 0x0e || ev[6] != 0) {
		hex("Read_BD_ADDR reply", ev, got);
		return fail("Read_BD_ADDR failed");
	}
	printf("bd_addr: %02x:%02x:%02x:%02x:%02x:%02x\n", ev[12], ev[11], ev[10], ev[9], ev[8], ev[7]);
	got = cmd(0x1001, NULL, 0, ev, sizeof(ev), 2000);
	if (got < 15 || ev[1] != 0x0e || ev[6] != 0) {
		hex("Read_Local_Version reply", ev, got);
		return fail("Read_Local_Version failed");
	}
	printf("local version: hci %u rev %#x lmp %u manufacturer %#x subversion %#x\n", ev[7], ev[8] | ev[9] << 8,
	       ev[10], ev[11] | ev[12] << 8, ev[13] | ev[14] << 8);
	return 0;
}

int firmware(const char *dir)
{
	unsigned psz = 0, nsz = 0;
	uint8_t *p = slurp(dir, "crbtfw21.tlv", &psz), *nv = p ? slurp(dir, "crnv21.bin", &nsz) : NULL;
	int rc = -1;
	if (!p || !nv) {
		fail("firmware files missing");
		goto out;
	}
	/* tlv_patch header after the 4-byte type/len: download mode at 14, product 16, rom 18, version 20 */
	printf("patch: type %u mode %u product %#x rom %#x version %#x\n", p[0], p[14], p[16] | p[17] << 8,
	       p[18] | p[19] << 8, p[20] | p[21] << 8);
	if (download("patch", p, psz, p[14] == 3))
		goto out;
	/* NVM: tag 17 (EDL_TAG_ID_HCI): in-band sleep off (we don't speak IBS), baud index 0 = 115200 */
	unsigned off = nv[0] == 4 ? 4 : 0, len = nv[off + 1] | nv[off + 2] << 8 | nv[off + 3] << 16;
	if (nv[off] != 2 || off + 4 + len > nsz) {
		fail("NVM layout unexpected");
		goto out;
	}
	for (unsigned at = off + 4; at + 12 <= off + 4 + len;) {
		unsigned tag = nv[at] | nv[at + 1] << 8, tl = nv[at + 2] | nv[at + 3] << 8;
		if (tag == 17 && tl >= 3) {
			printf("nvm: hci tag %02x %02x %02x -> ibs off, baud 115200\n", nv[at + 12], nv[at + 13], nv[at + 14]);
			nv[at + 12] &= 0x7f;
			nv[at + 14] = 0;
		}
		at += 12 + tl;
	}
	if (download("nvm", nv, nsz, 0))
		goto out;
	delay_ms(50);
	{	/* bt4: the chip's debug log (ACL handle 0xedc) floods the polled UART; at boot (busy CPU) bytes got
		 * lost mid-packet and H4 framing slipped. Turn it off as Linux btqca (qca_disable_soc_logging) and
		 * iOSPlus10 (step 16) do: 0xfc17 {0x14, 0x00}; a failure is only logged (may be WCN3991+ only). */
		uint8_t lp[2] = { 0x14, 0x00 }, lev[32];
		unsigned lg = cmd(0xfc17, lp, 2, lev, sizeof(lev), 1000);
		hex("logging-off reply", lev, lg);
	}
	if (hci_reset())
		goto out;
	drain(500);				/* bt3: let the post-reset burst (vendor events, debug log ACL) pass */
	if (info())
		goto out;
	rc = 0;
out:
	free(p);
	free(nv);
	return rc;
}

int off(void)
{
	int rc = 0;
	if (clocks_run() && ((sr(FW_REV) >> 8) & 0xff) == 2) {
		baud(2400);
		flow(0);
		uint8_t p = 0xc0;
		printf("pulse: 0xc0 at 2400 baud\n");
		rc = tx(&p, 1);
		delay_ms(10);
		seq_stop(S_CMD_CTRL, 1u << 12);
		sw(MANUAL_RFR, 0);
	} else {
		printf("off: SE3 not clocked or not UART: no pulse\n");
	}
	failed = 0;
	rc |= rail(0);
	/* only the SE3 branch vote: the wrapper AHB/core votes may be shared with other wrapper-0 users */
	printf("gcc: vote %#x &= ~%#x\n", rd32(GCC_VOTE), V_S3);
	wr32(GCC_VOTE, rd32(GCC_VOTE) & ~V_S3);
	return rc;
}

