/*
 * ipnet: IPv4 over the joined WLAN, from userland. A utun interface carries the phone's packets;
 * this file does what the kernel's Ethernet layer would (ARP both ways, Ethernet framing), a DHCP
 * client, then puts the lease on the utun and publishes it to configd with netsvc (phone/tools)
 * so IPMonitor routes through it and mDNSResponder gets the DNS servers.
 */
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include "link.h"
#include "radio.h"
#include "ipnet.h"

#define LOG(fmt, ...)	printf("[%6llu] ipnet: " fmt "\n", (unsigned long long)link_ms(), ##__VA_ARGS__)

/* sys/kern_control.h, sys/sys_domain.h, net/if_utun.h (not in the iOS SDK) */
#define SYSPROTO_CONTROL	2
#define AF_SYS_CONTROL		2
#define MAX_KCTL_NAME		96
struct ctl_info { uint32_t ctl_id; char ctl_name[MAX_KCTL_NAME]; };
struct sockaddr_ctl { uint8_t sc_len, sc_family; uint16_t ss_sysaddr; uint32_t sc_id, sc_unit, sc_reserved[5]; };
#define CTLIOCGINFO		_IOWR('N', 3, struct ctl_info)
#define UTUN_OPT_IFNAME		2

#define NETSVC		"/var/local/bin/netsvc"

static int tun = -1;
static char tun_name[IFNAMSIZ];
static int link_up_now;
static uint32_t my_ip, my_mask, router, dns[2], server, lease_s;	/* network order */
static uint64_t bound_ms;

static uint16_t csum(const void *p, uint32_t len, uint32_t sum)
{
	const uint8_t *b = p;
	for (uint32_t i = 0; i + 1 < len; i += 2)
		sum += (uint32_t)(b[i] << 8 | b[i + 1]);
	if (len & 1)
		sum += (uint32_t)(b[len - 1] << 8);
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)~sum;
}

static int tun_open(void)
{
	struct ctl_info ci;
	struct sockaddr_ctl sc;
	socklen_t nl = sizeof(tun_name);
	int fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
	if (fd < 0) {
		LOG("utun: socket: %s", strerror(errno));
		return -1;
	}
	memset(&ci, 0, sizeof(ci));
	snprintf(ci.ctl_name, sizeof(ci.ctl_name), "com.apple.net.utun_control");
	memset(&sc, 0, sizeof(sc));
	if (ioctl(fd, CTLIOCGINFO, &ci) < 0)
		goto fail;
	sc.sc_len = sizeof(sc);
	sc.sc_family = AF_SYSTEM;
	sc.ss_sysaddr = AF_SYS_CONTROL;
	sc.sc_id = ci.ctl_id;
	sc.sc_unit = 0;					/* the next free utunN */
	if (connect(fd, (struct sockaddr *)&sc, sizeof(sc)) < 0 ||
	    getsockopt(fd, SYSPROTO_CONTROL, UTUN_OPT_IFNAME, tun_name, &nl) < 0)
		goto fail;
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	tun = fd;
	LOG("utun: %s", tun_name);
	return 0;
fail:
	LOG("utun: %s", strerror(errno));
	close(fd);
	return -1;
}

/* the lease on the utun (point to point to the router) and to configd */
static void tun_configure(void)
{
	struct ifaliasreq ifra;
	struct sockaddr_in *a;
	char ip[16], mask[16], gw[16], d0[16], d1[16];
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	memset(&ifra, 0, sizeof(ifra));
	snprintf(ifra.ifra_name, sizeof(ifra.ifra_name), "%s", tun_name);
	a = (struct sockaddr_in *)&ifra.ifra_addr;
	a->sin_len = sizeof(*a);
	a->sin_family = AF_INET;
	a->sin_addr.s_addr = my_ip;
	a = (struct sockaddr_in *)&ifra.ifra_broadaddr;
	a->sin_len = sizeof(*a);
	a->sin_family = AF_INET;
	a->sin_addr.s_addr = router;
	a = (struct sockaddr_in *)&ifra.ifra_mask;
	a->sin_len = sizeof(*a);
	a->sin_family = AF_INET;
	a->sin_addr.s_addr = 0xffffffff;
	if (ioctl(s, SIOCAIFADDR, &ifra) < 0)
		LOG("%s: SIOCAIFADDR: %s", tun_name, strerror(errno));
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", tun_name);
	if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
		ifr.ifr_flags |= IFF_UP;
		ioctl(s, SIOCSIFFLAGS, &ifr);
	}
	close(s);
	inet_ntop(AF_INET, &my_ip, ip, sizeof(ip));
	inet_ntop(AF_INET, &my_mask, mask, sizeof(mask));
	inet_ntop(AF_INET, &router, gw, sizeof(gw));
	inet_ntop(AF_INET, &dns[0], d0, sizeof(d0));
	inet_ntop(AF_INET, &dns[1], d1, sizeof(d1));
	char *argv[] = { "netsvc", "up", tun_name, ip, mask, gw, dns[0] ? d0 : gw, dns[1] ? d1 : NULL, NULL };
	pid_t pid;
	int e = posix_spawn(&pid, NETSVC, NULL, NULL, argv, NULL);
	LOG("%s: %s/%s router %s dns %s %s; netsvc -> %s", tun_name, ip, mask, gw, dns[0] ? d0 : gw, dns[1] ? d1 : "",
	    e ? strerror(e) : "started");
}

/* the link went down: the lease off the utun and out of configd (IPMonitor falls back to another service) */
static void tun_unconfigure(void)
{
	if (!my_ip)
		return;
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", tun_name);
	struct sockaddr_in *a = (struct sockaddr_in *)&ifr.ifr_addr;
	a->sin_len = sizeof(*a);
	a->sin_family = AF_INET;
	a->sin_addr.s_addr = my_ip;
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (ioctl(s, SIOCDIFADDR, &ifr) < 0)
		LOG("%s: SIOCDIFADDR: %s", tun_name, strerror(errno));
	close(s);
	my_ip = 0;
	char *argv[] = { "netsvc", "down", tun_name, NULL };
	pid_t pid;
	int e = posix_spawn(&pid, NETSVC, NULL, NULL, argv, NULL);
	LOG("%s: lease dropped; netsvc down -> %s", tun_name, e ? strerror(e) : "started");
}

/* ---------------------------------------------------------------- Ethernet and ARP */

static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

#define NARP	16
static struct { uint32_t ip; uint8_t mac[6]; } arp[NARP];
static unsigned arp_next;

#define NPEND	16
static struct { uint32_t hop; uint16_t len; uint8_t pkt[1500]; } pend[NPEND];
static unsigned npend;
static uint64_t arp_asked_ms, arp_first_ms;

static int eth_send(const uint8_t *dst, uint16_t type, const uint8_t *payload, uint32_t len)
{
	static uint8_t e[14 + 1600];
	if (len > sizeof(e) - 14)
		return -1;
	memcpy(e, dst, 6);
	memcpy(e + 6, radio_mac, 6);
	e[12] = (uint8_t)(type >> 8);
	e[13] = (uint8_t)type;
	memcpy(e + 14, payload, len);
	return radio_tx_ether(e, 14 + len);
}

static const uint8_t *arp_find(uint32_t ip)
{
	for (unsigned i = 0; i < NARP; i++)
		if (arp[i].ip == ip)
			return arp[i].mac;
	return NULL;
}

static void arp_learn(uint32_t ip, const uint8_t *mac)
{
	if (!ip)
		return;
	for (unsigned i = 0; i < NARP; i++) {
		if (arp[i].ip == ip) {
			memcpy(arp[i].mac, mac, 6);
			return;
		}
	}
	unsigned i = arp_next++ % NARP;
	arp[i].ip = ip;
	memcpy(arp[i].mac, mac, 6);
}

static void arp_send(uint16_t op, const uint8_t *tha, uint32_t tpa, const uint8_t *dst)
{
	uint8_t a[28] = { 0x00, 0x01, 0x08, 0x00, 6, 4, 0, (uint8_t)op };
	memcpy(a + 8, radio_mac, 6);
	memcpy(a + 14, &my_ip, 4);
	memcpy(a + 18, tha, 6);
	memcpy(a + 24, &tpa, 4);
	eth_send(dst, 0x0806, a, sizeof(a));
}

static void arp_rx(const uint8_t *a, uint32_t len)
{
	uint32_t spa, tpa;
	if (len < 28 || a[0] != 0 || a[1] != 1 || a[2] != 8 || a[3] != 0)
		return;
	memcpy(&spa, a + 14, 4);
	memcpy(&tpa, a + 24, 4);
	arp_learn(spa, a + 8);
	if (a[7] == 1 && my_ip && tpa == my_ip)
		arp_send(2, a + 8, spa, a + 8);		/* who has our address: we do */
	/* packets waiting for this address */
	for (unsigned i = 0; i < npend;) {
		const uint8_t *mac = arp_find(pend[i].hop);
		if (mac) {
			eth_send(mac, 0x0800, pend[i].pkt, pend[i].len);
			pend[i] = pend[--npend];
		} else {
			i++;
		}
	}
}

/* an IPv4 packet out: to its next hop's MAC (ARP first if unknown) */
static void ip_out(const uint8_t *p, uint32_t len)
{
	uint32_t dst;
	if (len < 20 || len > sizeof(pend[0].pkt))
		return;
	memcpy(&dst, p + 16, 4);
	uint32_t h = ntohl(dst);
	if (dst == 0xffffffff || (my_mask && (dst | my_mask) == 0xffffffff)) {	/* limited or subnet broadcast */
		eth_send(bcast, 0x0800, p, len);
		return;
	}
	if ((h >> 28) == 0xe) {				/* multicast: 01:00:5e + low 23 bits */
		uint8_t m[6] = { 0x01, 0x00, 0x5e, (uint8_t)((h >> 16) & 0x7f), (uint8_t)(h >> 8), (uint8_t)h };
		eth_send(m, 0x0800, p, len);
		return;
	}
	uint32_t hop = (dst & my_mask) == (my_ip & my_mask) ? dst : router;
	const uint8_t *mac = arp_find(hop);
	if (mac) {
		eth_send(mac, 0x0800, p, len);
		return;
	}
	if (!npend)
		arp_first_ms = link_ms();
	if (npend < NPEND) {
		pend[npend].hop = hop;
		pend[npend].len = (uint16_t)len;
		memcpy(pend[npend].pkt, p, len);
		npend++;
	}
	arp_send(1, (const uint8_t[6]){ 0 }, hop, bcast);
	arp_asked_ms = link_ms();
}

/* ---------------------------------------------------------------- DHCP client */

enum { D_IDLE, D_SELECTING, D_REQUESTING, D_BOUND, D_RENEWING };
static int dstate;
static uint32_t xid, offered;
static uint64_t dsent_ms, dstart_ms;
static unsigned dtries;

static void dhcp_send(int type)
{
	uint8_t p[20 + 8 + 300] = { 0 }, *b = p + 28, *o;
	b[0] = 1;					/* BOOTREQUEST */
	b[1] = 1;					/* Ethernet */
	b[2] = 6;
	memcpy(b + 4, &xid, 4);
	uint16_t secs = htons((uint16_t)((link_ms() - dstart_ms) / 1000));
	memcpy(b + 8, &secs, 2);
	if (dstate == D_RENEWING)
		memcpy(b + 12, &my_ip, 4);		/* ciaddr */
	else
		b[10] = 0x80;				/* broadcast reply */
	memcpy(b + 28, radio_mac, 6);
	o = b + 236;
	*o++ = 99; *o++ = 130; *o++ = 83; *o++ = 99;	/* magic cookie */
	*o++ = 53; *o++ = 1; *o++ = (uint8_t)type;
	*o++ = 61; *o++ = 7; *o++ = 1;
	memcpy(o, radio_mac, 6);
	o += 6;
	if (type == 3 && dstate == D_REQUESTING) {
		*o++ = 50; *o++ = 4;
		memcpy(o, &offered, 4);
		o += 4;
		*o++ = 54; *o++ = 4;
		memcpy(o, &server, 4);
		o += 4;
	}
	*o++ = 12; *o++ = 6;
	memcpy(o, "iPhone", 6);
	o += 6;
	*o++ = 55; *o++ = 6; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 15; *o++ = 51; *o++ = 54;
	*o++ = 255;
	uint32_t blen = (uint32_t)(o - b);
	if (blen < 300)
		blen = 300;
	/* IP + UDP headers: 0.0.0.0 (or ours) -> 255.255.255.255, 68 -> 67, no UDP checksum */
	uint8_t *ip = p, *u = p + 20;
	uint16_t tot = (uint16_t)(20 + 8 + blen), ul = (uint16_t)(8 + blen);
	ip[0] = 0x45;
	ip[2] = (uint8_t)(tot >> 8);
	ip[3] = (uint8_t)tot;
	ip[8] = 64;
	ip[9] = 17;
	if (dstate == D_RENEWING)
		memcpy(ip + 12, &my_ip, 4);
	memset(ip + 16, 0xff, 4);
	uint16_t c = csum(ip, 20, 0);
	ip[10] = (uint8_t)(c >> 8);
	ip[11] = (uint8_t)c;
	u[1] = 68;
	u[3] = 67;
	u[4] = (uint8_t)(ul >> 8);
	u[5] = (uint8_t)ul;
	eth_send(bcast, 0x0800, p, tot);
	dsent_ms = link_ms();
	LOG("dhcp: %s (xid %08x, try %u)", type == 1 ? "DISCOVER" : "REQUEST", ntohl(xid), dtries + 1);
}

static void dhcp_start(void)
{
	arc4random_buf(&xid, sizeof(xid));
	dstate = D_SELECTING;
	dtries = 0;
	dstart_ms = link_ms();
	dhcp_send(1);
}

static void dhcp_rx(const uint8_t *b, uint32_t len)
{
	uint32_t yi, mask = 0, gw = 0, sid = 0, lt = 0, d[2] = { 0, 0 };
	int type = 0;
	if (len < 240 || b[0] != 2 || memcmp(b + 4, &xid, 4) || memcmp(b + 28, radio_mac, 6) ||
	    b[236] != 99 || b[237] != 130 || b[238] != 83 || b[239] != 99)
		return;
	memcpy(&yi, b + 16, 4);
	for (uint32_t o = 240; o < len && b[o] != 255;) {
		if (b[o] == 0) {
			o++;
			continue;
		}
		if (o + 2 > len || o + 2 + b[o + 1] > len)
			break;
		const uint8_t *v = b + o + 2;
		uint8_t l = b[o + 1];
		switch (b[o]) {
		case 53: if (l >= 1) type = v[0]; break;
		case 1: if (l >= 4) memcpy(&mask, v, 4); break;
		case 3: if (l >= 4) memcpy(&gw, v, 4); break;
		case 6: if (l >= 4) memcpy(&d[0], v, 4); if (l >= 8) memcpy(&d[1], v + 4, 4); break;
		case 51: if (l >= 4) { memcpy(&lt, v, 4); lt = ntohl(lt); } break;
		case 54: if (l >= 4) memcpy(&sid, v, 4); break;
		}
		o += 2 + l;
	}
	char a[16], s[16];
	inet_ntop(AF_INET, &yi, a, sizeof(a));
	inet_ntop(AF_INET, &sid, s, sizeof(s));
	if (type == 2 && dstate == D_SELECTING) {
		LOG("dhcp: OFFER %s from %s", a, s);
		offered = yi;
		server = sid;
		dstate = D_REQUESTING;
		dtries = 0;
		dhcp_send(3);
	} else if (type == 5 && (dstate == D_REQUESTING || dstate == D_RENEWING)) {
		int renewed = dstate == D_RENEWING && yi == my_ip;
		my_ip = yi;
		my_mask = mask ? mask : htonl(0xffffff00);
		router = gw;
		dns[0] = d[0];
		dns[1] = d[1];
		server = sid;
		lease_s = lt ? lt : 3600;
		dstate = D_BOUND;
		bound_ms = link_ms();
		LOG("*** dhcp: ACK %s, lease %u s ***", a, lease_s);
		if (!renewed)
			tun_configure();
	} else if (type == 6) {
		LOG("dhcp: NAK: starting over");
		dstate = D_IDLE;
		dhcp_start();
	}
}

static void dhcp_poll(void)
{
	uint64_t now = link_ms();
	if ((dstate == D_SELECTING || dstate == D_REQUESTING || dstate == D_RENEWING) && now - dsent_ms > 2000 + 1000u * dtries) {
		if (++dtries > 6) {
			LOG("dhcp: no answer, starting over");
			dhcp_start();
		} else {
			dhcp_send(dstate == D_SELECTING ? 1 : 3);
		}
	} else if (dstate == D_BOUND && now - bound_ms > 500ull * lease_s) {
		dstate = D_RENEWING;				/* T1 */
		dtries = 0;
		dhcp_send(3);
	}
}

/* ---------------------------------------------------------------- frames in, packets out */

static void ipnet_rx(const uint8_t *e, uint32_t len)
{
	if (len < 14)
		return;
	uint16_t type = (uint16_t)(e[12] << 8 | e[13]);
	const uint8_t *p = e + 14;
	len -= 14;
	if (type == 0x0806) {
		arp_rx(p, len);
		return;
	}
	if (type != 0x0800 || len < 20 || (p[0] >> 4) != 4)
		return;
	uint32_t hl = (p[0] & 15) * 4u, tot = (uint32_t)(p[2] << 8 | p[3]), src, dst;
	if (tot > len || hl < 20 || tot < hl)
		return;
	memcpy(&src, p + 12, 4);
	memcpy(&dst, p + 16, 4);
	if (memcmp(e, bcast, 6))
		arp_learn(src, e + 6);			/* unicast from that host: its MAC (or the router's) */
	if (p[9] == 17 && tot >= hl + 8 && p[hl + 2] == 0 && p[hl + 3] == 68) {
		dhcp_rx(p + hl + 8, tot - hl - 8);
		return;
	}
	if (tun < 0 || !my_ip || dstate == D_IDLE)
		return;
	static uint8_t out[4 + 2048];
	uint32_t af = htonl(AF_INET);
	memcpy(out, &af, 4);
	memcpy(out + 4, p, tot);
	write(tun, out, 4 + tot);
}

static void ipnet_link(int up)
{
	LOG("link %s", up ? "UP" : "down");
	link_up_now = up;
	if (up && dstate == D_IDLE)
		dhcp_start();
	if (!up) {
		dstate = D_IDLE;
		npend = 0;
		tun_unconfigure();
	}
}

int ipnet_init(void)
{
	radio_rx_hook = ipnet_rx;
	radio_link_hook = ipnet_link;
	return tun_open();
}

void ipnet_poll(void)
{
	static uint8_t in[4 + 2048];
	if (!link_up_now)
		return;
	dhcp_poll();
	if (npend && link_ms() - arp_first_ms > 3000) {
		npend = 0;					/* nobody answered */
	} else if (npend && link_ms() - arp_asked_ms > 1000) {
		arp_send(1, (const uint8_t[6]){ 0 }, pend[0].hop, bcast);
		arp_asked_ms = link_ms();
	}
	for (int n = 0; tun >= 0 && dstate >= D_BOUND && n < 16; n++) {
		ssize_t r = read(tun, in, sizeof(in));
		if (r <= 4)
			break;
		uint32_t af;
		memcpy(&af, in, 4);
		link_traffic++;	/* O12 */
		if (ntohl(af) == AF_INET)
			ip_out(in + 4, (uint32_t)r - 4);
	}
}
