/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * Ported from UniXNU-sheng package/ShengQrtr.c (https://github.com/UniXNU/UniXNU-sheng, commit 08cde20e). 13T changes: the
 * channel is a PaganiAdsp channel, the counters are the PaganiAdsp status and log lines, the intents follow the 13T devicetree; 13T
 * additions: the kernel client ports (PaganiAdsp.h, "IPC router clients") and the answer to a protection domain's crash report. */
/* The IPC router link to the audio DSP (GLINK channel "IPCRTR", which the DSP opens) and the service locator on it. The DSP's protection
 * domains (audio, charger, sensors) ask the application processor's locator which domains provide a service before they start; without
 * the answer for "avs/audio" the audio PD never comes up, and the battery manager runs in the charger PD. Everything runs in the DSP
 * thread's callbacks: each packet is handled at once and any answer is queued, so nothing here waits.
 *
 * Kernel clients (the sensor client of PaganiSensors) open ports of our node from their own threads. The router state stays the DSP
 * thread's: the clients' registry and a copy of the DSP's service list live under the core's registry lock (ops->lock), the DSP thread
 * updates both from the router's hooks (qcom_qrtr_watch) and calls the clients' callbacks with the lock dropped, and a client's send is
 * a whole QRTR packet built on its own stack and queued on the IPCRTR channel (pagani_adsp_core_send, thread safe). */
#include "AdspCore.h"
#include "AdspUtil.h"

/* The devicetree offers 0x800 x 5, 0x2000 x 3 and 0x4400 x 2 on IPCRTR; a channel has one size here, the largest (a request above it would
 * be refused and the DSP's sender would wait for good). */
enum {kLocalNode = 1, kLocatorPort = 1, kIntentSize = 0x4400, kIntentCount = 8, kMaxMessage = 512, kFirstPort = 0x4000,
      kLastPort = 0x7fff, kTracedPorts = 32, kTracedReports = 8};

/* The protection domains as the PKX110's own pd-mapper files describe them (modem_a image/adspr.jsn, adsps.jsn, adspua.jsn,
 * battmgr.jsn, cdspr.jsn, modemr.jsn, read 2026-10-30: the ADSP's QMI instance 74, CDSP 76, modem 180, service data never valid);
 * the same as the SM8550 table of Linux's in-kernel pd-mapper. Every domain also provides "tms/servreg" (QcomQrtr.c). */
static const char *const kAudio[] = {"avs/audio", 0};
static const char *const kGps[] = {"gps/gps_service", 0};
static const QcomPdDomain kDomains[] = {
    {"msm/adsp/audio_pd", 74, kAudio},  {"msm/adsp/root_pd", 74, 0}, {"msm/adsp/charger_pd", 74, 0},
    {"msm/adsp/sensor_pd", 74, 0},      {"msm/cdsp/root_pd", 76, 0}, {"msm/modem/root_pd", 180, kGps},
};

const QcomPdDomain *pagani_adsp_qrtr_domains(unsigned *count)
{
    if (count) *count = sizeof kDomains / sizeof kDomains[0];
    return kDomains;
}

static int send(void *context, const uint8_t *packet, uint32_t length)
{
    PaganiAdspCore *c = (PaganiAdspCore *)context;
    return pagani_adsp_core_send(c, c->qrtr.channel, packet, length) == PaganiAdspOk ? 0 : -1;
}

/* ---- the kernel clients' registry ---- */
struct PaganiAdspQrtrPort {
    uint32_t used, number;                       /* registry lock */
    PaganiAdspQrtrConfig config;
    PaganiAdspQrtrClient client;
    void *context;
    uint32_t has_server, node, port, instance;   /* registry lock: the server followed */
    uint32_t generation;                         /* registry lock: moves on every change of it */
    uint32_t announced;                          /* registry lock: our NEW_SERVER went out since the DSP's last hello */
};

/* One registry, of the core initialized last: the kernel has one core (a new one starts with an empty registry). */
static struct {
    PaganiAdspCore *core;                        /* atomic, release when set up */
    struct PaganiAdspQrtrPort port[PAGANI_ADSP_QRTR_PORTS];
    struct {uint32_t service, instance, node, port;} server[QCOM_QRTR_MAX_REMOTE];
    uint32_t servers, next_number, link_up, hello, remote_node;
    uint32_t traced, reports;                    /* DSP thread */
    uint32_t sent, refused, delivered, unrouted; /* atomic */
} gClients;

static PaganiAdspCore *client_core(void) {return __atomic_load_n(&gClients.core, __ATOMIC_ACQUIRE);}
#define CTX_OF(c) ((c)->ops->context)

/* A log line from any thread: built here, given to the platform's trace (the core's own line buffer is the DSP thread's). */
static void client_trace(PaganiAdspCore *c, const char *line)
{
    if (c->ops->trace) c->ops->trace(CTX_OF(c), line);
}

static int follows(const struct PaganiAdspQrtrPort *p, uint32_t service, uint32_t instance)
{
    return p->config.service && p->config.service == service &&
           (p->config.instance == PAGANI_ADSP_QRTR_ANY || p->config.instance == instance);
}

/* The first announced server a port follows, -1 when none (registry lock). */
static int best_server(const struct PaganiAdspQrtrPort *p)
{
    for (uint32_t i = 0; i < gClients.servers; ++i)
        if (follows(p, gClients.server[i].service, gClients.server[i].instance)) return (int)i;
    return -1;
}

typedef struct {
    void (*server)(void *context, int up, uint32_t node, uint32_t port, uint32_t instance);
    void *context;
    int up;
    uint32_t node, port, instance, number;
} Tell;

/* The router's hook: the DSP's service list changed. The copy and each port's server are updated under the lock, the callbacks run
 * after it. */
static void servers_changed(void *context, QcomQrtr *q, int up, uint32_t service, uint32_t instance, uint32_t node, uint32_t port)
{
    PaganiAdspCore *c = (PaganiAdspCore *)context;
    Tell tell[2 * PAGANI_ADSP_QRTR_PORTS];
    unsigned n = 0, followed = 0;
    c->ops->lock(CTX_OF(c));
    gClients.servers = q->remote_count;
    for (uint32_t i = 0; i < q->remote_count; ++i) {
        gClients.server[i].service = q->remote[i].service;
        gClients.server[i].instance = q->remote[i].instance;
        gClients.server[i].node = q->remote[i].node;
        gClients.server[i].port = q->remote[i].port;
    }
    for (unsigned k = 0; k < PAGANI_ADSP_QRTR_PORTS; ++k) {
        struct PaganiAdspQrtrPort *p = &gClients.port[k];
        if (!p->used || !p->config.service) continue;
        followed |= follows(p, service, instance);
        const int best = best_server(p);
        const int same = best >= 0 && p->has_server && gClients.server[best].node == p->node &&
                         gClients.server[best].port == p->port && gClients.server[best].instance == p->instance;
        if (same || (best < 0 && !p->has_server)) continue;
        if (p->has_server) tell[n++] = (Tell){p->client.server, p->context, 0, p->node, p->port, p->instance, p->number};
        p->has_server = best >= 0;
        if (best >= 0) {
            p->node = gClients.server[best].node;
            p->port = gClients.server[best].port;
            p->instance = gClients.server[best].instance;
            tell[n++] = (Tell){p->client.server, p->context, 1, p->node, p->port, p->instance, p->number};
        }
        p->generation++;
    }
    c->ops->unlock(CTX_OF(c));
    if (followed && gClients.traced < kTracedPorts) {
        PaganiText t;
        char line[120];
        gClients.traced++;
        pagani_text_start(&t, line, sizeof line);
        pagani_text_str(&t, up ? "qrtr server up" : "qrtr server gone");
        pagani_text_kx(&t, "svc", service);
        pagani_text_kx(&t, "inst", instance);
        pagani_text_kd(&t, "at", node);
        pagani_text_char(&t, ':');
        pagani_text_hex(&t, port);
        pagani_text_kd(&t, "told", n);
        pagani_adsp_core_trace(c, line);
    }
    for (unsigned i = 0; i < n; ++i)
        if (tell[i].server) tell[i].server(tell[i].context, tell[i].up, tell[i].node, tell[i].port, tell[i].instance);
}

/* The router's hook: data for a port that is none of our services. */
static void client_data(void *context, QcomQrtr *q, uint32_t number, uint32_t node, uint32_t from, const uint8_t *data, uint32_t length)
{
    PaganiAdspCore *c = (PaganiAdspCore *)context;
    void (*receive)(void *, uint32_t, uint32_t, const uint8_t *, uint32_t) = 0;
    void *target = 0;
    c->ops->lock(CTX_OF(c));
    for (unsigned k = 0; k < PAGANI_ADSP_QRTR_PORTS; ++k)
        if (gClients.port[k].used && gClients.port[k].number == number) {
            receive = gClients.port[k].client.receive;
            target = gClients.port[k].context;
            break;
        }
    c->ops->unlock(CTX_OF(c));
    if (receive) {
        __atomic_fetch_add(&gClients.delivered, 1u, __ATOMIC_RELAXED);
        receive(target, node, from, data, length);
    } else {
        q->rx_unrouted++;
        __atomic_fetch_add(&gClients.unrouted, 1u, __ATOMIC_RELAXED);
    }
}

/* Our servers (ports with an announce_service) go to the DSP after each of its hellos; a link that went away forgets that they did. */
static void announce_servers(PaganiAdspCore *c)
{
    QcomQrtr *q = &c->qrtr.router;
    uint32_t words[PAGANI_ADSP_QRTR_PORTS][4];
    unsigned n = 0;
    uint32_t node = 0;
    const uint32_t hello = gClients.link_up && q->hello_received;
    c->ops->lock(CTX_OF(c));
    if (hello && !gClients.hello) {
        gClients.hello = 1;
        gClients.remote_node = node = q->remote_node;
        for (unsigned k = 0; k < PAGANI_ADSP_QRTR_PORTS; ++k) {
            struct PaganiAdspQrtrPort *p = &gClients.port[k];
            if (!p->used || !p->config.announce_service || p->announced) continue;
            p->announced = 1;
            words[n][0] = p->config.announce_service;
            words[n][1] = p->config.announce_instance;
            words[n][2] = kLocalNode;
            words[n][3] = p->number;
            n++;
        }
    } else if (!hello && gClients.hello) {
        gClients.hello = 0;
        for (unsigned k = 0; k < PAGANI_ADSP_QRTR_PORTS; ++k) gClients.port[k].announced = 0;
    }
    c->ops->unlock(CTX_OF(c));
    for (unsigned i = 0; i < n; ++i) {
        uint8_t packet[QCOM_QRTR_HEADER + 20];
        const uint32_t size = qcom_qrtr_control_frame(packet, sizeof packet, QCOM_QRTR_NEW_SERVER, kLocalNode, QCOM_QRTR_PORT_CTRL, node,
                                                      words[i]);
        if (size) pagani_adsp_core_send(c, c->qrtr.channel, packet, size);
    }
}

static void counters(PaganiAdspCore *c)
{
    PaganiAdspQrtr *q = &c->qrtr;
    c->status.qrtr_node = q->router.hello_received ? q->router.remote_node : 0;
    c->status.qrtr_services = q->router.remote_count;
    c->status.locator_requests = q->requests;
    c->status.locator_answers = q->answers;
    if (q->router.hello_received && !q->hello_traced) {
        PaganiText t;
        char line[96];
        q->hello_traced = 1;
        pagani_text_start(&t, line, sizeof line);
        pagani_text_str(&t, "qrtr hello");
        pagani_text_kd(&t, "node", q->router.remote_node);
        pagani_adsp_core_trace(c, line);
    }
    /* The services the DSP announced, as service:instance, each time their number changes (the first eight times). */
    if (q->router.remote_count != q->last_services && q->traced_services < 8) {
        PaganiText t;
        char line[200];
        q->last_services = q->router.remote_count;
        q->traced_services++;
        pagani_text_start(&t, line, sizeof line);
        pagani_text_str(&t, "qrtr services");
        pagani_text_kd(&t, "n", q->router.remote_count);
        for (uint32_t i = 0; i < q->router.remote_count && t.used + 24 < sizeof line; ++i) {
            pagani_text_char(&t, ' ');
            pagani_text_hex(&t, q->router.remote[i].service);
            pagani_text_char(&t, ':');
            pagani_text_hex(&t, q->router.remote[i].instance);
        }
        pagani_adsp_core_trace(c, line);
    }
}

/* A protection domain's crash report (PFR): answered with success (as Linux's pd-mapper does), the domain's service and reason logged. */
static void crash_report(PaganiAdspCore *c, QcomQrtr *router, uint32_t port, uint32_t node, uint32_t from, const uint8_t *data,
                         uint32_t length)
{
    uint8_t answer[32];
    const uint8_t *service = 0, *reason = 0;
    uint32_t service_size = 0, reason_size = 0;
    const uint32_t n = qcom_servloc_pfr_answer(data, length, answer, sizeof answer, &service, &service_size, &reason, &reason_size);
    const int sent = n && !qcom_qrtr_send_data(router, port, node, from, answer, n);
    if (sent) c->qrtr.answers++;
    if (service) {
        PaganiText t;
        pagani_text_start(&t, c->status.note, sizeof c->status.note);
        pagani_text_str(&t, "pd crash: ");
        pagani_text_printable(&t, (const char *)service, service_size);
    }
    if (gClients.reports++ < kTracedReports) {
        PaganiText t;
        char line[240];
        pagani_text_start(&t, line, sizeof line);
        pagani_text_str(&t, "pd crash report ");
        if (service) pagani_text_printable(&t, (const char *)service, service_size < 64 ? service_size : 64);
        pagani_text_str(&t, " reason=");
        if (reason) pagani_text_printable(&t, (const char *)reason, reason_size < 120 ? reason_size : 120);
        pagani_text_kd(&t, "sent", sent);
        pagani_text_kd(&t, "from", node);
        pagani_text_char(&t, ':');
        pagani_text_udec(&t, from);
        pagani_adsp_core_trace(c, line);
    }
}

static void locator(void *context, QcomQrtr *router, uint32_t port, uint32_t node, uint32_t from, const uint8_t *data, uint32_t length)
{
    PaganiAdspCore *c = (PaganiAdspCore *)context;
    PaganiAdspQrtr *q = &c->qrtr;
    uint8_t answer[480];
    QcomQmiMessage m;
    uint32_t size = 0;
    const uint8_t *name = 0;
    q->requests++;
    if (!qcom_qmi_parse(data, length, &m) && m.type == QCOM_QMI_REQUEST && m.message == QCOM_SERVLOC_PFR) {
        crash_report(c, router, port, node, from, data, length);
        counters(c);
        return;
    }
    if (!qcom_qmi_parse(data, length, &m)) name = qcom_qmi_tlv(&m, 0x01, &size);
    const uint32_t n = qcom_servloc_answer(kDomains, sizeof kDomains / sizeof kDomains[0], data, length, answer, sizeof answer);
    const int sent = n && !qcom_qrtr_send_data(router, port, node, from, answer, n);
    if (sent) q->answers++;
    if (q->traced_requests < 24) {
        PaganiText t;
        char line[120];
        q->traced_requests++;
        pagani_text_start(&t, line, sizeof line);
        pagani_text_str(&t, "locator ask=");
        if (name) pagani_text_printable(&t, (const char *)name, size < 64 ? size : 64);
        pagani_text_kd(&t, "domains", n >= 19 ? (int64_t)(answer[17] | answer[18] << 8) : -1);
        pagani_text_kd(&t, "sent", sent);
        pagani_text_kd(&t, "from", node);
        pagani_text_char(&t, ':');
        pagani_text_udec(&t, from);
        pagani_adsp_core_trace(c, line);
    }
    counters(c);
}

static void link_state(void *context, int up)
{
    PaganiAdspCore *c = (PaganiAdspCore *)context;
    PaganiAdspQrtr *q = &c->qrtr;
    gClients.link_up = up != 0;
    if (up) {
        q->link_ups++;
        /* A fresh link: forget the old one's hello and services (the clients hear that they went), keep ours. */
        q->router.hello_sent = q->router.hello_received = 0;
        qcom_qrtr_forget_remote(&q->router);
        q->router.remote_node = QCOM_QRTR_NODE_BROADCAST;
        q->hello_traced = 0;
        q->last_services = 0;
        announce_servers(c);
        qcom_qrtr_start(&q->router);
    } else {
        q->link_downs++;
        /* Nothing on the DSP is reachable any more: its services go (and the clients hear it). */
        qcom_qrtr_forget_remote(&q->router);
        announce_servers(c);
    }
    counters(c);
}

static void link_receive(void *context, const uint8_t *data, uint32_t length)
{
    PaganiAdspCore *c = (PaganiAdspCore *)context;
    qcom_qrtr_receive(&c->qrtr.router, data, length);
    announce_servers(c);
    counters(c);
}

void pagani_adsp_qrtr_init(PaganiAdspCore *c)
{
    static const PaganiAdspClient client = {link_state, link_receive};
    const PaganiAdspChannelConfig config = {"IPCRTR", kIntentSize, kIntentCount, 0, kMaxMessage, 16};
    qcom_qrtr_init(&c->qrtr.router, kLocalNode, send, c);
    qcom_qrtr_add_service(&c->qrtr.router, QCOM_SERVLOC_SERVICE, QCOM_SERVLOC_INSTANCE, kLocatorPort, locator, c);
    qcom_qrtr_watch(&c->qrtr.router, servers_changed, client_data, c);
    c->qrtr.channel = pagani_adsp_core_channel(c, &config, &client, c);
    /* a new core: an empty registry, published last */
    __atomic_store_n(&gClients.core, (PaganiAdspCore *)0, __ATOMIC_RELEASE);
    pagani_zero(&gClients, sizeof gClients);
    gClients.next_number = kFirstPort;
    __atomic_store_n(&gClients.core, c, __ATOMIC_RELEASE);
}

/* ---- the clients' entry points (PaganiAdsp.h) ---- */
static int number_taken(uint32_t number)
{
    if (number == kLocatorPort) return 1;
    for (unsigned k = 0; k < PAGANI_ADSP_QRTR_PORTS; ++k)
        if (gClients.port[k].used && gClients.port[k].number == number) return 1;
    return 0;
}

PaganiAdspQrtrPort *pagani_adsp_qrtr_open(const PaganiAdspQrtrConfig *config, const PaganiAdspQrtrClient *client, void *context)
{
    PaganiAdspCore *c = client_core();
    struct PaganiAdspQrtrPort *p = 0;
    uint8_t packet[QCOM_QRTR_HEADER + 20];
    uint32_t announce = 0, node = 0;
    if (!c || !config || !client || (!config->service && !config->announce_service)) return 0;
    c->ops->lock(CTX_OF(c));
    for (unsigned k = 0; k < PAGANI_ADSP_QRTR_PORTS && !p; ++k)
        if (!gClients.port[k].used) p = &gClients.port[k];
    if (p) {
        uint32_t number = gClients.next_number;
        while (number_taken(number)) number = number >= kLastPort ? kFirstPort : number + 1;
        gClients.next_number = number >= kLastPort ? kFirstPort : number + 1;
        pagani_zero(p, sizeof *p);
        p->used = 1;
        p->number = number;
        p->config = *config;
        p->client = *client;
        p->context = context;
        p->generation = 1;
        const int best = best_server(p);
        if (best >= 0) {
            p->has_server = 1;
            p->node = gClients.server[best].node;
            p->port = gClients.server[best].port;
            p->instance = gClients.server[best].instance;
        }
        if (config->announce_service && gClients.hello) {
            const uint32_t words[4] = {config->announce_service, config->announce_instance, kLocalNode, number};
            p->announced = 1;
            node = gClients.remote_node;
            announce = qcom_qrtr_control_frame(packet, sizeof packet, QCOM_QRTR_NEW_SERVER, kLocalNode, QCOM_QRTR_PORT_CTRL, node,
                                               words);
        }
    }
    const uint32_t has = p ? p->has_server : 0, at_node = p ? p->node : 0, at_port = p ? p->port : 0;
    c->ops->unlock(CTX_OF(c));
    PaganiText t;
    char line[160];
    pagani_text_start(&t, line, sizeof line);
    pagani_text_str(&t, "[adsp] qrtr port");
    if (p) pagani_text_kx(&t, "n", p->number);
    else pagani_text_str(&t, " refused (all taken)");
    if (config->service) {
        pagani_text_kx(&t, "follow", config->service);
        if (config->instance != PAGANI_ADSP_QRTR_ANY) {pagani_text_char(&t, ':'); pagani_text_hex(&t, config->instance);}
    }
    if (config->announce_service) {
        pagani_text_kx(&t, "serve", config->announce_service);
        pagani_text_char(&t, ':');
        pagani_text_hex(&t, config->announce_instance);
    }
    if (has) {pagani_text_kd(&t, "server", at_node); pagani_text_char(&t, ':'); pagani_text_hex(&t, at_port);}
    client_trace(c, line);
    if (announce) pagani_adsp_core_send(c, c->qrtr.channel, packet, announce);
    return p;
}

uint32_t pagani_adsp_qrtr_port_number(const PaganiAdspQrtrPort *port)
{
    return port ? port->number : 0;
}

int pagani_adsp_qrtr_server(const PaganiAdspQrtrPort *port, PaganiAdspQrtrServer *out)
{
    PaganiAdspCore *c = client_core();
    int known = 0;
    if (out) pagani_zero(out, sizeof *out);
    if (!c || !port) return 0;
    c->ops->lock(CTX_OF(c));
    if (port->used) {
        known = port->has_server != 0;
        if (out) {
            out->node = known ? port->node : 0;
            out->port = known ? port->port : 0;
            out->instance = known ? port->instance : 0;
            out->generation = port->generation;
        }
    }
    c->ops->unlock(CTX_OF(c));
    return known;
}

int pagani_adsp_qrtr_lookup(uint32_t service, uint32_t instance, PaganiAdspQrtrServer *out)
{
    PaganiAdspCore *c = client_core();
    int known = 0;
    if (out) pagani_zero(out, sizeof *out);
    if (!c || !service) return 0;
    c->ops->lock(CTX_OF(c));
    for (uint32_t i = 0; i < gClients.servers && !known; ++i) {
        if (gClients.server[i].service != service || (instance != PAGANI_ADSP_QRTR_ANY && gClients.server[i].instance != instance)) continue;
        known = 1;
        if (out) {
            out->node = gClients.server[i].node;
            out->port = gClients.server[i].port;
            out->instance = gClients.server[i].instance;
        }
    }
    c->ops->unlock(CTX_OF(c));
    return known;
}

int pagani_adsp_qrtr_send(PaganiAdspQrtrPort *port, uint32_t node, uint32_t to_port, const void *data, uint32_t length)
{
    PaganiAdspCore *c = client_core();
    uint8_t packet[kMaxMessage];
    if (!c) return PaganiAdspOff;
    if (!port || !data || !length || !port->used) return PaganiAdspArgument;
    if (length > PAGANI_ADSP_QRTR_MAX_DATA) return PaganiAdspTooBig;
    const uint32_t n = qcom_qrtr_frame(packet, sizeof packet, QCOM_QRTR_DATA, kLocalNode, port->number, node, to_port,
                                       (const uint8_t *)data, length);
    if (!n) return PaganiAdspTooBig;
    if (!c->qrtr.channel) return PaganiAdspDown;
    const int rc = pagani_adsp_core_send(c, c->qrtr.channel, packet, n);
    __atomic_fetch_add(rc == PaganiAdspOk ? &gClients.sent : &gClients.refused, 1u, __ATOMIC_RELAXED);
    return rc;
}

void pagani_adsp_qrtr_close(PaganiAdspQrtrPort *port)
{
    PaganiAdspCore *c = client_core();
    uint8_t server[QCOM_QRTR_HEADER + 20], client[QCOM_QRTR_HEADER + 20];
    uint32_t server_size = 0, client_size = 0;
    if (!c || !port) return;
    c->ops->lock(CTX_OF(c));
    if (port->used) {
        port->used = 0;
        if (gClients.hello) {
            const uint32_t node = gClients.remote_node;
            const uint32_t gone[4] = {kLocalNode, port->number, 0, 0};
            if (port->announced) {
                const uint32_t words[4] = {port->config.announce_service, port->config.announce_instance, kLocalNode, port->number};
                server_size = qcom_qrtr_control_frame(server, sizeof server, QCOM_QRTR_DEL_SERVER, kLocalNode, QCOM_QRTR_PORT_CTRL, node,
                                                      words);
            }
            client_size = qcom_qrtr_control_frame(client, sizeof client, QCOM_QRTR_DEL_CLIENT, kLocalNode, port->number, node, gone);
        }
    }
    c->ops->unlock(CTX_OF(c));
    if (server_size) pagani_adsp_core_send(c, c->qrtr.channel, server, server_size);
    if (client_size) pagani_adsp_core_send(c, c->qrtr.channel, client, client_size);
}
