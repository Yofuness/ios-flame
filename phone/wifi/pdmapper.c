/*
 * pd-mapper: the service registry locator (QMI service 0x40), which tells
 * the modem's PD framework which protection domains provide a service. The
 * table is the firmware's own .jsn files (vendor/firmware/modemuw.jsn and
 * the radio's modemr.jsn), as github.com/andersson/pd-mapper reads them.
 */
#include <stdio.h>
#include <string.h>
#include "link.h"
#include "pdmapper.h"
#include "qmi.h"

#define GET_DOMAIN_LIST	0x21
#define PFR		0x24	/* protection domain failure report */

static const struct { const char *service, *domain; uint32_t instance; } db[] = {
	{ "tms/servreg", "msm/modem/root_pd", 180 },
	{ "gps/gps_service", "msm/modem/root_pd", 180 },
	{ "kernel/elf_loader", "msm/modem/wlan_pd", 180 },
	{ "tms/servreg", "msm/modem/wlan_pd", 180 },
	{ "wlan/fw", "msm/modem/wlan_pd", 180 },
	/* audio step 1 (research/notes/audio-sm7150-reference.md): the ADSP's PDs, as the SM7150 reference serves them */
	{ "tms/servreg", "msm/adsp/root_pd", 74 },
	{ "tms/pdr_enabled", "msm/adsp/root_pd", 74 },
	{ "tms/servreg", "msm/adsp/audio_pd", 74 },
	{ "avs/audio", "msm/adsp/audio_pd", 74 },
	{ "tms/servreg", "msm/adsp/sensor_pd", 74 },
};

int pdmapper_init(void)
{
	if (qrtr_new_server(PDMAPPER_SERVICE, 1, 1, PDMAPPER_PORT))
		return -1;
	printf("pd-mapper: serving at %u:%x\n", QRTR_NODE_APPS, PDMAPPER_PORT);
	return 0;
}

void pdmapper_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n)
{
	struct qmi_msg m;
	struct qmi_buf q;
	if (qmi_parse(d, n, &m) || m.type != QMI_REQUEST)
		return;
	qmi_start(&q, QMI_RESPONSE, m.txn, m.id);
	if (m.id == PFR) {
		/* a protection domain failed: the service name and the reason (the PD's SFR string) */
		uint16_t l1 = 0, l2 = 0;
		const uint8_t *svc = qmi_get(&m, 1, &l1), *why = qmi_get(&m, 2, &l2);
		printf("pd-mapper: PD FAILURE \"%.*s\": %.*s\n", svc ? l1 : 0, svc ? (const char *)svc : "",
		       why ? l2 : 0, why ? (const char *)why : "");
		qmi_tlv_result(&q, 0, 0);
	} else if (m.id != GET_DOMAIN_LIST) {
		qmi_dump(&m);
		qmi_tlv_result(&q, 1, 0x47);
	} else {
		uint16_t l = 0;
		const uint8_t *p = qmi_get(&m, 1, &l);
		char name[80] = { 0 };
		uint32_t offset = 0;
		if (p)
			memcpy(name, p, l < 79 ? l : 79);
		qmi_get_u32(&m, 0x10, &offset);
		/* domain_list[]: u8 count, then { u8 len + name, u32 instance, u8 valid, u32 data } */
		uint8_t list[512];
		size_t o = 1;
		uint16_t total = 0;
		for (unsigned i = 0; i < sizeof(db) / sizeof(db[0]); i++) {
			if (strcmp(db[i].service, name))
				continue;
			if (total++ < offset)
				continue;
			uint8_t nl = (uint8_t)strlen(db[i].domain);
			list[o++] = nl;
			memcpy(list + o, db[i].domain, nl);
			o += nl;
			memcpy(list + o, &db[i].instance, 4);
			o += 4;
			list[o++] = 0;
			memset(list + o, 0, 4);
			o += 4;
			list[0]++;
		}
		printf("pd-mapper: \"%s\" -> %u domain(s)\n", name, total);
		qmi_tlv_result(&q, 0, 0);
		qmi_tlv(&q, 0x10, &total, 2);
		uint16_t rev = 1;
		qmi_tlv(&q, 0x11, &rev, 2);
		qmi_tlv(&q, 0x12, list, (uint16_t)o);
	}
	qrtr_send(QRTR_TYPE_DATA, PDMAPPER_PORT, h->src_node, h->src_port, q.b, (uint32_t)qmi_finish(&q));
}
