/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Small helpers of the portable Wi-Fi sources: they build for the kernel (no C library) and for the host tests alike. The text builder
 * makes the one-line log records ("[wifi] ..."): it never writes past its buffer and always leaves it terminated. Taken from
 * PaganiAdsp AdspUtil.h with the names prefixed pw_ (both may be included in one build). */
#ifndef PAGANI_WIFI_UTIL_H
#define PAGANI_WIFI_UTIL_H
#include <stddef.h>
#include <stdint.h>

static inline void pw_copy(void *to, const void *from, size_t n)
{
    uint8_t *d = (uint8_t *)to;
    const uint8_t *s = (const uint8_t *)from;
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
}
static inline void pw_zero(void *to, size_t n)
{
    uint8_t *d = (uint8_t *)to;
    for (size_t i = 0; i < n; ++i) d[i] = 0;
}
/* A 32-bit register at offset off inside a mapped window of size bytes: aligned, whole, and no wrap for an offset read from a device. */
static inline int pw_reg_ok(uint32_t off, uint64_t size) {return !(off & 3) && (uint64_t)off + 4 <= size;}

static inline size_t pw_strlen(const char *s, size_t max)
{
    size_t n = 0;
    while (s && n < max && s[n]) ++n;
    return n;
}
static inline int pw_streq(const char *a, const char *b)
{
    size_t i = 0;
    if (!a || !b) return 0;
    for (; a[i] && b[i]; ++i) if (a[i] != b[i]) return 0;
    return a[i] == b[i];
}
/* Copies at most size - 1 characters and terminates. */
static inline void pw_strcpy(char *to, size_t size, const char *from)
{
    size_t i = 0;
    if (!size) return;
    for (; from && from[i] && i + 1 < size; ++i) to[i] = from[i];
    to[i] = 0;
}

/* Which of the stack's lines go to the boot log as well as to its own ring. The log's events ring holds 32 KiB, about 220 lines, and the UFS
 * log takes a copy of it every 3 s (pagani-ufslog-ms: down to 1 s). Errors (level 3 and below) always go; lines up to `level` until
 * PW_LOG_LINES of them went; debug lines (level 7, with pagani-wifi-log=7) from a bucket of PW_LOG_DEBUG_BURST lines, which a QMI session
 * fits in, refilled by one every PW_LOG_DEBUG_MS, at most PW_LOG_DEBUG_LINES in all. A line held back stays in the ring and is counted. */
#define PW_LOG_LINES 600u
#define PW_LOG_DEBUG_BURST 200u
#define PW_LOG_DEBUG_MS 16u
#define PW_LOG_DEBUG_LINES 3000u
typedef struct {
    uint32_t level, traced, debug_traced, held_back, tokens;
    uint64_t refilled_ms;
} PwLogGate;
static inline void pw_log_gate_init(PwLogGate *g, uint32_t level, uint64_t now_ms)
{
    pw_zero(g, sizeof *g);
    g->level = level;
    g->tokens = PW_LOG_DEBUG_BURST;
    g->refilled_ms = now_ms;
}
/* 1 when a line of this level goes to the boot log now. */
static inline int pw_log_gate(PwLogGate *g, int line_level, uint64_t now_ms)
{
    if (line_level > (int)g->level) return 0;
    if (line_level >= 7) {
        const uint64_t earned = (now_ms - g->refilled_ms) / PW_LOG_DEBUG_MS;
        g->tokens = earned >= PW_LOG_DEBUG_BURST - g->tokens ? PW_LOG_DEBUG_BURST : g->tokens + (uint32_t)earned;
        g->refilled_ms += earned * PW_LOG_DEBUG_MS;
        if (!g->tokens || g->debug_traced >= PW_LOG_DEBUG_LINES) {
            g->held_back++;
            return 0;
        }
        g->tokens--;
        g->debug_traced++;
        return 1;
    }
    if (line_level > 3 && g->traced >= PW_LOG_LINES) return 0;
    g->traced++;
    return 1;
}

typedef struct {
    char *buf;
    size_t size, used;
} PwText;

static inline void pw_text_start(PwText *t, char *buf, size_t size)
{
    t->buf = buf;
    t->size = size;
    t->used = 0;
    if (size) buf[0] = 0;
}
static inline void pw_text_char(PwText *t, char c)
{
    if (t->used + 1 < t->size) {
        t->buf[t->used++] = c;
        t->buf[t->used] = 0;
    }
}
static inline void pw_text_str(PwText *t, const char *s)
{
    for (size_t i = 0; s && s[i]; ++i) pw_text_char(t, s[i]);
}
/* At most n characters of s, nonprintable ones as '.'. */
static inline void pw_text_printable(PwText *t, const char *s, size_t n)
{
    for (size_t i = 0; s && i < n && s[i]; ++i) pw_text_char(t, s[i] >= 32 && s[i] < 127 ? s[i] : '.');
}
static inline void pw_text_udec(PwText *t, uint64_t v)
{
    char digits[20];
    unsigned n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < sizeof digits);
    while (n) pw_text_char(t, digits[--n]);
}
static inline void pw_text_dec(PwText *t, int64_t v)
{
    if (v < 0) {
        pw_text_char(t, '-');
        pw_text_udec(t, (uint64_t)0 - (uint64_t)v);
    } else {
        pw_text_udec(t, (uint64_t)v);
    }
}
/* 0x-prefixed, no leading zeros. */
static inline void pw_text_hex(PwText *t, uint64_t v)
{
    char digits[16];
    unsigned n = 0;
    do {
        digits[n++] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v && n < sizeof digits);
    pw_text_str(t, "0x");
    while (n) pw_text_char(t, digits[--n]);
}
/* " key=" then the value. */
static inline void pw_text_kd(PwText *t, const char *key, int64_t v)
{
    pw_text_char(t, ' ');
    pw_text_str(t, key);
    pw_text_char(t, '=');
    pw_text_dec(t, v);
}
static inline void pw_text_kx(PwText *t, const char *key, uint64_t v)
{
    pw_text_char(t, ' ');
    pw_text_str(t, key);
    pw_text_char(t, '=');
    pw_text_hex(t, v);
}
static inline void pw_text_ks(PwText *t, const char *key, const char *s)
{
    pw_text_char(t, ' ');
    pw_text_str(t, key);
    pw_text_char(t, '=');
    pw_text_str(t, s);
}
#endif
