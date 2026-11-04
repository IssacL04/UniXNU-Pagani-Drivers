/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Protocol buffers wire format: see SensorsPb.h. */
#include "SensorsPb.h"

/* ---- encoder ---- */
void spb_out(SpbOut *o, uint8_t *buf, uint32_t cap)
{
    o->buf = buf;
    o->cap = buf ? cap : 0;
    o->len = 0;
    o->overflow = 0;
}

static void byte(SpbOut *o, uint8_t value)
{
    if (o->overflow || o->len >= o->cap) {o->overflow = 1; return;}
    o->buf[o->len++] = value;
}

static unsigned varint_size(uint64_t value)
{
    unsigned n = 1;
    while (value >= 0x80) {value >>= 7; ++n;}
    return n;
}

void spb_varint(SpbOut *o, uint64_t value)
{
    while (value >= 0x80) {
        byte(o, (uint8_t)(value | 0x80));
        value >>= 7;
    }
    byte(o, (uint8_t)value);
}

void spb_tag(SpbOut *o, uint32_t field, uint32_t wire)
{
    spb_varint(o, (uint64_t)field << 3 | (wire & 7));
}

void spb_put_varint(SpbOut *o, uint32_t field, uint64_t value)
{
    spb_tag(o, field, SPB_VARINT);
    spb_varint(o, value);
}

void spb_put_int(SpbOut *o, uint32_t field, int64_t value)
{
    spb_tag(o, field, SPB_VARINT);
    spb_varint(o, (uint64_t)value);   /* two's complement: a negative int32/enum is ten bytes */
}

void spb_put_fixed32(SpbOut *o, uint32_t field, uint32_t value)
{
    spb_tag(o, field, SPB_I32);
    for (unsigned i = 0; i < 4; ++i) byte(o, (uint8_t)(value >> (8 * i)));
}

void spb_put_fixed64(SpbOut *o, uint32_t field, uint64_t value)
{
    spb_tag(o, field, SPB_I64);
    for (unsigned i = 0; i < 8; ++i) byte(o, (uint8_t)(value >> (8 * i)));
}

void spb_put_bytes(SpbOut *o, uint32_t field, const void *data, uint32_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    spb_tag(o, field, SPB_LEN);
    spb_varint(o, length);
    for (uint32_t i = 0; i < length && bytes; ++i) byte(o, bytes[i]);
    if (length && !bytes) o->overflow = 1;
}

void spb_put_string(SpbOut *o, uint32_t field, const char *text)
{
    uint32_t n = 0;
    while (text && text[n] && n < 0xffffu) ++n;
    spb_put_bytes(o, field, text, n);
}

uint32_t spb_open(SpbOut *o, uint32_t field)
{
    spb_tag(o, field, SPB_LEN);
    const uint32_t mark = o->len;
    byte(o, 0);   /* the length, one byte for now */
    return o->overflow ? 0xffffffffu : mark;
}

void spb_close(SpbOut *o, uint32_t mark)
{
    if (o->overflow || mark == 0xffffffffu || mark >= o->len) {o->overflow = 1; return;}
    const uint32_t length = o->len - mark - 1;
    const unsigned need = varint_size(length);
    if (need > 1) {
        const uint32_t extra = need - 1;
        if (o->len + extra > o->cap) {o->overflow = 1; return;}
        for (uint32_t i = o->len; i > mark + 1; --i) o->buf[i - 1 + extra] = o->buf[i - 1];
        o->len += extra;
    }
    uint64_t value = length;
    for (unsigned i = 0; i < need; ++i) {
        o->buf[mark + i] = (uint8_t)((value & 0x7f) | (i + 1 < need ? 0x80 : 0));
        value >>= 7;
    }
}

uint32_t spb_done(const SpbOut *o)
{
    return o->overflow ? 0 : o->len;
}

/* ---- decoder ---- */
void spb_in(SpbIn *in, const uint8_t *data, uint32_t length)
{
    in->p = data;
    in->end = data ? data + length : data;
    in->error = 0;
}

static int fail(SpbIn *in)
{
    in->error = 1;
    return -1;
}

static int read_varint(SpbIn *in, uint64_t *value)
{
    uint64_t v = 0;
    for (unsigned shift = 0; shift < 70; shift += 7) {
        if (in->p >= in->end) return fail(in);
        const uint8_t b = *in->p++;
        if (shift == 63 && b > 1) return fail(in);   /* more than 64 bits */
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) {*value = v; return 0;}
    }
    return fail(in);
}

int spb_next(SpbIn *in, uint32_t *field, uint32_t *wire)
{
    uint64_t tag = 0;
    if (in->error) return -1;
    if (in->p >= in->end) return 0;
    if (read_varint(in, &tag)) return -1;
    if (tag >> 3 == 0 || tag >> 3 > 0x1fffffffu) return fail(in);
    *field = (uint32_t)(tag >> 3);
    *wire = (uint32_t)(tag & 7);
    return 1;
}

int spb_varint_value(SpbIn *in, uint32_t wire, uint64_t *value)
{
    if (wire != SPB_VARINT) return fail(in);
    return read_varint(in, value);
}

int spb_fixed32_value(SpbIn *in, uint32_t wire, uint32_t *value)
{
    if (wire != SPB_I32 || in->end - in->p < 4) return fail(in);
    *value = (uint32_t)in->p[0] | (uint32_t)in->p[1] << 8 | (uint32_t)in->p[2] << 16 | (uint32_t)in->p[3] << 24;
    in->p += 4;
    return 0;
}

int spb_fixed64_value(SpbIn *in, uint32_t wire, uint64_t *value)
{
    if (wire != SPB_I64 || in->end - in->p < 8) return fail(in);
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)in->p[i] << (8 * i);
    *value = v;
    in->p += 8;
    return 0;
}

int spb_bytes_value(SpbIn *in, uint32_t wire, const uint8_t **data, uint32_t *length)
{
    uint64_t n = 0;
    if (wire != SPB_LEN || read_varint(in, &n)) return fail(in);
    if (n > (uint64_t)(in->end - in->p)) return fail(in);
    *data = in->p;
    *length = (uint32_t)n;
    in->p += n;
    return 0;
}

int spb_skip(SpbIn *in, uint32_t wire)
{
    uint64_t v = 0;
    const uint8_t *data = 0;
    uint32_t n = 0;
    switch (wire) {
    case SPB_VARINT: return read_varint(in, &v);
    case SPB_I64: if (in->end - in->p < 8) return fail(in); in->p += 8; return 0;
    case SPB_I32: if (in->end - in->p < 4) return fail(in); in->p += 4; return 0;
    case SPB_LEN: return spb_bytes_value(in, wire, &data, &n);
    default: return fail(in);   /* groups (3, 4) and the reserved wire types */
    }
}

int spb_fixed32_values(SpbIn *in, uint32_t wire, uint32_t *out, unsigned max, unsigned *count, unsigned *more)
{
    uint32_t value = 0;
    if (wire == SPB_I32) {
        if (spb_fixed32_value(in, wire, &value)) return -1;
        if (*count < max) out[(*count)++] = value;
        else if (more) (*more)++;
        return 0;
    }
    const uint8_t *data = 0;
    uint32_t n = 0;
    if (wire != SPB_LEN || spb_bytes_value(in, wire, &data, &n) || n % 4) return fail(in);
    for (uint32_t i = 0; i < n; i += 4) {
        value = (uint32_t)data[i] | (uint32_t)data[i + 1] << 8 | (uint32_t)data[i + 2] << 16 | (uint32_t)data[i + 3] << 24;
        if (*count < max) out[(*count)++] = value;
        else if (more) (*more)++;
    }
    return 0;
}

int spb_varint_values(SpbIn *in, uint32_t wire, int64_t *out, unsigned max, unsigned *count, unsigned *more)
{
    uint64_t value = 0;
    if (wire == SPB_VARINT) {
        if (read_varint(in, &value)) return -1;
        if (*count < max) out[(*count)++] = (int64_t)value;
        else if (more) (*more)++;
        return 0;
    }
    const uint8_t *data = 0;
    uint32_t n = 0;
    if (wire != SPB_LEN || spb_bytes_value(in, wire, &data, &n)) return fail(in);
    SpbIn packed;
    spb_in(&packed, data, n);
    while (packed.p < packed.end) {
        if (read_varint(&packed, &value)) return fail(in);
        if (*count < max) out[(*count)++] = (int64_t)value;
        else if (more) (*more)++;
    }
    return 0;
}
