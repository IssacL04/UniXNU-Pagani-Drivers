// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Host tests of the IPC router client ports (PaganiAdsp.h "IPC router clients", AdspQrtr.c) and the router hooks under them (QcomQrtr.c:
// qcom_qrtr_watch, qcom_qrtr_forget_remote, qcom_qrtr_find_server, the pure frame builders, the locator's crash report answer). The real
// AdspCore registers the IPCRTR channel; no DSP is booted: the channel's callbacks are driven by hand (link up/down, packets from the DSP
// at node 5) and what our side queues is taken off the channel's queue and checked byte by byte. The registry lock must never nest and
// must never be held across a callback: the clients' callbacks send from inside (the sensor client answers indications at once).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../AdspCore.h"
#include "../QcomQrtr.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

static uint32_t rd32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static uint32_t rd16(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8;}
static void wr32(uint8_t *p, uint32_t v) {for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));}

/* ---- the platform: a log, the lock depth, memory ---- */
static char logbuf[1 << 16];
static size_t logused;
static int lock_depth, in_callback;
static unsigned wakes;
static size_t allocated;
static void op_trace(void *c, const char *line)
{
    (void)c;
    const size_t n = strlen(line);
    if (logused + n + 2 < sizeof logbuf) {memcpy(logbuf + logused, line, n); logused += n; logbuf[logused++] = '\n'; logbuf[logused] = 0;}
}
static int logged(const char *text) {return strstr(logbuf, text) != 0;}
static void op_lock(void *c) {(void)c; CHECK(lock_depth == 0 && !in_callback); lock_depth++;}
static void op_unlock(void *c) {(void)c; CHECK(lock_depth == 1); lock_depth--;}
static void op_wake(void *c) {(void)c; wakes++;}
static void *op_alloc(void *c, size_t n) {(void)c; allocated += n; return calloc(1, n);}
static void op_free(void *c, void *p, size_t n) {(void)c; CHECK(allocated >= n); allocated -= n; free(p);}
static uint64_t op_now(void *c) {(void)c; return 0;}
static const PaganiAdspOps ops = {.now_ms = op_now, .wake = op_wake, .trace = op_trace, .lock = op_lock, .unlock = op_unlock,
                                  .alloc = op_alloc, .free = op_free};

static PaganiAdspCore core;
static PaganiAdspChannel *ipcrtr;

/* ---- what our side queued on IPCRTR ---- */
typedef struct {uint8_t bytes[512]; uint32_t length;} Packet;
static unsigned drain(Packet *out, unsigned max)
{
    unsigned n = 0;
    while (ipcrtr->count) {
        uint8_t *slot = ipcrtr->queue + (size_t)ipcrtr->head * (4u + ipcrtr->max_message);
        uint32_t length;
        memcpy(&length, slot, 4);
        CHECK(length <= sizeof out[0].bytes && n < max);
        memcpy(out[n].bytes, slot + 4, length);
        out[n].length = length;
        n++;
        ipcrtr->head = (ipcrtr->head + 1) % ipcrtr->depth;
        ipcrtr->count--;
    }
    return n;
}
static void check_header(const Packet *p, uint32_t type, uint32_t src_node, uint32_t src_port, uint32_t dst_node, uint32_t dst_port,
                         uint32_t size)
{
    CHECK(p->length == 32 + ((size + 3) & ~3u));
    CHECK(rd32(p->bytes) == 1 && rd32(p->bytes + 4) == type && rd32(p->bytes + 8) == src_node && rd32(p->bytes + 12) == src_port);
    CHECK(rd32(p->bytes + 16) == 0 && rd32(p->bytes + 20) == size && rd32(p->bytes + 24) == dst_node && rd32(p->bytes + 28) == dst_port);
}
static void check_control(const Packet *p, uint32_t command, uint32_t src_port, uint32_t dst_node, uint32_t a, uint32_t b, uint32_t c,
                          uint32_t d)
{
    check_header(p, command, 1, src_port, dst_node, QCOM_QRTR_PORT_CTRL, 20);
    CHECK(rd32(p->bytes + 32) == command && rd32(p->bytes + 36) == a && rd32(p->bytes + 40) == b && rd32(p->bytes + 44) == c &&
          rd32(p->bytes + 48) == d);
}

/* ---- packets from the DSP (node 5) ---- */
static uint32_t make(uint8_t *p, uint32_t type, uint32_t src_node, uint32_t src_port, uint32_t confirm, uint32_t dst_node, uint32_t dst_port,
                     const uint8_t *data, uint32_t size)
{
    wr32(p, 1); wr32(p + 4, type); wr32(p + 8, src_node); wr32(p + 12, src_port); wr32(p + 16, confirm); wr32(p + 20, size);
    wr32(p + 24, dst_node); wr32(p + 28, dst_port);
    if (size) memcpy(p + 32, data, size);
    const uint32_t padded = (size + 3) & ~3u;
    memset(p + 32 + size, 0, padded - size);
    return 32 + padded;
}
static void dsp_control(uint32_t command, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    uint8_t pkt[20], p[64];
    wr32(pkt, command); wr32(pkt + 4, a); wr32(pkt + 8, b); wr32(pkt + 12, c); wr32(pkt + 16, d);
    const uint32_t n = make(p, command, 5, QCOM_QRTR_PORT_CTRL, 0, 1, QCOM_QRTR_PORT_CTRL, pkt, sizeof pkt);
    ipcrtr->client.receive(ipcrtr->context, p, n);
}
static void dsp_data(uint32_t from, uint32_t to, uint32_t confirm, const uint8_t *data, uint32_t size)
{
    uint8_t p[600];
    const uint32_t n = make(p, QCOM_QRTR_DATA, 5, from, confirm, 1, to, data, size);
    ipcrtr->client.receive(ipcrtr->context, p, n);
}
static void link(int up) {ipcrtr->client.state(ipcrtr->context, up);}

/* ---- the clients ---- */
typedef struct {
    const char *name;
    unsigned ups, downs, messages;
    uint32_t node, port, instance, from_node, from_port, length;
    uint8_t last[512];
    PaganiAdspQrtrPort *self;
    int echo;                 /* answer each message from inside the callback */
} Client;
static void on_server(void *context, int up, uint32_t node, uint32_t port, uint32_t instance)
{
    Client *c = context;
    CHECK(lock_depth == 0);
    in_callback = 1;
    if (up) {c->ups++; c->node = node; c->port = port; c->instance = instance;}
    else {c->downs++; CHECK(c->node == node && c->port == port);}
    in_callback = 0;
}
static void on_receive(void *context, uint32_t node, uint32_t port, const uint8_t *data, uint32_t length)
{
    Client *c = context;
    CHECK(lock_depth == 0 && length <= sizeof c->last);
    c->messages++;
    c->from_node = node;
    c->from_port = port;
    c->length = length;
    memcpy(c->last, data, length);
    if (c->echo) CHECK(pagani_adsp_qrtr_send(c->self, node, port, data, length) == PaganiAdspOk);   /* sends from the callback */
}
static const PaganiAdspQrtrClient client_ops = {on_server, on_receive};

static void frames(void)
{
    uint8_t out[64];
    const uint8_t data[5] = {1, 2, 3, 4, 5};
    CHECK(qcom_qrtr_frame(out, sizeof out, QCOM_QRTR_DATA, 1, 0x4000, 5, 0x1a, data, 5) == 40);
    CHECK(rd32(out) == 1 && rd32(out + 4) == 1 && rd32(out + 8) == 1 && rd32(out + 12) == 0x4000 && rd32(out + 16) == 0);
    CHECK(rd32(out + 20) == 5 && rd32(out + 24) == 5 && rd32(out + 28) == 0x1a && !memcmp(out + 32, data, 5) && !out[37] && !out[39]);
    CHECK(qcom_qrtr_frame(out, 40, QCOM_QRTR_DATA, 1, 2, 3, 4, data, 5) == 40);       /* exactly fits, padding included */
    CHECK(qcom_qrtr_frame(out, 39, QCOM_QRTR_DATA, 1, 2, 3, 4, data, 5) == 0);        /* the padding does not fit */
    CHECK(qcom_qrtr_frame(out, 31, QCOM_QRTR_DATA, 1, 2, 3, 4, data, 0) == 0);        /* not even the header */
    CHECK(qcom_qrtr_frame(out, 32, QCOM_QRTR_DATA, 1, 2, 3, 4, 0, 0) == 32);
    CHECK(qcom_qrtr_frame(out, 64, QCOM_QRTR_DATA, 1, 2, 3, 4, 0, 4) == 0);           /* data missing */
    CHECK(qcom_qrtr_frame(0, 64, QCOM_QRTR_DATA, 1, 2, 3, 4, data, 4) == 0);
    CHECK(qcom_qrtr_frame(out, 64, QCOM_QRTR_DATA, 1, 2, 3, 4, data, 0xfffffffdu) == 0);   /* no wrap in the padding */
    const uint32_t words[4] = {0x190, 0x1, 1, 0x4000};
    CHECK(qcom_qrtr_control_frame(out, sizeof out, QCOM_QRTR_NEW_SERVER, 1, QCOM_QRTR_PORT_CTRL, 5, words) == 52);
    CHECK(rd32(out + 4) == QCOM_QRTR_NEW_SERVER && rd32(out + 12) == QCOM_QRTR_PORT_CTRL && rd32(out + 24) == 5 &&
          rd32(out + 28) == QCOM_QRTR_PORT_CTRL && rd32(out + 32) == QCOM_QRTR_NEW_SERVER && rd32(out + 36) == 0x190 &&
          rd32(out + 48) == 0x4000);
}

/* SERVREG_LOC_PFR_REQ: TLV 1 the service name, TLV 2 the reason; the answer is a bare success. */
static uint32_t pfr_request(uint8_t *out, uint16_t txn, const char *service, int zero, const char *reason)
{
    uint8_t *p = out + 7;
    const uint32_t n = (uint32_t)strlen(service) + (zero ? 1u : 0u), r = reason ? (uint32_t)strlen(reason) : 0;
    p[0] = 1; p[1] = (uint8_t)n; p[2] = 0; memcpy(p + 3, service, n); p += 3 + n;
    if (reason) {p[0] = 2; p[1] = (uint8_t)r; p[2] = (uint8_t)(r >> 8); memcpy(p + 3, reason, r); p += 3 + r;}
    out[0] = 0; out[1] = (uint8_t)txn; out[2] = (uint8_t)(txn >> 8); out[3] = 0x24; out[4] = 0;
    out[5] = (uint8_t)(p - out - 7); out[6] = (uint8_t)((p - out - 7) >> 8);
    return (uint32_t)(p - out);
}
static void pfr_answers(void)
{
    uint8_t req[400], ans[64];
    const uint8_t *service = 0, *reason = 0;
    uint32_t sn = 0, rn = 0;
    uint32_t m = pfr_request(req, 0x77, "msm/adsp/sensor_pd", 1, "err_fatal: sns_registry.c:123");
    uint32_t n = qcom_servloc_pfr_answer(req, m, ans, sizeof ans, &service, &sn, &reason, &rn);
    static const uint8_t expect[] = {2, 0x77, 0, 0x24, 0, 7, 0, 0x02, 4, 0, 0, 0, 0, 0};
    CHECK(n == sizeof expect && !memcmp(ans, expect, n));
    CHECK(sn == 18 && !memcmp(service, "msm/adsp/sensor_pd", 18) && rn == 29 && !memcmp(reason, "err_fatal: sns_registry.c:123", 29));
    m = pfr_request(req, 0x78, "msm/adsp/audio_pd", 0, 0);          /* no reason TLV */
    CHECK(qcom_servloc_pfr_answer(req, m, ans, sizeof ans, &service, &sn, &reason, &rn) == 14 && sn == 17 && !reason && rn == 0);
    CHECK(qcom_servloc_pfr_answer(req, m, ans, 13, 0, 0, 0, 0) == 0);  /* no room */
    req[3] = 0x21;                                                   /* not a report */
    CHECK(qcom_servloc_pfr_answer(req, m, ans, sizeof ans, 0, 0, 0, 0) == 0);
    req[3] = 0x24; req[0] = 2;                                       /* a response, not a request */
    CHECK(qcom_servloc_pfr_answer(req, m, ans, sizeof ans, 0, 0, 0, 0) == 0);
    req[0] = 0;
    CHECK(qcom_servloc_pfr_answer(req, m - 1, ans, sizeof ans, 0, 0, 0, 0) == 0);   /* truncated */
    char longname[80];
    memset(longname, 'a', 70); longname[70] = 0;
    m = pfr_request(req, 0x79, longname, 0, "x");
    CHECK(qcom_servloc_pfr_answer(req, m, ans, sizeof ans, 0, 0, 0, 0) == 0);   /* longer than a domain name may be */
}

static void off_before_init(void)
{
    const PaganiAdspQrtrConfig config = {0x190, PAGANI_ADSP_QRTR_ANY, 0, 0};
    Client c = {0};
    PaganiAdspQrtrServer s;
    CHECK(pagani_adsp_qrtr_open(&config, &client_ops, &c) == 0);
    CHECK(pagani_adsp_qrtr_send(0, 5, 1, "x", 1) == PaganiAdspOff);
    CHECK(pagani_adsp_qrtr_server(0, &s) == 0 && pagani_adsp_qrtr_lookup(0x190, PAGANI_ADSP_QRTR_ANY, &s) == 0);
    pagani_adsp_qrtr_close(0);
}

int main(void)
{
    Packet out[32];
    unsigned n;
    frames();
    pfr_answers();
    off_before_init();

    const PaganiAdspConfig config = {0};
    pagani_adsp_core_init(&core, &ops, &config, 0, 0);
    ipcrtr = core.qrtr.channel;
    CHECK(ipcrtr && !strcmp(ipcrtr->name, "IPCRTR") && ipcrtr->max_message == 512 && ipcrtr->depth == 16);

    /* bad arguments, a port that follows and serves nothing */
    Client sns = {.name = "sns"}, tftp = {.name = "tftp"}, late = {.name = "late"}, exact = {.name = "exact"};
    CHECK(pagani_adsp_qrtr_open(0, &client_ops, &sns) == 0);
    CHECK(pagani_adsp_qrtr_open(&(PaganiAdspQrtrConfig){0x190, PAGANI_ADSP_QRTR_ANY, 0, 0}, 0, &sns) == 0);
    CHECK(pagani_adsp_qrtr_open(&(PaganiAdspQrtrConfig){0, 0, 0, 0}, &client_ops, &sns) == 0);

    /* before the link: ports open, nothing goes out, no server known */
    sns.self = pagani_adsp_qrtr_open(&(PaganiAdspQrtrConfig){0x190, PAGANI_ADSP_QRTR_ANY, 0, 0}, &client_ops, &sns);
    tftp.self = pagani_adsp_qrtr_open(&(PaganiAdspQrtrConfig){0, 0, 0x1000, 0x1}, &client_ops, &tftp);
    CHECK(sns.self && tftp.self && pagani_adsp_qrtr_port_number(sns.self) == 0x4000 && pagani_adsp_qrtr_port_number(tftp.self) == 0x4001);
    PaganiAdspQrtrServer s;
    CHECK(pagani_adsp_qrtr_server(sns.self, &s) == 0 && s.generation == 1);
    CHECK(drain(out, 32) == 0);
    CHECK(logged("[adsp] qrtr port n=0x4000 follow=0x190") && logged("[adsp] qrtr port n=0x4001 serve=0x1000:0x1"));

    /* link up: our hello; the DSP's hello: the locator and our server announced to its control port */
    link(1);
    CHECK(drain(out, 32) == 1);
    check_control(&out[0], QCOM_QRTR_HELLO, QCOM_QRTR_PORT_CTRL, QCOM_QRTR_NODE_BROADCAST, 0, 0, 0, 0);
    dsp_control(QCOM_QRTR_HELLO, 0, 0, 0, 0);
    n = drain(out, 32);
    CHECK(n == 2);
    check_control(&out[0], QCOM_QRTR_NEW_SERVER, QCOM_QRTR_PORT_CTRL, 5, 0x40, 0x101, 1, 1);
    check_control(&out[1], QCOM_QRTR_NEW_SERVER, QCOM_QRTR_PORT_CTRL, 5, 0x1000, 0x1, 1, 0x4001);
    dsp_control(QCOM_QRTR_HELLO, 0, 0, 0, 0);       /* a repeated hello announces the locator again, our server once is enough */
    n = drain(out, 32);
    CHECK(n == 1 && rd32(out[0].bytes + 36) == 0x40);

    /* the DSP's services: other services do not concern the port; service 400 appears: one callback, level and generation */
    dsp_control(QCOM_QRTR_NEW_SERVER, 0x42, 0x4a01, 5, 33);
    CHECK(sns.ups == 0 && sns.downs == 0);
    dsp_control(QCOM_QRTR_NEW_SERVER, 0x190, 0x1, 5, 0x1a);
    CHECK(sns.ups == 1 && sns.node == 5 && sns.port == 0x1a && sns.instance == 0x1 && tftp.ups == 0);
    CHECK(pagani_adsp_qrtr_server(sns.self, &s) == 1 && s.node == 5 && s.port == 0x1a && s.instance == 1 && s.generation == 2);
    CHECK(logged("qrtr server up svc=0x190 inst=0x1 at=5:0x1a told=1"));
    dsp_control(QCOM_QRTR_NEW_SERVER, 0x190, 0x1, 5, 0x1a);      /* announced again: nothing new */
    CHECK(sns.ups == 1 && pagani_adsp_qrtr_server(sns.self, &s) == 1 && s.generation == 2);
    CHECK(pagani_adsp_qrtr_lookup(0x190, PAGANI_ADSP_QRTR_ANY, &s) == 1 && s.node == 5 && s.port == 0x1a);
    CHECK(pagani_adsp_qrtr_lookup(0x190, 0x201, &s) == 0 && pagani_adsp_qrtr_lookup(0x42, 0x4a01, &s) == 1 && s.port == 33);

    /* a port opened once the server is known: the level has it, no callback */
    late.self = pagani_adsp_qrtr_open(&(PaganiAdspQrtrConfig){0x190, PAGANI_ADSP_QRTR_ANY, 0, 0}, &client_ops, &late);
    CHECK(late.self && pagani_adsp_qrtr_port_number(late.self) == 0x4002 && late.ups == 0);
    CHECK(pagani_adsp_qrtr_server(late.self, &s) == 1 && s.port == 0x1a && s.generation == 1);
    late.node = s.node;      /* what a client does with the level */
    late.port = s.port;
    CHECK(logged("[adsp] qrtr port n=0x4002 follow=0x190 server=5:0x1a"));
    /* a port for one instance only */
    exact.self = pagani_adsp_qrtr_open(&(PaganiAdspQrtrConfig){0x190, 0x201, 0, 0}, &client_ops, &exact);
    CHECK(exact.self && pagani_adsp_qrtr_server(exact.self, &s) == 0);
    dsp_control(QCOM_QRTR_NEW_SERVER, 0x190, 0x201, 5, 0x30);
    CHECK(exact.ups == 1 && exact.port == 0x30 && sns.ups == 1 && late.ups == 0);   /* the others keep their first server */

    /* data: to a client port (with confirm_rx: a resume to the sender), to nobody (unrouted) */
    const uint8_t ind[7] = {4, 0, 0, 0x21, 0, 0, 0};
    dsp_data(0x1a, 0x4000, 1, ind, sizeof ind);
    CHECK(sns.messages == 1 && sns.from_node == 5 && sns.from_port == 0x1a && sns.length == 7 && !memcmp(sns.last, ind, 7));
    n = drain(out, 32);
    CHECK(n == 1);
    check_header(&out[0], QCOM_QRTR_RESUME_TX, 1, 0x4000, 5, 0x1a, 20);
    CHECK(rd32(out[0].bytes + 32) == QCOM_QRTR_RESUME_TX && rd32(out[0].bytes + 36) == 1 && rd32(out[0].bytes + 40) == 0x4000);
    const uint32_t unrouted = core.qrtr.router.rx_unrouted;
    dsp_data(0x1a, 0x4555, 0, ind, sizeof ind);
    CHECK(core.qrtr.router.rx_unrouted == unrouted + 1 && sns.messages == 1);
    /* a send from inside the receive callback (no lock held) */
    sns.echo = 1;
    dsp_data(0x1a, 0x4000, 0, ind, sizeof ind);
    sns.echo = 0;
    n = drain(out, 32);
    CHECK(n == 1 && sns.messages == 2);
    check_header(&out[0], QCOM_QRTR_DATA, 1, 0x4000, 5, 0x1a, 7);
    CHECK(!memcmp(out[0].bytes + 32, ind, 7) && out[0].bytes[39] == 0);

    /* sends: sizes up to 480 bytes of data (a 512-byte message), arguments */
    uint8_t big[481];
    for (unsigned i = 0; i < sizeof big; ++i) big[i] = (uint8_t)(i * 7);
    CHECK(pagani_adsp_qrtr_send(sns.self, 5, 0x1a, big, 480) == PaganiAdspOk);
    CHECK(pagani_adsp_qrtr_send(sns.self, 5, 0x1a, big, 481) == PaganiAdspTooBig);
    CHECK(pagani_adsp_qrtr_send(sns.self, 5, 0x1a, big, 0) == PaganiAdspArgument);
    CHECK(pagani_adsp_qrtr_send(sns.self, 5, 0x1a, 0, 4) == PaganiAdspArgument);
    CHECK(pagani_adsp_qrtr_send(0, 5, 0x1a, big, 4) == PaganiAdspArgument);
    CHECK(pagani_adsp_qrtr_send(sns.self, 5, 0x1a, big, 3) == PaganiAdspOk);
    n = drain(out, 32);
    CHECK(n == 2);
    check_header(&out[0], QCOM_QRTR_DATA, 1, 0x4000, 5, 0x1a, 480);
    CHECK(!memcmp(out[0].bytes + 32, big, 480));
    check_header(&out[1], QCOM_QRTR_DATA, 1, 0x4000, 5, 0x1a, 3);
    CHECK(out[1].bytes[35] == 0);
    /* a full queue is reported, not waited on */
    unsigned queued = 0;
    while (pagani_adsp_qrtr_send(late.self, 5, 0x1a, big, 8) == PaganiAdspOk) queued++;
    CHECK(queued == 16 && pagani_adsp_qrtr_send(late.self, 5, 0x1a, big, 8) == PaganiAdspFull);
    CHECK(drain(out, 32) == 16);

    /* the server goes (DEL_SERVER): both followers hear it, the generation moves; it comes back on the same port: a new generation */
    dsp_control(QCOM_QRTR_DEL_SERVER, 0x190, 0x1, 5, 0x1a);
    CHECK(sns.downs == 1 && late.downs == 1 && exact.downs == 0);
    /* the only service 400 left is instance 0x201: the any-instance ports take it (one change: gone, then the other) */
    CHECK(sns.ups == 2 && sns.port == 0x30 && late.ups == 1 && late.port == 0x30);
    CHECK(pagani_adsp_qrtr_server(sns.self, &s) == 1 && s.port == 0x30 && s.instance == 0x201 && s.generation == 3);
    dsp_control(QCOM_QRTR_DEL_SERVER, 0x190, 0x201, 5, 0x30);
    CHECK(sns.downs == 2 && late.downs == 2 && exact.downs == 1);
    CHECK(pagani_adsp_qrtr_server(sns.self, &s) == 0 && s.generation == 4);
    dsp_control(QCOM_QRTR_NEW_SERVER, 0x190, 0x1, 5, 0x1a);
    CHECK(sns.ups == 3 && pagani_adsp_qrtr_server(sns.self, &s) == 1 && s.port == 0x1a && s.generation == 5);
    /* the same node:port announced with another instance: gone, then up again */
    dsp_control(QCOM_QRTR_NEW_SERVER, 0x190, 0x101, 5, 0x1a);
    CHECK(sns.downs == 3 && sns.ups == 4 && sns.instance == 0x101);

    /* BYE: everything goes; the next hello announces our server again */
    dsp_control(QCOM_QRTR_BYE, 0, 0, 0, 0);
    CHECK(sns.downs == 4 && late.downs == 4 && pagani_adsp_qrtr_lookup(0x190, PAGANI_ADSP_QRTR_ANY, &s) == 0);
    CHECK(core.qrtr.router.remote_count == 0 && core.status.qrtr_services == 0);
    dsp_control(QCOM_QRTR_HELLO, 0, 0, 0, 0);
    n = drain(out, 32);
    CHECK(n == 2 && rd32(out[0].bytes + 36) == 0x40);
    check_control(&out[1], QCOM_QRTR_NEW_SERVER, QCOM_QRTR_PORT_CTRL, 5, 0x1000, 0x1, 1, 0x4001);
    dsp_control(QCOM_QRTR_NEW_SERVER, 0x190, 0x1, 5, 0x1a);
    CHECK(sns.ups == 5 && late.ups == 4);

    /* the link goes down: the server goes with it; up again: hello, no server until the DSP announces it */
    link(0);
    CHECK(sns.downs == 5 && late.downs == 5 && pagani_adsp_qrtr_server(sns.self, &s) == 0);
    CHECK(drain(out, 32) == 0);
    link(1);
    n = drain(out, 32);
    CHECK(n == 1 && rd32(out[0].bytes + 4) == QCOM_QRTR_HELLO);
    CHECK(sns.downs == 5 && sns.ups == 5);
    dsp_control(QCOM_QRTR_HELLO, 0, 0, 0, 0);
    n = drain(out, 32);
    CHECK(n == 2);
    check_control(&out[1], QCOM_QRTR_NEW_SERVER, QCOM_QRTR_PORT_CTRL, 5, 0x1000, 0x1, 1, 0x4001);
    dsp_control(QCOM_QRTR_NEW_SERVER, 0x190, 0x1, 5, 0x22);
    CHECK(sns.ups == 6 && sns.port == 0x22);

    /* data for our server port, from a DSP client */
    dsp_data(0x55, 0x4001, 0, ind, 3);
    CHECK(tftp.messages == 1 && tftp.from_port == 0x55 && tftp.length == 3);

    /* close: DEL_SERVER for our server, DEL_CLIENT from each port; data to a closed port is unrouted */
    pagani_adsp_qrtr_close(tftp.self);
    n = drain(out, 32);
    CHECK(n == 2);
    check_control(&out[0], QCOM_QRTR_DEL_SERVER, QCOM_QRTR_PORT_CTRL, 5, 0x1000, 0x1, 1, 0x4001);
    check_control(&out[1], QCOM_QRTR_DEL_CLIENT, 0x4001, 5, 1, 0x4001, 0, 0);
    pagani_adsp_qrtr_close(sns.self);
    n = drain(out, 32);
    CHECK(n == 1);
    check_control(&out[0], QCOM_QRTR_DEL_CLIENT, 0x4000, 5, 1, 0x4000, 0, 0);
    pagani_adsp_qrtr_close(sns.self);                 /* twice: nothing */
    CHECK(drain(out, 32) == 0);
    dsp_data(0x1a, 0x4000, 0, ind, 3);
    CHECK(sns.messages == 2 && core.qrtr.router.rx_unrouted == unrouted + 2);
    CHECK(pagani_adsp_qrtr_send(sns.self, 5, 0x22, big, 4) == PaganiAdspArgument);
    dsp_control(QCOM_QRTR_DEL_SERVER, 0x190, 0x1, 5, 0x22);
    CHECK(sns.downs == 5 && late.downs == 6);       /* a closed port hears nothing */

    /* every slot: 12 open at a time, numbers unique, a slot reused with a fresh number */
    PaganiAdspQrtrPort *ports[PAGANI_ADSP_QRTR_PORTS];
    Client many[PAGANI_ADSP_QRTR_PORTS];
    memset(many, 0, sizeof many);
    unsigned opened = 2;   /* late and exact are still open */
    for (unsigned i = 0; i < PAGANI_ADSP_QRTR_PORTS; ++i) {
        ports[i] = pagani_adsp_qrtr_open(&(PaganiAdspQrtrConfig){0x99, PAGANI_ADSP_QRTR_ANY, 0, 0}, &client_ops, &many[i]);
        if (ports[i]) opened++;
    }
    CHECK(opened == PAGANI_ADSP_QRTR_PORTS && !ports[PAGANI_ADSP_QRTR_PORTS - 2] && !ports[PAGANI_ADSP_QRTR_PORTS - 1]);
    CHECK(logged("[adsp] qrtr port refused (all taken)"));
    for (unsigned i = 0; i + 2 < PAGANI_ADSP_QRTR_PORTS; ++i)
        for (unsigned j = 0; j < i; ++j) CHECK(pagani_adsp_qrtr_port_number(ports[i]) != pagani_adsp_qrtr_port_number(ports[j]));
    pagani_adsp_qrtr_close(ports[0]);
    drain(out, 32);
    PaganiAdspQrtrPort *again = pagani_adsp_qrtr_open(&(PaganiAdspQrtrConfig){0x99, PAGANI_ADSP_QRTR_ANY, 0, 0}, &client_ops, &many[0]);
    CHECK(again && pagani_adsp_qrtr_port_number(again) == 0x4000 + 4 + PAGANI_ADSP_QRTR_PORTS - 2);
    for (unsigned i = 0; i + 2 < PAGANI_ADSP_QRTR_PORTS; ++i) many[i].node = 5;
    dsp_control(QCOM_QRTR_NEW_SERVER, 0x99, 1, 5, 0x60);
    unsigned heard = 0;
    for (unsigned i = 0; i + 2 < PAGANI_ADSP_QRTR_PORTS; ++i) heard += many[i].ups;
    CHECK(heard == PAGANI_ADSP_QRTR_PORTS - 2);

    /* the locator: a crash report is answered and logged, the status note says which domain */
    uint8_t req[200];
    const uint32_t m = pfr_request(req, 0x1234, "msm/adsp/sensor_pd", 0, "sns_registry: no config");
    dsp_data(0x77, 1, 0, req, m);
    n = drain(out, 32);
    CHECK(n == 1);
    check_header(&out[0], QCOM_QRTR_DATA, 1, 1, 5, 0x77, 14);
    CHECK(out[0].bytes[32] == 2 && rd16(out[0].bytes + 33) == 0x1234 && rd16(out[0].bytes + 35) == 0x24 && rd16(out[0].bytes + 37) == 7);
    CHECK(out[0].bytes[39] == 2 && rd32(out[0].bytes + 42) == 0);
    CHECK(logged("[adsp] pd crash report msm/adsp/sensor_pd reason=sns_registry: no config sent=1 from=5:119"));
    CHECK(!strcmp(core.status.note, "pd crash: msm/adsp/sensor_pd") && core.status.locator_answers == 1);

    /* a second core starts with an empty registry */
    for (uint32_t i = 0; i < core.channel_count; ++i) {
        op_free(0, core.channel[i].storage, (size_t)core.channel[i].intent_size * core.channel[i].intent_count);
        op_free(0, core.channel[i].queue, (size_t)core.channel[i].depth * (4u + core.channel[i].max_message));
    }
    pagani_adsp_core_init(&core, &ops, &config, 0, 0);
    ipcrtr = core.qrtr.channel;
    CHECK(pagani_adsp_qrtr_lookup(0x99, PAGANI_ADSP_QRTR_ANY, &s) == 0);
    PaganiAdspQrtrPort *fresh = pagani_adsp_qrtr_open(&(PaganiAdspQrtrConfig){0x190, PAGANI_ADSP_QRTR_ANY, 0, 0}, &client_ops, &sns);
    CHECK(fresh && pagani_adsp_qrtr_port_number(fresh) == 0x4000);
    for (uint32_t i = 0; i < core.channel_count; ++i) {
        op_free(0, core.channel[i].storage, (size_t)core.channel[i].intent_size * core.channel[i].intent_count);
        op_free(0, core.channel[i].queue, (size_t)core.channel[i].depth * (4u + core.channel[i].max_message));
    }
    CHECK(allocated == 0 && lock_depth == 0 && wakes > 0);
    printf("PASS qrtr client ports, router hooks and the crash report answer: %u checks\n", checks);
    return 0;
}
