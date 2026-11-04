/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* 可移植源码的小工具：内核（没有 C 库）和主机测试都能编。文本拼接用来生成单行日志 "[cam] ..."，绝不写出缓冲区，也总是留下结尾 0。
 * 取自 PaganiBt BtUtil.h（它取自 PaganiWifi WifiUtil.h），名字改成 pcam_ 前缀（几个驱动在同一个归档里）。 */
#ifndef PAGANI_CAM_UTIL_H
#define PAGANI_CAM_UTIL_H
#include <stddef.h>
#include <stdint.h>

static inline void pcam_copy(void *to, const void *from, size_t n)
{
    uint8_t *d = (uint8_t *)to;
    const uint8_t *s = (const uint8_t *)from;
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
}
static inline void pcam_zero(void *to, size_t n)
{
    uint8_t *d = (uint8_t *)to;
    for (size_t i = 0; i < n; ++i) d[i] = 0;
}
static inline size_t pcam_strlen(const char *s, size_t max)
{
    size_t n = 0;
    while (s && n < max && s[n]) ++n;
    return n;
}
static inline int pcam_streq(const char *a, const char *b)
{
    size_t i = 0;
    if (!a || !b) return 0;
    for (; a[i] && b[i]; ++i) if (a[i] != b[i]) return 0;
    return a[i] == b[i];
}
/* 在 hay 的前 n 字节里找 needle（以 0 结尾）。 */
static inline int pcam_contains(const uint8_t *hay, size_t n, const char *needle)
{
    const size_t m = pcam_strlen(needle, 64);
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
} PcamText;

static inline void pcam_text_start(PcamText *t, char *buf, size_t size)
{
    t->buf = buf;
    t->size = size;
    t->used = 0;
    if (size) buf[0] = 0;
}
static inline void pcam_text_char(PcamText *t, char c)
{
    if (t->used + 1 < t->size) {
        t->buf[t->used++] = c;
        t->buf[t->used] = 0;
    }
}
static inline void pcam_text_str(PcamText *t, const char *s)
{
    for (size_t i = 0; s && s[i]; ++i) pcam_text_char(t, s[i]);
}
/* 最多 n 个字符，不可打印的写成 '.'。 */
static inline void pcam_text_printable(PcamText *t, const uint8_t *s, size_t n)
{
    for (size_t i = 0; s && i < n && s[i]; ++i) pcam_text_char(t, s[i] >= 32 && s[i] < 127 ? (char)s[i] : '.');
}
static inline void pcam_text_udec(PcamText *t, uint64_t v)
{
    char digits[20];
    unsigned n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < sizeof digits);
    while (n) pcam_text_char(t, digits[--n]);
}
static inline void pcam_text_dec(PcamText *t, int64_t v)
{
    if (v < 0) {
        pcam_text_char(t, '-');
        pcam_text_udec(t, (uint64_t)0 - (uint64_t)v);
    } else {
        pcam_text_udec(t, (uint64_t)v);
    }
}
/* 带 0x 前缀，不补零。 */
static inline void pcam_text_hex(PcamText *t, uint64_t v)
{
    char digits[16];
    unsigned n = 0;
    do {
        digits[n++] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v && n < sizeof digits);
    pcam_text_str(t, "0x");
    while (n) pcam_text_char(t, digits[--n]);
}
/* " key=" 加值。 */
static inline void pcam_text_kd(PcamText *t, const char *key, int64_t v)
{
    pcam_text_char(t, ' ');
    pcam_text_str(t, key);
    pcam_text_char(t, '=');
    pcam_text_dec(t, v);
}
static inline void pcam_text_kx(PcamText *t, const char *key, uint64_t v)
{
    pcam_text_char(t, ' ');
    pcam_text_str(t, key);
    pcam_text_char(t, '=');
    pcam_text_hex(t, v);
}
/* 字节按两位十六进制连写，空格分隔，写不下时截断。 */
static inline void pcam_text_bytes(PcamText *t, const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        pcam_text_char(t, ' ');
        pcam_text_char(t, "0123456789abcdef"[data[i] >> 4]);
        pcam_text_char(t, "0123456789abcdef"[data[i] & 15]);
    }
}
#endif
