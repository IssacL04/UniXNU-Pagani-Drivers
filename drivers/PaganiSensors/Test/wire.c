// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Host tests of the wire formats: the protobuf encoder/decoder (SensorsPb.c) and the sensor client service's messages and QMI frames
// (SensorsSsc.c), against the 16 hand-built vectors of build-userspace/a830-inputs/sensors/ssc-facts.md appendix A.4 (bytes typed here
// from the facts, independently of the encoder), then round trips, the decoder's tolerance (packed and unpacked repeated fields, any
// field order, unknown fields, negative enums) and its refusals (truncation, groups, overlong varints), and a mutation fuzz under
// ASan/UBSan. Vector 4 (the QRTR data packet around the request) goes through PaganiAdsp's frame builder.
#include "check.h"
#include "../SensorsPb.h"
#include "../SensorsSsc.h"
#include "../../PaganiAdsp/QcomQrtr.h"

#define SUID_EX "0a 12 09 ef cd ab 89 67 45 23 01 11 10 32 54 76 98 ba dc fe"
static const SscSuid example = {0x0123456789abcdefull, 0xfedcba9876543210ull};

static const char *const V1 = "0a 05 61 63 63 65 6c 10 01 18 01";
static const char *const V2 = "0a 12 09 ab ab ab ab ab ab ab ab 11 ab ab ab ab ab ab ab ab 15 00 02 00 00 1a 04 08 01 10 00 22 0d 12 0b"
                              " 0a 05 61 63 63 65 6c 10 01 18 01";
static const char *const V3_HEAD = "00 01 00 20 00 37 00 01 30 00 2e 00";
static const char *const V3_TAIL = "10 01 00 00";
static const char *const V4_HEAD = "01 00 00 00 01 00 00 00 01 00 00 00 01 40 00 00 00 00 00 00 3e 00 00 00 05 00 00 00 1a 00 00 00";
static const char *const V5 = SUID_EX " 15 01 00 00 00 1a 04 08 01 10 00 22 00";
static const char *const V6_CONFIG = "0d 00 00 48 42";
static const char *const V6 = SUID_EX " 15 01 02 00 00 1a 04 08 01 10 00 22 07 12 05 0d 00 00 48 42";
static const char *const V6_BATCH = SUID_EX " 15 01 02 00 00 1a 04 08 01 10 00 22 0b 0a 02 08 00 12 05 0d 00 00 48 42";
static const char *const V7 = SUID_EX " 15 02 02 00 00 1a 04 08 01 10 00 22 00";
static const char *const V8 = SUID_EX " 15 0a 00 00 00 1a 04 08 01 10 00 22 00";
static const char *const V9 = "0d 00 00 00 3e 0d 00 00 1c 41 0d 00 00 00 bf 10 03";
static const char *const V10 = SUID_EX " 12 21 0d 01 04 00 00 11 90 78 56 34 12 00 00 00 1a 11 0d 00 00 00 3e 0d 00 00 1c 41 0d 00 00 00 bf"
                               " 10 03";
static const char *const V11_HEAD = "04 00 00 21 00 47 00 01 08 00 55 00 00 00 00 00 00 00 02 39 00 37 00";
static const char *const V12 = "02 01 00 20 00 19 00 02 04 00 00 00 00 00 10 08 00 55 00 00 00 00 00 00 00 11 04 00 00 00 00 00";
static const char *const V13 = "08 01 10 d2 09 18 03";
static const char *const V14 = "0a 05 61 63 63 65 6c 12 12 09 ef cd ab 89 67 45 23 01 11 10 32 54 76 98 ba dc fe";
static const char *const V15 = "0a 0f 08 00 12 0b 0a 09 12 07 6c 73 6d 36 64 73 76 0a 12 08 06 12 0e 0a 05 1d 00 00 c8 41 0a 05 1d 00 00"
                               " 48 42 0a 0f 08 10 12 0b 0a 09 21 00 00 00 00 00 00 00 00 0a 16 08 0b 12 12 0a 10 0a 0e 0a 05 1d 0a e8 9c"
                               " c2 0a 05 1d 0a e8 9c 42";
static const char *const V16 = "08 ff ff ff ff ff ff ff ff ff 01";

static void vectors(void)
{
    uint8_t want[256], got[256], inner[64];
    uint32_t w, n;

    /* 1, 2: the SUID request and its client request message */
    w = hex(V1, want, sizeof want);
    n = ssc_build_suid_req(inner, sizeof inner, "accel", 1, 1);
    CHECK(same_bytes(inner, n, want, w) && n == 11);
    w = hex(V2, want, sizeof want);
    n = ssc_build_request(got, sizeof got, &ssc_suid_lookup, SSC_MSG_SUID_REQ, SSC_DELIVERY_WAKEUP, inner, 11, -1);
    CHECK(same_bytes(got, n, want, w) && n == 46);
    /* 3: in the QMI request (transaction 1, use_jumbo_report 0) */
    uint8_t qmi[256], v3[256];
    uint32_t v3n = hex(V3_HEAD, v3, sizeof v3);
    memcpy(v3 + v3n, got, n);
    v3n += n;
    v3n += hex(V3_TAIL, v3 + v3n, sizeof v3 - v3n);
    const uint32_t q = ssc_build_qmi_request(qmi, sizeof qmi, 1, got, n, 0);
    CHECK(same_bytes(qmi, q, v3, v3n) && q == 62);
    /* 4: the QRTR data packet from our node 1, port 0x4001, to the ADSP's 5:0x1a, padded to 96 bytes */
    uint8_t v4[256], packet[256];
    uint32_t v4n = hex(V4_HEAD, v4, sizeof v4);
    memcpy(v4 + v4n, v3, v3n);
    v4n += v3n;
    v4[v4n++] = 0;
    v4[v4n++] = 0;
    const uint32_t pn = qcom_qrtr_frame(packet, sizeof packet, QCOM_QRTR_DATA, 1, 0x4001, 5, 0x1a, qmi, q);
    CHECK(same_bytes(packet, pn, v4, v4n) && pn == 96);
    /* 5: attribute request, empty request field */
    w = hex(V5, want, sizeof want);
    n = ssc_build_request(got, sizeof got, &example, SSC_MSG_ATTR_REQ, SSC_DELIVERY_WAKEUP, 0, 0, -1);
    CHECK(same_bytes(got, n, want, w) && n == 33);
    /* 6: sensor config 50 Hz, without and with an explicit batch_period 0 */
    w = hex(V6_CONFIG, want, sizeof want);
    n = ssc_build_sensor_config(inner, sizeof inner, float_bits(50.0f));
    CHECK(same_bytes(inner, n, want, w));
    w = hex(V6, want, sizeof want);
    n = ssc_build_request(got, sizeof got, &example, SSC_MSG_SENSOR_CONFIG, SSC_DELIVERY_WAKEUP, inner, 5, -1);
    CHECK(same_bytes(got, n, want, w) && n == 40);
    w = hex(V6_BATCH, want, sizeof want);
    n = ssc_build_request(got, sizeof got, &example, SSC_MSG_SENSOR_CONFIG, SSC_DELIVERY_WAKEUP, inner, 5, 0);
    CHECK(same_bytes(got, n, want, w) && n == 44);
    /* 7, 8: on-change config, disable */
    w = hex(V7, want, sizeof want);
    n = ssc_build_request(got, sizeof got, &example, SSC_MSG_ON_CHANGE_CONFIG, SSC_DELIVERY_WAKEUP, 0, 0, -1);
    CHECK(same_bytes(got, n, want, w));
    w = hex(V8, want, sizeof want);
    n = ssc_build_request(got, sizeof got, &example, SSC_MSG_DISABLE_REQ, SSC_DELIVERY_WAKEUP, 0, 0, -1);
    CHECK(same_bytes(got, n, want, w));

    /* 9: a sensor event (unpacked floats), decoded and re-encoded */
    w = hex(V9, want, sizeof want);
    SscSensorEvent se;
    CHECK(ssc_parse_sensor_event(want, w, &se) == 0 && se.count == 3 && se.more == 0 && se.status == 3);
    CHECK(se.data[0] == float_bits(0.125f) && se.data[1] == float_bits(9.75f) && se.data[2] == float_bits(-0.5f));
    SpbOut o;
    spb_out(&o, got, sizeof got);
    for (unsigned i = 0; i < 3; ++i) spb_put_fixed32(&o, 1, se.data[i]);
    spb_put_varint(&o, 2, 3);
    CHECK(same_bytes(got, spb_done(&o), want, w));

    /* 10, 11: the event message in a 0x21 report from client 0x55 */
    uint8_t v11[256];
    uint32_t v11n = hex(V11_HEAD, v11, sizeof v11);
    w = hex(V10, want, sizeof want);
    CHECK(w == 55);
    memcpy(v11 + v11n, want, w);
    v11n += w;
    CHECK(v11n == 78);
    SscQmi m;
    int has_client = 0;
    uint64_t client = 0;
    const uint8_t *msg = 0;
    uint32_t len = 0;
    CHECK(ssc_qmi_parse(v11, v11n, &m) == 0 && m.type == SSC_QMI_INDICATION && m.message == SSC_QMI_REPORT && m.transaction == 0);
    CHECK(ssc_qmi_report(&m, &has_client, &client, &msg, &len) == 0 && has_client && client == 0x55 && len == 55);
    SscEvents events;
    SscEvent ev;
    CHECK(ssc_events_open(&events, msg, len) == 0 && events.has_suid && ssc_suid_equal(&events.suid, &example));
    CHECK(ssc_events_next(&events, &ev) == 1 && ev.msg_id == SSC_MSG_SENSOR_EVENT && ev.timestamp == 0x1234567890ull && ev.length == 17);
    CHECK(ssc_parse_sensor_event(ev.payload, ev.length, &se) == 0 && se.count == 3 && se.data[1] == float_bits(9.75f));
    CHECK(ssc_events_next(&events, &ev) == 0);
    /* rebuilt by the encoder: the same bytes */
    spb_out(&o, got, sizeof got);
    uint32_t mark = spb_open(&o, 1);
    spb_put_fixed64(&o, 1, example.low);
    spb_put_fixed64(&o, 2, example.high);
    spb_close(&o, mark);
    mark = spb_open(&o, 2);
    spb_put_fixed32(&o, 1, 1025);
    spb_put_fixed64(&o, 2, 0x1234567890ull);
    hex(V9, inner, sizeof inner);
    spb_put_bytes(&o, 3, inner, 17);
    spb_close(&o, mark);
    CHECK(same_bytes(got, spb_done(&o), want, w));

    /* 12: the success response with client id 0x55 */
    w = hex(V12, want, sizeof want);
    SscResponse r;
    CHECK(ssc_qmi_parse(want, w, &m) == 0 && m.type == SSC_QMI_RESPONSE && m.transaction == 1 && m.message == SSC_QMI_REQ);
    CHECK(ssc_qmi_response(&m, &r) == 0 && r.result == 0 && r.error == 0 && r.has_client && r.client_id == 0x55 && r.has_status &&
          r.status == 0 && ssc_response_ok(&r));

    /* 13: proximity NEAR, raw 1234, high accuracy */
    w = hex(V13, want, sizeof want);
    SscProximityEvent pe;
    CHECK(ssc_parse_proximity_event(want, w, &pe) == 0 && pe.type == SSC_PROXIMITY_NEAR && pe.raw_adc == 1234 && pe.status == 3);
    spb_out(&o, got, sizeof got);
    spb_put_varint(&o, 1, 1);
    spb_put_varint(&o, 2, 1234);
    spb_put_varint(&o, 3, 3);
    CHECK(same_bytes(got, spb_done(&o), want, w));

    /* 14: a SUID event */
    w = hex(V14, want, sizeof want);
    SscSuidEvent sue;
    CHECK(ssc_parse_suid_event(want, w, &sue) == 0 && !strcmp(sue.data_type, "accel") && sue.count == 1 &&
          ssc_suid_equal(&sue.suid[0], &example));

    /* 15: attributes: NAME, RATES, STREAM_TYPE, RANGES */
    w = hex(V15, want, sizeof want);
    CHECK(w == 78);
    SscAttrs a;
    ssc_attrs_clear(&a);
    CHECK(ssc_parse_attr_event(want, w, &a) == 0 && a.seen == 4 && !strcmp(a.name, "lsm6dsv"));
    CHECK(a.rate_count == 2 && a.rates[0] == float_bits(25.0f) && a.rates[1] == float_bits(50.0f));
    CHECK(a.stream_type == 0 && a.range_count == 1 && bits_float(a.ranges[0][0]) == -78.4532f && bits_float(a.ranges[0][1]) == 78.4532f);
    CHECK(a.available == -1 && a.physical == -1 && a.vendor[0] == 0);

    /* 16: a negative enum is a ten-byte varint */
    w = hex(V16, want, sizeof want);
    spb_out(&o, got, sizeof got);
    spb_put_int(&o, 1, -1);
    CHECK(same_bytes(got, spb_done(&o), want, w));
    SpbIn in;
    uint32_t field, wire;
    uint64_t value;
    spb_in(&in, want, w);
    CHECK(spb_next(&in, &field, &wire) == 1 && field == 1 && wire == SPB_VARINT && spb_varint_value(&in, wire, &value) == 0);
    CHECK((int64_t)value == -1 && spb_next(&in, &field, &wire) == 0);
}

static void tolerance(void)
{
    uint8_t buf[256];
    SpbOut o;
    SscSensorEvent se;
    /* packed floats, status first, an unknown varint, fixed64 and bytes field in between, status twice (the last wins) */
    spb_out(&o, buf, sizeof buf);
    spb_put_varint(&o, 2, 1);
    spb_put_varint(&o, 9, 77);
    const uint32_t floats[4] = {float_bits(1.5f), float_bits(-2.25f), float_bits(1e-3f), float_bits(3.0e5f)};
    uint8_t packed[16];
    for (unsigned i = 0; i < 4; ++i) for (unsigned b = 0; b < 4; ++b) packed[4 * i + b] = (uint8_t)(floats[i] >> (8 * b));
    spb_put_bytes(&o, 1, packed, 16);
    spb_put_fixed64(&o, 12, 0x1122334455667788ull);
    spb_put_bytes(&o, 13, "xyz", 3);
    spb_put_fixed32(&o, 1, float_bits(7.0f));       /* one more, unpacked after the packed run */
    spb_put_varint(&o, 2, 2);
    CHECK(ssc_parse_sensor_event(buf, spb_done(&o), &se) == 0 && se.count == 5 && se.status == 2);
    CHECK(!memcmp(se.data, floats, sizeof floats) && se.data[4] == float_bits(7.0f));
    /* more data than kept: counted */
    spb_out(&o, buf, sizeof buf);
    for (unsigned i = 0; i < 20; ++i) spb_put_fixed32(&o, 1, float_bits((float)i));
    CHECK(ssc_parse_sensor_event(buf, spb_done(&o), &se) == 0 && se.count == 16 && se.more == 4 && se.status == -1);
    /* a suid event with ten SUIDs: eight kept */
    spb_out(&o, buf, sizeof buf);
    spb_put_string(&o, 1, "gyro");
    for (unsigned i = 0; i < 10; ++i) {
        const uint32_t mark = spb_open(&o, 2);
        spb_put_fixed64(&o, 1, i);
        spb_put_fixed64(&o, 2, ~(uint64_t)i);
        spb_close(&o, mark);
    }
    SscSuidEvent sue;
    CHECK(ssc_parse_suid_event(buf, spb_done(&o), &sue) == 0 && sue.count == 8 && sue.more == 2 && sue.suid[7].low == 7 &&
          sue.suid[7].high == ~(uint64_t)7);
    /* an event message whose SUID comes after its events */
    spb_out(&o, buf, sizeof buf);
    for (unsigned i = 0; i < 3; ++i) {
        const uint32_t mark = spb_open(&o, 2);
        spb_put_fixed32(&o, 1, 769 + i);
        spb_put_fixed64(&o, 2, 1000 + i);
        spb_close(&o, mark);
    }
    uint32_t mark = spb_open(&o, 1);
    spb_put_fixed64(&o, 1, 5);
    spb_put_fixed64(&o, 2, 6);
    spb_close(&o, mark);
    SscEvents events;
    SscEvent ev;
    CHECK(ssc_events_open(&events, buf, spb_done(&o)) == 0 && events.has_suid && events.suid.low == 5 && events.suid.high == 6);
    for (unsigned i = 0; i < 3; ++i) CHECK(ssc_events_next(&events, &ev) == 1 && ev.msg_id == 769 + i && ev.timestamp == 1000 + i && !ev.length);
    CHECK(ssc_events_next(&events, &ev) == 0);
    /* attributes: AVAILABLE, PHYSICAL, VENDOR, API, VERSION, RIGID_BODY (packed sint is not a thing: one datum each), an unknown id */
    spb_out(&o, buf, sizeof buf);
    struct {uint32_t id; int kind; int64_t v; const char *s;} attrs[] = {
        {SSC_ATTR_AVAILABLE, 5, 1, 0}, {SSC_ATTR_PHYSICAL_SENSOR, 5, 0, 0}, {SSC_ATTR_VENDOR, 2, 0, "oplus"}, {SSC_ATTR_API, 2, 0, "sns_accel.proto"},
        {SSC_ATTR_VERSION, 4, 0x10203, 0}, {SSC_ATTR_RIGID_BODY, 4, 0, 0}, {99, 4, 5, 0}, {SSC_ATTR_HW_ID, 4, -2, 0},
    };
    for (unsigned i = 0; i < sizeof attrs / sizeof attrs[0]; ++i) {
        const uint32_t attr = spb_open(&o, 1);
        spb_put_int(&o, 1, attrs[i].id);
        const uint32_t value = spb_open(&o, 2);
        const uint32_t datum = spb_open(&o, 1);
        if (attrs[i].kind == 5) spb_put_varint(&o, 5, (uint64_t)attrs[i].v);
        else if (attrs[i].kind == 2) spb_put_string(&o, 2, attrs[i].s);
        else spb_put_fixed64(&o, 4, (uint64_t)attrs[i].v);
        spb_close(&o, datum);
        spb_close(&o, value);
        spb_close(&o, attr);
    }
    SscAttrs a;
    ssc_attrs_clear(&a);
    CHECK(ssc_parse_attr_event(buf, spb_done(&o), &a) == 0 && a.seen == 8 && a.available == 1 && a.physical == 0);
    CHECK(!strcmp(a.vendor, "oplus") && !strcmp(a.api, "sns_accel.proto") && a.version == 0x10203 && a.rigid_count == 1 &&
          a.rigid_body[0] == 0 && a.hw_id == -2);
    /* a physical config event and an error event */
    spb_out(&o, buf, sizeof buf);
    spb_put_fixed32(&o, 1, float_bits(104.0f));
    spb_put_varint(&o, 2, 1);
    spb_put_fixed32(&o, 3, float_bits(-156.9f));
    spb_put_fixed32(&o, 3, float_bits(156.9f));
    spb_put_fixed32(&o, 4, float_bits(0.0048f));
    spb_put_string(&o, 5, "HIGH_PERF");
    spb_put_varint(&o, 6, 240);
    spb_put_varint(&o, 7, 1);
    spb_put_varint(&o, 8, 1);
    SscPhysicalConfig pc;
    CHECK(ssc_parse_physical_config(buf, spb_done(&o), &pc) == 0 && pc.sample_rate == float_bits(104.0f) && pc.water_mark == 1);
    CHECK(pc.range_count == 2 && pc.range[1] == float_bits(156.9f) && !strcmp(pc.op_mode, "HIGH_PERF") && pc.active_current == 240 && pc.dri == 1);
    spb_out(&o, buf, sizeof buf);
    spb_put_varint(&o, 1, 5);
    int64_t error = 0;
    CHECK(ssc_parse_error_event(buf, spb_done(&o), &error) == 0 && error == 5);
}

static void refusals(void)
{
    uint8_t buf[64];
    SscSensorEvent se;
    SpbIn in;
    uint32_t field, wire;
    /* truncated fixed32, length past the end, a group, field 0, an eleven-byte varint, a varint above 64 bits */
    static const char *const bad[] = {"0d 00 00 48", "0a 05 01 02", "0b 0c", "00 01", "08 ff ff ff ff ff ff ff ff ff ff 01",
                                      "08 ff ff ff ff ff ff ff ff ff 7f", "0a 04 00 00 00 41 05"};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        const uint32_t n = hex(bad[i], buf, sizeof buf);
        CHECK(ssc_parse_sensor_event(buf, n, &se) == -1);
    }
    /* the wrong wire type for a value */
    hex("08 01", buf, sizeof buf);
    spb_in(&in, buf, 2);
    uint32_t f32;
    CHECK(spb_next(&in, &field, &wire) == 1 && spb_fixed32_value(&in, wire, &f32) == -1 && in.error && spb_next(&in, &field, &wire) == -1);
    /* the encoder never writes past its buffer */
    SpbOut o;
    spb_out(&o, buf, 10);
    spb_put_string(&o, 1, "a very long data type");
    CHECK(spb_done(&o) == 0 && o.overflow && o.len <= 10);
    CHECK(ssc_build_suid_req(buf, 5, "accel", 1, 1) == 0);
    CHECK(ssc_build_request(buf, 32, &ssc_suid_lookup, 1, 0, 0, 0, -1) == 0);
    CHECK(ssc_build_qmi_request(buf, sizeof buf, 1, buf, 0, 0) == 0);
    uint8_t big[1100];
    CHECK(ssc_build_qmi_request(big, sizeof big, 1, big, 1001, 0) == 0);   /* more than the service takes */
    /* a nested message longer than 127 bytes: the length grows to two bytes and the content moves */
    uint8_t payload[300], out[400];
    for (unsigned i = 0; i < sizeof payload; ++i) payload[i] = (uint8_t)(i * 13);
    const uint32_t n = ssc_build_request(out, sizeof out, &ssc_suid_lookup, 512, 1, payload, 200, -1);
    CHECK(n > 200);
    spb_in(&in, out, n);
    int got_payload = 0;
    while (spb_next(&in, &field, &wire) == 1) {
        const uint8_t *p;
        uint32_t len;
        if (field != 4) {CHECK(spb_skip(&in, wire) == 0); continue;}
        CHECK(spb_bytes_value(&in, wire, &p, &len) == 0 && len == 203);   /* 12 c8 01 + 200 */
        SpbIn req;
        spb_in(&req, p, len);
        CHECK(spb_next(&req, &field, &wire) == 1 && field == 2 && spb_bytes_value(&req, wire, &p, &len) == 0 && len == 200 &&
              !memcmp(p, payload, 200));
        got_payload = 1;
    }
    CHECK(got_payload && !in.error);
    /* QMI: no result TLV; a report whose count runs past its TLV; a normal report above 1000 bytes; a jumbo one may be larger */
    SscQmi m;
    SscResponse r;
    hex("02 01 00 20 00 07 00 10 04 00 01 00 00 00", buf, sizeof buf);
    CHECK(ssc_qmi_parse(buf, 14, &m) == 0 && ssc_qmi_response(&m, &r) == -1);
    hex("02 01 00 20 00 07 00 02 04 00 01 00 31 00", buf, sizeof buf);
    CHECK(ssc_qmi_parse(buf, 14, &m) == 0 && ssc_qmi_response(&m, &r) == 0 && r.result == 1 && r.error == 0x31 && !ssc_response_ok(&r));
    hex("02 01 00 20 00 0e 00 02 04 00 00 00 00 00 11 04 00 05 00 00 00", buf, sizeof buf);
    CHECK(ssc_qmi_parse(buf, 21, &m) == 0 && ssc_qmi_response(&m, &r) == 0 && r.has_status && r.status == 5 && !ssc_response_ok(&r));
    hex("04 00 00 21 00 07 00 02 04 00 05 00 01 02", buf, sizeof buf);
    int has_client;
    uint64_t client;
    const uint8_t *msg;
    uint32_t len;
    CHECK(ssc_qmi_parse(buf, 14, &m) == 0 && ssc_qmi_report(&m, &has_client, &client, &msg, &len) == -1);
    static uint8_t huge[11000];
    huge[0] = 4; huge[3] = 0x21; huge[5] = (uint8_t)((3 + 2 + 1200) & 0xff); huge[6] = (uint8_t)((3 + 2 + 1200) >> 8);
    huge[7] = 2; huge[8] = (uint8_t)((2 + 1200) & 0xff); huge[9] = (uint8_t)((2 + 1200) >> 8); huge[10] = (uint8_t)(1200 & 0xff);
    huge[11] = (uint8_t)(1200 >> 8);
    CHECK(ssc_qmi_parse(huge, 7 + 5 + 1200, &m) == 0 && ssc_qmi_report(&m, &has_client, &client, &msg, &len) == -1);
    huge[3] = 0x22;
    CHECK(ssc_qmi_parse(huge, 7 + 5 + 1200, &m) == 0 && ssc_qmi_report(&m, &has_client, &client, &msg, &len) == 0 && len == 1200 &&
          !has_client);
    CHECK(ssc_qmi_parse(buf, 6, &m) == -1);
    hex("04 00 00 21 00 30 00", buf, sizeof buf);
    CHECK(ssc_qmi_parse(buf, 7, &m) == -1);       /* length past the message */
}

/* Round trips of the requests through a decoder written against the field numbers, and a mutation fuzz of every decoder. */
static uint64_t rng = 88172645463325252ull;
static uint64_t next(void) {rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng;}

static void round_trips(void)
{
    for (unsigned round = 0; round < 3000; ++round) {
        const SscSuid suid = {next(), next()};
        const uint32_t msg = (uint32_t)(next() % 2000), delivery = (uint32_t)(next() & 1);
        uint8_t payload[160], out[400];
        const uint32_t plen = (uint32_t)(next() % 160);
        for (uint32_t i = 0; i < plen; ++i) payload[i] = (uint8_t)next();
        const int64_t batch = next() % 3 == 0 ? -1 : (int64_t)(next() % 2000000);
        const uint32_t n = ssc_build_request(out, sizeof out, &suid, msg, delivery, payload, plen, batch);
        CHECK(n > 0);
        SpbIn in;
        uint32_t field, wire;
        int fields = 0;
        spb_in(&in, out, n);
        while (spb_next(&in, &field, &wire) == 1) {
            const uint8_t *p;
            uint32_t len;
            uint32_t v32;
            if (field == 1) {
                SscEvents fake;
                CHECK(spb_bytes_value(&in, wire, &p, &len) == 0);
                SpbIn s;
                uint64_t low = 0, high = 0;
                spb_in(&s, p, len);
                while (spb_next(&s, &field, &wire) == 1) {
                    if (field == 1) CHECK(spb_fixed64_value(&s, wire, &low) == 0);
                    else CHECK(spb_fixed64_value(&s, wire, &high) == 0);
                }
                CHECK(low == suid.low && high == suid.high);
                (void)fake;
                fields |= 1;
            } else if (field == 2) {
                CHECK(spb_fixed32_value(&in, wire, &v32) == 0 && v32 == msg);
                fields |= 2;
            } else if (field == 3) {
                CHECK(spb_bytes_value(&in, wire, &p, &len) == 0 && len == 4 && p[0] == 0x08 && p[1] == 1 && p[2] == 0x10 && p[3] == delivery);
                fields |= 4;
            } else if (field == 4) {
                CHECK(spb_bytes_value(&in, wire, &p, &len) == 0);
                SpbIn r;
                spb_in(&r, p, len);
                int saw_batch = 0, saw_payload = 0;
                while (spb_next(&r, &field, &wire) == 1) {
                    const uint8_t *q;
                    uint32_t qn;
                    CHECK(spb_bytes_value(&r, wire, &q, &qn) == 0);
                    if (field == 1) {
                        SpbIn b;
                        uint64_t period = 0;
                        spb_in(&b, q, qn);
                        CHECK(spb_next(&b, &field, &wire) == 1 && field == 1 && spb_varint_value(&b, wire, &period) == 0 &&
                              (int64_t)period == batch);
                        saw_batch = 1;
                    } else {
                        CHECK(field == 2 && qn == plen && !memcmp(q, payload, plen));
                        saw_payload = 1;
                    }
                }
                CHECK(saw_batch == (batch >= 0) && saw_payload == (plen > 0));
                fields |= 8;
            } else {
                CHECK(0);
            }
        }
        CHECK(fields == 15 && !in.error);
    }
}

static void fuzz(void)
{
    uint8_t base[3][128], buf[160];
    uint32_t bn[3];
    bn[0] = hex(V10, base[0], sizeof base[0]);
    bn[1] = hex(V15, base[1], sizeof base[1]);
    bn[2] = hex(V14, base[2], sizeof base[2]);
    for (unsigned round = 0; round < 200000; ++round) {
        const unsigned which = (unsigned)(next() % 3);
        uint32_t n = bn[which];
        memcpy(buf, base[which], n);
        const unsigned flips = 1 + (unsigned)(next() % 4);
        for (unsigned f = 0; f < flips; ++f) buf[next() % n] ^= (uint8_t)(1u << (next() % 8));
        if (next() % 4 == 0) n = (uint32_t)(next() % (n + 1));
        SscEvents events;
        SscEvent ev;
        SscSensorEvent se;
        SscAttrs a;
        SscSuidEvent sue;
        SscProximityEvent pe;
        SscPhysicalConfig pc;
        int64_t error;
        if (ssc_events_open(&events, buf, n) == 0) {
            int guard = 0;
            while (ssc_events_next(&events, &ev) == 1 && guard++ < 64) {
                (void)ssc_parse_sensor_event(ev.payload, ev.length, &se);
                (void)ssc_parse_proximity_event(ev.payload, ev.length, &pe);
                (void)ssc_parse_physical_config(ev.payload, ev.length, &pc);
                (void)ssc_parse_error_event(ev.payload, ev.length, &error);
            }
        }
        ssc_attrs_clear(&a);
        (void)ssc_parse_attr_event(buf, n, &a);
        (void)ssc_parse_suid_event(buf, n, &sue);
        CHECK(a.rate_count <= SSC_MAX_RATES && sue.count <= SSC_MAX_SUIDS && strlen(a.name) < sizeof a.name);
    }
}

int main(void)
{
    vectors();
    tolerance();
    refusals();
    round_trips();
    fuzz();
    printf("PASS wire formats: the 16 vectors of ssc-facts.md A.4, tolerance, refusals, 3000 round trips, 200000 mutations: %u checks\n",
           checks);
    return 0;
}
