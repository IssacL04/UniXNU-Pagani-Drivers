/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Helpers of the portable haptics sources (no C library: they build for the kernel and the host tests alike). The text builder makes
 * the one-line log records ("[haptics] ..."), never writes past its buffer and always leaves it terminated (as PaganiBattery's
 * BatteryText.h). */
#ifndef PAGANI_HAPTICS_TEXT_H
#define PAGANI_HAPTICS_TEXT_H
#include <stddef.h>
#include <stdint.h>

static inline void pgh_zero(void *to, size_t n)
{
    uint8_t *d = (uint8_t *)to;
    for (size_t i = 0; i < n; ++i) d[i] = 0;
}

typedef struct {
    char *buf;
    size_t size, used;
} PghText;

static inline void pgh_text_start(PghText *t, char *buf, size_t size)
{
    t->buf = buf;
    t->size = size;
    t->used = 0;
    if (size) buf[0] = 0;
}
static inline void pgh_text_char(PghText *t, char c)
{
    if (t->used + 1 < t->size) {
        t->buf[t->used++] = c;
        t->buf[t->used] = 0;
    }
}
static inline void pgh_text_str(PghText *t, const char *s)
{
    for (size_t i = 0; s && s[i]; ++i) pgh_text_char(t, s[i]);
}
static inline void pgh_text_udec(PghText *t, uint64_t v)
{
    char digits[20];
    unsigned n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < sizeof digits);
    while (n) pgh_text_char(t, digits[--n]);
}
static inline void pgh_text_dec(PghText *t, int64_t v)
{
    if (v < 0) {
        pgh_text_char(t, '-');
        pgh_text_udec(t, (uint64_t)(-(v + 1)) + 1);
    } else {
        pgh_text_udec(t, (uint64_t)v);
    }
}
static inline void pgh_text_hex2(PghText *t, unsigned v)
{
    static const char hex[] = "0123456789abcdef";
    pgh_text_char(t, hex[(v >> 4) & 15]);
    pgh_text_char(t, hex[v & 15]);
}
static inline void pgh_text_hex(PghText *t, uint64_t v)
{
    static const char hex[] = "0123456789abcdef";
    char digits[16];
    unsigned n = 0;
    pgh_text_str(t, "0x");
    do {
        digits[n++] = hex[v & 15];
        v >>= 4;
    } while (v && n < sizeof digits);
    while (n) pgh_text_char(t, digits[--n]);
}
/* " key=value" fields */
static inline void pgh_text_kd(PghText *t, const char *key, int64_t v)
{
    pgh_text_char(t, ' ');
    pgh_text_str(t, key);
    pgh_text_char(t, '=');
    pgh_text_dec(t, v);
}
static inline void pgh_text_kx(PghText *t, const char *key, uint64_t v)
{
    pgh_text_char(t, ' ');
    pgh_text_str(t, key);
    pgh_text_char(t, '=');
    pgh_text_hex(t, v);
}
static inline void pgh_text_ks(PghText *t, const char *key, const char *v)
{
    pgh_text_char(t, ' ');
    pgh_text_str(t, key);
    pgh_text_char(t, '=');
    pgh_text_str(t, v);
}

#endif
