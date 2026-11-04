/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Small helpers of the portable ADSP sources: they build for the kernel (no C library) and for the host tests alike. The text builder
 * makes the one-line log records ("[adsp] ..."): it never writes past its buffer and always leaves it terminated. */
#ifndef PAGANI_ADSP_UTIL_H
#define PAGANI_ADSP_UTIL_H
#include <stddef.h>
#include <stdint.h>

static inline void pagani_copy(void *to, const void *from, size_t n)
{
    uint8_t *d = (uint8_t *)to;
    const uint8_t *s = (const uint8_t *)from;
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
}
static inline void pagani_zero(void *to, size_t n)
{
    uint8_t *d = (uint8_t *)to;
    for (size_t i = 0; i < n; ++i) d[i] = 0;
}
static inline size_t pagani_strlen(const char *s, size_t max)
{
    size_t n = 0;
    while (s && n < max && s[n]) ++n;
    return n;
}
static inline int pagani_streq(const char *a, const char *b)
{
    size_t i = 0;
    if (!a || !b) return 0;
    for (; a[i] && b[i]; ++i) if (a[i] != b[i]) return 0;
    return a[i] == b[i];
}
/* Copies at most size - 1 characters and terminates. */
static inline void pagani_strcpy(char *to, size_t size, const char *from)
{
    size_t i = 0;
    if (!size) return;
    for (; from && from[i] && i + 1 < size; ++i) to[i] = from[i];
    to[i] = 0;
}

typedef struct {
    char *buf;
    size_t size, used;
} PaganiText;

static inline void pagani_text_start(PaganiText *t, char *buf, size_t size)
{
    t->buf = buf;
    t->size = size;
    t->used = 0;
    if (size) buf[0] = 0;
}
static inline void pagani_text_char(PaganiText *t, char c)
{
    if (t->used + 1 < t->size) {
        t->buf[t->used++] = c;
        t->buf[t->used] = 0;
    }
}
static inline void pagani_text_str(PaganiText *t, const char *s)
{
    for (size_t i = 0; s && s[i]; ++i) pagani_text_char(t, s[i]);
}
/* At most n characters of s, nonprintable ones as '.'. */
static inline void pagani_text_printable(PaganiText *t, const char *s, size_t n)
{
    for (size_t i = 0; s && i < n && s[i]; ++i) pagani_text_char(t, s[i] >= 32 && s[i] < 127 ? s[i] : '.');
}
static inline void pagani_text_udec(PaganiText *t, uint64_t v)
{
    char digits[20];
    unsigned n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < sizeof digits);
    while (n) pagani_text_char(t, digits[--n]);
}
static inline void pagani_text_dec(PaganiText *t, int64_t v)
{
    if (v < 0) {
        pagani_text_char(t, '-');
        pagani_text_udec(t, (uint64_t)0 - (uint64_t)v);
    } else {
        pagani_text_udec(t, (uint64_t)v);
    }
}
/* 0x-prefixed, no leading zeros. */
static inline void pagani_text_hex(PaganiText *t, uint64_t v)
{
    char digits[16];
    unsigned n = 0;
    do {
        digits[n++] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v && n < sizeof digits);
    pagani_text_str(t, "0x");
    while (n) pagani_text_char(t, digits[--n]);
}
/* " key=" then the value. */
static inline void pagani_text_kd(PaganiText *t, const char *key, int64_t v)
{
    pagani_text_char(t, ' ');
    pagani_text_str(t, key);
    pagani_text_char(t, '=');
    pagani_text_dec(t, v);
}
static inline void pagani_text_kx(PaganiText *t, const char *key, uint64_t v)
{
    pagani_text_char(t, ' ');
    pagani_text_str(t, key);
    pagani_text_char(t, '=');
    pagani_text_hex(t, v);
}
static inline void pagani_text_ks(PaganiText *t, const char *key, const char *s)
{
    pagani_text_char(t, ' ');
    pagani_text_str(t, key);
    pagani_text_char(t, '=');
    pagani_text_str(t, s);
}
#endif
