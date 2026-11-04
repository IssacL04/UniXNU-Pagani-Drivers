/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* CamPattern.h。二维码的数据结构和放置规则取自 ISO/IEC 18004，代码是自己写的。 */
#include "CamPattern.h"
#include <stdlib.h>
#include <string.h>

/* ---- GF(256)，本原多项式 0x11d ---- */
static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    unsigned r = 0, x = a;
    for (unsigned i = 0; i < 8; ++i) {
        if (b & (1u << i)) r ^= x;
        x <<= 1;
        if (x & 0x100u) x ^= 0x11du;
    }
    return (uint8_t)r;
}

void pcam_qr_ecc(const uint8_t data[PcamQrDataWords], uint8_t ecc[PcamQrEcWords])
{
    /* 生成多项式 (x - α^0)(x - α^1)…(x - α^25)，首项系数 1 不存，g[0] 是 x^25 的系数 */
    uint8_t g[PcamQrEcWords];
    memset(g, 0, sizeof g);
    g[PcamQrEcWords - 1] = 1;
    uint8_t root = 1;
    for (unsigned i = 0; i < PcamQrEcWords; ++i) {
        for (unsigned j = 0; j < PcamQrEcWords; ++j) {
            g[j] = gf_mul(g[j], root);
            if (j + 1 < PcamQrEcWords) g[j] ^= g[j + 1];
        }
        root = gf_mul(root, 2);
    }
    memset(ecc, 0, PcamQrEcWords);
    for (unsigned i = 0; i < PcamQrDataWords; ++i) {
        const uint8_t factor = data[i] ^ ecc[0];
        memmove(ecc, ecc + 1, PcamQrEcWords - 1);
        ecc[PcamQrEcWords - 1] = 0;
        for (unsigned j = 0; j < PcamQrEcWords; ++j) ecc[j] ^= gf_mul(g[j], factor);
    }
}

/* ---- 码字 ---- */
static void put_bits(uint8_t *words, unsigned *at, unsigned value, unsigned count)
{
    for (unsigned i = count; i-- > 0;) {
        if ((value >> i) & 1u) words[*at >> 3] |= (uint8_t)(0x80u >> (*at & 7));
        ++*at;
    }
}

static int code_words(const char *text, size_t length, uint8_t out[PcamQrTotalWords])
{
    if (!text || !length || length > PcamQrMaxText) return -1;
    memset(out, 0, PcamQrTotalWords);
    unsigned at = 0;
    put_bits(out, &at, 4, 4);                       /* 字节模式 */
    put_bits(out, &at, (unsigned)length, 8);        /* 版本 1..9 的字数位 */
    for (size_t i = 0; i < length; ++i) put_bits(out, &at, (uint8_t)text[i], 8);
    const unsigned capacity = PcamQrDataWords * 8;
    const unsigned terminator = capacity - at < 4 ? capacity - at : 4;
    put_bits(out, &at, 0, terminator);
    at = (at + 7) & ~7u;
    for (uint8_t pad = 0xec; at < capacity; pad ^= 0xec ^ 0x11) put_bits(out, &at, pad, 8);
    pcam_qr_ecc(out, out + PcamQrDataWords);
    return 0;
}

/* ---- 矩阵 ---- */
typedef struct {
    uint8_t *module;    /* size × size */
    uint8_t function[PcamQrSize][PcamQrSize];
} Canvas;

static void set(Canvas *c, int row, int col, unsigned dark)
{
    c->module[row * PcamQrSize + col] = (uint8_t)dark;
    c->function[row][col] = 1;
}

static void finder(Canvas *c, int top, int left)
{
    for (int r = -1; r <= 7; ++r)
        for (int k = -1; k <= 7; ++k) {
            const int row = top + r, col = left + k;
            if (row < 0 || col < 0 || row >= PcamQrSize || col >= PcamQrSize) continue;
            const int ring = r < 0 || r > 6 || k < 0 || k > 6 ? 0 :
                             r == 0 || r == 6 || k == 0 || k == 6 ? 1 :
                             r >= 2 && r <= 4 && k >= 2 && k <= 4 ? 1 : 0;
            set(c, row, col, (unsigned)ring);
        }
}

static void alignment(Canvas *c, int center_row, int center_col)
{
    for (int r = -2; r <= 2; ++r)
        for (int k = -2; k <= 2; ++k) {
            const int edge = r == -2 || r == 2 || k == -2 || k == 2;
            set(c, center_row + r, center_col + k, (unsigned)(edge || (r == 0 && k == 0)));
        }
}

static unsigned format_bits(unsigned mask)
{
    /* 纠错 M 的指示位是 00，BCH(15,5) 生成多项式 0x537，再异或 0x5412 */
    const unsigned data = mask;
    unsigned rem = data;
    for (unsigned i = 0; i < 10; ++i) rem = (rem << 1) ^ (((rem >> 9) & 1u) * 0x537u);
    return ((data << 10) | (rem & 0x3ffu)) ^ 0x5412u;
}

static void format_info(Canvas *c, unsigned mask)
{
    const unsigned bits = format_bits(mask);
#define BIT(i) ((bits >> (i)) & 1u)
    /* 左上角一份 */
    for (int i = 0; i <= 5; ++i) set(c, i, 8, BIT(i));
    set(c, 7, 8, BIT(6));
    set(c, 8, 8, BIT(7));
    set(c, 8, 7, BIT(8));
    for (int i = 9; i < 15; ++i) set(c, 8, 14 - i, BIT(i));
    /* 另外两个角一份 */
    for (int i = 0; i < 8; ++i) set(c, 8, PcamQrSize - 1 - i, BIT(i));
    for (int i = 8; i < 15; ++i) set(c, PcamQrSize - 15 + i, 8, BIT(i));
#undef BIT
    set(c, PcamQrSize - 8, 8, 1);       /* 固定的深色模块，行 4V + 9 */
}

static unsigned masked(unsigned mask, int row, int col)
{
    switch (mask) {
    case 0: return (row + col) % 2 == 0;
    case 1: return row % 2 == 0;
    case 2: return col % 3 == 0;
    case 3: return (row + col) % 3 == 0;
    case 4: return (row / 2 + col / 3) % 2 == 0;
    case 5: return row * col % 2 + row * col % 3 == 0;
    case 6: return (row * col % 2 + row * col % 3) % 2 == 0;
    default: return ((row + col) % 2 + row * col % 3) % 2 == 0;
    }
}

int pcam_qr_encode(const char *text, size_t length, unsigned mask, PcamQr *out)
{
    uint8_t words[PcamQrTotalWords];
    if (!out || mask > 7 || code_words(text, length, words)) return -1;
    Canvas c;
    c.module = &out->module[0][0];
    memset(out, 0, sizeof *out);
    memset(c.function, 0, sizeof c.function);
    finder(&c, 0, 0);
    finder(&c, 0, PcamQrSize - 7);
    finder(&c, PcamQrSize - 7, 0);
    for (int i = 8; i < PcamQrSize - 8; ++i) {
        set(&c, 6, i, i % 2 == 0);
        set(&c, i, 6, i % 2 == 0);
    }
    alignment(&c, 22, 22);              /* 版本 3 只有这一个 */
    format_info(&c, mask);
    /* 两列一组，从右下角开始之字形，竖直定时列跳过 */
    unsigned bit = 0;
    const unsigned total = PcamQrTotalWords * 8;
    for (int right = PcamQrSize - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        const int upward = ((right + 1) & 2) == 0;
        for (int step = 0; step < PcamQrSize; ++step) {
            const int row = upward ? PcamQrSize - 1 - step : step;
            for (int k = 0; k < 2; ++k) {
                const int col = right - k;
                if (c.function[row][col]) continue;
                unsigned dark = 0;
                if (bit < total) dark = (words[bit >> 3] >> (7 - (bit & 7))) & 1u;
                ++bit;                  /* 版本 3 末尾有 7 个剩余位，是 0 */
                out->module[row][col] = (uint8_t)(dark ^ masked(mask, row, col));
            }
        }
    }
    return bit == total + 7 ? 0 : -2;
}

/* ---- 一帧 ---- */
/* 全范围 YCbCr 的八条：白、黄、青、绿、品红、红、蓝、黑（BT.601 系数）。 */
static const uint8_t kBars[8][3] = {
    {255, 128, 128}, {226, 0, 149}, {179, 170, 0}, {150, 44, 21},
    {105, 212, 235}, {76, 85, 255}, {29, 255, 107}, {0, 128, 128},
};

int pcam_pattern_init(PcamPattern *p, unsigned width, unsigned height)
{
    if (!p || width < 64 || height < 64 || (width & 1) || (height & 1) || width > 8192 || height > 8192) return -1;
    memset(p, 0, sizeof *p);
    PcamQr qr;
    if (pcam_qr_encode(PCAM_QR_TEXT, strlen(PCAM_QR_TEXT), 0, &qr)) return -2;
    const unsigned modules = PcamQrSize + 2 * PcamQrQuiet;
    const unsigned shorter = width < height ? width : height;
    unsigned scale = shorter * 3 / 4 / modules;
    if (!scale) scale = 1;
    p->width = width;
    p->height = height;
    p->tile = modules * scale;
    if (p->tile > shorter) return -3;
    p->tile_left = ((width - p->tile) / 2) & ~1u;
    p->tile_top = ((height - p->tile) / 2) & ~1u;
    p->line_y = malloc((size_t)width * 2);
    p->line_uv = malloc((size_t)width * 2);
    p->tile_luma = malloc((size_t)p->tile * p->tile);
    if (!p->line_y || !p->line_uv || !p->tile_luma) {
        pcam_pattern_free(p);
        return -4;
    }
    for (unsigned x = 0; x < width; x += 2) {
        const uint8_t *bar = kBars[x * 8 / width];
        p->line_y[x] = p->line_y[x + 1] = p->line_y[width + x] = p->line_y[width + x + 1] = bar[0];
        p->line_uv[x] = p->line_uv[width + x] = bar[1];
        p->line_uv[x + 1] = p->line_uv[width + x + 1] = bar[2];
    }
    for (unsigned r = 0; r < p->tile; ++r)
        for (unsigned k = 0; k < p->tile; ++k) {
            const unsigned mr = r / scale, mk = k / scale;
            const int inside = mr >= PcamQrQuiet && mk >= PcamQrQuiet && mr < PcamQrQuiet + PcamQrSize && mk < PcamQrQuiet + PcamQrSize;
            p->tile_luma[(size_t)r * p->tile + k] = inside && qr.module[mr - PcamQrQuiet][mk - PcamQrQuiet] ? 0 : 255;
        }
    return 0;
}

void pcam_pattern_free(PcamPattern *p)
{
    if (!p) return;
    free(p->line_y);
    free(p->line_uv);
    free(p->tile_luma);
    p->line_y = p->line_uv = p->tile_luma = NULL;
}

void pcam_pattern_fill(const PcamPattern *p, uint32_t frame, uint8_t *y, size_t y_stride, uint8_t *uv, size_t uv_stride)
{
    const unsigned shift = (unsigned)(((uint64_t)frame * PcamScrollPixels) % p->width) & ~1u;
    for (unsigned r = 0; r < p->height; ++r) memcpy(y + r * y_stride, p->line_y + shift, p->width);
    for (unsigned r = 0; r < p->height / 2; ++r) memcpy(uv + r * uv_stride, p->line_uv + shift, p->width);
    /* 二维码：亮度逐行贴上去，色度置中性，否则白底发彩 */
    for (unsigned r = 0; r < p->tile; ++r) memcpy(y + (size_t)(p->tile_top + r) * y_stride + p->tile_left, p->tile_luma + (size_t)r * p->tile, p->tile);
    for (unsigned r = 0; r < p->tile / 2; ++r) memset(uv + (size_t)(p->tile_top / 2 + r) * uv_stride + p->tile_left, 128, p->tile);
    /* 帧号条：格子贴在第一行，低位在左 */
    for (unsigned bit = 0; bit < PcamStampBits; ++bit) {
        const uint8_t value = ((frame >> bit) & 1u) ? 255 : 0;
        for (unsigned r = 0; r < PcamStampCell; ++r) memset(y + r * y_stride + bit * PcamStampCell, value, PcamStampCell);
        for (unsigned r = 0; r < PcamStampCell / 2; ++r) memset(uv + r * uv_stride + bit * PcamStampCell, 128, PcamStampCell);
    }
}

uint32_t pcam_pattern_stamp(const uint8_t *y, size_t y_stride)
{
    uint32_t frame = 0;
    for (unsigned bit = 0; bit < PcamStampBits; ++bit)
        if (y[y_stride * (PcamStampCell / 2) + bit * PcamStampCell + PcamStampCell / 2] >= 128) frame |= 1u << bit;
    return frame;
}
