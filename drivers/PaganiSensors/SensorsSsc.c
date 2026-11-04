/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The sensor client service's QMI frames and protobuf messages: see SensorsSsc.h. */
#include "SensorsSsc.h"

const SscSuid ssc_suid_lookup = {0xababababababababull, 0xababababababababull};

int ssc_suid_equal(const SscSuid *a, const SscSuid *b)
{
    return a->low == b->low && a->high == b->high;
}

static uint16_t le16(const uint8_t *p) {return (uint16_t)(p[0] | (uint16_t)p[1] << 8);}
static uint32_t le32(const uint8_t *p) {return (uint32_t)le16(p) | (uint32_t)le16(p + 2) << 16;}
static uint64_t le64(const uint8_t *p) {return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;}
static void put16(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}

/* ---- requests ---- */
uint32_t ssc_build_suid_req(uint8_t *out, uint32_t cap, const char *data_type, int register_updates, int default_only)
{
    SpbOut o;
    spb_out(&o, out, cap);
    spb_put_string(&o, 1, data_type ? data_type : "");   /* required: written even when empty (every data type) */
    spb_put_varint(&o, 2, register_updates ? 1 : 0);
    spb_put_varint(&o, 3, default_only ? 1 : 0);
    return spb_done(&o);
}

uint32_t ssc_build_sensor_config(uint8_t *out, uint32_t cap, uint32_t rate_bits)
{
    SpbOut o;
    spb_out(&o, out, cap);
    spb_put_fixed32(&o, 1, rate_bits);
    return spb_done(&o);
}

uint32_t ssc_build_request(uint8_t *out, uint32_t cap, const SscSuid *suid, uint32_t msg_id, uint32_t delivery, const uint8_t *payload,
                           uint32_t payload_length, int64_t batch_period_us)
{
    SpbOut o;
    if (!suid) return 0;
    spb_out(&o, out, cap);
    uint32_t mark = spb_open(&o, 1);                     /* suid */
    spb_put_fixed64(&o, 1, suid->low);
    spb_put_fixed64(&o, 2, suid->high);
    spb_close(&o, mark);
    spb_put_fixed32(&o, 2, msg_id);                       /* msg_id */
    mark = spb_open(&o, 3);                               /* susp_config: both fields required */
    spb_put_varint(&o, 1, SSC_PROC_APSS);
    spb_put_varint(&o, 2, delivery);
    spb_close(&o, mark);
    mark = spb_open(&o, 4);                               /* request: required, even empty */
    if (batch_period_us >= 0) {
        const uint32_t batching = spb_open(&o, 1);
        spb_put_varint(&o, 1, (uint64_t)batch_period_us);   /* batch_period, required inside batching */
        spb_close(&o, batching);
    }
    if (payload_length) spb_put_bytes(&o, 2, payload, payload_length);
    spb_close(&o, mark);
    return spb_done(&o);
}

uint32_t ssc_build_qmi_request(uint8_t *out, uint32_t cap, uint16_t transaction, const uint8_t *message, uint32_t length, int jumbo)
{
    const uint32_t total = SSC_QMI_HEADER + 3 + 2 + length + (jumbo >= 0 ? 4u : 0u);
    if (!out || !message || !length || length > SSC_QMI_MAX_PAYLOAD || total > cap) return 0;
    out[0] = SSC_QMI_REQUEST;
    put16(out + 1, transaction);
    put16(out + 3, SSC_QMI_REQ);
    put16(out + 5, total - SSC_QMI_HEADER);
    out[7] = 0x01;
    put16(out + 8, length + 2);
    put16(out + 10, length);
    for (uint32_t i = 0; i < length; ++i) out[12 + i] = message[i];
    if (jumbo >= 0) {
        uint8_t *p = out + 12 + length;
        p[0] = 0x10;
        put16(p + 1, 1);
        p[3] = jumbo ? 1 : 0;
    }
    return total;
}

/* ---- QMI ---- */
int ssc_qmi_parse(const uint8_t *data, uint32_t length, SscQmi *m)
{
    if (!data || length < SSC_QMI_HEADER) return -1;
    m->type = data[0];
    m->transaction = le16(data + 1);
    m->message = le16(data + 3);
    m->length = le16(data + 5);
    if (m->length > length - SSC_QMI_HEADER) return -1;
    m->tlvs = data + SSC_QMI_HEADER;
    return 0;
}

const uint8_t *ssc_qmi_tlv(const SscQmi *m, uint8_t type, uint32_t *size)
{
    for (uint32_t at = 0; at + 3 <= m->length;) {
        const uint32_t n = le16(m->tlvs + at + 1);
        if (n > m->length - at - 3) return 0;
        if (m->tlvs[at] == type) {
            *size = n;
            return m->tlvs + at + 3;
        }
        at += 3 + n;
    }
    return 0;
}

int ssc_qmi_response(const SscQmi *m, SscResponse *r)
{
    uint32_t n = 0;
    *r = (SscResponse){0, 0, 0, 0, 0, 0};
    const uint8_t *result = ssc_qmi_tlv(m, 0x02, &n);
    if (!result || n < 4) return -1;
    r->result = le16(result);
    r->error = le16(result + 2);
    const uint8_t *client = ssc_qmi_tlv(m, 0x10, &n);
    if (client && n >= 8) {r->has_client = 1; r->client_id = le64(client);}
    const uint8_t *status = ssc_qmi_tlv(m, 0x11, &n);
    if (status && n >= 4) {r->has_status = 1; r->status = le32(status);}
    return 0;
}

int ssc_response_ok(const SscResponse *r)
{
    return r->result == 0 && r->error == 0 && (!r->has_status || r->status == 0);
}

int ssc_qmi_report(const SscQmi *m, int *has_client, uint64_t *client_id, const uint8_t **message, uint32_t *length)
{
    uint32_t n = 0;
    const uint8_t *client = ssc_qmi_tlv(m, 0x01, &n);
    *has_client = client && n >= 8;
    *client_id = *has_client ? le64(client) : 0;
    const uint8_t *events = ssc_qmi_tlv(m, 0x02, &n);
    if (!events || n < 2) return -1;
    const uint32_t count = le16(events);
    const uint32_t limit = m->message == SSC_QMI_JUMBO_REPORT ? SSC_QMI_MAX_JUMBO : SSC_QMI_MAX_PAYLOAD;
    if (count > n - 2 || count > limit) return -1;
    *message = events + 2;
    *length = count;
    return 0;
}

/* ---- events ---- */
static int read_suid(const uint8_t *p, uint32_t n, SscSuid *suid)
{
    SpbIn in;
    uint32_t field, wire;
    int r;
    *suid = (SscSuid){0, 0};
    spb_in(&in, p, n);
    while ((r = spb_next(&in, &field, &wire)) == 1) {
        if (field == 1 && wire == SPB_I64) {if (spb_fixed64_value(&in, wire, &suid->low)) return -1;}
        else if (field == 2 && wire == SPB_I64) {if (spb_fixed64_value(&in, wire, &suid->high)) return -1;}
        else if (spb_skip(&in, wire)) return -1;
    }
    return r;
}

int ssc_events_open(SscEvents *e, const uint8_t *message, uint32_t length)
{
    SpbIn in;
    uint32_t field, wire;
    int r;
    e->has_suid = 0;
    e->suid = (SscSuid){0, 0};
    spb_in(&in, message, length);
    while ((r = spb_next(&in, &field, &wire)) == 1) {
        if (field == 1 && wire == SPB_LEN) {
            const uint8_t *p = 0;
            uint32_t n = 0;
            if (spb_bytes_value(&in, wire, &p, &n) || read_suid(p, n, &e->suid)) return -1;
            e->has_suid = 1;
        } else if (spb_skip(&in, wire)) {
            return -1;
        }
    }
    if (r < 0) return -1;
    spb_in(&e->in, message, length);
    return 0;
}

int ssc_events_next(SscEvents *e, SscEvent *ev)
{
    uint32_t field, wire;
    int r;
    while ((r = spb_next(&e->in, &field, &wire)) == 1) {
        if (field != 2 || wire != SPB_LEN) {
            if (spb_skip(&e->in, wire)) return -1;
            continue;
        }
        const uint8_t *p = 0;
        uint32_t n = 0;
        if (spb_bytes_value(&e->in, wire, &p, &n)) return -1;
        SpbIn in;
        *ev = (SscEvent){0, 0, 0, 0};
        spb_in(&in, p, n);
        int s;
        while ((s = spb_next(&in, &field, &wire)) == 1) {
            if (field == 1 && wire == SPB_I32) {if (spb_fixed32_value(&in, wire, &ev->msg_id)) return -1;}
            else if (field == 2 && wire == SPB_I64) {if (spb_fixed64_value(&in, wire, &ev->timestamp)) return -1;}
            else if (field == 3 && wire == SPB_LEN) {if (spb_bytes_value(&in, wire, &ev->payload, &ev->length)) return -1;}
            else if (spb_skip(&in, wire)) return -1;
        }
        return s < 0 ? -1 : 1;
    }
    return r;
}

/* ---- payloads ---- */
int ssc_parse_sensor_event(const uint8_t *p, uint32_t n, SscSensorEvent *ev)
{
    SpbIn in;
    uint32_t field, wire;
    uint64_t v = 0;
    int r;
    ev->count = ev->more = 0;
    ev->status = -1;
    spb_in(&in, p, n);
    while ((r = spb_next(&in, &field, &wire)) == 1) {
        if (field == 1 && (wire == SPB_I32 || wire == SPB_LEN)) {
            if (spb_fixed32_values(&in, wire, ev->data, SSC_MAX_DATA, &ev->count, &ev->more)) return -1;
        } else if (field == 2 && wire == SPB_VARINT) {
            if (spb_varint_value(&in, wire, &v)) return -1;
            ev->status = (int64_t)v;
        } else if (spb_skip(&in, wire)) {
            return -1;
        }
    }
    return r < 0 ? -1 : 0;
}

int ssc_parse_proximity_event(const uint8_t *p, uint32_t n, SscProximityEvent *ev)
{
    SpbIn in;
    uint32_t field, wire;
    uint64_t v = 0;
    int r;
    ev->type = -1;
    ev->raw_adc = 0;
    ev->status = -1;
    spb_in(&in, p, n);
    while ((r = spb_next(&in, &field, &wire)) == 1) {
        if (field >= 1 && field <= 3 && wire == SPB_VARINT) {
            if (spb_varint_value(&in, wire, &v)) return -1;
            if (field == 1) ev->type = (int64_t)v;
            else if (field == 2) ev->raw_adc = v;
            else ev->status = (int64_t)v;
        } else if (spb_skip(&in, wire)) {
            return -1;
        }
    }
    return r < 0 || ev->type < 0 ? -1 : 0;
}

static void copy_text(char *to, uint32_t size, const uint8_t *from, uint32_t n)
{
    uint32_t i = 0;
    if (!size) return;
    for (; i < n && i + 1 < size; ++i) to[i] = from[i] >= 32 && from[i] < 127 ? (char)from[i] : '.';
    to[i] = 0;
}

int ssc_parse_suid_event(const uint8_t *p, uint32_t n, SscSuidEvent *ev)
{
    SpbIn in;
    uint32_t field, wire;
    int r;
    ev->data_type[0] = 0;
    ev->count = ev->more = 0;
    spb_in(&in, p, n);
    while ((r = spb_next(&in, &field, &wire)) == 1) {
        const uint8_t *bytes = 0;
        uint32_t size = 0;
        if (field == 1 && wire == SPB_LEN) {
            if (spb_bytes_value(&in, wire, &bytes, &size)) return -1;
            copy_text(ev->data_type, sizeof ev->data_type, bytes, size);
        } else if (field == 2 && wire == SPB_LEN) {
            SscSuid suid;
            if (spb_bytes_value(&in, wire, &bytes, &size) || read_suid(bytes, size, &suid)) return -1;
            if (ev->count < SSC_MAX_SUIDS) ev->suid[ev->count++] = suid;
            else ev->more++;
        } else if (spb_skip(&in, wire)) {
            return -1;
        }
    }
    return r < 0 ? -1 : 0;
}

/* sns_std_attr_value {repeated data {oneof: 1 subtype (attr_value), 2 str, 3 flt (float), 4 sint (sfixed64), 5 boolean}} */
typedef struct {
    uint32_t kind;                    /* the data's field: 1..5 */
    const uint8_t *bytes;             /* subtype, str */
    uint32_t size;
    uint32_t flt;
    int64_t sint;
} AttrDatum;

static int attr_values(const uint8_t *p, uint32_t n, AttrDatum *out, unsigned max, unsigned *count)
{
    SpbIn in;
    uint32_t field, wire;
    int r;
    *count = 0;
    spb_in(&in, p, n);
    while ((r = spb_next(&in, &field, &wire)) == 1) {
        if (field != 1 || wire != SPB_LEN) {
            if (spb_skip(&in, wire)) return -1;
            continue;
        }
        const uint8_t *data = 0;
        uint32_t size = 0;
        if (spb_bytes_value(&in, wire, &data, &size)) return -1;
        AttrDatum d = {0, 0, 0, 0, 0};
        SpbIn one;
        spb_in(&one, data, size);
        int s;
        while ((s = spb_next(&one, &field, &wire)) == 1) {
            uint64_t v = 0;
            if ((field == 1 || field == 2) && wire == SPB_LEN) {if (spb_bytes_value(&one, wire, &d.bytes, &d.size)) return -1;}
            else if (field == 3 && wire == SPB_I32) {if (spb_fixed32_value(&one, wire, &d.flt)) return -1;}
            else if (field == 4 && wire == SPB_I64) {if (spb_fixed64_value(&one, wire, &v)) return -1; d.sint = (int64_t)v;}
            else if (field == 5 && wire == SPB_VARINT) {if (spb_varint_value(&one, wire, &v)) return -1; d.sint = v != 0;}
            else {if (spb_skip(&one, wire)) return -1; continue;}
            d.kind = field;
        }
        if (s < 0) return -1;
        if (d.kind && *count < max) out[(*count)++] = d;
    }
    return r < 0 ? -1 : 0;
}

void ssc_attrs_clear(SscAttrs *a)
{
    uint8_t *b = (uint8_t *)a;
    for (uint32_t i = 0; i < sizeof *a; ++i) b[i] = 0;
    a->available = a->physical = a->dri = -1;
    a->version = a->stream_type = a->fifo_size = a->hw_id = -1;
}

static void attribute(SscAttrs *a, int64_t id, const uint8_t *value, uint32_t size)
{
    AttrDatum d[SSC_MAX_RATES];
    unsigned n = 0;
    if (attr_values(value, size, d, SSC_MAX_RATES, &n) || !n) return;
    a->seen++;
    switch (id) {
    case SSC_ATTR_NAME: if (d[0].kind == 2) copy_text(a->name, sizeof a->name, d[0].bytes, d[0].size); break;
    case SSC_ATTR_VENDOR: if (d[0].kind == 2) copy_text(a->vendor, sizeof a->vendor, d[0].bytes, d[0].size); break;
    case SSC_ATTR_TYPE: if (d[0].kind == 2) copy_text(a->type, sizeof a->type, d[0].bytes, d[0].size); break;
    case SSC_ATTR_API: if (d[0].kind == 2) copy_text(a->api, sizeof a->api, d[0].bytes, d[0].size); break;
    case SSC_ATTR_AVAILABLE: if (d[0].kind == 5) a->available = (int)d[0].sint; break;
    case SSC_ATTR_PHYSICAL_SENSOR: if (d[0].kind == 5) a->physical = (int)d[0].sint; break;
    case SSC_ATTR_DRI: if (d[0].kind == 5) a->dri = (int)d[0].sint; break;
    case SSC_ATTR_VERSION: if (d[0].kind == 4) a->version = d[0].sint; break;
    case SSC_ATTR_STREAM_TYPE: if (d[0].kind == 4) a->stream_type = d[0].sint; break;
    case SSC_ATTR_FIFO_SIZE: if (d[0].kind == 4) a->fifo_size = d[0].sint; break;
    case SSC_ATTR_HW_ID: if (d[0].kind == 4) a->hw_id = d[0].sint; break;
    case SSC_ATTR_RATES:
        a->rate_count = 0;
        for (unsigned i = 0; i < n; ++i) if (d[i].kind == 3 && a->rate_count < SSC_MAX_RATES) a->rates[a->rate_count++] = d[i].flt;
        break;
    case SSC_ATTR_RESOLUTIONS:
        a->resolution_count = 0;
        for (unsigned i = 0; i < n; ++i)
            if (d[i].kind == 3 && a->resolution_count < SSC_MAX_RANGES) a->resolutions[a->resolution_count++] = d[i].flt;
        break;
    case SSC_ATTR_RIGID_BODY:
        a->rigid_count = 0;
        for (unsigned i = 0; i < n; ++i) if (d[i].kind == 4 && a->rigid_count < SSC_MAX_RANGES) a->rigid_body[a->rigid_count++] = d[i].sint;
        break;
    case SSC_ATTR_ACTIVE_CURRENT:
        a->current_count = 0;
        for (unsigned i = 0; i < n; ++i)
            if (d[i].kind == 4 && a->current_count < SSC_MAX_RANGES) a->active_current[a->current_count++] = d[i].sint;
        break;
    case SSC_ATTR_RANGES:
        a->range_count = 0;
        for (unsigned i = 0; i < n && a->range_count < SSC_MAX_RANGES; ++i) {
            AttrDatum pair[2];
            unsigned k = 0;
            if (d[i].kind != 1 || attr_values(d[i].bytes, d[i].size, pair, 2, &k) || k != 2 || pair[0].kind != 3 || pair[1].kind != 3)
                continue;
            a->ranges[a->range_count][0] = pair[0].flt;
            a->ranges[a->range_count][1] = pair[1].flt;
            a->range_count++;
        }
        break;
    default:
        break;
    }
}

int ssc_parse_attr_event(const uint8_t *p, uint32_t n, SscAttrs *a)
{
    SpbIn in;
    uint32_t field, wire;
    int r;
    spb_in(&in, p, n);
    while ((r = spb_next(&in, &field, &wire)) == 1) {
        if (field != 1 || wire != SPB_LEN) {
            if (spb_skip(&in, wire)) return -1;
            continue;
        }
        const uint8_t *attr = 0;
        uint32_t size = 0;
        if (spb_bytes_value(&in, wire, &attr, &size)) return -1;
        SpbIn one;
        int64_t id = -1;
        const uint8_t *value = 0;
        uint32_t value_size = 0;
        int s;
        spb_in(&one, attr, size);
        while ((s = spb_next(&one, &field, &wire)) == 1) {
            uint64_t v = 0;
            if (field == 1 && wire == SPB_VARINT) {if (spb_varint_value(&one, wire, &v)) return -1; id = (int64_t)v;}
            else if (field == 2 && wire == SPB_LEN) {if (spb_bytes_value(&one, wire, &value, &value_size)) return -1;}
            else if (spb_skip(&one, wire)) return -1;
        }
        if (s < 0) return -1;
        if (id >= 0 && value) attribute(a, id, value, value_size);
    }
    return r < 0 ? -1 : 0;
}

int ssc_parse_physical_config(const uint8_t *p, uint32_t n, SscPhysicalConfig *c)
{
    SpbIn in;
    uint32_t field, wire;
    int r;
    unsigned more = 0;
    uint8_t *b = (uint8_t *)c;
    for (uint32_t i = 0; i < sizeof *c; ++i) b[i] = 0;
    c->dri = -1;
    spb_in(&in, p, n);
    while ((r = spb_next(&in, &field, &wire)) == 1) {
        uint64_t v = 0;
        const uint8_t *bytes = 0;
        uint32_t size = 0;
        if (field == 1 && wire == SPB_I32) {if (spb_fixed32_value(&in, wire, &c->sample_rate)) return -1;}
        else if (field == 2 && wire == SPB_VARINT) {if (spb_varint_value(&in, wire, &v)) return -1; c->water_mark = (uint32_t)v;}
        else if (field == 3 && (wire == SPB_I32 || wire == SPB_LEN)) {if (spb_fixed32_values(&in, wire, c->range, 2, &c->range_count, &more)) return -1;}
        else if (field == 4 && wire == SPB_I32) {if (spb_fixed32_value(&in, wire, &c->resolution)) return -1;}
        else if (field == 5 && wire == SPB_LEN) {if (spb_bytes_value(&in, wire, &bytes, &size)) return -1; copy_text(c->op_mode, sizeof c->op_mode, bytes, size);}
        else if (field == 6 && wire == SPB_VARINT) {if (spb_varint_value(&in, wire, &v)) return -1; c->active_current = (uint32_t)v;}
        else if (field == 8 && wire == SPB_VARINT) {if (spb_varint_value(&in, wire, &v)) return -1; c->dri = v != 0;}
        else if (spb_skip(&in, wire)) return -1;
    }
    return r < 0 ? -1 : 0;
}

int ssc_parse_error_event(const uint8_t *p, uint32_t n, int64_t *error)
{
    SpbIn in;
    uint32_t field, wire;
    int r;
    *error = -1;
    spb_in(&in, p, n);
    while ((r = spb_next(&in, &field, &wire)) == 1) {
        uint64_t v = 0;
        if (field == 1 && wire == SPB_VARINT) {if (spb_varint_value(&in, wire, &v)) return -1; *error = (int64_t)v;}
        else if (spb_skip(&in, wire)) return -1;
    }
    return r < 0 ? -1 : 0;
}
