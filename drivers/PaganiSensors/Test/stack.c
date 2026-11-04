// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// The whole host-side stack: the sensor manager (SensorsCore.c) on PaganiAdsp's real IPC router client ports (AdspQrtr.c, QcomQrtr.c,
// the real AdspCore channel queue), driven the way PaganiSensors.cpp drives it (one port per connection, every message copied into a
// ring by the router's callback and handed to the manager later, the server read as a level with its generation on every edge), against
// a DSP side made of its own QcomQrtr router (node 5) hosting the sensor client service on port 0x1a with a small hub: client ids per
// port, SUID events, attributes, a stream. Checked: the bytes cross both routers intact (QRTR framing, padding, routing by port), the
// service announced late, the manager's requests from eight distinct ports, samples arriving, DEL_SERVER and BYE as the service restarts
// and the manager starting over, a link that goes down and up, and the queue limits (the IPCRTR channel's 16 messages).
#include "check.h"
#include "../SensorsCore.h"
#include "../SensorsPb.h"
#include "../../PaganiAdsp/AdspCore.h"
#include "../../PaganiAdsp/QcomQrtr.h"

/* ---- PaganiAdsp's platform: just what AdspQrtr and the channel registry use ---- */
static uint64_t now_ms;
static int lock_depth;
static size_t allocated;
static char adsp_log[1 << 16];
static size_t adsp_used;
static uint64_t op_now(void *c) {(void)c; return now_ms;}
static void op_lock(void *c) {(void)c; CHECK(lock_depth == 0); lock_depth++;}
static void op_unlock(void *c) {(void)c; CHECK(lock_depth == 1); lock_depth--;}
static void op_wake(void *c) {(void)c;}
static void *op_alloc(void *c, size_t n) {(void)c; allocated += n; return calloc(1, n);}
static void op_free(void *c, void *p, size_t n) {(void)c; allocated -= n; free(p);}
static void adsp_trace(void *c, const char *line)
{
    (void)c;
    const size_t n = strlen(line);
    if (adsp_used + n + 2 < sizeof adsp_log) {memcpy(adsp_log + adsp_used, line, n); adsp_used += n; adsp_log[adsp_used++] = '\n'; adsp_log[adsp_used] = 0;}
}
static const PaganiAdspOps adsp_ops = {.now_ms = op_now, .wake = op_wake, .trace = adsp_trace, .lock = op_lock, .unlock = op_unlock,
                                       .alloc = op_alloc, .free = op_free};
static PaganiAdspCore adsp;
static PaganiAdspChannel *ipcrtr;

/* ---- the DSP side: its router (node 5) and the hub on port 0x1a ---- */
static QcomQrtr dsp;
static uint8_t to_ap[64][600];
static uint32_t to_ap_len[64];
static unsigned to_ap_count;
static int dsp_send(void *c, const uint8_t *packet, uint32_t length)
{
    (void)c;
    CHECK(to_ap_count < 64 && length <= sizeof to_ap[0]);
    memcpy(to_ap[to_ap_count], packet, length);
    to_ap_len[to_ap_count++] = length;
    return 0;
}
static struct {
    uint32_t clients[16][2];      /* node:port of each client connection seen */
    unsigned client_count;
    unsigned requests, streams;
    uint32_t stream_node, stream_port;
    SscSuid stream_suid;
    uint64_t next_sample;
} hub;
static const SscSuid accel_suid = {0x1234, 0x5678};
static uint32_t hub_port = 0x1a;

static uint64_t client_id(uint32_t node, uint32_t port)
{
    for (unsigned i = 0; i < hub.client_count; ++i) if (hub.clients[i][0] == node && hub.clients[i][1] == port) return 0x900 + i;
    CHECK(hub.client_count < 16);
    hub.clients[hub.client_count][0] = node;
    hub.clients[hub.client_count][1] = port;
    return 0x900 + hub.client_count++;
}
static void put16(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}
static void put64(uint8_t *p, uint64_t v) {for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i));}
static void hub_report(uint32_t node, uint32_t port, const SscSuid *suid, uint32_t msg_id, const uint8_t *payload, uint32_t plen)
{
    uint8_t msg[400], ind[480];
    SpbOut o;
    spb_out(&o, msg, sizeof msg);
    uint32_t mark = spb_open(&o, 1);
    spb_put_fixed64(&o, 1, suid->low);
    spb_put_fixed64(&o, 2, suid->high);
    spb_close(&o, mark);
    mark = spb_open(&o, 2);
    spb_put_fixed32(&o, 1, msg_id);
    spb_put_fixed64(&o, 2, now_ms * 19200ull);
    if (plen) spb_put_bytes(&o, 3, payload, plen);
    spb_close(&o, mark);
    const uint32_t n = spb_done(&o);
    ind[0] = 4; put16(ind + 1, 0); put16(ind + 3, 0x21); put16(ind + 5, 11 + 5 + n);
    ind[7] = 1; put16(ind + 8, 8); put64(ind + 10, client_id(node, port));
    ind[18] = 2; put16(ind + 19, n + 2); put16(ind + 21, n);
    memcpy(ind + 23, msg, n);
    CHECK(qcom_qrtr_send_data(&dsp, hub_port, node, port, ind, 23 + n) == 0);
}
static void hub_receive(void *c, QcomQrtr *q, uint32_t port, uint32_t node, uint32_t from, const uint8_t *data, uint32_t length)
{
    (void)c; (void)q;
    CHECK(port == hub_port && node == 1 && from >= 0x4000 && from <= 0x7fff);
    SscQmi m;
    uint32_t size = 0;
    CHECK(ssc_qmi_parse(data, length, &m) == 0 && m.type == 0 && m.message == 0x20);
    const uint8_t *tlv = ssc_qmi_tlv(&m, 1, &size);
    CHECK(tlv && size > 2);
    hub.requests++;
    /* the response */
    uint8_t r[32];
    r[0] = 2; put16(r + 1, m.transaction); put16(r + 3, 0x20); put16(r + 5, 7 + 11);
    r[7] = 2; put16(r + 8, 4); put16(r + 10, 0); put16(r + 12, 0);
    r[14] = 0x10; put16(r + 15, 8); put64(r + 17, client_id(node, from));
    CHECK(qcom_qrtr_send_data(&dsp, hub_port, node, from, r, 25) == 0);
    /* what it asked */
    SpbIn in;
    uint32_t field, wire, msg_id = 0;
    SscSuid suid = {0, 0};
    const uint8_t *payload = 0;
    uint32_t plen = 0;
    spb_in(&in, tlv + 2, size - 2);
    while (spb_next(&in, &field, &wire) == 1) {
        const uint8_t *p;
        uint32_t n;
        if (field == 2) {CHECK(spb_fixed32_value(&in, wire, &msg_id) == 0); continue;}
        CHECK(spb_bytes_value(&in, wire, &p, &n) == 0);
        if (field == 1) {
            SpbIn s;
            spb_in(&s, p, n);
            while (spb_next(&s, &field, &wire) == 1) CHECK(spb_fixed64_value(&s, wire, field == 1 ? &suid.low : &suid.high) == 0);
        } else if (field == 4) {
            SpbIn s;
            spb_in(&s, p, n);
            while (spb_next(&s, &field, &wire) == 1) CHECK(spb_bytes_value(&s, wire, &payload, &plen) == 0);
        }
    }
    if (ssc_suid_equal(&suid, &ssc_suid_lookup)) {
        SpbIn s;
        const uint8_t *type = 0;
        uint32_t tn = 0;
        spb_in(&s, payload, plen);
        CHECK(spb_next(&s, &field, &wire) == 1 && field == 1 && spb_bytes_value(&s, wire, &type, &tn) == 0);
        if (tn == 5 && !memcmp(type, "accel", 5)) {
            uint8_t e[64];
            SpbOut o;
            spb_out(&o, e, sizeof e);
            spb_put_string(&o, 1, "accel");
            const uint32_t mark = spb_open(&o, 2);
            spb_put_fixed64(&o, 1, accel_suid.low);
            spb_put_fixed64(&o, 2, accel_suid.high);
            spb_close(&o, mark);
            hub_report(node, from, &ssc_suid_lookup, SSC_MSG_SUID_EVENT, e, spb_done(&o));
            hub_report(node, from, &ssc_suid_lookup, SSC_MSG_SUID_DONE_EVENT, 0, 0);
        }
        return;
    }
    CHECK(ssc_suid_equal(&suid, &accel_suid));
    if (msg_id == SSC_MSG_ATTR_REQ) {
        uint8_t a[64];
        SpbOut o;
        spb_out(&o, a, sizeof a);
        const uint32_t attr = spb_open(&o, 1);
        spb_put_int(&o, 1, SSC_ATTR_STREAM_TYPE);
        const uint32_t value = spb_open(&o, 2);
        const uint32_t datum = spb_open(&o, 1);
        spb_put_fixed64(&o, 4, 0);
        spb_close(&o, datum);
        spb_close(&o, value);
        spb_close(&o, attr);
        hub_report(node, from, &accel_suid, SSC_MSG_ATTR_EVENT, a, spb_done(&o));
    } else if (msg_id == SSC_MSG_SENSOR_CONFIG) {
        hub.streams++;
        hub.stream_node = node;
        hub.stream_port = from;
        hub.stream_suid = suid;
        hub.next_sample = now_ms;
    } else if (msg_id == SSC_MSG_DISABLE_REQ) {
        hub.stream_port = 0;
    }
}

/* the AP side's queued packets go to the DSP router; the DSP router's packets to the AP's channel */
static unsigned crossed;
static void pump(void)
{
    for (int any = 1; any;) {
        any = 0;
        while (ipcrtr->count) {
            uint8_t *slot = ipcrtr->queue + (size_t)ipcrtr->head * (4u + ipcrtr->max_message);
            uint32_t length;
            uint8_t packet[600];
            memcpy(&length, slot, 4);
            CHECK(length <= 512);
            memcpy(packet, slot + 4, length);
            ipcrtr->head = (ipcrtr->head + 1) % ipcrtr->depth;
            ipcrtr->count--;
            CHECK(qcom_qrtr_receive(&dsp, packet, length) == 0);
            crossed++;
            any = 1;
        }
        for (unsigned i = 0; i < to_ap_count; ++i) {
            ipcrtr->client.receive(ipcrtr->context, to_ap[i], to_ap_len[i]);
            crossed++;
            any = 1;
        }
        to_ap_count = 0;
    }
}

/* ---- PaganiSensors.cpp's way: ports, a ring, the level on every edge ---- */
static PaganiAdspQrtrPort *ports[SNS_CONNS];
static struct {uint32_t conn, node, port, length; uint8_t data[1088];} ring[48];
static uint32_t head, tail, dropped, edges;
static void on_server(void *c, int up, uint32_t node, uint32_t port, uint32_t instance)
{
    (void)c; (void)up; (void)node; (void)port; (void)instance;
    CHECK(lock_depth == 0);
    edges++;
}
static void on_receive(void *c, uint32_t node, uint32_t port, const uint8_t *data, uint32_t length)
{
    CHECK(lock_depth == 0);
    if (head - tail >= 48 || length > sizeof ring[0].data) {dropped++; return;}
    ring[head % 48].conn = (uint32_t)(uintptr_t)c;
    ring[head % 48].node = node;
    ring[head % 48].port = port;
    ring[head % 48].length = length;
    memcpy(ring[head % 48].data, data, length);
    head++;
}
static int sns_send(void *c, unsigned conn, uint32_t node, uint32_t port, const uint8_t *data, uint32_t length)
{
    (void)c;
    return pagani_adsp_qrtr_send(ports[conn], node, port, data, length) == PaganiAdspOk ? 0 : -1;
}
static unsigned samples;
static SnsSample last;
static void sns_sample(void *c, const SnsSample *s) {(void)c; samples++; last = *s;}
static char sns_log[1 << 16];
static size_t sns_used;
static void sns_trace(void *c, const char *line)
{
    (void)c;
    const size_t n = strlen(line);
    if (sns_used + n + 2 < sizeof sns_log) {memcpy(sns_log + sns_used, line, n); sns_used += n; sns_log[sns_used++] = '\n'; sns_log[sns_used] = 0;}
}
static const SnsOps sns_ops = {0, op_now, sns_send, sns_trace, sns_sample, 0, 0};
static SnsCore core;
static uint32_t last_edges = 0xffffffffu;

static void thread_round(void)
{
    if (edges != last_edges) {
        last_edges = edges;
        PaganiAdspQrtrServer s;
        const int up = pagani_adsp_qrtr_server(ports[0], &s);
        sns_core_server(&core, up, s.node, s.port, s.instance, s.generation);
    }
    while (tail != head) {
        sns_core_receive(&core, ring[tail % 48].conn, ring[tail % 48].node, ring[tail % 48].port, ring[tail % 48].data,
                         ring[tail % 48].length);
        tail++;
    }
    sns_core_tick(&core);
}
static void run(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; ++i) {
        if (hub.stream_port && now_ms >= hub.next_sample) {
            hub.next_sample = now_ms + 20;
            uint8_t p[32];
            SpbOut o;
            spb_out(&o, p, sizeof p);
            spb_put_fixed32(&o, 1, float_bits(0.0f));
            spb_put_fixed32(&o, 1, float_bits(-9.80665f));
            spb_put_fixed32(&o, 1, float_bits(0.0f));
            spb_put_varint(&o, 2, 3);
            hub_report(hub.stream_node, hub.stream_port, &hub.stream_suid, SSC_MSG_SENSOR_EVENT, p, spb_done(&o));
        }
        pump();
        thread_round();
        pump();
        now_ms++;
    }
}
static void dsp_control(uint32_t command, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    uint8_t pkt[20], p[64];
    for (unsigned i = 0; i < 4; ++i) pkt[i] = (uint8_t)(command >> (8 * i));
    const uint32_t w[4] = {a, b, c, d};
    for (unsigned k = 0; k < 4; ++k) for (unsigned i = 0; i < 4; ++i) pkt[4 + 4 * k + i] = (uint8_t)(w[k] >> (8 * i));
    const uint32_t n = qcom_qrtr_frame(p, sizeof p, command, 5, QCOM_QRTR_PORT_CTRL, 1, QCOM_QRTR_PORT_CTRL, pkt, 20);
    ipcrtr->client.receive(ipcrtr->context, p, n);
}

int main(void)
{
    const PaganiAdspConfig config = {0};
    pagani_adsp_core_init(&adsp, &adsp_ops, &config, 0, 0);
    ipcrtr = adsp.qrtr.channel;
    qcom_qrtr_init(&dsp, 5, dsp_send, 0);

    /* PaganiSensors at boot: the ports first (before the DSP is up), the manager waiting */
    SnsConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.mode = 3;
    cfg.log = 2;
    for (unsigned i = 0; i < 3; ++i) sfx_axes_identity(&cfg.axes[i]);
    strcpy(cfg.type[PaganiSensorAccel], "accel");
    static const PaganiAdspQrtrClient client = {on_server, on_receive};
    for (unsigned conn = 0; conn < SNS_CONNS; ++conn) {
        ports[conn] = pagani_adsp_qrtr_open(&(PaganiAdspQrtrConfig){SSC_QMI_SERVICE, PAGANI_ADSP_QRTR_ANY, 0, 0}, &client,
                                            (void *)(uintptr_t)conn);
        CHECK(ports[conn] && pagani_adsp_qrtr_port_number(ports[conn]) == 0x4000 + conn);
    }
    sns_core_init(&core, &sns_ops, &cfg);
    run(1000);
    CHECK(!core.up && hub.requests == 0);

    /* the link and both hellos; the sensor PD's service only later */
    ipcrtr->client.state(ipcrtr->context, 1);
    pump();
    qcom_qrtr_start(&dsp);
    pump();
    CHECK(dsp.hello_received && adsp.qrtr.router.hello_received && qcom_qrtr_find_remote(&dsp, 0x40) >= 0);
    run(2000);
    CHECK(!core.up);
    CHECK(qcom_qrtr_add_service(&dsp, SSC_QMI_SERVICE, 0x1, 0x1a, hub_receive, 0) == 0);   /* announced at once: NEW_SERVER */
    pump();
    CHECK(edges >= SNS_CONNS);
    sns_core_demand(&core, PaganiSensorAccel, 20000);
    run(3000);
    CHECK(core.up && core.node == 5 && core.port == 0x1a && core.sensor[PaganiSensorAccel].found);
    CHECK(hub.streams == 1 && samples > 100 && last.value[2] == 0 && last.value[1] == 65536);   /* -9.81 on y: +1 g on iOS */
    CHECK(hub.client_count == 2);   /* the lookup connection's port and the accelerometer's: distinct client ids */
    CHECK(core.conn[0].client_id == 0x900 && core.conn[1].client_id == 0x901);
    CHECK(strstr(sns_log, "stream accel on hz=50.000") && !dropped);

    /* the sensor PD restarts: DEL_SERVER, then the service on a new port; the manager starts over through the same ports */
    dsp_control(QCOM_QRTR_DEL_SERVER, SSC_QMI_SERVICE, 0x1, 5, 0x1a);
    hub.stream_port = 0;
    run(500);
    CHECK(!core.up && strstr(sns_log, "service gone"));
    memset(&hub, 0, sizeof hub);
    hub_port = 0x2b;           /* the restarted PD's port (the DSP router's table edited in place) */
    CHECK(dsp.local_count == 1 && dsp.local[0].service == SSC_QMI_SERVICE);
    dsp.local[0].port = 0x2b;
    dsp_control(QCOM_QRTR_NEW_SERVER, SSC_QMI_SERVICE, 0x1, 5, 0x2b);
    run(10);
    CHECK(core.up && core.port == 0x2b);
    /* the router on our side routes to its port 0x2b, our router's copy says so; the DSP's own router delivers to 0x2b */
    run(3000);
    CHECK(hub.streams == 1 && hub.requests == 3);   /* SUID lookup, attributes, stream: only the accelerometer is asked for */

    /* BYE: the whole DSP went; then its hello and service again */
    dsp_control(QCOM_QRTR_BYE, 0, 0, 0, 0);
    hub.stream_port = 0;
    run(100);
    CHECK(!core.up);
    memset(&hub, 0, sizeof hub);
    dsp_control(QCOM_QRTR_HELLO, 0, 0, 0, 0);
    dsp_control(QCOM_QRTR_NEW_SERVER, SSC_QMI_SERVICE, 0x1, 5, 0x2b);
    const unsigned before = samples;
    run(3000);
    CHECK(core.up && hub.streams == 1 && samples > before + 100);

    /* the link down and up: everything again */
    ipcrtr->client.state(ipcrtr->context, 0);
    hub.stream_port = 0;
    run(100);
    CHECK(!core.up);
    ipcrtr->client.state(ipcrtr->context, 1);
    pump();
    memset(&hub, 0, sizeof hub);
    dsp_control(QCOM_QRTR_HELLO, 0, 0, 0, 0);
    dsp_control(QCOM_QRTR_NEW_SERVER, SSC_QMI_SERVICE, 0x1, 5, 0x2b);
    run(3000);
    CHECK(core.up && hub.streams == 1);
    CHECK(lock_depth == 0 && core.malformed == 0 && core.stale == 0);
    for (uint32_t i = 0; i < adsp.channel_count; ++i) {
        op_free(0, adsp.channel[i].storage, (size_t)adsp.channel[i].intent_size * adsp.channel[i].intent_count);
        op_free(0, adsp.channel[i].queue, (size_t)adsp.channel[i].depth * (4u + adsp.channel[i].max_message));
    }
    CHECK(allocated == 0);
    printf("PASS stack: the manager on PaganiAdsp's router client ports against a DSP router and hub, %u packets crossed, %u samples: "
           "%u checks\n", crossed, samples, checks);
    return 0;
}
