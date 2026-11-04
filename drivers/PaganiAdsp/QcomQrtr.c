/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * Ported from UniXNU-sheng package/QcomQrtr.c (https://github.com/UniXNU/UniXNU-sheng, commit 08cde20e). 13T additions: the hooks for
 * kernel clients (qcom_qrtr_watch, qcom_qrtr_forget_remote, qcom_qrtr_find_server, the pure frame builders), the locator's answer to a
 * protection domain's crash report (qcom_servloc_pfr_answer) and a RESUME_TX that names the receiver (sheng's named the sender: a flow
 * controlled sender never resumes, which a stream of indications soon reaches). */
#include "QcomQrtr.h"

static uint16_t le16(const uint8_t *p) {return (uint16_t)(p[0] | (uint16_t)p[1] << 8);}
static uint32_t le32(const uint8_t *p) {return (uint32_t)le16(p) | (uint32_t)le16(p + 2) << 16;}
static void put16(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}
static void put32(uint8_t *p, uint32_t v) {put16(p, v); put16(p + 2, v >> 16);}

#define CTRL_BYTES 20u  /* command and four words */
#define MAX_PACKET 512u

void qcom_qrtr_init(QcomQrtr *q, uint32_t local_node, QcomQrtrSend send, void *send_context)
{
    *q = (QcomQrtr){0};
    q->local_node = local_node;
    q->remote_node = QCOM_QRTR_NODE_BROADCAST;
    q->send = send;
    q->send_context = send_context;
}

uint32_t qcom_qrtr_frame(uint8_t *out, uint32_t capacity, uint32_t type, uint32_t src_node, uint32_t src_port, uint32_t dst_node,
                         uint32_t dst_port, const uint8_t *data, uint32_t length)
{
    if (!out || capacity < QCOM_QRTR_HEADER || (length && !data) || length > capacity - QCOM_QRTR_HEADER) return 0;
    const uint32_t padded = (length + 3) & ~3u;
    if (padded > capacity - QCOM_QRTR_HEADER) return 0;
    put32(out, 1);
    put32(out + 4, type);
    put32(out + 8, src_node);
    put32(out + 12, src_port);
    put32(out + 16, 0);  /* confirm_rx: we never ask for flow control acks */
    put32(out + 20, length);
    put32(out + 24, dst_node);
    put32(out + 28, dst_port);
    for (uint32_t i = 0; i < padded; ++i) out[QCOM_QRTR_HEADER + i] = i < length ? data[i] : 0;
    return QCOM_QRTR_HEADER + padded;
}

uint32_t qcom_qrtr_control_frame(uint8_t *out, uint32_t capacity, uint32_t command, uint32_t src_node, uint32_t src_port,
                                 uint32_t dst_node, const uint32_t words[4])
{
    uint8_t pkt[CTRL_BYTES] = {0};
    put32(pkt, command);
    for (unsigned i = 0; words && i < 4; ++i) put32(pkt + 4 + 4 * i, words[i]);
    return qcom_qrtr_frame(out, capacity, command, src_node, src_port, dst_node, QCOM_QRTR_PORT_CTRL, pkt, sizeof pkt);
}

static int emit(QcomQrtr *q, uint32_t type, uint32_t from_port, uint32_t node, uint32_t port, const uint8_t *data, uint32_t length)
{
    uint8_t packet[MAX_PACKET];
    const uint32_t n = qcom_qrtr_frame(packet, sizeof packet, type, q->local_node, from_port, node, port, data, length);
    if (!n) return -1;
    if (q->send(q->send_context, packet, n)) {q->tx_refused++; return -1;}
    q->tx_packets++;
    return 0;
}

static int control(QcomQrtr *q, uint32_t command, const uint32_t words[4])
{
    uint8_t pkt[CTRL_BYTES] = {0};
    put32(pkt, command);
    for (unsigned i = 0; words && i < 4; ++i) put32(pkt + 4 + 4 * i, words[i]);
    return emit(q, command, QCOM_QRTR_PORT_CTRL, q->remote_node, QCOM_QRTR_PORT_CTRL, pkt, sizeof pkt);
}

static void announce(QcomQrtr *q, unsigned i)
{
    const uint32_t words[4] = {q->local[i].service, q->local[i].instance, q->local_node, q->local[i].port};
    control(q, QCOM_QRTR_NEW_SERVER, words);
}

int qcom_qrtr_add_service(QcomQrtr *q, uint32_t service, uint32_t instance, uint32_t port, QcomQrtrReceive receive, void *context)
{
    if (q->local_count == QCOM_QRTR_MAX_LOCAL || port == QCOM_QRTR_PORT_CTRL) return -1;
    const unsigned i = q->local_count++;
    q->local[i].service = service; q->local[i].instance = instance; q->local[i].port = port;
    q->local[i].receive = receive; q->local[i].context = context;
    if (q->hello_received) announce(q, i);
    return 0;
}

void qcom_qrtr_start(QcomQrtr *q)
{
    if (!control(q, QCOM_QRTR_HELLO, 0)) q->hello_sent = 1;
}

int qcom_qrtr_find_remote(const QcomQrtr *q, uint32_t service)
{
    for (uint32_t i = 0; i < q->remote_count; ++i) if (q->remote[i].service == service) return (int)i;
    return -1;
}

int qcom_qrtr_find_server(const QcomQrtr *q, uint32_t service, uint32_t instance)
{
    for (uint32_t i = 0; i < q->remote_count; ++i)
        if (q->remote[i].service == service && (instance == QCOM_QRTR_ANY_INSTANCE || q->remote[i].instance == instance)) return (int)i;
    return -1;
}

void qcom_qrtr_watch(QcomQrtr *q, QcomQrtrServers servers, QcomQrtrReceive other, void *context)
{
    q->servers = servers;
    q->other = other;
    q->watch_context = context;
}

static void tell(QcomQrtr *q, int up, uint32_t service, uint32_t instance, uint32_t node, uint32_t port)
{
    if (q->servers) q->servers(q->watch_context, q, up, service, instance, node, port);
}

void qcom_qrtr_forget_remote(QcomQrtr *q)
{
    /* last first, so each report sees the list without the server it reports */
    while (q->remote_count) {
        const uint32_t i = --q->remote_count;
        tell(q, 0, q->remote[i].service, q->remote[i].instance, q->remote[i].node, q->remote[i].port);
    }
}

static void remote_server(QcomQrtr *q, const uint8_t *w, int add)
{
    const uint32_t service = le32(w), instance = le32(w + 4), node = le32(w + 8), port = le32(w + 12);
    if (!service && !instance && !node && !port) return;  /* the end of a listing */
    for (uint32_t i = 0; i < q->remote_count; ++i) {
        if (q->remote[i].node != node || q->remote[i].port != port) continue;
        const uint32_t old_service = q->remote[i].service, old_instance = q->remote[i].instance;
        if (add) {
            if (old_service == service && old_instance == instance) return;   /* announced again */
            q->remote[i].service = service; q->remote[i].instance = instance;
            tell(q, 0, old_service, old_instance, node, port);
            tell(q, 1, service, instance, node, port);
            return;
        }
        q->remote[i] = q->remote[--q->remote_count];
        tell(q, 0, old_service, old_instance, node, port);
        return;
    }
    if (!add) return;
    if (q->remote_count == QCOM_QRTR_MAX_REMOTE) {q->remote_full++; return;}
    q->remote[q->remote_count].service = service; q->remote[q->remote_count].instance = instance;
    q->remote[q->remote_count].node = node; q->remote[q->remote_count].port = port;
    q->remote_count++;
    tell(q, 1, service, instance, node, port);
}

int qcom_qrtr_receive(QcomQrtr *q, const uint8_t *packet, uint32_t length)
{
    if (length < QCOM_QRTR_HEADER || le32(packet) != 1) {q->rx_bad++; return -1;}
    const uint32_t type = le32(packet + 4), src_node = le32(packet + 8), src_port = le32(packet + 12);
    const uint32_t confirm = le32(packet + 16), size = le32(packet + 20), dst_port = le32(packet + 28);
    if (size > length - QCOM_QRTR_HEADER) {q->rx_bad++; return -1;}
    const uint8_t *data = packet + QCOM_QRTR_HEADER;
    q->rx_packets++;
    if (type == QCOM_QRTR_DATA) {
        q->rx_data++;
        unsigned i = 0;
        while (i < q->local_count && q->local[i].port != dst_port) ++i;
        if (i < q->local_count) q->local[i].receive(q->local[i].context, q, dst_port, src_node, src_port, data, size);
        else if (q->other) q->other(q->watch_context, q, dst_port, src_node, src_port, data, size);
        else q->rx_unrouted++;
        if (confirm) {
            /* The sender stops at its flow window until this comes. It finds its flow by the address it sent to, so the packet names the
             * port that took the data, and goes from that port to the sender's (Linux qrtr_send_resume_tx and qrtr_tx_resume, as facts). */
            uint8_t pkt[CTRL_BYTES] = {0};
            put32(pkt, QCOM_QRTR_RESUME_TX);
            put32(pkt + 4, q->local_node);
            put32(pkt + 8, dst_port);
            if (!emit(q, QCOM_QRTR_RESUME_TX, dst_port, src_node, src_port, pkt, sizeof pkt)) q->resumes_sent++;
        }
        return 0;
    }
    /* control packets carry their command again in the payload */
    if (size < 4) {q->rx_bad++; return -1;}
    switch (type) {
    case QCOM_QRTR_HELLO:
        q->remote_node = src_node;
        if (!q->hello_sent) qcom_qrtr_start(q);
        q->hello_received = 1;
        for (unsigned i = 0; i < q->local_count; ++i) announce(q, i);
        break;
    case QCOM_QRTR_NEW_SERVER:
    case QCOM_QRTR_DEL_SERVER:
        if (size >= CTRL_BYTES) remote_server(q, data + 4, type == QCOM_QRTR_NEW_SERVER);
        break;
    case QCOM_QRTR_BYE:
        qcom_qrtr_forget_remote(q);
        q->hello_received = 0;
        break;
    default:  /* DEL_CLIENT, RESUME_TX (we never ask), PING, lookups (the remote keeps its own name service) */
        break;
    }
    return 0;
}

int qcom_qrtr_send_data(QcomQrtr *q, uint32_t from_port, uint32_t node, uint32_t port, const uint8_t *data, uint32_t length)
{
    return emit(q, QCOM_QRTR_DATA, from_port, node, port, data, length);
}

/* ---- QMI ---- */
int qcom_qmi_parse(const uint8_t *data, uint32_t length, QcomQmiMessage *m)
{
    if (length < QCOM_QMI_HEADER) return -1;
    m->type = data[0];
    m->transaction = le16(data + 1);
    m->message = le16(data + 3);
    m->length = le16(data + 5);
    if (m->length > length - QCOM_QMI_HEADER) return -1;
    m->tlvs = data + QCOM_QMI_HEADER;
    return 0;
}

const uint8_t *qcom_qmi_tlv(const QcomQmiMessage *m, uint8_t type, uint32_t *size)
{
    for (uint32_t at = 0; at + 3 <= m->length;) {
        const uint32_t n = le16(m->tlvs + at + 1);
        if (n > m->length - at - 3) return 0;
        if (m->tlvs[at] == type) {*size = n; return m->tlvs + at + 3;}
        at += 3 + n;
    }
    return 0;
}

/* ---- the service locator ---- */
#define NAME_MAX_ 64u
#define LIST_MAX 32u

static int same(const char *a, const uint8_t *b, uint32_t n)
{
    uint32_t i = 0;
    for (; i < n && a[i]; ++i) if ((uint8_t)a[i] != b[i]) return 0;
    return i == n && !a[i];
}

static int provides(const QcomPdDomain *d, const uint8_t *name, uint32_t n)
{
    if (same("tms/servreg", name, n)) return 1;
    for (unsigned i = 0; d->services && d->services[i]; ++i) if (same(d->services[i], name, n)) return 1;
    return 0;
}

uint32_t qcom_servloc_answer(const QcomPdDomain *domains, unsigned count, const uint8_t *request, uint32_t length, uint8_t *out,
                             uint32_t capacity)
{
    QcomQmiMessage m;
    uint32_t name_size = 0, offset_size = 0;
    if (qcom_qmi_parse(request, length, &m) || m.type != QCOM_QMI_REQUEST || m.message != QCOM_SERVLOC_GET_DOMAIN_LIST) return 0;
    const uint8_t *name = qcom_qmi_tlv(&m, 0x01, &name_size);
    const uint8_t *skip = qcom_qmi_tlv(&m, 0x10, &offset_size);
    if (!name || name_size > NAME_MAX_ || (skip && offset_size != 4)) return 0;
    while (name_size && !name[name_size - 1]) --name_size;  /* a sender may count the terminating zero */
    const uint32_t first = skip ? le32(skip) : 0;
    if (capacity < QCOM_QMI_HEADER + 7 + 5 + 5 + 4) return 0;
    uint8_t *p = out + QCOM_QMI_HEADER;
    /* result: success, no error */
    p[0] = 0x02; put16(p + 1, 4); put32(p + 3, 0); p += 7;
    uint32_t total = 0, listed = 0;
    uint8_t *list = p + 5 + 5, *entry = list + 4;
    for (unsigned i = 0; i < count; ++i) {
        if (!provides(&domains[i], name, name_size)) continue;
        const uint32_t index = total++;
        if (index < first || listed == LIST_MAX) continue;
        uint32_t n = 0;
        while (domains[i].domain[n] && n < NAME_MAX_) ++n;
        if ((uint32_t)(entry - out) + 1 + n + 9 > capacity) return 0;
        entry[0] = (uint8_t)n;
        for (uint32_t b = 0; b < n; ++b) entry[1 + b] = (uint8_t)domains[i].domain[b];
        entry += 1 + n;
        put32(entry, domains[i].instance);  /* instance, then service data valid (no) and service data */
        entry[4] = 0;
        put32(entry + 5, 0);
        entry += 9;
        listed++;
    }
    /* total domains, the database revision, then the list (a one-byte count before its entries) */
    p[0] = 0x10; put16(p + 1, 2); put16(p + 3, total); p += 5;
    p[0] = 0x11; put16(p + 1, 2); put16(p + 3, 1); p += 5;
    if (listed) {
        p[0] = 0x12; put16(p + 1, (uint32_t)(entry - p - 3)); p[3] = (uint8_t)listed;
        p = entry;
    }
    out[0] = QCOM_QMI_RESPONSE;
    put16(out + 1, m.transaction);
    put16(out + 3, m.message);
    put16(out + 5, (uint32_t)(p - out - QCOM_QMI_HEADER));
    return (uint32_t)(p - out);
}

/* ---- a protection domain's crash report ---- */
#define REASON_MAX 256u

uint32_t qcom_servloc_pfr_answer(const uint8_t *request, uint32_t length, uint8_t *out, uint32_t capacity, const uint8_t **service,
                                 uint32_t *service_length, const uint8_t **reason, uint32_t *reason_length)
{
    QcomQmiMessage m;
    uint32_t name_size = 0, why_size = 0;
    if (qcom_qmi_parse(request, length, &m) || m.type != QCOM_QMI_REQUEST || m.message != QCOM_SERVLOC_PFR) return 0;
    const uint8_t *name = qcom_qmi_tlv(&m, 0x01, &name_size);
    const uint8_t *why = qcom_qmi_tlv(&m, 0x02, &why_size);
    if (!name || name_size > NAME_MAX_ + 1 || (why && why_size > REASON_MAX + 1)) return 0;
    if (capacity < QCOM_QMI_HEADER + 7) return 0;
    while (name_size && !name[name_size - 1]) --name_size;   /* a sender may count the terminating zero */
    while (why && why_size && !why[why_size - 1]) --why_size;
    if (service) *service = name;
    if (service_length) *service_length = name_size;
    if (reason) *reason = why;
    if (reason_length) *reason_length = why ? why_size : 0;
    out[0] = QCOM_QMI_RESPONSE;
    put16(out + 1, m.transaction);
    put16(out + 3, m.message);
    put16(out + 5, 7);
    /* result: success, no error */
    out[7] = 0x02; put16(out + 8, 4); put32(out + 10, 0);
    return QCOM_QMI_HEADER + 7;
}
