/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Small helpers of the portable battery sources (PaganiBattery): they build for the kernel (no C library) and for the host tests alike.
 * The text builder makes the one-line log records ("[battery] ..."): it never writes past its buffer and always leaves it terminated. */
#ifndef PAGANI_BATTERY_TEXT_H
#define PAGANI_BATTERY_TEXT_H
#include <stddef.h>
#include <stdint.h>

static inline void pbat_zero(void *to, size_t n)
{
    uint8_t *d = (uint8_t *)to;
    for (size_t i = 0; i < n; ++i) d[i] = 0;
}
static inline void pbat_copy(void *to, const void *from, size_t n)
{
    uint8_t *d = (uint8_t *)to;
    const uint8_t *s = (const uint8_t *)from;
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
}
static inline void pbat_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline uint32_t pbat_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

typedef struct {
    char *buf;
    size_t size, used;
} PbatText;

static inline void pbat_text_start(PbatText *t, char *buf, size_t size)
{
    t->buf = buf;
    t->size = size;
    t->used = 0;
    if (size) buf[0] = 0;
}
static inline void pbat_text_char(PbatText *t, char c)
{
    if (t->used + 1 < t->size) {
        t->buf[t->used++] = c;
        t->buf[t->used] = 0;
    }
}
static inline void pbat_text_str(PbatText *t, const char *s)
{
    for (size_t i = 0; s && s[i]; ++i) pbat_text_char(t, s[i]);
}
static inline void pbat_text_udec(PbatText *t, uint64_t v)
{
    char digits[20];
    unsigned n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < sizeof digits);
    while (n) pbat_text_char(t, digits[--n]);
}
static inline void pbat_text_dec(PbatText *t, int64_t v)
{
    if (v < 0) {
        pbat_text_char(t, '-');
        pbat_text_udec(t, (uint64_t)(-(v + 1)) + 1);
    } else {
        pbat_text_udec(t, (uint64_t)v);
    }
}
static inline void pbat_text_hex(PbatText *t, uint64_t v)
{
    static const char hex[] = "0123456789abcdef";
    char digits[16];
    unsigned n = 0;
    pbat_text_str(t, "0x");
    do {
        digits[n++] = hex[v & 15];
        v >>= 4;
    } while (v && n < sizeof digits);
    while (n) pbat_text_char(t, digits[--n]);
}
/* " key=value" fields. */
static inline void pbat_text_kd(PbatText *t, const char *key, int64_t v)
{
    pbat_text_char(t, ' ');
    pbat_text_str(t, key);
    pbat_text_char(t, '=');
    pbat_text_dec(t, v);
}
static inline void pbat_text_kx(PbatText *t, const char *key, uint64_t v)
{
    pbat_text_char(t, ' ');
    pbat_text_str(t, key);
    pbat_text_char(t, '=');
    pbat_text_hex(t, v);
}
static inline void pbat_text_ks(PbatText *t, const char *key, const char *v)
{
    pbat_text_char(t, ' ');
    pbat_text_str(t, key);
    pbat_text_char(t, '=');
    pbat_text_str(t, v);
}
/* Hundredths as "12.34" (temperatures: hundredths of a degree). */
static inline void pbat_text_centi(PbatText *t, const char *key, int64_t centi)
{
    pbat_text_char(t, ' ');
    pbat_text_str(t, key);
    pbat_text_char(t, '=');
    if (centi < 0) {
        pbat_text_char(t, '-');
        centi = -centi;
    }
    pbat_text_udec(t, (uint64_t)centi / 100);
    pbat_text_char(t, '.');
    const unsigned frac = (unsigned)((uint64_t)centi % 100);
    pbat_text_char(t, (char)('0' + frac / 10));
    pbat_text_char(t, (char)('0' + frac % 10));
}

#endif
