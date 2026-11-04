/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
#include "BtBridge.h"
#include "BtStandIn.h"
#include "BtTlv.h"
#include "BtUtil.h"

enum {NoteConnect, NoteRefused, NoteGone, NoteVsc, NoteReady, NoteUp, NoteRefusedCommand, NoteGuard};
enum {kRingMask = PbBridgeRingBytes - 1};

static void lock(PbBridge *b) {b->io->lock(b->io->ctx, 1);}
static void unlock(PbBridge *b) {b->io->lock(b->io->ctx, 0);}
static uint64_t now_ms(PbBridge *b) {return b->io->now_ms(b->io->ctx);}

static uint32_t ring_used(const PbRing *r) {return r->tail - r->head;}
static uint32_t ring_free(const PbRing *r) {return PbBridgeRingBytes - ring_used(r);}
static void ring_clear(PbRing *r) {r->head = r->tail;}
/* 一条记录：2 字节长度，后面是内容。 */
static void ring_in(PbRing *r, const uint8_t *data, uint32_t bytes)
{
    for (uint32_t i = 0; i < bytes; ++i) r->data[r->tail++ & kRingMask] = data[i];
}
static int ring_put(PbRing *r, const uint8_t *data, uint32_t bytes)
{
    if (ring_free(r) < bytes + 2) return 0;
    const uint8_t size[2] = {(uint8_t)bytes, (uint8_t)(bytes >> 8)};
    ring_in(r, size, 2);
    ring_in(r, data, bytes);
    return 1;
}
/* 读出最前面一条，不取走。空的时候返回 0。 */
static uint32_t ring_peek(const PbRing *r, uint8_t *out)
{
    if (!ring_used(r)) return 0;
    const uint32_t bytes = (uint32_t)r->data[r->head & kRingMask] | (uint32_t)r->data[(r->head + 1) & kRingMask] << 8;
    for (uint32_t i = 0; i < bytes; ++i) out[i] = r->data[(r->head + 2 + i) & kRingMask];
    return bytes;
}
static void ring_drop(PbRing *r, uint32_t bytes) {r->head += bytes + 2;}

/* 持锁时留的话，锁外由 report 说出去。满了就丢，只记个数。 */
static void note(PbBridge *b, uint32_t kind, uint32_t x, uint32_t y)
{
    if (b->note_tail - b->note_head == PbBridgeNotes) {
        b->notes_lost++;
        return;
    }
    PbBridgeNote *n = &b->notes[b->note_tail++ % PbBridgeNotes];
    n->kind = kind;
    n->a = x;
    n->b = y;
}
static void say(PbBridge *b, const PbBridgeNote *n)
{
    char line[160];
    PbText t;
    pb_text_start(&t, line, sizeof line);
    switch (n->kind) {
    case NoteConnect:
        pb_text_str(&t, "[bt] client connected");
        pb_text_kd(&t, "pid", n->a);
        pb_text_kd(&t, "unit", n->b);
        break;
    case NoteRefused:
        pb_text_str(&t, "[bt] client refused");
        pb_text_kd(&t, "pid", n->a);
        break;
    case NoteGone:
        pb_text_str(&t, "[bt] client gone");
        pb_text_kd(&t, "ready", n->a);
        pb_text_kd(&t, "failed_starts", n->b);
        break;
    case NoteVsc:
        pb_text_str(&t, "[bt] vsc answered locally");
        pb_text_kx(&t, "opcode", n->a);
        pb_text_kd(&t, "kinds", n->b);
        break;
    case NoteReady:
        pb_text_str(&t, "[bt] stack init finished");
        pb_text_kd(&t, "ms", n->a);
        break;
    case NoteUp:
        pb_text_str(&t, "[bt] bridge up");
        pb_text_kd(&t, "held_bytes", n->a);
        pb_text_kd(&t, "client", n->b);
        break;
    case NoteRefusedCommand:
        pb_text_str(&t, "[bt] controller refused");
        pb_text_kx(&t, "opcode", n->a);
        pb_text_kx(&t, "status", n->b);
        break;
    default:                                /* NoteGuard */
        pb_text_str(&t, "[bt] GUARD");
        pb_text_kd(&t, "why", n->a);
        pb_text_kd(&t, "pending", n->b);
        pb_text_str(&t, " last");
        for (unsigned i = PbBridgeRecent; i > 0; --i) {
            if (b->recent_count < i) continue;
            pb_text_char(&t, ' ');
            pb_text_hex(&t, b->recent[(b->recent_count - i) % PbBridgeRecent]);
        }
        break;
    }
    if (b->io->log) b->io->log(b->io->ctx, line);
}
static void report(PbBridge *b)
{
    for (;;) {
        lock(b);
        if (b->note_head == b->note_tail) {
            unlock(b);
            return;
        }
        const PbBridgeNote n = b->notes[b->note_head++ % PbBridgeNotes];
        unlock(b);
        say(b, &n);
    }
}

static void trip(PbBridge *b, unsigned why)
{
    if (b->guard) return;
    b->guard = (uint8_t)why;
    b->link = PbLinkDead;
    b->kill_pid = b->client ? b->pid : 0;
    ring_clear(&b->to_uart);
    note(b, NoteGuard, why, b->pending);
    b->io->wake_room(b->io->ctx);
}

/* 把上行环里的帧交给客户端，放不下的留着。 */
static void flush_client(PbBridge *b)
{
    while (ring_used(&b->to_client)) {
        if (!b->client) {
            ring_clear(&b->to_client);
            return;
        }
        const uint32_t bytes = ring_peek(&b->to_client, b->scratch);
        const int rc = b->io->client_put(b->io->ctx, b->unit, b->scratch, bytes);
        if (rc > 0) {
            b->put_full++;
            return;
        }
        ring_drop(&b->to_client, bytes);
        if (rc < 0) b->put_lost++;
        else b->frames_up++;
    }
}

/* 等 ring 腾出 bytes 字节的一条记录。等的时候放开锁，所以回来要重新看状态。 */
static int room_for(PbBridge *b, PbRing *r, uint32_t bytes)
{
    const uint64_t until = now_ms(b) + PbBridgeRoomMs;
    while (ring_free(r) < bytes + 2) {
        if (!b->client || b->link == PbLinkDead || now_ms(b) >= until) return 0;
        b->io->wait_room(b->io->ctx, PbBridgeWaitStepMs);
        if (r == &b->to_client) flush_client(b);
    }
    return 1;
}

static void answer(PbBridge *b, const uint8_t *reply, uint32_t bytes)
{
    if (room_for(b, &b->to_client, bytes) && ring_put(&b->to_client, reply, bytes)) flush_client(b);
    else b->tx_dropped++;
}

static void remember_vsc(PbBridge *b, uint16_t opcode)
{
    b->vsc_answered++;
    for (uint32_t i = 0; i < b->vsc_kinds; ++i) if (b->vsc[i] == opcode) return;
    if (b->vsc_kinds < PbBridgeVscMax) b->vsc[b->vsc_kinds++] = opcode;
    note(b, NoteVsc, opcode, b->vsc_kinds);
}

/* 初始化表的最后一条发出去了：这个客户端活着走完了 HCI 初始化。 */
static void stack_ready(PbBridge *b, uint64_t now)
{
    if (!b->client) return;
    b->ready = 1;
    b->failed_starts = 0;
    if (!b->ready_noted) {
        b->ready_noted = 1;
        note(b, NoteReady, (uint32_t)(now - b->connected_at), 0);
    }
}

/* 客户端写完了一包。 */
static void from_client_packet(PbBridge *b)
{
    const uint8_t *p = b->from_client.buf;
    const uint32_t bytes = b->from_client.have;
    uint8_t reply[PbStandInMax];
    uint32_t reply_bytes = 0;
    uint16_t vendor = 0;
    if (p[0] == PbH4Cmd) b->recent[b->recent_count++ % PbBridgeRecent] = (uint16_t)(p[1] | (uint16_t)p[2] << 8);
    /* 成帧已经保证了包的长度，这里只有回和不回两种 */
    if (pb_h4_filter_tx(p, bytes, reply, &reply_bytes, &vendor) == PbFilterAnswered) {
        remember_vsc(b, vendor);
        answer(b, reply, reply_bytes);
    } else if (b->link == PbLinkStandIn) {
        if (p[0] == PbH4Cmd) {
            if (p[1] == (uint8_t)PbOpErroneousData && p[2] == (uint8_t)(PbOpErroneousData >> 8)) stack_ready(b, now_ms(b));
            reply_bytes = pb_standin_reply(p, bytes, b->address, reply);
            if (reply_bytes) answer(b, reply, reply_bytes);
        }
    } else if (b->link == PbLinkDead) {
        b->tx_dropped++;
    } else if (room_for(b, &b->to_uart, bytes) && ring_put(&b->to_uart, p, bytes)) {
        b->io->kick(b->io->ctx);
    } else {
        b->tx_dropped++;
    }
}

/* 一包发下去了。 */
static void after_write(PbBridge *b, uint64_t now)
{
    b->packets_out++;
    const uint8_t *p = b->out;
    if (p[0] != PbH4Cmd) return;
    const uint16_t opcode = (uint16_t)(p[1] | (uint16_t)p[2] << 8);
    if (opcode == PbOpHostCompleted) return;
    if (!b->pending) b->pending_at = now;
    b->pending++;
    if (opcode == PbOpErroneousData) stack_ready(b, now);
}

/* Command Complete 和 Command Status 结清一条命令，opcode 0 只是控制器在报额度。返回 1：这是内核自己那条 HCI_Reset 的应答，不上交。 */
static int command_answered(PbBridge *b, const uint8_t *f, uint32_t bytes, uint64_t now)
{
    uint16_t opcode;
    uint8_t status = 0;
    if (f[0] != PbH4Evt) return 0;
    if (f[1] == PbEvtCommandComplete && bytes >= 6) {
        opcode = (uint16_t)(f[4] | (uint16_t)f[5] << 8);
        if (bytes >= 7) status = f[6];
    } else if (f[1] == PbEvtCommandStatus && bytes >= 7) {
        opcode = (uint16_t)(f[5] | (uint16_t)f[6] << 8);
        status = f[3];
    } else {
        return 0;
    }
    if (!opcode) return 0;
    /* 初始化表里 0x0c33、0x0c5b 这些 Android 没发过，控制器不认的话 bluetoothd 会 abort：第一轮要看得见是哪一条 */
    if (status && b->refused_logged < PbBridgeLogRefusals) {
        b->refused_logged++;
        note(b, NoteRefusedCommand, opcode, status);
    }
    b->pending_at = now;
    if (opcode == PbOpReset) {
        b->pending = 0;                 /* 控制器复位了，别的命令不会再有应答 */
        if (b->own_reset) {
            b->own_reset = 0;
            return 1;
        }
    } else if (b->pending) {
        b->pending--;
    }
    return 0;
}

static void on_frame(PbBridge *b, const uint8_t *f, uint32_t bytes, uint64_t now)
{
    b->frames_in++;
    if (pb_frame_crashed(f, bytes)) {
        trip(b, PbGuardCrash);
        return;
    }
    const int own = command_answered(b, f, bytes, now);
    const int verdict = pb_h4_filter_rx(f, bytes);
    if (verdict == PbFilterDrop) {
        b->frames_dropped++;
    } else if (verdict != PbFilterPass) {
        b->frames_bad++;
    } else if (!b->client || own) {
        b->frames_no_client++;
    } else if (!ring_put(&b->to_client, f, bytes)) {
        b->up_full++;                   /* PbBridgeReadRoom 管着，到不了这里 */
    }
}

static void feed(PbBridge *b, const uint8_t *data, uint32_t bytes, uint64_t now)
{
    for (uint32_t i = 0; i < bytes && b->link == PbLinkUp; ++i) {
        const int r = pb_h4_rx_byte(&b->rx, data[i]);
        if (r == PbH4Ibs) {
            if (pb_ibs_rx(&b->ibs, b->rx.buf[0], now) & PbIbsSendAck) b->ack_due = 1;
        } else if (r == PbH4Frame) {
            on_frame(b, b->rx.buf, b->rx.have, now);
        }
    }
    flush_client(b);
}

/* 时间都是加出来比：客户端线程可能刚写过比 now 新的 connected_at，减法会回绕。 */
static void check_guards(PbBridge *b, uint64_t now)
{
    if (b->pending && now >= b->pending_at + PbBridgeCommandMs) trip(b, PbGuardCommand);
    const uint64_t from = b->connected_at > b->up_at ? b->connected_at : b->up_at;
    if (b->client && !b->ready && now >= from + PbBridgeInitMs) trip(b, PbGuardInit);
    if (b->failed_starts >= PbBridgeFailedStarts) trip(b, PbGuardStarts);
}

void pb_bridge_init(PbBridge *b, const PbBridgeIo *io, const uint8_t address[6], int stand_in)
{
    pb_zero(b, sizeof *b);
    b->io = io;
    b->link = stand_in ? PbLinkStandIn : PbLinkStarting;
    if (address) pb_copy(b->address, address, 6);
    pb_h4_rx_reset(&b->rx);
    pb_h4_tx_reset(&b->from_client);
    pb_ibs_init(&b->ibs, PbIbsTriesHal, 0);
}

int pb_bridge_connect(PbBridge *b, uint32_t unit, int pid)
{
    int rc = PbBridgeOk;
    lock(b);
    if (b->link == PbLinkDead) {
        b->refused++;
        note(b, NoteRefused, (uint32_t)pid, 0);
        rc = PbBridgeRefused;
    } else if (b->client) {
        rc = PbBridgeBusy;
    } else {
        b->client = 1;
        b->unit = unit;
        b->pid = pid;
        b->ready = b->ready_noted = 0;
        b->connected_at = now_ms(b);
        b->connections++;
        pb_h4_tx_reset(&b->from_client);
        ring_clear(&b->to_client);
        if (b->logged_connections < PbBridgeLogConnections) {
            b->logged_connections++;
            note(b, NoteConnect, (uint32_t)pid, unit);
        }
    }
    unlock(b);
    report(b);
    return rc;
}

void pb_bridge_disconnect(PbBridge *b, uint32_t unit)
{
    /* 还没发的全部作废。控制器上可能还有连接和扫描：发一条 HCI_Reset 让它停下，应答不上交。下一个客户端自己还会再发一条 */
    static const uint8_t reset[4] = {PbH4Cmd, (uint8_t)PbOpReset, (uint8_t)(PbOpReset >> 8), 0};
    lock(b);
    if (!b->client || b->unit != unit) {
        unlock(b);
        return;
    }
    b->client = 0;
    ring_clear(&b->to_client);
    ring_clear(&b->to_uart);
    if (b->link == PbLinkUp) {
        if (!b->ready) b->failed_starts++;
        b->own_reset = (uint8_t)ring_put(&b->to_uart, reset, sizeof reset);
        b->io->kick(b->io->ctx);
    }
    if (b->logged_connections < PbBridgeLogConnections) note(b, NoteGone, b->ready, b->failed_starts);
    b->io->wake_room(b->io->ctx);
    unlock(b);
    report(b);
}

void pb_bridge_write(PbBridge *b, uint32_t unit, const uint8_t *data, uint32_t bytes)
{
    lock(b);
    for (uint32_t i = 0; i < bytes && b->client && b->unit == unit; ++i)
        if (pb_h4_tx_byte(&b->from_client, data[i]) == PbH4Frame) from_client_packet(b);
    unlock(b);
    report(b);
}

void pb_bridge_up(PbBridge *b, const PbH4Rx *rx, const uint8_t *rest, uint32_t rest_bytes)
{
    lock(b);
    if (b->link == PbLinkStarting) {
        const uint64_t now = now_ms(b);
        if (rx) b->rx = *rx;
        b->link = PbLinkUp;
        b->up_at = b->active_at = now;
        note(b, NoteUp, ring_used(&b->to_uart), b->client);
        feed(b, rest, rest_bytes, now);
        b->io->kick(b->io->ctx);
    }
    unlock(b);
    report(b);
}

void pb_bridge_fail(PbBridge *b)
{
    lock(b);
    trip(b, PbGuardBringup);
    unlock(b);
    report(b);
}

int pb_bridge_step(PbBridge *b)
{
    const PbBridgeIo *io = b->io;
    int moved = 0;
    lock(b);
    if (b->link != PbLinkUp) {
        const int stop = b->link == PbLinkDead;
        unlock(b);
        return stop ? PbBridgeStop : PbBridgeIdlePollMs;
    }
    const uint32_t bytes = ring_peek(&b->to_uart, b->out);
    if (bytes) {
        ring_drop(&b->to_uart, bytes);
        io->wake_room(io->ctx);
    }
    unlock(b);
    if (bytes) {
        /* 这里写的时候不取串口：单线程，靠 RFR 硬件流控顶住，一包在 8 Mbps 下不到 1.5 ms */
        const int rc = io->uart_write(io->ctx, b->out, bytes, PbBridgeWriteMs);
        const uint64_t now = now_ms(b);
        lock(b);
        if (rc) trip(b, PbGuardWrite);
        else after_write(b, now);
        unlock(b);
        moved = 1;
    }
    for (unsigned round = 0; round < PbBridgeReadRounds; ++round) {
        lock(b);
        const int stopped = b->link != PbLinkUp;
        const int room = ring_free(&b->to_client) >= PbBridgeReadRoom;
        if (!stopped && !room) {
            b->rx_paused++;
            flush_client(b);
        }
        unlock(b);
        if (stopped || !room) break;
        const uint32_t got = io->uart_read(io->ctx, b->in, sizeof b->in);
        if (!got) break;
        const uint64_t now = now_ms(b);
        lock(b);
        feed(b, b->in, got, now);
        const int ack = b->ack_due;
        b->ack_due = 0;
        unlock(b);
        moved = 1;
        if (ack) {
            /* 帧边界上才发得：这时候发送方向没有包在途 */
            const uint8_t byte = PbIbsWakeAck;
            const int rc = io->uart_write(io->ctx, &byte, 1, PbBridgeWriteMs);
            if (rc) {
                lock(b);
                trip(b, PbGuardWrite);
                unlock(b);
            }
        }
    }
    const uint64_t now = now_ms(b);
    int sleep_ms;
    lock(b);
    if (moved) b->active_at = now;
    flush_client(b);
    if (b->link == PbLinkUp) check_guards(b, now);
    if (b->link != PbLinkUp) sleep_ms = PbBridgeStop;
    else if (ring_used(&b->to_uart)) sleep_ms = 0;
    else if (b->pending || now < b->active_at + PbBridgeActiveMs) sleep_ms = PbBridgeBusyPollMs;
    else sleep_ms = b->client ? PbBridgeIdlePollMs : PbBridgeNoClientPollMs;
    unlock(b);
    report(b);
    return sleep_ms;
}
