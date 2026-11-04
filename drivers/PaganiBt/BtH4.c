/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
#include "BtH4.h"
#include "BtTlv.h"

void pb_h4_rx_reset(PbH4Rx *r)
{
    r->have = r->need = r->skip = 0;
    r->junk = r->oversize = r->frames = 0;
}

/* 头收到第几个字节时能算出总长。 */
static uint32_t header_bytes(uint8_t type)
{
    switch (type) {
    case PbH4Evt: return 3;                 /* 04 evt plen */
    case PbH4Sco: return 4;                 /* 03 handle2 plen */
    case PbH4Acl: return 5;                 /* 02 handle2 len2 */
    case PbH4Iso: return 5;                 /* 05 handle2 len2（14 位） */
    case PbH4Peri: return 4;                /* 34 host evt len */
    default: return 0;
    }
}
static uint32_t total_bytes(const uint8_t *b)
{
    switch (b[0]) {
    case PbH4Evt: return 3u + b[2];
    case PbH4Sco: return 4u + b[3];
    case PbH4Acl: return 5u + (uint32_t)(b[3] | (uint32_t)b[4] << 8);
    case PbH4Iso: return 5u + ((uint32_t)(b[3] | (uint32_t)b[4] << 8) & 0x3fffu);
    default: return 4u + b[3];              /* 0x34 */
    }
}

int pb_h4_rx_byte(PbH4Rx *r, uint8_t byte)
{
    if (r->skip) {
        r->skip--;
        return PbH4More;
    }
    if (r->need && r->have == r->need) r->have = r->need = 0;   /* 上一帧已经交出去了 */
    if (!r->have) {
        if (byte == PbIbsWakeAck || byte == PbIbsWake || byte == PbIbsSleep) {
            r->buf[0] = byte;
            return PbH4Ibs;
        }
        if (!header_bytes(byte)) {
            r->junk++;
            return PbH4Junk;
        }
    }
    r->buf[r->have++] = byte;
    if (!r->need && r->have == header_bytes(r->buf[0])) {
        r->need = total_bytes(r->buf);
        if (r->need > sizeof r->buf) {
            r->skip = r->need - r->have;
            r->oversize++;
            r->have = r->need = 0;
            return PbH4Oversize;
        }
    }
    if (r->need && r->have == r->need) {
        r->frames++;
        return PbH4Frame;
    }
    return PbH4More;
}

void pb_h4_tx_reset(PbH4Tx *t)
{
    t->have = t->need = t->skip = 0;
    t->junk = t->oversize = t->packets = 0;
}

static uint32_t tx_header_bytes(uint8_t type)
{
    switch (type) {
    case PbH4Cmd: return 4;                 /* 01 opcode2 plen */
    case PbH4Sco: return 4;                 /* 03 handle2 plen */
    case PbH4Acl: return 5;                 /* 02 handle2 len2 */
    default: return 0;
    }
}

int pb_h4_tx_byte(PbH4Tx *t, uint8_t byte)
{
    if (t->skip) {
        t->skip--;
        return PbH4More;
    }
    if (t->need && t->have == t->need) t->have = t->need = 0;   /* 上一包已经交出去了 */
    if (!t->have && !tx_header_bytes(byte)) {
        t->junk++;
        return PbH4Junk;
    }
    t->buf[t->have++] = byte;
    if (!t->need && t->have == tx_header_bytes(t->buf[0])) {
        t->need = t->buf[0] == PbH4Acl ? 5u + (uint32_t)(t->buf[3] | (uint32_t)t->buf[4] << 8) : 4u + t->buf[3];
        if (t->need > sizeof t->buf) {
            t->skip = t->need - t->have;
            t->oversize++;
            t->have = t->need = 0;
            return PbH4Oversize;
        }
    }
    if (t->need && t->have == t->need) {
        t->packets++;
        return PbH4Frame;
    }
    return PbH4More;
}

int pb_h4_filter_rx(const uint8_t *f, uint32_t bytes)
{
    if (!bytes) return PbFilterBad;
    switch (f[0]) {
    case PbH4Peri:
        return PbFilterDrop;
    case PbH4Evt:
        if (bytes < 3) return PbFilterBad;
        return f[1] == PbEvtVendor ? PbFilterDrop : PbFilterPass;
    case PbH4Acl:
        if (bytes < 5) return PbFilterBad;
        return (uint32_t)(f[1] | (uint32_t)f[2] << 8) == PbDebugHandle ? PbFilterDrop : PbFilterPass;
    case PbH4Sco:
        return PbFilterPass;
    default:
        return PbFilterBad;                 /* 含 ISO：bluetoothd 的 H4 接收不认类型 5，留在内核层挡掉 */
    }
}

int pb_h4_filter_tx(const uint8_t *p, uint32_t bytes, uint8_t *reply, uint32_t *reply_bytes, uint16_t *vendor_opcode)
{
    if (reply_bytes) *reply_bytes = 0;
    if (bytes < 2) return PbFilterBad;
    switch (p[0]) {
    case PbH4Cmd: {
        if (bytes < 4 || bytes != 4u + p[3]) return PbFilterBad;
        if ((p[2] & 0xfc) != 0xfc) return PbFilterPass;
        if (vendor_opcode) *vendor_opcode = (uint16_t)(p[1] | (uint16_t)p[2] << 8);
        if (reply && reply_bytes) {
            /* Command Complete：1 个命令额度，原命令号，状态 0x01（未知命令） */
            const uint8_t answer[7] = {PbH4Evt, PbEvtCommandComplete, 4, 1, p[1], p[2], 1};
            for (unsigned i = 0; i < sizeof answer; ++i) reply[i] = answer[i];
            *reply_bytes = sizeof answer;
        }
        return PbFilterAnswered;
    }
    case PbH4Acl:
        if (bytes < 5 || bytes != 5u + (uint32_t)(p[3] | (uint32_t)p[4] << 8)) return PbFilterBad;
        return PbFilterPass;
    case PbH4Sco:
        if (bytes < 4 || bytes != 4u + p[3]) return PbFilterBad;
        return PbFilterPass;
    default:
        return PbFilterBad;
    }
}
