// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Host tests of the sensor manager (SensorsCore.c) against a simulated sensor hub behind a fake IPC router, on a simulated clock:
//   - the hub decodes every request (QMI 0x20, sns_client_request_msg) with its own reader, answers each with a response carrying a
//     client id per connection, then acts like the sensor client service: SUID events per data type (a sensor may appear late,
//     register_updates), the discovery-done event, attribute events, streams at the configured rate with a physical config event,
//     on-change sensors reporting their state at once, disable; and on demand: refusals, dropped answers, missing attribute events;
//   - scenarios: the service appears late (reminders), the happy path with every sensor, the axis map and the unit conversions on the
//     samples, iOS demand changes (rates picked from the attributes, off), proximity on and off, refusals with backoff, timeouts, a
//     stream given up after 8 refusals, the service gone and back (new port; same port with a new generation), mode 2 (no stream), a
//     late barometer, attributes never sent (defaults), the list connection, malformed and stale messages, the log budget under a
//     flapping service, and the timers driven only by sns_core_idle_ms.
#include "check.h"
#include "../SensorsCore.h"
#include "../SensorsPb.h"

/* a failing check also shows the end of the log */
static void dump_log(void);
#undef CHECK
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); dump_log(); exit(1);} ++checks;} while (0)

/* ---- the clock and the log ---- */
static uint64_t now_ms;
static char logbuf[1 << 20];
static size_t logused;
static unsigned lines;
static uint64_t op_now(void *c) {(void)c; return now_ms;}
static void op_trace(void *c, const char *line)
{
    (void)c;
    const size_t n = strlen(line);
    lines++;
    CHECK(!strncmp(line, "[sensors] ", 10) && n < 240);
    if (logused + n + 2 < sizeof logbuf) {memcpy(logbuf + logused, line, n); logused += n; logbuf[logused++] = '\n'; logbuf[logused] = 0;}
}
static int logged(const char *text) {return strstr(logbuf, text) != 0;}
static unsigned count_lines(const char *text)
{
    unsigned n = 0;
    for (const char *p = logbuf; (p = strstr(p, text)); ++p) n++;
    return n;
}
static void dump_log(void) {fprintf(stderr, "---- log (end) ----\n%s", logused > 8000 ? logbuf + logused - 8000 : logbuf);}

/* ---- the hub ---- */
#define HUB_NODE 5u
static uint32_t hub_port = 0x1a;
typedef struct {
    const char *type;
    SscSuid suid;
    uint64_t appears_ms;          /* present from then on */
    int stream_type;              /* attribute STREAM_TYPE */
    float rates[4];
    unsigned nrates;
    float value[3];               /* what it measures */
    int refuse_config;            /* config requests to refuse */
    int drop_attr;                /* never send the attribute event */
} HubSensor;
static HubSensor hub_sensors[8];
static unsigned hub_count;
typedef struct {
    int active;
    unsigned sensor;
    uint32_t conn;
    uint32_t period_ms;
    uint64_t next_ms;
    int on_change;
    uint32_t rate_bits;
} HubStream;
static HubStream streams[16];
typedef struct {
    uint64_t at;
    unsigned conn;
    uint32_t node, port;
    uint32_t length;
    uint8_t bytes[1100];
} Msg;
static Msg queue[512];
static unsigned queued;
static struct {
    unsigned requests[SNS_CONNS];
    unsigned msg_count[1100];
    int refuse_all;               /* refuse every request */
    int drop_answers;             /* answer nothing (the next N requests) */
    int lookups_done[SNS_CONNS];
    uint32_t last_config_rate[8];
    unsigned disables[8], on_changes[8], configs[8], attrs[8];
    int prox_near;
    float lux;
    uint64_t ticks;
    char lookup_types[SNS_CONNS][8][32];
    unsigned lookup_n[SNS_CONNS];
    int default_only[SNS_CONNS];
} hub;

static void put16(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}
static void put64(uint8_t *p, uint64_t v) {for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i));}

static void enqueue(uint64_t at, unsigned conn, const uint8_t *data, uint32_t length)
{
    CHECK(queued < sizeof queue / sizeof queue[0] && length <= sizeof queue[0].bytes);
    Msg *m = &queue[queued++];
    m->at = at;
    m->conn = conn;
    m->node = HUB_NODE;
    m->port = hub_port;
    m->length = length;
    memcpy(m->bytes, data, length);
}

static void respond(unsigned conn, uint16_t txn, uint16_t result, uint16_t error)
{
    uint8_t r[32];
    r[0] = 2; put16(r + 1, txn); put16(r + 3, 0x20); put16(r + 5, 7 + 11);
    r[7] = 2; put16(r + 8, 4); put16(r + 10, result); put16(r + 12, error);
    r[14] = 0x10; put16(r + 15, 8); put64(r + 17, 0x100 + conn);
    enqueue(now_ms + 1, conn, r, 25);
}

/* one report: the event message (suid, one event) in a 0x21 indication */
static void report(unsigned conn, uint64_t delay, const SscSuid *suid, uint32_t msg_id, const uint8_t *payload, uint32_t plen)
{
    uint8_t msg[1000], ind[1100];
    SpbOut o;
    spb_out(&o, msg, sizeof msg);
    uint32_t mark = spb_open(&o, 1);
    spb_put_fixed64(&o, 1, suid->low);
    spb_put_fixed64(&o, 2, suid->high);
    spb_close(&o, mark);
    mark = spb_open(&o, 2);
    spb_put_fixed32(&o, 1, msg_id);
    spb_put_fixed64(&o, 2, (now_ms + delay) * 19200ull);
    if (plen) spb_put_bytes(&o, 3, payload, plen);
    spb_close(&o, mark);
    const uint32_t n = spb_done(&o);
    CHECK(n);
    ind[0] = 4; put16(ind + 1, 0); put16(ind + 3, 0x21); put16(ind + 5, 11 + 5 + n);
    ind[7] = 1; put16(ind + 8, 8); put64(ind + 10, 0x100 + conn);
    ind[18] = 2; put16(ind + 19, n + 2); put16(ind + 21, n);
    memcpy(ind + 23, msg, n);
    enqueue(now_ms + delay, conn, ind, 23 + n);
}

static int present(unsigned i) {return now_ms >= hub_sensors[i].appears_ms;}

static void suid_events(unsigned conn, const char *type, int default_only, uint64_t delay)
{
    unsigned matched = 0;
    for (unsigned i = 0; i < hub_count; ++i) {
        if (!present(i) || (type[0] && strcmp(type, hub_sensors[i].type))) continue;
        if (default_only && matched) continue;   /* the default only */
        uint8_t p[128];
        SpbOut o;
        spb_out(&o, p, sizeof p);
        spb_put_string(&o, 1, hub_sensors[i].type);
        const uint32_t mark = spb_open(&o, 2);
        spb_put_fixed64(&o, 1, hub_sensors[i].suid.low);
        spb_put_fixed64(&o, 2, hub_sensors[i].suid.high);
        spb_close(&o, mark);
        report(conn, delay, &ssc_suid_lookup, SSC_MSG_SUID_EVENT, p, spb_done(&o));
        matched++;
    }
}

static void attr_event(unsigned conn, unsigned i)
{
    uint8_t p[600];
    SpbOut o;
    const HubSensor *s = &hub_sensors[i];
    spb_out(&o, p, sizeof p);
    /* NAME, VENDOR, TYPE, AVAILABLE, STREAM_TYPE, RATES, RANGES, PHYSICAL, RIGID_BODY */
    struct {uint32_t id; int kind; const char *s; int64_t v;} simple[] = {
        {SSC_ATTR_NAME, 2, "sim_sensor", 0}, {SSC_ATTR_VENDOR, 2, "unixnu", 0}, {SSC_ATTR_TYPE, 2, s->type, 0},
        {SSC_ATTR_AVAILABLE, 5, 0, 1}, {SSC_ATTR_STREAM_TYPE, 4, 0, s->stream_type}, {SSC_ATTR_PHYSICAL_SENSOR, 5, 0, 1},
        {SSC_ATTR_RIGID_BODY, 4, 0, 0},
    };
    for (unsigned k = 0; k < sizeof simple / sizeof simple[0]; ++k) {
        const uint32_t attr = spb_open(&o, 1);
        spb_put_int(&o, 1, simple[k].id);
        const uint32_t value = spb_open(&o, 2);
        const uint32_t datum = spb_open(&o, 1);
        if (simple[k].kind == 2) spb_put_string(&o, 2, simple[k].s);
        else if (simple[k].kind == 5) spb_put_varint(&o, 5, (uint64_t)simple[k].v);
        else spb_put_fixed64(&o, 4, (uint64_t)simple[k].v);
        spb_close(&o, datum);
        spb_close(&o, value);
        spb_close(&o, attr);
    }
    if (s->nrates) {
        const uint32_t attr = spb_open(&o, 1);
        spb_put_int(&o, 1, SSC_ATTR_RATES);
        const uint32_t value = spb_open(&o, 2);
        for (unsigned r = 0; r < s->nrates; ++r) {
            const uint32_t datum = spb_open(&o, 1);
            spb_put_fixed32(&o, 3, float_bits(s->rates[r]));
            spb_close(&o, datum);
        }
        spb_close(&o, value);
        spb_close(&o, attr);
    }
    const uint32_t attr = spb_open(&o, 1);
    spb_put_int(&o, 1, SSC_ATTR_RANGES);
    const uint32_t value = spb_open(&o, 2);
    const uint32_t datum = spb_open(&o, 1);
    const uint32_t sub = spb_open(&o, 1);
    for (unsigned b = 0; b < 2; ++b) {
        const uint32_t d = spb_open(&o, 1);
        spb_put_fixed32(&o, 3, float_bits(b ? 78.4532f : -78.4532f));
        spb_close(&o, d);
    }
    spb_close(&o, sub);
    spb_close(&o, datum);
    spb_close(&o, value);
    spb_close(&o, attr);
    CHECK(spb_done(&o));
    report(conn, 20, &s->suid, SSC_MSG_ATTR_EVENT, p, spb_done(&o));
}

static void sample(unsigned conn, unsigned i, uint64_t delay)
{
    uint8_t p[64];
    SpbOut o;
    const HubSensor *s = &hub_sensors[i];
    spb_out(&o, p, sizeof p);
    if (!strcmp(s->type, "proximity")) {
        spb_put_varint(&o, 1, hub.prox_near ? 1 : 0);
        spb_put_varint(&o, 2, hub.prox_near ? 4000 : 12);
        spb_put_varint(&o, 3, 3);
        report(conn, delay, &s->suid, SSC_MSG_PROXIMITY_EVENT, p, spb_done(&o));
        return;
    }
    const unsigned n = !strcmp(s->type, "ambient_light") ? 2 : !strcmp(s->type, "pressure") ? 1 : 3;
    for (unsigned k = 0; k < n; ++k) spb_put_fixed32(&o, 1, float_bits(!strcmp(s->type, "ambient_light") && !k ? hub.lux : s->value[k]));
    spb_put_varint(&o, 2, 3);
    report(conn, delay, &s->suid, SSC_MSG_SENSOR_EVENT, p, spb_done(&o));
}

static int find_sensor(const SscSuid *suid)
{
    for (unsigned i = 0; i < hub_count; ++i) if (ssc_suid_equal(suid, &hub_sensors[i].suid)) return (int)i;
    return -1;
}

static HubStream *stream_of(unsigned conn, unsigned sensor)
{
    for (unsigned k = 0; k < 16; ++k) if (streams[k].active && streams[k].conn == conn && streams[k].sensor == sensor) return &streams[k];
    return 0;
}
static void stop_stream(unsigned conn, unsigned sensor)
{
    HubStream *s = stream_of(conn, sensor);
    if (s) s->active = 0;
}
static void start_stream(unsigned conn, unsigned sensor, uint32_t rate_bits, int on_change)
{
    stop_stream(conn, sensor);
    for (unsigned k = 0; k < 16; ++k) {
        if (streams[k].active) continue;
        const float hz = bits_float(rate_bits);
        streams[k] = (HubStream){1, sensor, conn, on_change ? 0 : (uint32_t)(1000.0f / hz + 0.5f), now_ms + 5, on_change, rate_bits};
        if (!on_change) {
            uint8_t p[32];
            SpbOut o;
            spb_out(&o, p, sizeof p);
            spb_put_fixed32(&o, 1, rate_bits);
            spb_put_varint(&o, 2, 1);
            spb_put_string(&o, 5, "NORMAL");
            report(conn, 2, &hub_sensors[sensor].suid, SSC_MSG_PHYSICAL_CONFIG_EVENT, p, spb_done(&o));
        } else {
            sample(conn, sensor, 3);   /* an on-change sensor reports its state at once */
        }
        return;
    }
    CHECK(0);
}

/* the router to the hub: one request */
static int op_send(void *c, unsigned conn, uint32_t node, uint32_t port, const uint8_t *data, uint32_t length)
{
    (void)c;
    CHECK(node == HUB_NODE && port == hub_port && conn < SNS_CONNS && length <= 480);
    SscQmi m;
    CHECK(ssc_qmi_parse(data, length, &m) == 0 && m.type == 0 && m.message == 0x20);
    uint32_t size = 0;
    const uint8_t *tlv = ssc_qmi_tlv(&m, 1, &size);
    CHECK(tlv && size >= 2 && (uint32_t)(tlv[0] | tlv[1] << 8) == size - 2);
    uint32_t jumbo_size = 0;
    const uint8_t *jumbo = ssc_qmi_tlv(&m, 0x10, &jumbo_size);
    CHECK(jumbo && jumbo_size == 1 && jumbo[0] == 0);         /* never jumbo reports (PaganiAdsp's 2 KiB intents) */
    hub.requests[conn]++;
    /* the request message, read with the hub's own walk over the fields */
    SpbIn in;
    uint32_t field, wire;
    SscSuid suid = {0, 0};
    uint32_t msg_id = 0;
    const uint8_t *payload = 0;
    uint32_t plen = 0;
    int susp = 0, req = 0;
    spb_in(&in, tlv + 2, size - 2);
    while (spb_next(&in, &field, &wire) == 1) {
        const uint8_t *p;
        uint32_t n;
        if (field == 1) {
            CHECK(spb_bytes_value(&in, wire, &p, &n) == 0);
            SpbIn s;
            spb_in(&s, p, n);
            while (spb_next(&s, &field, &wire) == 1) CHECK(spb_fixed64_value(&s, wire, field == 1 ? &suid.low : &suid.high) == 0);
        } else if (field == 2) {
            CHECK(spb_fixed32_value(&in, wire, &msg_id) == 0);
        } else if (field == 3) {
            CHECK(spb_bytes_value(&in, wire, &p, &n) == 0 && n == 4 && p[0] == 0x08 && p[1] == 1 && p[2] == 0x10 && p[3] == 0);
            susp = 1;
        } else if (field == 4) {
            CHECK(spb_bytes_value(&in, wire, &p, &n) == 0);
            SpbIn r;
            spb_in(&r, p, n);
            while (spb_next(&r, &field, &wire) == 1) {
                CHECK(field == 2);   /* no batching */
                CHECK(spb_bytes_value(&r, wire, &payload, &plen) == 0);
            }
            req = 1;
        } else {
            CHECK(0);
        }
    }
    CHECK(susp && req && !in.error && msg_id < sizeof hub.msg_count / sizeof hub.msg_count[0]);
    hub.msg_count[msg_id]++;
    if (hub.drop_answers) {hub.drop_answers--; return 0;}
    if (hub.refuse_all) {respond(conn, m.transaction, 1, 0x30); return 0;}
    if (ssc_suid_equal(&suid, &ssc_suid_lookup)) {
        CHECK(msg_id == SSC_MSG_SUID_REQ && (conn == SNS_CONN_LOOKUP || conn == SNS_CONN_LIST));
        SpbIn s;
        char type[32] = {0};
        uint64_t reg = 0, def = 0;
        spb_in(&s, payload, plen);
        while (spb_next(&s, &field, &wire) == 1) {
            if (field == 1) {
                const uint8_t *t;
                uint32_t n;
                CHECK(spb_bytes_value(&s, wire, &t, &n) == 0 && n < sizeof type);
                memcpy(type, t, n);
            } else if (field == 2) {
                CHECK(spb_varint_value(&s, wire, &reg) == 0);
            } else {
                CHECK(spb_varint_value(&s, wire, &def) == 0);
            }
        }
        CHECK(reg == 1 && (conn == SNS_CONN_LIST ? def == 0 : def == 1));
        respond(conn, m.transaction, 0, 0);
        if (hub.lookup_n[conn] < 8) strcpy(hub.lookup_types[conn][hub.lookup_n[conn]++], type);
        hub.default_only[conn] = (int)def;
        suid_events(conn, type, (int)def, 30);
        if (!hub.lookups_done[conn] && conn == SNS_CONN_LOOKUP) {
            hub.lookups_done[conn] = 1;
            report(conn, 200, &ssc_suid_lookup, SSC_MSG_SUID_DONE_EVENT, 0, 0);
        }
        return 0;
    }
    const int i = find_sensor(&suid);
    CHECK(i >= 0 && conn >= 1 && conn <= PaganiSensorKinds);
    if (msg_id == SSC_MSG_ATTR_REQ) {
        CHECK(!plen);
        hub.attrs[i]++;
        respond(conn, m.transaction, 0, 0);
        if (!hub_sensors[i].drop_attr) attr_event(conn, (unsigned)i);
        return 0;
    }
    if ((msg_id == SSC_MSG_SENSOR_CONFIG || msg_id == SSC_MSG_ON_CHANGE_CONFIG) && hub_sensors[i].refuse_config) {
        hub_sensors[i].refuse_config--;
        respond(conn, m.transaction, 1, 0x30);
        return 0;
    }
    respond(conn, m.transaction, 0, 0);
    if (msg_id == SSC_MSG_SENSOR_CONFIG) {
        SpbIn s;
        uint32_t rate = 0;
        spb_in(&s, payload, plen);
        CHECK(spb_next(&s, &field, &wire) == 1 && field == 1 && spb_fixed32_value(&s, wire, &rate) == 0);
        hub.configs[i]++;
        hub.last_config_rate[i] = rate;
        start_stream(conn, (unsigned)i, rate, 0);
    } else if (msg_id == SSC_MSG_ON_CHANGE_CONFIG) {
        CHECK(!plen);
        hub.on_changes[i]++;
        start_stream(conn, (unsigned)i, 0, 1);
    } else if (msg_id == SSC_MSG_DISABLE_REQ) {
        CHECK(!plen);
        hub.disables[i]++;
        stop_stream(conn, (unsigned)i);
    } else {
        CHECK(0);
    }
    return 0;
}

/* ---- what iOS gets ---- */
static struct {
    unsigned count[PaganiSensorKinds];
    SnsSample last[PaganiSensorKinds];
    unsigned found[PaganiSensorKinds];
    int orientation[16];
    unsigned orientations;
} ios;
static void op_sample(void *c, const SnsSample *s)
{
    (void)c;
    CHECK(s->kind < PaganiSensorKinds);
    ios.count[s->kind]++;
    ios.last[s->kind] = *s;
}
static void op_found(void *c, unsigned kind) {(void)c; CHECK(kind < PaganiSensorKinds); ios.found[kind]++;}

static void op_orientation(void *c, int state)
{
    (void)c;
    CHECK(state >= 0 && state < SNO_STATES && ios.orientations < 16);
    ios.orientation[ios.orientations++] = state;
}

static const SnsOps ops = {0, op_now, op_send, op_trace, op_sample, op_found, op_orientation};
static SnsCore core;
static uint32_t generation;

static void deliver_due(void)
{
    for (int any = 1; any;) {
        any = 0;
        for (unsigned k = 0; k < queued; ++k) {
            if (queue[k].at > now_ms) continue;
            Msg m = queue[k];
            memmove(&queue[k], &queue[k + 1], (queued - k - 1) * sizeof queue[0]);
            queued--;
            sns_core_receive(&core, m.conn, m.node, m.port, m.bytes, m.length);
            any = 1;
            break;
        }
    }
}
static void hub_tick(void)
{
    for (unsigned k = 0; k < 16; ++k) {
        HubStream *s = &streams[k];
        if (!s->active || s->on_change || now_ms < s->next_ms) continue;
        s->next_ms = now_ms + s->period_ms;
        sample(s->conn, s->sensor, 0);
    }
}
/* Fixed 1 ms steps: every message delivered when due. */
static void run(uint32_t ms)
{
    const uint64_t end = now_ms + ms;
    while (now_ms < end) {
        hub_tick();
        deliver_due();
        sns_core_tick(&core);
        const uint32_t idle = sns_core_idle_ms(&core);
        CHECK(idle >= 1 && idle <= 60000);
        now_ms++;
    }
}
/* Only at the core's own deadlines (and when the hub has something): the timers must be right. */
static void run_idle(uint32_t ms)
{
    const uint64_t end = now_ms + ms;
    while (now_ms < end) {
        hub_tick();
        deliver_due();
        sns_core_tick(&core);
        uint64_t next = now_ms + sns_core_idle_ms(&core);
        for (unsigned k = 0; k < queued; ++k) if (queue[k].at < next) next = queue[k].at;
        for (unsigned k = 0; k < 16; ++k) if (streams[k].active && !streams[k].on_change && streams[k].next_ms < next) next = streams[k].next_ms;
        if (next <= now_ms) next = now_ms + 1;
        now_ms = next < end ? next : end;
    }
}

static void hub_reset(void)
{
    memset(&hub, 0, sizeof hub);
    memset(streams, 0, sizeof streams);
    queued = 0;
    hub.lux = 312.4f;
    hub_count = 0;
    const HubSensor table[] = {
        {"accel", {0x1111, 0xa}, 0, 0, {25.0f, 50.0f, 104.0f, 208.0f}, 4, {0.0f, 0.0f, 9.80665f}, 0, 0},
        {"gyro", {0x2222, 0xb}, 0, 0, {25.0f, 50.0f, 104.0f, 208.0f}, 4, {0.01f, -0.02f, 0.5f}, 0, 0},
        {"mag", {0x3333, 0xc}, 0, 0, {10.0f, 25.0f, 50.0f, 100.0f}, 4, {20.5f, -3.25f, -41.0f}, 0, 0},
        {"ambient_light", {0x4444, 0xd}, 0, 1, {0}, 0, {0}, 0, 0},
        {"proximity", {0x5555, 0xe}, 0, 1, {0}, 0, {0}, 0, 0},
        {"wise_light", {0x6666, 0xf}, 0, 1, {0}, 0, {0}, 0, 0},
    };
    memcpy(hub_sensors, table, sizeof table);
    hub_count = sizeof table / sizeof table[0];
}

static uint32_t core_orient;

static void core_reset(uint32_t mode, int list, unsigned log, const char *axes)
{
    SnsConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.mode = mode;
    for (unsigned i = 0; i < 3; ++i) sfx_axes_identity(&cfg.axes[i]);
    if (axes) CHECK(sfx_axes_parse(axes, &cfg.axes[0], &cfg.axes[1], &cfg.axes[2]) == 0);
    strcpy(cfg.type[PaganiSensorAccel], "accel");
    strcpy(cfg.type[PaganiSensorGyro], "gyro");
    strcpy(cfg.type[PaganiSensorMag], "mag");
    strcpy(cfg.type[PaganiSensorLight], "ambient_light");
    strcpy(cfg.type[PaganiSensorProximity], "proximity");
    strcpy(cfg.type[PaganiSensorPressure], "pressure");
    cfg.list = (uint32_t)list;
    cfg.log = log;
    cfg.orient = core_orient;
    memset(&ios, 0, sizeof ios);
    logused = 0;
    logbuf[0] = 0;
    lines = 0;
    sns_core_init(&core, &ops, &cfg);
}
static void service(int up)
{
    generation++;
    sns_core_server(&core, up, HUB_NODE, hub_port, 0x1, generation);
}

static void happy(void)
{
    hub_reset();
    now_ms = 1000;
    hub_port = 0x1a;
    core_reset(3, 0, 1, "a-y+x+z");
    /* the service appears after 75 s: reminders at 10 s and 60 s, nothing sent before */
    run(75000);
    CHECK(count_lines("waiting for the sensor client service (400)") == 2 && logged("(400) s=10") && logged("(400) s=60"));
    CHECK(hub.requests[0] == 0);
    PaganiSensorsStatus st;
    sns_core_status(&core, &st);
    CHECK(st.stage == PaganiSensorsStPorts && st.found == 0);
    service(1);
    CHECK(logged("service up node=5 port=0x1a instance=0x1 gen=1 waited_ms=75000"));
    run(3000);
    /* discovery: one request per data type on connection 0, five found, the barometer missing */
    CHECK(hub.lookup_n[0] == 6 && !strcmp(hub.lookup_types[0][0], "accel") && !strcmp(hub.lookup_types[0][5], "pressure"));
    CHECK(logged("discovery done found=accel,gyro,mag,light,prox missing=pressure"));
    CHECK(logged("suid accel type=accel n=1 0xa:0x1111"));
    for (unsigned k = 0; k < PaganiSensorPressure; ++k) CHECK(ios.found[k] == 1);
    CHECK(ios.found[PaganiSensorPressure] == 0);
    /* attributes on each sensor's own connection */
    CHECK(logged("attr accel name=sim_sensor vendor=unixnu type=accel avail=1 phys=1 stream=0 hz=25.000,50.000,104.000,208.000 range=-78.453..78.453 rigid=0"));
    for (unsigned i = 0; i < 5; ++i) CHECK(hub.attrs[i] == 1);
    /* ambient light always on (on change, reported at once); nothing else without iOS asking */
    CHECK(hub.on_changes[3] == 1 && hub.configs[0] == 0 && hub.on_changes[4] == 0);
    CHECK(ios.count[PaganiSensorLight] == 1 && ios.last[PaganiSensorLight].value[0] == 312);
    CHECK(logged("stream light on change demand_us=0") && logged("first light 312.400 lux=312"));
    sns_core_status(&core, &st);
    CHECK(st.stage == PaganiSensorsStData && st.found == 0x1f && st.described == 0x1f && st.streaming == (1u << PaganiSensorLight));
    /* iOS asks for the accelerometer every 10 ms: 104 Hz, the first attribute rate at or above 100 */
    sns_core_demand(&core, PaganiSensorAccel, 10000);
    run(1000);
    CHECK(hub.configs[0] == 1 && hub.last_config_rate[0] == float_bits(104.0f) && logged("stream accel on hz=104.000 demand_us=10000"));
    CHECK(logged("hw accel hz=104.000 mode=NORMAL wm=1 dri=-1"));
    CHECK(ios.count[PaganiSensorAccel] >= 95 && ios.count[PaganiSensorAccel] <= 110);
    /* lying face up: the hub's +9.80665 on z is -1 g on iOS; the map a-y+x+z puts the hub's -y on x (all zero but z) */
    const SnsSample *a = &ios.last[PaganiSensorAccel];
    CHECK(a->value[0] == 0 && a->value[1] == 0 && a->value[2] == -65536 && a->milli[2] == 9807 && a->status == 3);
    CHECK(a->timestamp % 19200 == 0 && a->timestamp / 19200 <= now_ms);
    /* 50 Hz, then off */
    sns_core_demand(&core, PaganiSensorAccel, 20000);
    run(500);
    CHECK(hub.configs[0] == 2 && hub.last_config_rate[0] == float_bits(50.0f));
    sns_core_demand(&core, PaganiSensorAccel, 20000);   /* the same: nothing sent */
    run(100);
    CHECK(hub.configs[0] == 2);
    sns_core_demand(&core, PaganiSensorAccel, 0);
    run(500);
    CHECK(hub.disables[0] == 1 && !stream_of(1, 0) && logged("stream accel off demand_us=0"));
    const unsigned accel_count = ios.count[PaganiSensorAccel];
    run(1000);
    CHECK(ios.count[PaganiSensorAccel] <= accel_count + 1);
    /* a rate above every attribute rate: the highest; below every one: the lowest */
    sns_core_demand(&core, PaganiSensorGyro, 1000);
    run(300);
    CHECK(hub.last_config_rate[1] == float_bits(208.0f));
    sns_core_demand(&core, PaganiSensorMag, 1000000);
    run(1500);
    CHECK(hub.last_config_rate[2] == float_bits(10.0f));
    /* gyro and magnetometer: same axes as the hub (identity maps), 16.16 */
    CHECK(ios.last[PaganiSensorGyro].value[2] == 32768 && abs(ios.last[PaganiSensorGyro].value[0] - 655) <= 1);
    CHECK(ios.last[PaganiSensorMag].value[0] == 1343488 && ios.last[PaganiSensorMag].value[1] == -212992 &&
          ios.last[PaganiSensorMag].value[2] == -2686976);
    /* proximity while iOS has a detection mode: on change, the state at once; off again */
    hub.prox_near = 1;
    sns_core_demand(&core, PaganiSensorProximity, 1);
    run(200);
    CHECK(hub.on_changes[4] == 1 && ios.count[PaganiSensorProximity] == 1 && ios.last[PaganiSensorProximity].value[0] == 1 &&
          ios.last[PaganiSensorProximity].value[1] == 4000);
    sns_core_demand(&core, PaganiSensorProximity, 0);
    run(200);
    CHECK(hub.disables[4] == 1);
    /* each connection had its own client id; every request was answered */
    for (unsigned conn = 1; conn <= 5; ++conn) CHECK(core.conn[conn].has_client && core.conn[conn].client_id == 0x100 + conn);
    CHECK(core.refused == 0 && core.malformed == 0 && core.unexpected == 0 && core.stale == 0);
    printf("  happy path: %u lines, %u requests, accel %u gyro %u mag %u light %u prox %u samples\n", lines, core.sent,
           ios.count[0], ios.count[1], ios.count[2], ios.count[3], ios.count[4]);
}

static void refusals_and_timeouts(void)
{
    hub_reset();
    now_ms = 5000;
    core_reset(3, 0, 1, 0);
    hub_sensors[1].refuse_config = 3;     /* gyro: three refusals, then accepted (backoff 1, 2, 4 s) */
    hub_sensors[2].refuse_config = 100;   /* mag: never */
    service(1);
    run(2000);
    sns_core_demand(&core, PaganiSensorGyro, 10000);
    sns_core_demand(&core, PaganiSensorMag, 10000);
    run(500);
    CHECK(hub.configs[1] == 0 && logged("stream gyro request refused msg=513 tries=1"));
    run(8000);
    CHECK(hub.configs[1] == 1 && ios.count[PaganiSensorGyro] > 0 && hub.msg_count[SSC_MSG_SENSOR_CONFIG] >= 4);
    /* the magnetometer: 8 refusals over 1+2+4+8+16+32+60 s, then given up */
    run(130000);
    CHECK(logged("FAIL st=-6 err=-5 sensor=mag stream request refused every time; given up until the service restarts"));
    const unsigned asked = hub.requests[3];
    run(120000);
    CHECK(hub.requests[3] == asked);
    PaganiSensorsStatus st;
    sns_core_status(&core, &st);
    CHECK(st.error == PaganiSensorsErrRefused && !(st.streaming & (1u << PaganiSensorMag)));
    /* answers dropped: the request times out after 2 s and goes again */
    hub.drop_answers = 1;
    sns_core_demand(&core, PaganiSensorAccel, 20000);
    run(1000);
    CHECK(hub.msg_count[SSC_MSG_SENSOR_CONFIG] > 0 && core.conn[1].timeouts == 0 && core.conn[1].pending);
    run(4000);
    CHECK(core.conn[1].timeouts == 1 && hub.configs[0] == 1 && logged("stream accel request unanswered msg=513 tries=1"));
    /* the service restarts: the given-up magnetometer is tried again */
    hub_sensors[2].refuse_config = 0;
    service(0);
    service(1);
    run(5000);
    CHECK(hub.configs[2] == 1 && ios.count[PaganiSensorMag] > 0);
    printf("  refusals, timeouts, give up: %u lines\n", lines);
}

static void restarts(void)
{
    hub_reset();
    now_ms = 100;
    core_reset(3, 0, 1, 0);
    service(1);
    run(2000);
    sns_core_demand(&core, PaganiSensorAccel, 10000);
    sns_core_demand(&core, PaganiSensorProximity, 1);
    run(2000);
    CHECK(hub.configs[0] == 1 && hub.on_changes[4] == 1 && hub.on_changes[3] == 1);
    /* the service goes: no request while it is away; it comes back on another port: everything again, as iOS still asks */
    service(0);
    CHECK(logged("service gone node=5 port=0x1a gen=") && logged("; streams and SUIDs dropped"));
    memset(streams, 0, sizeof streams);
    const unsigned before = hub.requests[0] + hub.requests[1];
    run(15000);
    CHECK(hub.requests[0] + hub.requests[1] == before && logged("(400) s=10"));
    hub_port = 0x2b;
    memset(hub.lookups_done, 0, sizeof hub.lookups_done);
    service(1);
    run(3000);
    CHECK(hub.configs[0] == 2 && hub.on_changes[4] == 2 && hub.on_changes[3] == 2 && hub.attrs[0] == 2);
    /* a message from the old port is stale */
    const uint32_t stale = core.stale;
    uint8_t junk[8] = {4, 0, 0, 0x21, 0, 0, 0};
    sns_core_receive(&core, 1, HUB_NODE, 0x1a, junk, 7);
    CHECK(core.stale == stale + 1);
    /* the same node and port with a new generation: a restart nobody saw go */
    memset(streams, 0, sizeof streams);
    memset(hub.lookups_done, 0, sizeof hub.lookups_done);
    generation++;
    sns_core_server(&core, 1, HUB_NODE, hub_port, 0x1, generation);
    CHECK(logged("back at once (restarted), starting over"));
    run(3000);
    CHECK(hub.configs[0] == 3 && hub.on_changes[4] == 3);
    hub_port = 0x1a;
    printf("  service gone and back: %u lines\n", lines);
}

static void mode2_late_and_defaults(void)
{
    /* mode 2: lookups and attributes, never a stream */
    hub_reset();
    now_ms = 100;
    core_reset(2, 1, 1, 0);
    service(1);
    sns_core_demand(&core, PaganiSensorAccel, 10000);
    run(5000);
    CHECK(hub.msg_count[SSC_MSG_SENSOR_CONFIG] == 0 && hub.msg_count[SSC_MSG_ON_CHANGE_CONFIG] == 0 && hub.attrs[0] == 1);
    /* the list connection: every data type, all SUIDs; logged, never used to pick */
    CHECK(hub.lookup_n[SNS_CONN_LIST] == 1 && hub.lookup_types[SNS_CONN_LIST][0][0] == 0 && !hub.default_only[SNS_CONN_LIST]);
    CHECK(logged("list wise_light n=1 0xf:0x6666") && logged("list accel n=1 0xa:0x1111"));
    /* a barometer that appears 20 s in: the hub reports it to the lookup that asked (register_updates) */
    hub_reset();
    now_ms = 100;
    hub_sensors[hub_count++] = (HubSensor){"pressure", {0x7777, 0x10}, 20000, 0, {1.0f, 25.0f}, 2, {1013.25f, 0, 0}, 0, 0};
    core_reset(3, 0, 1, 0);
    service(1);
    run(5000);
    CHECK(!core.sensor[PaganiSensorPressure].found && logged("missing=pressure"));
    suid_events(SNS_CONN_LOOKUP, "pressure", 1, 0);   /* nothing yet: not present */
    run(16000);
    suid_events(SNS_CONN_LOOKUP, "pressure", 1, 10);  /* the hub's update once it is */
    run(1000);
    CHECK(core.sensor[PaganiSensorPressure].found && ios.found[PaganiSensorPressure] == 1 && hub.attrs[6] == 1);
    sns_core_demand(&core, PaganiSensorPressure, 1000000);
    run(3000);
    CHECK(hub.last_config_rate[6] == float_bits(1.0f) && ios.count[PaganiSensorPressure] >= 2 &&
          ios.last[PaganiSensorPressure].value[0] == 6640435);
    /* attributes never sent: three tries 5 s apart, then defaults; the stream still starts at the asked rate */
    hub_reset();
    now_ms = 100;
    for (unsigned i = 0; i < hub_count; ++i) hub_sensors[i].drop_attr = 1;
    core_reset(3, 0, 1, 0);
    service(1);
    sns_core_demand(&core, PaganiSensorAccel, 16667);
    run(17000);
    CHECK(hub.attrs[0] == 3 && logged("attr accel no attribute event, going on with defaults"));
    CHECK(hub.configs[0] == 1 && hub.last_config_rate[0] == float_bits(59.998f) && ios.count[PaganiSensorAccel] > 0);
    CHECK(hub.on_changes[3] == 1);   /* ambient light: on change by default */
    printf("  mode 2, list, late barometer, missing attributes: %u lines\n", lines);
}

static void robustness_and_budget(void)
{
    hub_reset();
    now_ms = 100;
    core_reset(3, 0, 1, 0);
    service(1);
    run(3000);
    /* malformed and unexpected messages: counted, nothing breaks */
    uint8_t junk[64];
    for (unsigned i = 0; i < sizeof junk; ++i) junk[i] = (uint8_t)(i * 37);
    for (uint32_t n = 0; n < sizeof junk; ++n) sns_core_receive(&core, n % SNS_CONNS, HUB_NODE, hub_port, junk, n);
    sns_core_receive(&core, 99, HUB_NODE, hub_port, junk, 10);
    const uint8_t response_nobody_asked[] = {2, 0x33, 0x33, 0x20, 0, 7, 0, 2, 4, 0, 0, 0, 0, 0};
    sns_core_receive(&core, 2, HUB_NODE, hub_port, response_nobody_asked, sizeof response_nobody_asked);
    CHECK(core.malformed > 0 && core.unexpected > 0);
    /* a report for a SUID the connection does not have */
    const SscSuid other = {9, 9};
    report(1, 0, &other, SSC_MSG_SENSOR_EVENT, junk, 0);
    const uint32_t unexpected = core.unexpected;
    run(5);
    CHECK(core.unexpected == unexpected + 1);
    /* a flapping service: the log stays within its budget */
    unsigned before = lines;
    for (unsigned i = 0; i < 300; ++i) {
        memset(streams, 0, sizeof streams);
        memset(hub.lookups_done, 0, sizeof hub.lookups_done);
        service(0);
        service(1);
        run(300);
    }
    CHECK(lines - before < 300 + 32 + 2 * 300 / 64 + 20);   /* the budget, the first transitions and every 64th, a few more */
    CHECK(core.budget == 0 && logged(" n=576; streams and SUIDs dropped"));
    /* with pagani-sensors-log=0 only failures */
    hub_reset();
    now_ms = 100;
    core_reset(3, 0, 0, 0);
    hub_sensors[0].refuse_config = 100;
    service(1);
    sns_core_demand(&core, PaganiSensorAccel, 10000);
    run(140000);
    CHECK(logged("FAIL st=-6 err=-5 sensor=accel") && !logged("attr accel") && !logged("first light"));
    printf("  malformed input, log budget: %u lines in the quiet run\n", lines);
}

static void timers_by_idle(void)
{
    /* The same refusal and give-up path, ticking only when sns_core_idle_ms says (no 1 ms polling). */
    hub_reset();
    now_ms = 100;
    core_reset(3, 0, 1, 0);
    hub_sensors[0].refuse_config = 2;
    hub_sensors[2].refuse_config = 100;
    run_idle(30000);
    CHECK(logged("(400) s=10") && !logged("(400) s=60"));
    service(1);
    sns_core_demand(&core, PaganiSensorAccel, 10000);
    sns_core_demand(&core, PaganiSensorMag, 10000);
    hub.drop_answers = 0;
    run_idle(200000);
    CHECK(hub.configs[0] == 1 && ios.count[PaganiSensorAccel] > 1000);
    CHECK(logged("FAIL st=-6 err=-5 sensor=mag"));
    run_idle(3700000);
    CHECK(logged("[sensors] summary up=1"));
    printf("  timers driven by idle only\n");
}

/* The orientation on: the accelerometer streams with no iOS request, at no less than 25 Hz whatever iOS asks; turning the phone is one
 * state each, after the hold; with it off nothing of this happens. A repeated detection mode gives the proximity state again. */
static void orientation_and_proximity(void)
{
    hub_reset();
    now_ms = 1000;
    hub_port = 0x1a;
    core_orient = 1;
    core_reset(3, 0, 1, 0);
    core_orient = 0;
    service(1);
    run(2000);
    CHECK(hub.configs[0] == 1 && hub.last_config_rate[0] == float_bits(25.0f) && logged("stream accel on hz=25.000 demand_us=0"));
    /* lying face up (the hub's +z): one state, the first after the hold */
    run(1000);
    CHECK(ios.orientations == 1 && ios.orientation[0] == SNO_FACE_UP);
    /* upright: gravity on the hub's +y (supporting force), iOS y = -1 */
    hub_sensors[0].value[0] = 0.0f; hub_sensors[0].value[1] = 9.80665f; hub_sensors[0].value[2] = 0.0f;
    run(1000);
    CHECK(ios.orientations == 2 && ios.orientation[1] == SNO_PORTRAIT);
    /* on its left side: iOS x = -1, the hub's +x */
    hub_sensors[0].value[0] = 9.80665f; hub_sensors[0].value[1] = 0.0f;
    run(1000);
    CHECK(ios.orientations == 3 && ios.orientation[2] == SNO_LANDSCAPE_LEFT);
    run(2000);
    CHECK(ios.orientations == 3);
    /* iOS asks for faster: faster; asks for slower than 25 Hz, or stops: 25 Hz stays */
    sns_core_demand(&core, PaganiSensorAccel, 10000);
    run(300);
    CHECK(hub.last_config_rate[0] == float_bits(104.0f));
    sns_core_demand(&core, PaganiSensorAccel, 200000);
    run(300);
    CHECK(hub.last_config_rate[0] == float_bits(25.0f));
    const unsigned configs = hub.configs[0], disables = hub.disables[0];
    sns_core_demand(&core, PaganiSensorAccel, 0);
    run(300);
    CHECK(hub.configs[0] == configs && hub.disables[0] == disables && ios.count[PaganiSensorAccel] > 100);
    /* proximity: a mode on (the hub reports at once), the same request again gives the state again, nothing goes to the hub */
    hub.prox_near = 1;
    sns_core_demand(&core, PaganiSensorProximity, 1);
    run(200);
    CHECK(hub.on_changes[4] == 1 && ios.count[PaganiSensorProximity] == 1 && ios.last[PaganiSensorProximity].value[0] == 1);
    const unsigned requests = hub.requests[1 + PaganiSensorProximity];
    sns_core_demand(&core, PaganiSensorProximity, 1);
    sns_core_demand(&core, PaganiSensorProximity, 1);
    CHECK(ios.count[PaganiSensorProximity] == 3 && ios.last[PaganiSensorProximity].value[0] == 1);
    run(200);
    CHECK(hub.requests[1 + PaganiSensorProximity] == requests);
    /* another sensor's repeat, and proximity off, repeat nothing */
    const unsigned lights = ios.count[PaganiSensorLight];
    sns_core_demand(&core, PaganiSensorLight, 7);
    sns_core_demand(&core, PaganiSensorLight, 7);
    CHECK(ios.count[PaganiSensorLight] == lights);
    sns_core_demand(&core, PaganiSensorProximity, 0);
    run(200);
    sns_core_demand(&core, PaganiSensorProximity, 0);
    CHECK(ios.count[PaganiSensorProximity] == 3 && hub.disables[4] == 1);
    printf("  orientation and repeated detection modes\n");
}

int main(void)
{
    happy();
    refusals_and_timeouts();
    restarts();
    mode2_late_and_defaults();
    robustness_and_budget();
    timers_by_idle();
    orientation_and_proximity();
    if (getenv("SENSORS_TEST_LOG")) dump_log();
    printf("PASS sensor manager against a simulated hub: %u checks\n", checks);
    return 0;
}
