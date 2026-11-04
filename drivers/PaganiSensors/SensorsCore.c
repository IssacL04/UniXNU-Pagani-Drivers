/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The sensor manager: see SensorsCore.h. */
#include "SensorsCore.h"
#include "SensorsText.h"

enum {
    kResponseMs = 2000,         /* a request's response */
    kSendRetryMs = 200,         /* the router queue was full */
    kAttrMs = 5000,             /* the attribute event after the request */
    kAttrTries = 3,
    kStreamTries = 8,           /* a stream refused this often in a row is given up */
    kBackoffMaxMs = 60000,
    kDiscoveryMs = 15000,
    kQuietMs = 10000,           /* no sample this long after a stream start: logged once */
    kSummaryMs = 3600000,
    kBudget = 300,              /* ordinary log lines per boot (pagani-sensors-log=1) */
    kListLines = 64,
    kIdleMaxMs = 60000,
    kListBit = 1u << PaganiSensorKinds,
    kOrientIntervalUs = 40000,  /* the accelerometer's slowest rate while the orientation is computed: 25 Hz */
};

static const char *const kNames[PaganiSensorKinds] = {"accel", "gyro", "mag", "light", "prox", "pressure"};

const char *sns_kind_name(unsigned kind)
{
    return kind < PaganiSensorKinds ? kNames[kind] : "?";
}

#define CTX (c->ops->context)
static uint64_t now(SnsCore *c) {return c->ops->now_ms(CTX);}

static void zero(void *p, uint32_t n)
{
    uint8_t *b = (uint8_t *)p;
    for (uint32_t i = 0; i < n; ++i) b[i] = 0;
}
static int same_text(const char *a, const char *b)
{
    uint32_t i = 0;
    for (; a[i] && b[i]; ++i) if (a[i] != b[i]) return 0;
    return a[i] == b[i];
}
static uint64_t backoff(unsigned tries)
{
    const unsigned shift = tries > 7 ? 6 : (tries ? tries - 1 : 0);
    const uint64_t ms = 1000ull << shift;
    return ms > kBackoffMaxMs ? kBackoffMaxMs : ms;
}

/* ---- the log ---- */
static SntText *begin(SnsCore *c, SntText *t, const char *what)
{
    snt_start(t, c->line, sizeof c->line);
    snt_str(t, "[sensors] ");
    snt_str(t, what);
    return t;
}
static void end(SnsCore *c)
{
    if (c->ops->trace) c->ops->trace(CTX, c->line);
}
/* An ordinary line may go out: always with pagani-sensors-log=2, never with 0, within the budget with 1. Failures do not ask. */
static int ordinary(SnsCore *c)
{
    if (c->cfg.log >= 2) return 1;
    if (!c->cfg.log || !c->budget) return 0;
    c->budget--;
    return 1;
}
static void suid_text(SntText *t, const SscSuid *s)
{
    snt_hex(t, s->high);
    snt_char(t, ':');
    snt_hex(t, s->low);
}
static void rate_text(SntText *t, uint32_t bits)
{
    snt_milli(t, sfx_milli(bits));
}
static void fail_line(SnsCore *c, unsigned kind, int32_t stage, int32_t error, const char *why)
{
    SntText t;
    c->error = error;
    c->failures++;
    begin(c, &t, "FAIL");
    snt_kd(&t, "st", -stage);
    snt_kd(&t, "err", error);
    if (kind < PaganiSensorKinds) snt_ks(&t, "sensor", kNames[kind]);
    snt_char(&t, ' ');
    snt_str(&t, why);
    end(c);
}

/* ---- requests ---- */
static int request(SnsCore *c, unsigned conn, const SscSuid *suid, uint32_t msg, const uint8_t *payload, uint32_t length, uint32_t tag)
{
    SnsConn *k = &c->conn[conn];
    uint8_t message[160], qmi[200];
    const uint32_t n = ssc_build_request(message, sizeof message, suid, msg, SSC_DELIVERY_WAKEUP, payload, length, -1);
    uint16_t txn = (uint16_t)(k->next_txn + 1);
    if (!txn) txn = 1;
    const uint32_t q = n ? ssc_build_qmi_request(qmi, sizeof qmi, txn, message, n, 0) : 0;
    if (!q) {
        c->failures++;
        return -1;
    }
    if (c->ops->send(CTX, conn, c->node, c->port, qmi, q)) {
        c->refused++;
        return -1;
    }
    k->next_txn = txn;
    k->pending = 1;
    k->pending_txn = txn;
    k->pending_msg = msg;
    k->pending_tag = tag;
    k->sent_ms = now(c);
    k->requests++;
    c->sent++;
    return 0;
}

/* ---- the state ---- */
static void restart(SnsCore *c)
{
    for (unsigned i = 0; i < SNS_CONNS; ++i) {
        SnsConn *k = &c->conn[i];
        k->pending = 0;
        k->has_client = 0;
        k->client_id = 0;
    }
    for (unsigned kind = 0; kind < PaganiSensorKinds; ++kind) {
        SnsSensor *s = &c->sensor[kind];
        const uint32_t demand = s->demand_us, events = s->events, errors = s->errors, bad = s->bad, skipped = s->skipped;
        zero(s, sizeof *s);
        s->demand_us = demand;
        s->events = events;
        s->errors = errors;
        s->bad = bad;
        s->skipped = skipped;
        ssc_attrs_clear(&s->attrs);
    }
    c->lookup_sent = 0;
    c->lookup_tries = 0;
    c->lookup_retry_at = 0;
    c->discovery_done = c->discovery_logged = 0;
    c->listed = 0;
}

void sns_core_init(SnsCore *c, const SnsOps *ops, const SnsConfig *config)
{
    zero(c, sizeof *c);
    c->ops = ops;
    c->cfg = *config;
    if (!c->cfg.als_permille) c->cfg.als_permille = 1000;
    for (unsigned i = 0; i < 3; ++i) if (!sfx_axes_valid(&c->cfg.axes[i])) sfx_axes_identity(&c->cfg.axes[i]);
    for (unsigned i = 0; i < PaganiSensorKinds; ++i) c->cfg.type[i][SSC_TYPE_MAX - 1] = 0;
    c->budget = kBudget;
    c->stage = PaganiSensorsStPorts;
    sno_init(&c->orient);
    restart(c);
    const uint64_t t = now(c);
    c->wait_since = t;
    c->reminder_at = t + 10000;
    c->summary_at = t + kSummaryMs;
}

void sns_core_stage(SnsCore *c, int32_t stage, int32_t error)
{
    c->stage = stage;
    if (error) c->error = error;
}

static void step(SnsCore *c);

/* The service's transitions: the first 32 logged, then every 64th (a flapping sensor PD must not fill the log). */
static int transition(SnsCore *c)
{
    c->transitions++;
    return c->transitions <= 32 || !(c->transitions % 64);
}

void sns_core_server(SnsCore *c, int up, uint32_t node, uint32_t port, uint32_t instance, uint32_t generation)
{
    SntText t;
    const uint64_t at = now(c);
    if (up && c->up && c->node == node && c->port == port && c->instance == instance && c->generation == generation) return;
    if (c->up) {
        if (transition(c)) {
            begin(c, &t, "service gone");
            snt_kd(&t, "node", c->node);
            snt_kx(&t, "port", c->port);
            snt_kd(&t, "gen", c->generation);
            snt_kd(&t, "n", c->transitions);
            snt_str(&t, up ? "; back at once (restarted), starting over" : "; streams and SUIDs dropped");
            end(c);
        }
        restart(c);
        c->up = 0;
        c->wait_since = at;
        c->reminders = 0;
        c->reminder_at = at + 10000;
    }
    c->generation = generation;
    if (!up) return;
    c->up = 1;
    c->node = node;
    c->port = port;
    c->instance = instance;
    c->discovery_started = at;
    if (transition(c)) {
        begin(c, &t, "service up");
        snt_kd(&t, "node", node);
        snt_kx(&t, "port", port);
        snt_kx(&t, "instance", instance);
        snt_kd(&t, "gen", generation);
        snt_kd(&t, "waited_ms", (int64_t)(at - c->wait_since));
        end(c);
    }
    step(c);
}

/* ---- the lookups ---- */
static void discovery_line(SnsCore *c, const char *what)
{
    SntText t;
    if (!ordinary(c)) return;
    begin(c, &t, "discovery ");
    snt_str(&t, what);
    snt_str(&t, " found=");
    unsigned n = 0;
    for (unsigned k = 0; k < PaganiSensorKinds; ++k)
        if (c->sensor[k].found) {if (n++) snt_char(&t, ','); snt_str(&t, kNames[k]);}
    if (!n) snt_str(&t, "none");
    snt_str(&t, " missing=");
    n = 0;
    for (unsigned k = 0; k < PaganiSensorKinds; ++k)
        if (c->cfg.type[k][0] && !c->sensor[k].found) {if (n++) snt_char(&t, ','); snt_str(&t, c->cfg.type[k]);}
    if (!n) snt_str(&t, "none");
    snt_kd(&t, "ms", (int64_t)(now(c) - c->discovery_started));
    end(c);
}

static void found(SnsCore *c, unsigned kind, const SscSuidEvent *e)
{
    SnsSensor *s = &c->sensor[kind];
    SntText t;
    if (!e->count) {
        if (ordinary(c)) {begin(c, &t, "suid "); snt_str(&t, kNames[kind]); snt_ks(&t, "type", c->cfg.type[kind]); snt_str(&t, " none"); end(c);}
        return;
    }
    if (s->found) {
        s->suids = e->count + e->more;
        if (!ssc_suid_equal(&s->suid, &e->suid[0]) && ordinary(c)) {
            begin(c, &t, "suid ");
            snt_str(&t, kNames[kind]);
            snt_str(&t, " another default ");
            suid_text(&t, &e->suid[0]);
            snt_str(&t, " (kept the first)");
            end(c);
        }
        return;
    }
    s->found = 1;
    s->suid = e->suid[0];
    s->suids = e->count + e->more;
    if (ordinary(c)) {
        begin(c, &t, "suid ");
        snt_str(&t, kNames[kind]);
        snt_ks(&t, "type", e->data_type);
        snt_kd(&t, "n", s->suids);
        snt_char(&t, ' ');
        suid_text(&t, &s->suid);
        end(c);
    }
    if (c->ops->found) c->ops->found(CTX, kind);
}

static void list_line(SnsCore *c, const SscSuidEvent *e)
{
    SntText t;
    if (c->listed >= kListLines || !c->cfg.log) return;
    c->listed++;
    begin(c, &t, "list ");
    snt_str(&t, e->data_type[0] ? e->data_type : "(empty)");
    snt_kd(&t, "n", e->count + e->more);
    for (unsigned i = 0; i < e->count && i < 2; ++i) {snt_char(&t, ' '); suid_text(&t, &e->suid[i]);}
    end(c);
}

static void lookup_event(SnsCore *c, unsigned conn, const SscEvent *e)
{
    SntText t;
    if (e->msg_id == SSC_MSG_SUID_EVENT) {
        SscSuidEvent se;
        if (ssc_parse_suid_event(e->payload, e->length, &se)) {c->malformed++; return;}
        if (conn == SNS_CONN_LIST) {list_line(c, &se); return;}
        int matched = 0;
        for (unsigned k = 0; k < PaganiSensorKinds; ++k)
            if (c->cfg.type[k][0] && same_text(c->cfg.type[k], se.data_type)) {matched = 1; found(c, k, &se);}
        if (!matched) list_line(c, &se);
    } else if (e->msg_id == SSC_MSG_SUID_DONE_EVENT) {
        if (conn == SNS_CONN_LOOKUP && !c->discovery_done) {
            c->discovery_done = 1;
            discovery_line(c, "done");
        }
    } else if (e->msg_id == SSC_MSG_ERROR_EVENT) {
        int64_t error = -1;
        ssc_parse_error_event(e->payload, e->length, &error);
        begin(c, &t, "lookup error event");
        snt_kd(&t, "error", error);
        snt_kd(&t, "conn", conn);
        end(c);
    } else {
        c->unexpected++;
    }
}

static void lookup_answer(SnsCore *c, unsigned tag, int ok, int timeout)
{
    SntText t;
    if (ok) {
        c->lookup_sent |= 1u << tag;
        c->lookup_tries = 0;
        return;
    }
    c->lookup_tries++;
    c->lookup_retry_at = now(c) + backoff(c->lookup_tries);
    if (c->lookup_tries <= 3 || !(c->lookup_tries % 16)) {
        begin(c, &t, "suid request ");
        snt_str(&t, tag < PaganiSensorKinds ? c->cfg.type[tag] : "(every type)");
        snt_str(&t, timeout ? " unanswered" : " refused");
        snt_kd(&t, "tries", c->lookup_tries);
        end(c);
    }
}

static void lookup_step(SnsCore *c, uint64_t t)
{
    uint8_t req[64];
    if (!c->conn[SNS_CONN_LOOKUP].pending && t >= c->lookup_retry_at) {
        for (unsigned k = 0; k < PaganiSensorKinds; ++k) {
            if (!c->cfg.type[k][0] || (c->lookup_sent & (1u << k))) continue;
            const uint32_t n = ssc_build_suid_req(req, sizeof req, c->cfg.type[k], 1, 1);
            if (request(c, SNS_CONN_LOOKUP, &ssc_suid_lookup, SSC_MSG_SUID_REQ, req, n, k)) c->lookup_retry_at = t + kSendRetryMs;
            break;
        }
    }
    if (c->cfg.list && !(c->lookup_sent & kListBit) && !c->conn[SNS_CONN_LIST].pending && t >= c->lookup_retry_at) {
        const uint32_t n = ssc_build_suid_req(req, sizeof req, "", 1, 0);
        if (request(c, SNS_CONN_LIST, &ssc_suid_lookup, SSC_MSG_SUID_REQ, req, n, PaganiSensorKinds)) c->lookup_retry_at = t + kSendRetryMs;
    }
    if (!c->discovery_done && !c->discovery_logged && t - c->discovery_started >= kDiscoveryMs) {
        c->discovery_logged = 1;
        discovery_line(c, "not done after 15 s");
    }
}

/* ---- the sensors ---- */
static uint32_t pick_rate(const SnsSensor *s, uint32_t interval_us)
{
    uint64_t mhz = 1000000000ull / (interval_us ? interval_us : 1);
    if (mhz < 1000) mhz = 1000;
    if (mhz > 500000) mhz = 500000;
    const uint32_t want = sfx_float_from_ratio(mhz, 1000);
    if (s->attr_state != 2 || !s->attrs.rate_count) return want;
    uint32_t best = 0, top = 0;
    for (unsigned i = 0; i < s->attrs.rate_count; ++i) {
        const uint32_t r = s->attrs.rates[i];
        if (!sfx_positive(r)) continue;
        if (r > top) top = r;
        if (r >= want && (!best || r < best)) best = r;
    }
    return best ? best : top ? top : want;
}

static void desired(SnsCore *c, unsigned kind, int *want, uint32_t *rate)
{
    const SnsSensor *s = &c->sensor[kind];
    const int described = s->attr_state == 2;
    uint32_t interval = s->demand_us;
    *want = SNS_OFF;
    *rate = 0;
    switch (kind) {
    case PaganiSensorAccel:
    case PaganiSensorGyro:
    case PaganiSensorMag:
        if (c->cfg.motion_hz) interval = 1000000u / c->cfg.motion_hz ? 1000000u / c->cfg.motion_hz : 1;
        if (kind == PaganiSensorAccel && c->cfg.orient && (!interval || interval > kOrientIntervalUs)) interval = kOrientIntervalUs;
        break;
    case PaganiSensorLight:
        interval = 200000;          /* always on: auto-brightness; 5 Hz if it only streams */
        break;
    case PaganiSensorProximity:
        if (!c->cfg.prox_always && !s->demand_us) return;
        interval = 200000;
        break;
    default:
        break;
    }
    if (!interval) return;
    const int on_change_kind = kind == PaganiSensorLight || kind == PaganiSensorProximity;
    if (described ? s->attrs.stream_type == SSC_STREAM_ON_CHANGE : on_change_kind) {
        *want = SNS_ON_CHANGE;
        return;
    }
    *want = SNS_STREAMING;
    *rate = pick_rate(s, interval);
}

static void stream_line(SnsCore *c, unsigned kind, const char *what)
{
    SnsSensor *s = &c->sensor[kind];
    SntText t;
    if (!ordinary(c)) return;
    begin(c, &t, "stream ");
    snt_str(&t, kNames[kind]);
    snt_char(&t, ' ');
    snt_str(&t, what);
    if (s->active == SNS_STREAMING) {snt_str(&t, " hz="); rate_text(&t, s->active_rate);}
    snt_kd(&t, "demand_us", s->demand_us);
    end(c);
}

static void sensor_answer(SnsCore *c, unsigned kind, uint32_t msg, int ok, int timeout)
{
    SnsSensor *s = &c->sensor[kind];
    SntText t;
    const uint64_t at = now(c);
    if (msg == SSC_MSG_ATTR_REQ) {
        if (ok) return;                              /* the attributes come in an event */
        s->attr_state = 0;
        s->retry_at = at + backoff(s->attr_tries);
        if (s->attr_tries >= kAttrTries) {
            s->attr_state = 3;
            s->retry_at = 0;
            if (ordinary(c)) {begin(c, &t, "attr "); snt_str(&t, kNames[kind]); snt_str(&t, " refused, going on with defaults"); end(c);}
        }
        return;
    }
    if (ok) {
        const int changed = s->active != s->requested || s->active_rate != s->requested_rate;
        s->active = s->requested;
        s->active_rate = s->requested_rate;
        s->tries = 0;
        if (s->active && (changed || !s->active_since)) {
            s->active_since = at;
            s->first_logged = 0;
            s->quiet_logged = 0;
        }
        if (!s->active) s->active_since = 0;
        stream_line(c, kind, s->active == SNS_OFF ? "off" : s->active == SNS_ON_CHANGE ? "on change" : "on");
        return;
    }
    s->tries++;
    if (s->tries >= kStreamTries) {
        s->failed = 1;
        fail_line(c, kind, PaganiSensorsStStreaming, timeout ? PaganiSensorsErrTimeout : PaganiSensorsErrRefused,
                  "stream request refused every time; given up until the service restarts");
        return;
    }
    s->retry_at = at + backoff(s->tries);
    if (s->tries <= 2) {
        begin(c, &t, "stream ");
        snt_str(&t, kNames[kind]);
        snt_str(&t, timeout ? " request unanswered" : " request refused");
        snt_kd(&t, "msg", msg);
        snt_kd(&t, "tries", s->tries);
        end(c);
    }
}

static void sensor_step(SnsCore *c, unsigned kind, uint64_t t)
{
    SnsSensor *s = &c->sensor[kind];
    SnsConn *k = &c->conn[1 + kind];
    SntText line;
    if (!s->found || s->failed || k->pending) return;
    if (s->attr_state == 1) {
        if (t < s->attr_deadline) return;
        s->attr_state = s->attr_tries >= kAttrTries ? 3 : 0;
        if (s->attr_state == 3 && ordinary(c)) {
            begin(c, &line, "attr ");
            snt_str(&line, kNames[kind]);
            snt_str(&line, " no attribute event, going on with defaults");
            end(c);
        }
    }
    if (t < s->retry_at) return;
    if (s->attr_state == 0) {
        if (request(c, 1 + kind, &s->suid, SSC_MSG_ATTR_REQ, 0, 0, 0)) {s->retry_at = t + kSendRetryMs; return;}
        s->attr_tries++;
        s->attr_state = 1;
        s->attr_deadline = t + kAttrMs;
        return;
    }
    if (c->cfg.mode < 3) return;
    desired(c, kind, &s->want, &s->want_rate);
    if (s->want == s->active && (s->want != SNS_STREAMING || s->want_rate == s->active_rate)) return;
    int rc;
    if (s->want == SNS_OFF) {
        rc = request(c, 1 + kind, &s->suid, SSC_MSG_DISABLE_REQ, 0, 0, 0);
    } else if (s->want == SNS_ON_CHANGE) {
        rc = request(c, 1 + kind, &s->suid, SSC_MSG_ON_CHANGE_CONFIG, 0, 0, 0);
    } else {
        uint8_t config[16];
        const uint32_t n = ssc_build_sensor_config(config, sizeof config, s->want_rate);
        rc = request(c, 1 + kind, &s->suid, SSC_MSG_SENSOR_CONFIG, config, n, 0);
    }
    if (rc) {s->retry_at = t + kSendRetryMs; return;}
    s->requested = s->want;
    s->requested_rate = s->want == SNS_STREAMING ? s->want_rate : 0;
}

static void attr_line(SnsCore *c, unsigned kind)
{
    const SscAttrs *a = &c->sensor[kind].attrs;
    SntText t;
    if (!ordinary(c)) return;
    begin(c, &t, "attr ");
    snt_str(&t, kNames[kind]);
    snt_ks(&t, "name", a->name[0] ? a->name : "?");
    snt_ks(&t, "vendor", a->vendor[0] ? a->vendor : "?");
    if (a->type[0]) snt_ks(&t, "type", a->type);
    snt_kd(&t, "avail", a->available);
    snt_kd(&t, "phys", a->physical);
    snt_kd(&t, "stream", a->stream_type);
    if (a->rate_count) {
        snt_str(&t, " hz=");
        for (unsigned i = 0; i < a->rate_count && i < 6; ++i) {if (i) snt_char(&t, ','); rate_text(&t, a->rates[i]);}
    }
    if (a->range_count) {
        snt_str(&t, " range=");
        rate_text(&t, a->ranges[0][0]);
        snt_str(&t, "..");
        rate_text(&t, a->ranges[0][1]);
    }
    if (a->resolution_count) {snt_str(&t, " res="); rate_text(&t, a->resolutions[0]);}
    if (a->rigid_count) snt_kd(&t, "rigid", a->rigid_body[0]);
    if (a->hw_id >= 0) snt_kd(&t, "hw", a->hw_id);
    if (a->version >= 0) snt_kx(&t, "ver", (uint64_t)a->version);
    end(c);
}

static void sample_line(SnsCore *c, const SnsSample *s, const char *what)
{
    SntText t;
    begin(c, &t, what);
    snt_char(&t, ' ');
    snt_str(&t, kNames[s->kind]);
    if (s->kind == PaganiSensorProximity) {
        snt_ks(&t, "state", s->value[0] ? "near" : "far");
        snt_kd(&t, "adc", s->value[1]);
    } else {
        const unsigned n = s->kind <= PaganiSensorMag ? 3 : 1;
        for (unsigned i = 0; i < n; ++i) {snt_char(&t, ' '); snt_milli(&t, s->milli[i]);}
        if (s->kind == PaganiSensorLight) snt_kd(&t, "lux", s->value[0]);
    }
    snt_kd(&t, "acc", s->status);
    snt_kx(&t, "ts", s->timestamp);
    end(c);
}

static void deliver(SnsCore *c, SnsSensor *s, const SnsSample *sample)
{
    s->events++;
    s->sample_ms = now(c);
    s->last = *sample;
    if (c->ops->sample) c->ops->sample(CTX, sample);
    if (!s->first_logged && (c->cfg.log >= 2 || ordinary(c))) {
        s->first_logged = 1;
        sample_line(c, sample, "first");
    }
}

static void data_event(SnsCore *c, unsigned kind, const SscEvent *e)
{
    SnsSensor *s = &c->sensor[kind];
    SscSensorEvent ev;
    SnsSample out;
    zero(&out, sizeof out);
    out.kind = kind;
    out.timestamp = e->timestamp;
    if (ssc_parse_sensor_event(e->payload, e->length, &ev)) {s->bad++; c->malformed++; return;}
    out.status = (int32_t)ev.status;
    const unsigned need = kind <= PaganiSensorMag ? 3 : 1;
    if (ev.count < need) {s->bad++; return;}
    for (unsigned i = 0; i < need; ++i) out.milli[i] = sfx_milli(ev.data[i]);
    if (kind <= PaganiSensorMag) {
        int32_t v[3];
        for (unsigned i = 0; i < 3; ++i) v[i] = kind == PaganiSensorAccel ? sfx_accel_ios(ev.data[i]) : sfx_fixed(ev.data[i]);
        sfx_axes_apply(&c->cfg.axes[kind], v, out.value);
    } else if (kind == PaganiSensorLight) {
        out.value[0] = (int32_t)sfx_lux(ev.data[0], c->cfg.als_permille);
        out.value[1] = ev.count > 1 ? (int32_t)sfx_scale(ev.data[1], 1, 1, 0, -0x7fffffffll, 0x7fffffffll) : 0;
    } else if (kind == PaganiSensorPressure) {
        out.value[0] = sfx_pressure_kpa(ev.data[0]);
    } else {
        s->skipped++;
        return;
    }
    deliver(c, s, &out);
    if (kind == PaganiSensorAccel && c->cfg.orient) {
        const int state = sno_update(&c->orient, out.value[0], out.value[1], out.value[2], now(c));
        if (state >= 0 && c->ops->orientation) c->ops->orientation(CTX, state);
    }
}

static void proximity_event(SnsCore *c, const SscEvent *e)
{
    SnsSensor *s = &c->sensor[PaganiSensorProximity];
    SscProximityEvent ev;
    SnsSample out;
    if (ssc_parse_proximity_event(e->payload, e->length, &ev)) {s->bad++; c->malformed++; return;}
    zero(&out, sizeof out);
    out.kind = PaganiSensorProximity;
    out.timestamp = e->timestamp;
    out.value[0] = ev.type == SSC_PROXIMITY_NEAR;
    out.value[1] = ev.raw_adc > 0x7fffffffu ? 0x7fffffff : (int32_t)ev.raw_adc;
    out.milli[0] = out.value[0] * 1000;
    out.status = (int32_t)ev.status;
    const int changed = s->events && s->last.value[0] != out.value[0];
    deliver(c, s, &out);
    if (changed && c->cfg.log >= 2) sample_line(c, &out, "prox");
}

static void sensor_event(SnsCore *c, unsigned kind, const SscEvent *e)
{
    SnsSensor *s = &c->sensor[kind];
    SntText t;
    switch (e->msg_id) {
    case SSC_MSG_ATTR_EVENT:
        ssc_attrs_clear(&s->attrs);
        if (ssc_parse_attr_event(e->payload, e->length, &s->attrs)) {c->malformed++; s->attr_state = 3; return;}
        s->attr_state = 2;
        s->retry_at = 0;
        attr_line(c, kind);
        if (s->attrs.available == 0) {
            s->failed = 1;
            fail_line(c, kind, PaganiSensorsStDescribed, PaganiSensorsErrOff, "the hub says the sensor is not available");
        }
        return;
    case SSC_MSG_ERROR_EVENT: {
        int64_t error = -1;
        ssc_parse_error_event(e->payload, e->length, &error);
        s->errors++;
        if (s->errors <= 4 || c->cfg.log >= 2) {
            begin(c, &t, "error event ");
            snt_str(&t, kNames[kind]);
            snt_kd(&t, "error", error);
            snt_kd(&t, "n", s->errors);
            end(c);
        }
        return;
    }
    case SSC_MSG_PHYSICAL_CONFIG_EVENT: {
        SscPhysicalConfig pc;
        if (ssc_parse_physical_config(e->payload, e->length, &pc)) {c->malformed++; return;}
        if (ordinary(c)) {
            begin(c, &t, "hw ");
            snt_str(&t, kNames[kind]);
            snt_str(&t, " hz=");
            rate_text(&t, pc.sample_rate);
            if (pc.range_count == 2) {snt_str(&t, " range="); rate_text(&t, pc.range[0]); snt_str(&t, ".."); rate_text(&t, pc.range[1]);}
            if (pc.op_mode[0]) snt_ks(&t, "mode", pc.op_mode);
            snt_kd(&t, "wm", pc.water_mark);
            snt_kd(&t, "dri", pc.dri);
            end(c);
        }
        return;
    }
    case SSC_MSG_SENSOR_EVENT:
        if (kind == PaganiSensorProximity) {s->skipped++; return;}
        data_event(c, kind, e);
        return;
    case SSC_MSG_PROXIMITY_EVENT:
    case SSC_MSG_PROXIMITY_RECURRENT:
        if (kind == PaganiSensorProximity) proximity_event(c, e);
        else s->skipped++;
        return;
    default:
        s->skipped++;   /* flush, resampler and calibration events, the hub's own extras */
        return;
    }
}

/* ---- QMI ---- */
static void response(SnsCore *c, unsigned conn, const SscQmi *m)
{
    SnsConn *k = &c->conn[conn];
    SscResponse r;
    if (!k->pending || m->transaction != k->pending_txn) {c->unexpected++; return;}
    k->pending = 0;
    k->responses++;
    c->responses++;
    const int parsed = !ssc_qmi_response(m, &r);
    if (parsed && r.has_client) {k->has_client = 1; k->client_id = r.client_id;}
    const int ok = parsed && ssc_response_ok(&r);
    if (!ok) {k->refused++; c->refused++;}
    if (conn == SNS_CONN_LOOKUP || conn == SNS_CONN_LIST) lookup_answer(c, k->pending_tag, ok, 0);
    else sensor_answer(c, conn - 1, k->pending_msg, ok, 0);
}

static void report(SnsCore *c, unsigned conn, const SscQmi *m)
{
    SnsConn *k = &c->conn[conn];
    int has_client = 0, r;
    uint64_t client = 0;
    const uint8_t *message = 0;
    uint32_t length = 0;
    SscEvents events;
    SscEvent e;
    if (ssc_qmi_report(m, &has_client, &client, &message, &length) || ssc_events_open(&events, message, length)) {c->malformed++; return;}
    k->reports++;
    c->reports++;
    if (has_client && k->has_client && client != k->client_id) c->unexpected++;
    if (conn == SNS_CONN_LOOKUP || conn == SNS_CONN_LIST) {
        if (events.has_suid && !ssc_suid_equal(&events.suid, &ssc_suid_lookup)) {c->unexpected++; return;}
        while ((r = ssc_events_next(&events, &e)) == 1) lookup_event(c, conn, &e);
        if (r < 0) c->malformed++;
        return;
    }
    const unsigned kind = conn - 1;
    SnsSensor *s = &c->sensor[kind];
    if (!s->found || (events.has_suid && !ssc_suid_equal(&events.suid, &s->suid))) {c->unexpected++; return;}
    while ((r = ssc_events_next(&events, &e)) == 1) sensor_event(c, kind, &e);
    if (r < 0) c->malformed++;
}

void sns_core_receive(SnsCore *c, unsigned conn, uint32_t node, uint32_t port, const uint8_t *data, uint32_t length)
{
    SscQmi m;
    if (conn >= SNS_CONNS) return;
    if (!c->up || node != c->node || port != c->port) {c->stale++; return;}
    if (ssc_qmi_parse(data, length, &m)) {c->malformed++; return;}
    if (m.type == SSC_QMI_RESPONSE && m.message == SSC_QMI_REQ) response(c, conn, &m);
    else if (m.type == SSC_QMI_INDICATION && (m.message == SSC_QMI_REPORT || m.message == SSC_QMI_JUMBO_REPORT)) report(c, conn, &m);
    else c->unexpected++;
    step(c);
}

void sns_core_demand(SnsCore *c, unsigned kind, uint32_t interval_us)
{
    if (kind >= PaganiSensorKinds) return;
    SnsSensor *s = &c->sensor[kind];
    if (s->demand_us == interval_us) {
        if (kind == PaganiSensorProximity && interval_us && s->active && s->events && c->ops->sample) c->ops->sample(CTX, &s->last);
        return;
    }
    s->demand_us = interval_us;
    step(c);
}

/* ---- time ---- */
static void timeouts(SnsCore *c, uint64_t t)
{
    for (unsigned i = 0; i < SNS_CONNS; ++i) {
        SnsConn *k = &c->conn[i];
        if (!k->pending || t - k->sent_ms < kResponseMs) continue;
        k->pending = 0;
        k->timeouts++;
        if (i == SNS_CONN_LOOKUP || i == SNS_CONN_LIST) lookup_answer(c, k->pending_tag, 0, 1);
        else sensor_answer(c, i - 1, k->pending_msg, 0, 1);
    }
}

static void quiet(SnsCore *c, uint64_t t)
{
    SntText line;
    for (unsigned kind = 0; kind < PaganiSensorKinds; ++kind) {
        SnsSensor *s = &c->sensor[kind];
        if (!s->active || !s->active_since || s->quiet_logged || t - s->active_since < kQuietMs) continue;
        s->quiet_logged = 1;
        if (s->sample_ms >= s->active_since) continue;   /* samples came */
        begin(c, &line, "stream ");
        snt_str(&line, kNames[kind]);
        snt_str(&line, " accepted but no sample in 10 s");
        snt_kd(&line, "errors", s->errors);
        snt_kd(&line, "skipped", s->skipped);
        end(c);
    }
}

static void summary(SnsCore *c)
{
    SntText t;
    begin(c, &t, "summary");
    snt_kd(&t, "up", c->up);
    for (unsigned k = 0; k < PaganiSensorKinds; ++k) snt_kd(&t, kNames[k], c->sensor[k].events);
    snt_kd(&t, "reports", c->reports);
    snt_kd(&t, "refused", c->refused);
    snt_kd(&t, "malformed", c->malformed);
    snt_kd(&t, "stale", c->stale);
    snt_kd(&t, "unexpected", c->unexpected);
    end(c);
}

static void step(SnsCore *c)
{
    const uint64_t t = now(c);
    if (t >= c->summary_at) {
        c->summary_at = t + kSummaryMs;
        if (c->reports || c->up) summary(c);
    }
    if (!c->up) {
        if (t >= c->reminder_at) {
            SntText line;
            static const uint32_t next[] = {50000, 240000, 1500000};
            begin(c, &line, "waiting for the sensor client service (400)");
            snt_kd(&line, "s", (int64_t)((t - c->wait_since) / 1000));
            end(c);
            c->reminder_at = t + (c->reminders < 3 ? next[c->reminders] : 1800000u);
            c->reminders++;
        }
        return;
    }
    timeouts(c, t);
    lookup_step(c, t);
    for (unsigned kind = 0; kind < PaganiSensorKinds; ++kind) sensor_step(c, kind, t);
    quiet(c, t);
}

void sns_core_tick(SnsCore *c)
{
    step(c);
}

uint32_t sns_core_idle_ms(SnsCore *c)
{
    const uint64_t t = now(c);
    uint64_t next = t + kIdleMaxMs;
#define SOONER(at) do {if ((at) && (at) < next) next = (at);} while (0)
    SOONER(c->summary_at);
    if (!c->up) {
        SOONER(c->reminder_at);
    } else {
        for (unsigned i = 0; i < SNS_CONNS; ++i) if (c->conn[i].pending) SOONER(c->conn[i].sent_ms + kResponseMs);
        if (c->lookup_retry_at > t) SOONER(c->lookup_retry_at);
        if (!c->discovery_done && !c->discovery_logged) SOONER(c->discovery_started + kDiscoveryMs);
        for (unsigned k = 0; k < PaganiSensorKinds; ++k) {
            const SnsSensor *s = &c->sensor[k];
            if (!s->found || s->failed) continue;
            if (s->attr_state == 1) SOONER(s->attr_deadline);
            if (s->retry_at > t) SOONER(s->retry_at);
            if (s->active && s->active_since && !s->quiet_logged) SOONER(s->active_since + kQuietMs);
        }
    }
#undef SOONER
    if (next <= t) return 1;
    return (uint32_t)(next - t);
}

void sns_core_status(const SnsCore *c, PaganiSensorsStatus *out)
{
    int32_t stage = c->stage;
    if (stage >= PaganiSensorsStPorts) {
        stage = c->up ? PaganiSensorsStService : PaganiSensorsStPorts;
        for (unsigned k = 0; k < PaganiSensorKinds; ++k) {
            const SnsSensor *s = &c->sensor[k];
            if (s->found && stage < PaganiSensorsStDiscovered) stage = PaganiSensorsStDiscovered;
            if (s->attr_state == 2 && stage < PaganiSensorsStDescribed) stage = PaganiSensorsStDescribed;
            if (s->active && stage < PaganiSensorsStStreaming) stage = PaganiSensorsStStreaming;
            if (s->active && s->events && stage < PaganiSensorsStData) stage = PaganiSensorsStData;
        }
    }
    out->stage = stage;
    out->error = c->error;
    out->mode = c->cfg.mode;
    out->node = c->up ? c->node : 0;
    out->port = c->up ? c->port : 0;
    out->instance = c->up ? c->instance : 0;
    out->found = out->described = out->streaming = 0;
    for (unsigned k = 0; k < PaganiSensorKinds; ++k) {
        const SnsSensor *s = &c->sensor[k];
        if (s->found) out->found |= 1u << k;
        if (s->attr_state == 2) out->described |= 1u << k;
        if (s->active) out->streaming |= 1u << k;
        out->events[k] = s->events;
    }
    out->requests = c->sent;
    out->responses = c->responses;
    out->refused = c->refused;
    out->reports = c->reports;
    out->malformed = c->malformed;
}
