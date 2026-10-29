/*
 * btbridge: hands the WCN3990 Bluetooth controller to iOS bluetoothd (docs/features/bluetooth.md plan stages 2-3,
 * research/notes/bt-ios.md). Loads the firmware as `btup fw` does (bt_uart.c), then opens a pty, links
 * /dev/cu.bluetooth (or /dev/uart.bluetooth) to its slave and pumps H4 packets both ways between the pty master and
 * the polled SE3 UART. bluetoothd job: BT_HCI_TRANSPORT=H4 BT_BAUDRATE=115200 (the line stays at 115200, termios on
 * the pty is ignored).
 *
 *   btbridge [-n] [-d DIR]   -n: no H4 shim (packets pass unchanged, still logged); DIR: firmware (/var/local/fw/bt)
 * Log: /var/local/btbridge.log (the previous run's: .prev). Off: /var/local/btbridge.off. Runs in the foreground
 * (launchd job org.iosonpixel.btbridge, tools/sunfish-install-btbridge.sh).
 *
 * The shim (bt-ios step 4; facts from iOSPlus10 patches 0333/0336/0337, MIT, research/re/iosplus10/, and the
 * hakios-sm8150 notes; no code taken):
 *   - 0x1002 Read_Local_Supported_Commands: octet 18 bits 2+3 set (Read/Write_Default_Erroneous_Data_Reporting);
 *     bluetoothd's init stops after 0x0c31 without them. Then 0x0c5a/0x0c5b are answered here when the chip lacked them.
 *   - OGF 0x3f (Apple/Broadcom vendor commands): never sent to the chip (QCA uses that space for other things);
 *     answered here with Command Complete status 0 + 8 data bytes, byte 0 = the command's first parameter (bluetoothd's
 *     BCM parser drops answers with fewer than 2 data bytes, and matches 0xfce9 by that sub-opcode echo).
 *   - 0x0c6d Write_LE_Host_Support: Simultaneous_LE_Host -> 0 before the chip; a remaining chip error -> status 0.
 *   - 0x2007 chip status 0x0c -> status 0, 0 dBm.
 *   - chip vendor events (0xff) are not forwarded; every other chip error is logged.
 * Always: ACL from handle 0xedc (the QCA debug log) is dropped, not forwarded.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <util.h>
#include <sys/stat.h>
#include "pmem.h"
#include "bt_uart.h"

#define LOG_PATH	"/var/local/btbridge.log"
#define OFF_FLAG	"/var/local/btbridge.off"
#define WLAND_LOG	"/var/local/wland.log"
#define DEBUG_HANDLE	0xedc
#define VSC_DATA	8

static int shim = 1, master = -1;
static unsigned n_cmd, n_evt, n_acl_up, n_acl_down, n_local, n_dbg;
static int cmds_known, chip_err_rd, chip_err_wr;

static void hexline(const char *dir, const uint8_t *b, unsigned n, unsigned max)
{
	printf("%s", dir);
	for (unsigned i = 0; i < n && i < max; i++)
		printf(" %02x", b[i]);
	printf("%s\n", n > max ? " ..." : "");
}

/* to bluetoothd */
static void up(const uint8_t *p, unsigned n)
{
	for (unsigned at = 0; at < n;) {
		ssize_t w = write(master, p + at, n - at);
		if (w > 0) {
			at += (unsigned)w;
		} else if (w < 0 && errno != EAGAIN && errno != EINTR) {
			printf("pty: write: %s (%u bytes lost)\n", strerror(errno), n - at);
			return;
		} else {
			poll(&(struct pollfd){ .fd = master, .events = POLLOUT }, 1, 100);
		}
	}
}

static void log_event(const char *dir, const uint8_t *p, unsigned n)
{
	if (n >= 7 && p[1] == 0x0e)
		printf("%s evt CC op %#06x status %#x len %u\n", dir, p[4] | p[5] << 8, p[6], p[2]);
	else if (n >= 7 && p[1] == 0x0f)
		printf("%s evt CS op %#06x status %#x\n", dir, p[5] | p[6] << 8, p[3]);
	else
		hexline(dir, p, n, 24);
}

/* a local Command Complete: status, then data (n bytes) */
static void answer(uint16_t op, uint8_t status, const uint8_t *data, unsigned n)
{
	uint8_t ev[7 + 32] = { 0x04, 0x0e, (uint8_t)(4 + n), 1, (uint8_t)op, (uint8_t)(op >> 8), status };
	memcpy(ev + 7, data, n);
	n_local++;
	log_event("<- local", ev, 7 + n);
	up(ev, 7 + n);
}

/* bluetoothd -> chip; returns 1 when the shim answered it */
static int host_cmd(uint8_t *p, unsigned n)
{
	uint16_t op = p[1] | p[2] << 8;
	n_cmd++;
	hexline("-> cmd", p, n, 4 + 16);
	if (!shim)
		return 0;
	if (op >> 10 == 0x3f) {
		/* btbridge shim: vendor commands answered here, status 0 + VSC_DATA bytes (sub-opcode echo) */
		uint8_t d[VSC_DATA] = { p[3] && n > 4 ? p[4] : 0 };
		printf("shim: %#06x answered locally, status 0 + %u data bytes\n", op, VSC_DATA);
		answer(op, 0, d, VSC_DATA);
		return 1;
	}
	if (cmds_known && ((op == 0x0c5b && !chip_err_wr) || (op == 0x0c5a && !chip_err_rd))) {
		/* btbridge shim: not in the chip's mask (we set the bit) -> answer: write status 0, read status 0 + 0x00 */
		uint8_t d = 0;
		printf("shim: %#06x answered locally, status 0\n", op);
		answer(op, 0, &d, op == 0x0c5a);
		return 1;
	}
	if (op == 0x0c6d && p[3] >= 2 && n >= 6 && p[5]) {
		/* btbridge shim: Simultaneous_LE_Host must be 0 since Core 4.1 */
		printf("shim: 0x0c6d Simultaneous_LE_Host %#x -> 0\n", p[5]);
		p[5] = 0;
	}
	return 0;
}

/* chip -> bluetoothd; returns 0 when the packet is dropped */
static int chip_evt(uint8_t *p, unsigned n)
{
	n_evt++;
	log_event("<- chip", p, n);
	if (n >= 7 && ((p[1] == 0x0e && p[6]) || (p[1] == 0x0f && p[3]))) {
		uint16_t op = p[1] == 0x0e ? p[4] | p[5] << 8 : p[5] | p[6] << 8;
		printf("chip error: op %#06x status %#x%s\n", op, p[1] == 0x0e ? p[6] : p[3],
		       op == 0x200b || op == 0x200c || op == 0x2006 ? " (legacy LE scan/adv: extended translation?)" : "");
	}
	if (!shim)
		return 1;
	if (p[1] == 0xff) {
		printf("shim: vendor event dropped\n");
		return 0;
	}
	if (p[1] != 0x0e || n < 7)
		return 1;
	uint16_t op = p[4] | p[5] << 8;
	if (op == 0x1002 && !p[6] && n >= 7 + 64) {
		uint8_t *m = p + 7;
		cmds_known = 1;
		chip_err_rd = !!(m[18] & 0x04);
		chip_err_wr = !!(m[18] & 0x08);
		hexline("shim: supported commands", m, 64, 64);
		if ((m[18] & 0x0c) != 0x0c) {
			/* btbridge shim: Read/Write_Default_Erroneous_Data_Reporting bits, else init stops after 0x0c31 */
			printf("shim: supported commands octet 18 %#x -> %#x\n", m[18], m[18] | 0x0c);
			m[18] |= 0x0c;
		}
	} else if (op == 0x0c6d && p[6]) {
		printf("shim: 0x0c6d status %#x -> 0\n", p[6]);
		p[6] = 0;
	} else if (op == 0x2007 && p[6] == 0x0c && n >= 8) {
		printf("shim: 0x2007 status 0xc -> 0, 0 dBm\n");
		p[6] = 0;
		p[7] = 0;
	}
	return 1;
}

/* H4 framing of the host stream: 01 cmd, 02 ACL, 03 SCO; anything else is dropped a byte at a time */
static uint8_t hb[4096];
static unsigned hn, host_dropped;

static void host_input(void)
{
	ssize_t r = read(master, hb + hn, sizeof(hb) - hn);
	if (r <= 0)
		return;
	hn += (unsigned)r;
	for (;;) {
		unsigned need = 0;
		if (!hn)
			break;
		if (hb[0] == 0x01 && hn >= 4)
			need = 4u + hb[3];
		else if (hb[0] == 0x02 && hn >= 5)
			need = 5u + (hb[3] | hb[4] << 8);
		else if (hb[0] == 0x03 && hn >= 4)
			need = 4u + hb[3];
		if (hb[0] < 1 || hb[0] > 3 || need > sizeof(hb)) {
			host_dropped++;
			memmove(hb, hb + 1, --hn);
			continue;
		}
		if (!need || hn < need)
			break;
		int done = 0;
		if (hb[0] == 0x01)
			done = host_cmd(hb, need);
		else if (n_acl_up++ < 8 || n_acl_up % 256 == 0)
			printf("-> %s handle %#x len %u\n", hb[0] == 0x02 ? "acl" : "sco", (hb[1] | hb[2] << 8) & 0xfff,
			       need - (hb[0] == 0x02 ? 5 : 4));
		if (!done && tx(hb, need)) {
			printf("uart: tx of %u bytes failed\n", need);
			failed = 0;
		}
		memmove(hb, hb + need, hn -= need);
	}
}

static void chip_input(void)
{
	static uint8_t p[2100];
	unsigned n = pkt(p, sizeof(p), 1);
	if (!n)
		return;
	if (p[0] == 0x02) {
		unsigned h = (p[1] | p[2] << 8) & 0xfff;
		if (h == DEBUG_HANDLE) {
			n_dbg++;
			return;
		}
		if (n_acl_down++ < 8 || n_acl_down % 256 == 0)
			printf("<- acl handle %#x len %u\n", h, n - 5);
		up(p, n);
	} else if (chip_evt(p, n)) {
		up(p, n);
	}
}

/* pty + device link; returns the slave fd (kept open: raw mode, and no EIO on the master while no client is there) */
static int open_pty(void)
{
	int s;
	char name[128];
	if (openpty(&master, &s, name, NULL, NULL)) {
		printf("pty: openpty: %s\n", strerror(errno));
		return -1;
	}
	struct termios t;
	tcgetattr(s, &t);
	cfmakeraw(&t);
	tcsetattr(s, TCSANOW, &t);
	fchmod(s, 0666);			/* bluetoothd runs as mobile */
	fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
	printf("pty: master %d slave %s\n", master, name);
	static const char *links[] = { "/dev/cu.bluetooth", "/dev/uart.bluetooth" };
	int ok = 0;
	for (unsigned i = 0; i < 2 && !ok; i++) {
		unlink(links[i]);
		int rc = symlink(name, links[i]);
		printf("pty: symlink %s -> %s: %s\n", links[i], name, rc ? strerror(errno) : "ok");
		ok = !rc;
	}
	if (!ok)
		printf("pty: no device link: bluetoothd will not find the transport\n");
	return s;
}

static void wait_wifi(void)
{
	char line[256];
	for (int t = 0; t < 60; t++) {
		FILE *f = fopen(WLAND_LOG, "r");
		int up = 0;
		while (f && fgets(line, sizeof(line), f))
			up |= !!strstr(line, "modem ready");
		if (f)
			fclose(f);
		if (up) {
			printf("wifi: rails up after %d s\n", t);
			return;
		}
		sleep(1);
	}
	printf("wifi: no \"modem ready\" in %s after 60 s: trying anyway\n", WLAND_LOG);
}

int main(int argc, char **argv)
{
	const char *dir = "/var/local/fw/bt";
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-n"))
			shim = 0;
		else if (!strcmp(argv[i], "-d") && i + 1 < argc)
			dir = argv[++i];
		else {
			fprintf(stderr, "usage: btbridge [-n] [-d DIR]\n");
			return 2;
		}
	}
	if (!access(OFF_FLAG, F_OK)) {
		fprintf(stderr, "btbridge: %s exists: off\n", OFF_FLAG);
		return 0;
	}
	if (isatty(1) == 0) {
		rename(LOG_PATH, LOG_PATH ".prev");
		freopen(LOG_PATH, "a", stdout);
		dup2(1, 2);
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("btbridge: start, shim %s, firmware %s\n", shim ? "on" : "off (-n)", dir);
	pm_init();
	wait_wifi();
	int ok = 0;
	for (int k = 1; k <= 3 && !ok; k++) {
		failed = 0;
		ok = !(power() || version() || firmware(dir));
		if (!ok) {
			printf("btbridge: bring-up %d failed\n", k);
			off();
			sleep(5);
		}
	}
	if (!ok) {
		printf("btbridge: no controller, stop\n");
		return 1;
	}
	if (open_pty() < 0)
		return 1;
	printf("btbridge: ready, pumping\n");
	double stats = now_s() + 60;
	for (;;) {
		struct pollfd pf = { .fd = master, .events = POLLIN };
		if (poll(&pf, 1, 0) > 0 && (pf.revents & POLLIN))
			host_input();
		chip_input();
		if (now_s() > stats) {
			printf("stats: cmd %u evt %u local %u acl up %u down %u debug-log %u; host dropped %u, uart dropped %u, "
			       "overruns %u\n", n_cmd, n_evt, n_local, n_acl_up, n_acl_down, n_dbg, host_dropped, rx_dropped,
			       rx_overruns);
			stats = now_s() + 60;
		}
	}
}
