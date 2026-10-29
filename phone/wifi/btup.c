/*
 * btup: WCN3990 Bluetooth bring-up from userland (/dev/pmem), stages (a)-(d) of research/notes/bt-plan.md:
 * QUP wrapper 0 SE3 (0x88c000, hsuart0) as a polled GENI UART, the BT rails over RPMh, the QCA power pulses,
 * HCI over H4, the QCA version, TLV patch + NVM download.
 *
 *   btup status       read-only: GCC votes/branches, SE3 GENI registers (only if its clocks run), TLMM gpio38-41
 *   btup uart-loop    SE3 as UART 115200 8N1 with the GENI internal loopback; bytes must come back. Restores.
 *   btup power        ldoa10 1800 mV vote, pins qup03, clocks, UART, pulses 0xC0 (2400) + 0xFC (115200); RX 500 ms
 *   btup hci-reset    power + HCI_Reset (01 03 0c 00), expects 04 0e 04 01 03 0c 00
 *   btup version      power + EDL patch version request (01 00 fc 01 19): product / patch / ROM / SoC id
 *   btup fw DIR       power + version + DIR/crbtfw21.tlv + DIR/crnv21.bin, HCI reset, 500 ms event drain,
 *                     Read_BD_ADDR, Read_Local_Version; stays powered
 *   btup fw DIR --scan N   the above, then classic inquiry (10.24 s) and an LE passive scan of N s
 *   btup off          0xC0 pulse, drop our ldoa10 vote and the SE3 clock vote (not the Wi-Fi rails)
 * Every step logs, the first error stops. The xtal/core/pa rails (ldoc1/ldoc2/ldoc10) are wland's: run Wi-Fi first.
 * The UART, power and firmware code is in bt_uart.c (shared with btbridge).
 *
 * Facts (register numbers, sequences, chip replies) from xorbidden/hakios-sm7150 hakios.bt_wcn3990.c (no licence
 * file: used as a reference only, no code taken; research/notes/hackios-survey.md) and Linux qcom_geni_serial.c,
 * geni-se.h, gcc-sm7150.c, hci_qca.c, btqca.c. DT: ../recon/live.dts:884-897 (bt_wcn3990), :9299 (uart), :14526 (pins).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pmem.h"
#include "bt_uart.h"

/* ---- scan (bt3): classic inquiry, then LE passive scan; addresses de-duplicated ---- */

static struct dev { uint8_t a[6], le; int8_t rssi; char name[33]; unsigned seen; } devs[128];
static unsigned ndev;

static void eir_name(const uint8_t *d, unsigned n, char *name)
{
	for (unsigned i = 0; i + 1 < n && d[i];) {
		unsigned l = d[i];
		if (i + 1 + l > n)
			break;
		if ((d[i + 1] == 0x08 || d[i + 1] == 0x09) && l > 1 && (!name[0] || d[i + 1] == 0x09)) {
			unsigned k = l - 1 < 32 ? l - 1 : 32;
			for (unsigned j = 0; j < k; j++)
				name[j] = d[i + 2 + j] >= 0x20 && d[i + 2 + j] < 0x7f ? (char)d[i + 2 + j] : '?';
			name[k] = 0;
		}
		i += 1 + l;
	}
}

static void found(const uint8_t *a, int le, int rssi, const uint8_t *eir, unsigned eirn)
{
	struct dev *d = NULL;
	for (unsigned i = 0; i < ndev; i++)
		if (devs[i].le == le && !memcmp(devs[i].a, a, 6))
			d = &devs[i];
	int fresh = !d;
	if (!d && ndev < sizeof(devs) / sizeof(devs[0])) {
		d = &devs[ndev++];
		memset(d, 0, sizeof(*d));
		memcpy(d->a, a, 6);
		d->le = (uint8_t)le;
	}
	if (!d)
		return;
	d->seen++;
	d->rssi = (int8_t)rssi;
	char had = d->name[0];
	if (eir)
		eir_name(eir, eirn, d->name);
	if (fresh || (!had && d->name[0]))
		printf("%s %02x:%02x:%02x:%02x:%02x:%02x rssi %d%s%s\n", le ? "le" : "classic", a[5], a[4], a[3], a[2],
		       a[1], a[0], rssi, d->name[0] ? " name " : "", d->name);
}

static int scan(unsigned secs)
{
	uint8_t ev[260];
	/* event mask with LE Meta (bit 61) and the inquiry events, as Linux sets it */
	static const uint8_t mask[8] = { 0xff, 0xff, 0xfb, 0xff, 0x07, 0xf8, 0xbf, 0x3d };
	unsigned got = cmd(0x0c01, mask, 8, ev, sizeof(ev), 2000);
	if (got < 7 || ev[1] != 0x0e || ev[6])
		return fail("Set_Event_Mask failed");
	static const uint8_t mode = 2;		/* inquiry results with RSSI or extended (EIR) */
	got = cmd(0x0c45, &mode, 1, ev, sizeof(ev), 2000);
	if (got < 7 || ev[1] != 0x0e || ev[6])
		printf("scan: Write_Inquiry_Mode status %#x (results may come without RSSI)\n", got >= 7 ? ev[6] : 0xff);
	static const uint8_t inq[5] = { 0x33, 0x8b, 0x9e, 0x08, 0x00 };	/* GIAC, 8 x 1.28 s, no limit */
	got = cmd(0x0401, inq, 5, ev, sizeof(ev), 2000);
	if (got < 7 || ev[1] != 0x0f || ev[3])
		return fail("Inquiry not started");
	printf("scan: classic inquiry 10.24 s\n");
	double end = now_s() + 15;
	for (;;) {
		double left = (end - now_s()) * 1000;
		got = left > 0 ? event(ev, sizeof(ev), (unsigned)left) : 0;
		if (!got) {
			printf("scan: no Inquiry Complete within 15 s\n");
			break;
		}
		const uint8_t *e = ev + 3;
		unsigned num = got > 3 ? e[0] : 0;
		if (ev[1] == 0x01) {
			printf("scan: inquiry complete, status %#x\n", e[0]);
			break;
		} else if (ev[1] == 0x02 && got >= 3 + 1 + num * 14u) {	/* arrays: addr, psrm, res x2, cod, clk */
			for (unsigned i = 0; i < num; i++)
				found(e + 1 + 6 * i, 0, 0, NULL, 0);
		} else if (ev[1] == 0x22 && got >= 3 + 1 + num * 14u) {	/* addr, psrm, res, cod, clk, rssi */
			for (unsigned i = 0; i < num; i++)
				found(e + 1 + 6 * i, 0, (int8_t)e[1 + 13 * num + i], NULL, 0);
		} else if (ev[1] == 0x2f && got >= 3 + 15u) {
			found(e + 1, 0, (int8_t)e[14], e + 15, got - 18);
		} else
			decode(ev, got);
	}
	static const uint8_t lep[7] = { 0x00, 0x10, 0x00, 0x10, 0x00, 0x00, 0x00 };	/* passive, 10 ms / 10 ms */
	got = cmd(0x200b, lep, 7, ev, sizeof(ev), 2000);
	if (got < 7 || ev[1] != 0x0e || ev[6])
		return fail("LE Set Scan Parameters failed");
	static const uint8_t on[2] = { 1, 1 }, offv[2] = { 0, 0 };
	got = cmd(0x200c, on, 2, ev, sizeof(ev), 2000);
	if (got < 7 || ev[1] != 0x0e || ev[6])
		return fail("LE Set Scan Enable failed");
	printf("scan: le passive %u s\n", secs);
	end = now_s() + secs;
	for (double left; (left = (end - now_s()) * 1000) > 0;) {
		got = event(ev, sizeof(ev), (unsigned)left);
		if (!got)
			break;
		if (ev[1] != 0x3e || got < 5 || ev[3] != 0x02) {
			decode(ev, got);
			continue;
		}
		/* LE Advertising Report: num, then per report type, addr type, addr, data len, data, rssi */
		const uint8_t *r = ev + 5, *lim = ev + (got < sizeof(ev) ? got : sizeof(ev));
		for (unsigned i = 0; i < ev[4] && r + 9 <= lim; i++) {
			unsigned dl = r[8];
			if (r + 9 + dl + 1 > lim)
				break;
			found(r + 2, 1, (int8_t)r[9 + dl], r + 9, dl);
			r += 10 + dl;
		}
	}
	got = cmd(0x200c, offv, 2, ev, sizeof(ev), 2000);
	printf("scan: le disable status %#x\n", got >= 7 && ev[1] == 0x0e ? ev[6] : 0xff);
	unsigned c = 0, l = 0, named = 0;
	for (unsigned i = 0; i < ndev; i++) {
		devs[i].le ? l++ : c++;
		named += !!devs[i].name[0];
	}
	printf("scan: %u devices (%u classic, %u le, %u named); acl %u packets, dropped %u bytes, fifo overruns %u\n",
	       ndev, c, l, named, acl_pkts, rx_dropped, rx_overruns);
	return 0;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 2) {
		fprintf(stderr, "usage: btup status|uart-loop|power|hci-reset|version|fw DIR [--scan SECONDS]|off\n");
		return 2;
	}
	const char *m = argv[1];
	pm_init();
	int rc;
	if (!strcmp(m, "status")) {
		status();
		rc = 0;
	} else if (!strcmp(m, "uart-loop"))
		rc = uart_loop();
	else if (!strcmp(m, "power"))
		rc = power();
	else if (!strcmp(m, "hci-reset"))
		rc = power() || hci_reset();
	else if (!strcmp(m, "version"))
		rc = power() || version();
	else if (!strcmp(m, "fw") && argc > 2) {
		/* bt3: --scan N in the same run (the chip loses the patch at power-off) */
		unsigned secs = argc > 4 && !strcmp(argv[3], "--scan") ? (unsigned)atoi(argv[4]) : 0;
		rc = power() || version() || firmware(argv[2]) || (secs && scan(secs));
	}
	else if (!strcmp(m, "off"))
		rc = off();
	else {
		fprintf(stderr, "btup: unknown mode %s\n", m);
		return 2;
	}
	printf("btup %s: %s\n", m, rc ? "FAILED" : "ok");
	return rc ? 1 : 0;
}
