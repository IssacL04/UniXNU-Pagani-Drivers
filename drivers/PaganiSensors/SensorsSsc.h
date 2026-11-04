/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The Qualcomm sensor client service of the ADSP's sensor protection domain (SSC/QSH "sns_client", QMI service 400 over the IPC
 * router): the QMI frames and the protobuf messages, our own encoder and decoder (SensorsPb.h). Message ids, TLVs and field numbers are
 * facts from Qualcomm's own BSD-3 sources (qualcomm/sensinghub: sns_client_api_v01, apis/proto), checked against the public protocol
 * descriptions; no code was taken (build-userspace/a830-inputs/sensors/ssc-facts.md, sections 2 to 4 and appendix A).
 *
 * QMI (after the 7-byte header: type, transaction, message id, TLV length):
 *   0x20 request   TLV 0x01 u16 n + n bytes of sns_client_request_msg (n <= 1000), TLV 0x10 u8 use_jumbo_report (optional)
 *   0x20 response  TLV 0x02 u16 result + u16 error (0/0: received), TLV 0x10 u64 client id, TLV 0x11 u32 result (0: received)
 *   0x21 report    TLV 0x01 u64 client id, TLV 0x02 u16 n + n bytes of sns_client_event_msg (n <= 1000)
 *   0x22 jumbo     the same, n <= 10240 (only when use_jumbo_report was set; we never set it: PaganiAdsp's 2 KiB intents)
 * Every request is answered at once by a response that only says it was received; the data come in reports. */
#ifndef SENSORS_SSC_H
#define SENSORS_SSC_H
#include <stdint.h>
#include "SensorsPb.h"
#ifdef __cplusplus
extern "C" {
#endif

#define SSC_QMI_SERVICE 0x190u
#define SSC_QMI_HEADER 7u
#define SSC_QMI_MAX_PAYLOAD 1000u       /* protobuf bytes of a request or a report */
#define SSC_QMI_MAX_JUMBO 10240u
enum {SSC_QMI_REQUEST = 0, SSC_QMI_RESPONSE = 2, SSC_QMI_INDICATION = 4};
enum {SSC_QMI_SUPPORTED = 0x1e, SSC_QMI_REQ = 0x20, SSC_QMI_REPORT = 0x21, SSC_QMI_JUMBO_REPORT = 0x22, SSC_QMI_VERSION = 0x24};

/* Message ids: the framework's (0..255), the SUID sensor's, the standard sensor's and the per data type ones (only unique within a data
 * type: 768 is the SUID event of the SUID sensor and the physical configuration event of a physical sensor; 769 the discovery-done event
 * of the SUID sensor and the proximity event of a proximity sensor). */
enum {
    SSC_MSG_ATTR_REQ = 1, SSC_MSG_FLUSH_REQ = 2, SSC_MSG_DEBUG_REQ = 3, SSC_MSG_DISABLE_REQ = 10,
    SSC_MSG_ATTR_EVENT = 128, SSC_MSG_FLUSH_EVENT = 129, SSC_MSG_ERROR_EVENT = 130, SSC_MSG_DEBUG_RESP = 131,
    SSC_MSG_RESAMPLER_CONFIG_EVENT = 133,
    SSC_MSG_SUID_REQ = 512, SSC_MSG_SUID_EVENT = 768, SSC_MSG_SUID_DONE_EVENT = 769,
    SSC_MSG_SENSOR_CONFIG = 513, SSC_MSG_ON_CHANGE_CONFIG = 514,
    SSC_MSG_PHYSICAL_CONFIG_EVENT = 768, SSC_MSG_SENSOR_EVENT = 1025,
    SSC_MSG_PROXIMITY_EVENT = 769, SSC_MSG_PROXIMITY_RECURRENT = 1031,
    SSC_MSG_CAL_EVENT = 1022,
};
/* Attribute ids (sns_std_sensor.proto). */
enum {
    SSC_ATTR_NAME = 0, SSC_ATTR_VENDOR = 1, SSC_ATTR_TYPE = 2, SSC_ATTR_AVAILABLE = 3, SSC_ATTR_VERSION = 4, SSC_ATTR_API = 5,
    SSC_ATTR_RATES = 6, SSC_ATTR_RESOLUTIONS = 7, SSC_ATTR_FIFO_SIZE = 8, SSC_ATTR_ACTIVE_CURRENT = 9, SSC_ATTR_SLEEP_CURRENT = 10,
    SSC_ATTR_RANGES = 11, SSC_ATTR_OP_MODES = 12, SSC_ATTR_DRI = 13, SSC_ATTR_STREAM_SYNC = 14, SSC_ATTR_EVENT_SIZE = 15,
    SSC_ATTR_STREAM_TYPE = 16, SSC_ATTR_DYNAMIC = 17, SSC_ATTR_HW_ID = 18, SSC_ATTR_RIGID_BODY = 19, SSC_ATTR_PHYSICAL_SENSOR = 21,
};
enum {SSC_STREAM_STREAMING = 0, SSC_STREAM_ON_CHANGE = 1, SSC_STREAM_SINGLE = 2};
enum {SSC_DELIVERY_WAKEUP = 0, SSC_DELIVERY_NO_WAKEUP = 1};
enum {SSC_PROC_APSS = 1};
enum {SSC_PROXIMITY_FAR = 0, SSC_PROXIMITY_NEAR = 1};

typedef struct {
    uint64_t low, high;
} SscSuid;
extern const SscSuid ssc_suid_lookup;   /* the SUID sensor itself: 0xabababababababab twice */
int ssc_suid_equal(const SscSuid *a, const SscSuid *b);

/* ---- requests (each returns the encoded length, 0 when it did not fit) ---- */
/* sns_suid_req {data_type, register_updates, default_only} */
uint32_t ssc_build_suid_req(uint8_t *out, uint32_t cap, const char *data_type, int register_updates, int default_only);
/* sns_std_sensor_config {sample_rate} (float bits, Hz) */
uint32_t ssc_build_sensor_config(uint8_t *out, uint32_t cap, uint32_t rate_bits);
/* sns_client_request_msg {suid, msg_id, susp_config {APSS, delivery}, request {[batching {batch_period}], [payload]}}; batch_period_us
 * < 0 leaves batching out (no batching). */
uint32_t ssc_build_request(uint8_t *out, uint32_t cap, const SscSuid *suid, uint32_t msg_id, uint32_t delivery, const uint8_t *payload,
                           uint32_t payload_length, int64_t batch_period_us);
/* The QMI request 0x20 around a request message; jumbo < 0 leaves TLV 0x10 out. */
uint32_t ssc_build_qmi_request(uint8_t *out, uint32_t cap, uint16_t transaction, const uint8_t *message, uint32_t length, int jumbo);

/* ---- QMI ---- */
typedef struct {
    uint8_t type;
    uint16_t transaction, message;
    const uint8_t *tlvs;
    uint32_t length;
} SscQmi;
int ssc_qmi_parse(const uint8_t *data, uint32_t length, SscQmi *m);            /* 0, or -1 when not a QMI message */
const uint8_t *ssc_qmi_tlv(const SscQmi *m, uint8_t type, uint32_t *size);    /* null when absent or malformed */
typedef struct {
    uint16_t result, error;            /* TLV 0x02 */
    int has_client;
    uint64_t client_id;                /* TLV 0x10 */
    int has_status;
    uint32_t status;                   /* TLV 0x11 */
} SscResponse;
int ssc_qmi_response(const SscQmi *m, SscResponse *r);                         /* 0, or -1 without TLV 0x02 */
int ssc_response_ok(const SscResponse *r);
/* A report (0x21 or 0x22): the client id (TLV 0x01, optional) and the event message (TLV 0x02). 0, or -1. */
int ssc_qmi_report(const SscQmi *m, int *has_client, uint64_t *client_id, const uint8_t **message, uint32_t *length);

/* ---- sns_client_event_msg {suid, events: {msg_id, timestamp, payload}...} ---- */
typedef struct {
    uint32_t msg_id;
    uint64_t timestamp;                /* QTimer ticks (19.2 MHz) */
    const uint8_t *payload;
    uint32_t length;
} SscEvent;
typedef struct {
    SscSuid suid;
    int has_suid;
    SpbIn in;
} SscEvents;
int ssc_events_open(SscEvents *e, const uint8_t *message, uint32_t length);   /* 0 (the SUID read), -1 malformed */
int ssc_events_next(SscEvents *e, SscEvent *ev);                              /* 1 one event, 0 the end, -1 malformed */

/* ---- event payloads ---- */
#define SSC_MAX_DATA 16u
typedef struct {
    uint32_t data[SSC_MAX_DATA];       /* float bits */
    unsigned count, more;
    int64_t status;                    /* 0 unreliable, 1 low, 2 medium, 3 high accuracy */
} SscSensorEvent;
int ssc_parse_sensor_event(const uint8_t *p, uint32_t n, SscSensorEvent *ev);

typedef struct {
    int64_t type;                      /* SSC_PROXIMITY_FAR / _NEAR */
    uint64_t raw_adc;
    int64_t status;
} SscProximityEvent;
int ssc_parse_proximity_event(const uint8_t *p, uint32_t n, SscProximityEvent *ev);

#define SSC_MAX_SUIDS 8u
#define SSC_TYPE_MAX 32u
typedef struct {
    char data_type[SSC_TYPE_MAX];
    SscSuid suid[SSC_MAX_SUIDS];
    unsigned count, more;
} SscSuidEvent;
int ssc_parse_suid_event(const uint8_t *p, uint32_t n, SscSuidEvent *ev);

#define SSC_MAX_RATES 16u
#define SSC_MAX_RANGES 4u
typedef struct {
    unsigned seen;                     /* attributes read */
    char name[32], vendor[32], type[SSC_TYPE_MAX], api[48];
    int available, physical, dri;      /* -1 when not given */
    int64_t version, stream_type, fifo_size, hw_id;   /* -1 when not given */
    uint32_t rates[SSC_MAX_RATES];     /* Hz, float bits */
    unsigned rate_count;
    uint32_t ranges[SSC_MAX_RANGES][2];
    unsigned range_count;
    uint32_t resolutions[SSC_MAX_RANGES];
    unsigned resolution_count;
    int64_t rigid_body[SSC_MAX_RANGES];
    unsigned rigid_count;
    int64_t active_current[SSC_MAX_RANGES];
    unsigned current_count;
} SscAttrs;
void ssc_attrs_clear(SscAttrs *a);
int ssc_parse_attr_event(const uint8_t *p, uint32_t n, SscAttrs *a);

typedef struct {
    uint32_t sample_rate;              /* float bits */
    uint32_t water_mark, active_current;
    uint32_t range[2];
    unsigned range_count;
    uint32_t resolution;
    char op_mode[16];
    int64_t dri;                       /* -1 when not given */
} SscPhysicalConfig;
int ssc_parse_physical_config(const uint8_t *p, uint32_t n, SscPhysicalConfig *c);

int ssc_parse_error_event(const uint8_t *p, uint32_t n, int64_t *error);

#ifdef __cplusplus
}
#endif
#endif
