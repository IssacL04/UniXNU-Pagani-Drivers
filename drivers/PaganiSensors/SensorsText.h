/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* One-line log records ("[sensors] ..." BootProgress ramoops U lines): a bounded text builder for kernel and host alike (no C library).
 * It never writes past its buffer and always leaves it terminated. */
#ifndef SENSORS_TEXT_H
#define SENSORS_TEXT_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *buf;
    size_t size, used;
} SntText;

static inline void snt_start(SntText *t, char *buf, size_t size)
{
    t->buf = buf;
    t->size = size;
    t->used = 0;
    if (size) buf[0] = 0;
}
static inline void snt_char(SntText *t, char c)
{
    if (t->used + 1 < t->size) {
        t->buf[t->used++] = c;
        t->buf[t->used] = 0;
    }
}
static inline void snt_str(SntText *t, const char *s)
{
    for (size_t i = 0; s && s[i]; ++i) snt_char(t, s[i]);
}
static inline void snt_udec(SntText *t, uint64_t v)
{
    char digits[20];
    unsigned n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < sizeof digits);
    while (n) snt_char(t, digits[--n]);
}
static inline void snt_dec(SntText *t, int64_t v)
{
    if (v < 0) {
        snt_char(t, '-');
        snt_udec(t, (uint64_t)0 - (uint64_t)v);
    } else {
        snt_udec(t, (uint64_t)v);
    }
}
static inline void snt_hex(SntText *t, uint64_t v)
{
    char digits[16];
    unsigned n = 0;
    do {
        digits[n++] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v && n < sizeof digits);
    snt_str(t, "0x");
    while (n) snt_char(t, digits[--n]);
}
/* A fixed-point value with three decimals (milli-units), "-1.250". */
static inline void snt_milli(SntText *t, int64_t milli)
{
    if (milli < 0) {
        snt_char(t, '-');
        milli = -milli;
    }
    snt_udec(t, (uint64_t)milli / 1000);
    snt_char(t, '.');
    const unsigned frac = (unsigned)((uint64_t)milli % 1000);
    snt_char(t, (char)('0' + frac / 100));
    snt_char(t, (char)('0' + frac / 10 % 10));
    snt_char(t, (char)('0' + frac % 10));
}
static inline void snt_kd(SntText *t, const char *key, int64_t v)
{
    snt_char(t, ' ');
    snt_str(t, key);
    snt_char(t, '=');
    snt_dec(t, v);
}
static inline void snt_kx(SntText *t, const char *key, uint64_t v)
{
    snt_char(t, ' ');
    snt_str(t, key);
    snt_char(t, '=');
    snt_hex(t, v);
}
static inline void snt_ks(SntText *t, const char *key, const char *s)
{
    snt_char(t, ' ');
    snt_str(t, key);
    snt_char(t, '=');
    snt_str(t, s);
}
static inline void snt_km(SntText *t, const char *key, int64_t milli)
{
    snt_char(t, ' ');
    snt_str(t, key);
    snt_char(t, '=');
    snt_milli(t, milli);
}
#endif
