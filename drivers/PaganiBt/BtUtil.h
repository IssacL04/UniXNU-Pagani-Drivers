/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* 可移植源码的小工具：内核（没有 C 库）和主机测试都能编。文本拼接用来生成单行日志 "[bt] ..."，绝不写出缓冲区，也总是留下结尾 0。
 * 取自 PaganiWifi WifiUtil.h，名字改成 pb_ 前缀（两个驱动在同一个归档里）。 */
#ifndef PAGANI_BT_UTIL_H
#define PAGANI_BT_UTIL_H
#include <stddef.h>
#include <stdint.h>

static inline void pb_copy(void *to, const void *from, size_t n)
{
    uint8_t *d = (uint8_t *)to;
    const uint8_t *s = (const uint8_t *)from;
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
}
static inline void pb_zero(void *to, size_t n)
{
    uint8_t *d = (uint8_t *)to;
    for (size_t i = 0; i < n; ++i) d[i] = 0;
}
static inline size_t pb_strlen(const char *s, size_t max)
{
    size_t n = 0;
    while (s && n < max && s[n]) ++n;
    return n;
}
static inline int pb_streq(const char *a, const char *b)
{
    size_t i = 0;
    if (!a || !b) return 0;
    for (; a[i] && b[i]; ++i) if (a[i] != b[i]) return 0;
    return a[i] == b[i];
}
/* 在 hay 的前 n 字节里找 needle（以 0 结尾）。 */
static inline int pb_contains(const uint8_t *hay, size_t n, const char *needle)
{
    const size_t m = pb_strlen(needle, 64);
    if (!m || n < m) return 0;
    for (size_t i = 0; i + m <= n; ++i) {
        size_t k = 0;
        while (k < m && hay[i + k] == (uint8_t)needle[k]) ++k;
        if (k == m) return 1;
    }
    return 0;
}

typedef struct {
    char *buf;
    size_t size, used;
} PbText;

static inline void pb_text_start(PbText *t, char *buf, size_t size)
{
    t->buf = buf;
    t->size = size;
    t->used = 0;
    if (size) buf[0] = 0;
}
static inline void pb_text_char(PbText *t, char c)
{
    if (t->used + 1 < t->size) {
        t->buf[t->used++] = c;
        t->buf[t->used] = 0;
    }
}
static inline void pb_text_str(PbText *t, const char *s)
{
    for (size_t i = 0; s && s[i]; ++i) pb_text_char(t, s[i]);
}
/* 最多 n 个字符，不可打印的写成 '.'。 */
static inline void pb_text_printable(PbText *t, const uint8_t *s, size_t n)
{
    for (size_t i = 0; s && i < n && s[i]; ++i) pb_text_char(t, s[i] >= 32 && s[i] < 127 ? (char)s[i] : '.');
}
static inline void pb_text_udec(PbText *t, uint64_t v)
{
    char digits[20];
    unsigned n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < sizeof digits);
    while (n) pb_text_char(t, digits[--n]);
}
static inline void pb_text_dec(PbText *t, int64_t v)
{
    if (v < 0) {
        pb_text_char(t, '-');
        pb_text_udec(t, (uint64_t)0 - (uint64_t)v);
    } else {
        pb_text_udec(t, (uint64_t)v);
    }
}
/* 带 0x 前缀，不补零。 */
static inline void pb_text_hex(PbText *t, uint64_t v)
{
    char digits[16];
    unsigned n = 0;
    do {
        digits[n++] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v && n < sizeof digits);
    pb_text_str(t, "0x");
    while (n) pb_text_char(t, digits[--n]);
}
/* " key=" 加值。 */
static inline void pb_text_kd(PbText *t, const char *key, int64_t v)
{
    pb_text_char(t, ' ');
    pb_text_str(t, key);
    pb_text_char(t, '=');
    pb_text_dec(t, v);
}
static inline void pb_text_kx(PbText *t, const char *key, uint64_t v)
{
    pb_text_char(t, ' ');
    pb_text_str(t, key);
    pb_text_char(t, '=');
    pb_text_hex(t, v);
}
/* 字节按两位十六进制连写，空格分隔，写不下时截断。 */
static inline void pb_text_bytes(PbText *t, const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        pb_text_char(t, ' ');
        pb_text_char(t, "0123456789abcdef"[data[i] >> 4]);
        pb_text_char(t, "0123456789abcdef"[data[i] & 15]);
    }
}
#endif
