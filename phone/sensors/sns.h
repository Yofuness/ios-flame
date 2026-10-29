/*
 * From ios-redfin (DenialOfGravity, https://github.com/DenialOfGravity/ios-redfin) phone/sensors/sns.h,
 * Copyright (c) 2026 dipa-studio, MIT License (see LICENSE.redfin). Sunfish changes are marked "sunfish".
 */
/*
 * Qualcomm SEE ("Sensors Execution Environment") client protocol, as the Android sensors HAL and libssc
 * (codeberg.org/DylanVanAssche/libssc) speak it: QMI service 400 (SNS client) over QRTR, carrying protobuf
 * messages. Transport-agnostic: these functions build and parse byte buffers only.
 *
 * QMI (service 400, IDL sns_client_api_v01):
 *   0x20 SNS_CLIENT_REQ   TLV 0x01 payload (u16 length + protobuf sns_client_request_msg),
 *                         TLV 0x10 use_jumbo_report (u8, optional)
 *        SNS_CLIENT_RESP  TLV 0x02 result (u16 result, u16 error), TLV 0x10 client_id (u64), TLV 0x11 result (u32)
 *   0x21 SNS_CLIENT_REPORT_IND        TLV 0x01 client_id (u64), TLV 0x02 payload (u16 length + sns_client_event_msg)
 *   0x22 SNS_CLIENT_JUMBO_REPORT_IND  same layout, larger payload
 *
 * Protobuf (field numbers as in libssc's copies of sns_client.proto / sns_std.proto / sns_std_sensor.proto /
 * sns_suid.proto / sns_proximity.proto; see the WS-S notes in docs/redfin/qol-plan.md for what was verified):
 *   sns_std_suid            { 1 fixed64 suid_low; 2 fixed64 suid_high }
 *   sns_client_request_msg  { 1 sns_std_suid suid; 2 fixed32 msg_id; 3 suspend_config susp_config;
 *                             4 sns_std_request request }
 *     suspend_config        { 1 enum client_proc_type (APSS = 1); 2 enum delivery_type (WAKEUP = 0, NO_WAKEUP = 1) }
 *   sns_std_request         { 1 batch_spec batching; 2 bytes payload; 3 bool is_passive }
 *   sns_client_event_msg    { 1 sns_std_suid suid; 2 repeated sns_client_event events }
 *     sns_client_event      { 1 fixed32 msg_id; 2 fixed64 timestamp; 3 bytes payload }
 *   SUID lookup: sensor abababab abababab / abababab abababab,
 *     512 sns_suid_req      { 1 string data_type; 2 bool register_updates; 3 bool default_only }
 *     768 sns_suid_event    { 1 string data_type; 2 repeated sns_std_suid suid }
 *   1   sns_std_attr_req    { 1 bool register_updates }
 *   128 sns_std_attr_event  { 1 repeated sns_std_attr attributes }
 *       sns_std_attr        { 1 int32 attr_id; 2 sns_std_attr_value value }
 *       sns_std_attr_value  { 1 repeated data values }, data { 1 subtype (sns_std_attr_value); 2 string str;
 *                                                             3 float flt; 4 sint64 sint; 5 bool boolean }
 *   130 sns_std_error_event { 1 enum error }
 *   513 sns_std_sensor_config { 1 float sample_rate }  (streaming sensors)
 *   514 on-change config (empty payload)               (proximity, ambient_light)
 *   768 sns_std_sensor_physical_config_event (ignored)
 *   1025 sns_std_sensor_event { 1 repeated float data; 2 enum status (0 unreliable .. 3 high) }
 *   proximity: sns_proximity_event { 1 enum proximity_event_type (0 far, 1 near); 2 uint32 raw_adc; 3 status }
 *              (msg id 769 on redfin's TCS3701; also accepted >= 1024, told apart from 1025 by wire type)
 *   10  disable (empty request)
 */
#ifndef SNS_H
#define SNS_H
#include <stdint.h>
#include <stddef.h>

#define SNS_QMI_SERVICE		400
#define SNS_QMI_CLIENT_REQ	0x20
#define SNS_QMI_REPORT_IND	0x21
#define SNS_QMI_JUMBO_IND	0x22

#define SNS_MSG_DISABLE		10
#define SNS_MSG_ATTR_REQ	1
#define SNS_MSG_FLUSH_REQ	2
#define SNS_MSG_ATTR_EVENT	128
#define SNS_MSG_FLUSH_EVENT	129
#define SNS_MSG_ERROR_EVENT	130
#define SNS_MSG_SUID_REQ	512
#define SNS_MSG_SENSOR_CONFIG	513
#define SNS_MSG_ONCHANGE_CONFIG	514
#define SNS_MSG_SUID_EVENT	768
#define SNS_MSG_PHYS_CONFIG_EVENT 768	/* same number, from a sensor's own SUID */
#define SNS_MSG_SENSOR_EVENT	1025
#define SNS_MSG_PROX_EVENT	769	/* SNS_PROXIMITY_MSGID_SNS_PROXIMITY_EVENT (seen on the phone) */

#define SNS_PROC_APSS		1
#define SNS_DELIVERY_WAKEUP	0
#define SNS_DELIVERY_NO_WAKEUP	1

/* sns_std_sensor_attr_id */
enum {
	SNS_ATTR_NAME = 0, SNS_ATTR_VENDOR = 1, SNS_ATTR_TYPE = 2, SNS_ATTR_AVAILABLE = 3, SNS_ATTR_VERSION = 4,
	SNS_ATTR_API = 5, SNS_ATTR_RATES = 6, SNS_ATTR_RESOLUTIONS = 7, SNS_ATTR_FIFO_SIZE = 8,
	SNS_ATTR_ACTIVE_CURRENT = 9, SNS_ATTR_SLEEP_CURRENT = 10, SNS_ATTR_RANGES = 11, SNS_ATTR_OP_MODES = 12,
	SNS_ATTR_DRI = 13, SNS_ATTR_STREAM_SYNC = 14, SNS_ATTR_EVENT_SIZE = 15, SNS_ATTR_STREAM_TYPE = 16,
	SNS_ATTR_DYNAMIC = 17, SNS_ATTR_HW_ID = 18, SNS_ATTR_RIGID_BODY = 19, SNS_ATTR_PLACEMENT = 20,
	SNS_ATTR_PHYSICAL_SENSOR = 21, SNS_ATTR_PHYSICAL_SENSOR_TESTS = 22, SNS_ATTR_SELECTED_RESOLUTION = 23,
	SNS_ATTR_SELECTED_RANGE = 24,
};
/* sns_std_sensor_stream_type */
enum { SNS_STREAM_STREAMING = 0, SNS_STREAM_ON_CHANGE = 1, SNS_STREAM_SINGLE_OUTPUT = 2 };

struct sns_suid {
	uint64_t low, high;
};
extern const struct sns_suid sns_suid_lookup;	/* abababab... */

/* Protobuf payloads (return the length, 0 on overflow). */
size_t sns_enc_suid_req(uint8_t *out, size_t cap, const char *data_type, int register_updates, int default_only);
size_t sns_enc_sensor_config(uint8_t *out, size_t cap, float sample_rate);
size_t sns_enc_attr_req(uint8_t *out, size_t cap, int register_updates);
/* sns_client_request_msg wrapping payload (may be empty). */
size_t sns_enc_client_request(uint8_t *out, size_t cap, const struct sns_suid *suid, uint32_t msg_id,
    const uint8_t *payload, size_t plen, int delivery);
/* The whole QMI SNS_CLIENT_REQ message around a sns_client_request_msg. */
size_t sns_qmi_request(uint8_t *out, size_t cap, uint16_t txn, const uint8_t *req, size_t rlen, int jumbo);

/* A received QMI message on the SNS service. */
struct sns_qmi {
	uint8_t type;		/* 2 response, 4 indication */
	uint16_t txn, msg_id;
	uint16_t result, error;	/* response */
	int has_client_id;
	uint64_t client_id;
	const uint8_t *payload;	/* indication: the sns_client_event_msg */
	size_t plen;
};
int sns_qmi_parse(const uint8_t *d, size_t n, struct sns_qmi *q);

/* Walk a sns_client_event_msg: cb once per event; returns events seen or -1 if malformed. */
typedef void (*sns_event_cb)(void *ctx, const struct sns_suid *suid, uint32_t msg_id, uint64_t ts,
    const uint8_t *payload, size_t plen);
int sns_parse_event_msg(const uint8_t *p, size_t n, sns_event_cb cb, void *ctx);

/* sns_suid_event: fills data_type and up to max SUIDs; returns the SUID count or -1. */
int sns_parse_suid_event(const uint8_t *p, size_t n, char *data_type, size_t dtlen, struct sns_suid *suids, int max);

/* sns_std_attr_event, reduced to what sensd prints/uses. */
struct sns_attrs {
	char name[64], vendor[64], type[32];
	int available, stream_type, physical, hw_id, have_stream_type;
	int64_t version;
	float rates[16];
	int nrates;
	float range_min, range_max, resolution;
	int nattrs;
};
int sns_parse_attr_event(const uint8_t *p, size_t n, struct sns_attrs *a);

/* sns_std_sensor_event: returns the number of floats (≤ max) or -1; *status = accuracy 0-3. */
int sns_parse_sensor_event(const uint8_t *p, size_t n, float *data, int max, int *status);
/* sns_proximity_event: returns 0 if p looks like one (field 1 a varint), -1 otherwise. */
int sns_parse_prox_event(const uint8_t *p, size_t n, int *near, uint32_t *raw, int *status);
/* sns_std_error_event */
int sns_parse_error_event(const uint8_t *p, size_t n);

#endif
