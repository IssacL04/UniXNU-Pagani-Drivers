/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Helpers of the portable torch sources (no C library: they build for the kernel and the host tests alike), copied from PaganiHaptics'
 * HapticsText.h with this module's prefix. The text builder makes the one-line log records ("[torch] ..."), never writes past its
 * buffer and always leaves it terminated. */
#ifndef PAGANI_TORCH_TEXT_H
#define PAGANI_TORCH_TEXT_H
#include <stddef.h>
#include <stdint.h>

static inline void pgt_zero(void *to, size_t n)
{
    uint8_t *d = (uint8_t *)to;
    for (size_t i = 0; i < n; ++i) d[i] = 0;
}

typedef struct {
    char *buf;
    size_t size, used;
} PgtText;

static inline void pgt_text_start(PgtText *t, char *buf, size_t size)
{
    t->buf = buf;
    t->size = size;
    t->used = 0;
    if (size) buf[0] = 0;
}
static inline void pgt_text_char(PgtText *t, char c)
{
    if (t->used + 1 < t->size) {
        t->buf[t->used++] = c;
        t->buf[t->used] = 0;
    }
}
static inline void pgt_text_str(PgtText *t, const char *s)
{
    for (size_t i = 0; s && s[i]; ++i) pgt_text_char(t, s[i]);
}
static inline void pgt_text_udec(PgtText *t, uint64_t v)
{
    char digits[20];
    unsigned n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < sizeof digits);
    while (n) pgt_text_char(t, digits[--n]);
}
static inline void pgt_text_dec(PgtText *t, int64_t v)
{
    if (v < 0) {
        pgt_text_char(t, '-');
        pgt_text_udec(t, (uint64_t)(-(v + 1)) + 1);
    } else {
        pgt_text_udec(t, (uint64_t)v);
    }
}
static inline void pgt_text_hex2(PgtText *t, unsigned v)
{
    static const char hex[] = "0123456789abcdef";
    pgt_text_char(t, hex[(v >> 4) & 15]);
    pgt_text_char(t, hex[v & 15]);
}
static inline void pgt_text_hex(PgtText *t, uint64_t v)
{
    static const char hex[] = "0123456789abcdef";
    char digits[16];
    unsigned n = 0;
    pgt_text_str(t, "0x");
    do {
        digits[n++] = hex[v & 15];
        v >>= 4;
    } while (v && n < sizeof digits);
    while (n) pgt_text_char(t, digits[--n]);
}
/* " key=value" fields */
static inline void pgt_text_kd(PgtText *t, const char *key, int64_t v)
{
    pgt_text_char(t, ' ');
    pgt_text_str(t, key);
    pgt_text_char(t, '=');
    pgt_text_dec(t, v);
}
static inline void pgt_text_kx(PgtText *t, const char *key, uint64_t v)
{
    pgt_text_char(t, ' ');
    pgt_text_str(t, key);
    pgt_text_char(t, '=');
    pgt_text_hex(t, v);
}
static inline void pgt_text_ks(PgtText *t, const char *key, const char *v)
{
    pgt_text_char(t, ' ');
    pgt_text_str(t, key);
    pgt_text_char(t, '=');
    pgt_text_str(t, v);
}

#endif
