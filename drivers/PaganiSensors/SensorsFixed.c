/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Integer arithmetic on float bit patterns and the 13T's axis maps: see SensorsFixed.h. */
#include "SensorsFixed.h"

#define INT64_TOP 0x7fffffffffffffffll

static int64_t clamp(int64_t v, int64_t lo, int64_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

int64_t sfx_scale(uint32_t bits, int64_t num, int64_t den, int shift, int64_t lo, int64_t hi)
{
    const uint32_t exponent = (bits >> 23) & 0xff, fraction = bits & 0x7fffff;
    const int negative = (int)(bits >> 31) ^ (num < 0);
    if (den <= 0) return clamp(0, lo, hi);
    if (exponent == 0xff) return fraction ? clamp(0, lo, hi) : (negative ? lo : hi);
    uint64_t mant = exponent ? (fraction | 0x800000u) : fraction;
    int power = (exponent ? (int)exponent - 150 : -149) + shift;
    const uint64_t magnitude_num = num < 0 ? (uint64_t)0 - (uint64_t)num : (uint64_t)num;
    if (!mant || !magnitude_num) return clamp(0, lo, hi);
    uint64_t product = mant * magnitude_num;                 /* < 2^24 * 2^39 */
    uint64_t result;
    if (power >= 0) {
        if (power > 62 || product > ((uint64_t)INT64_TOP >> power)) return negative ? lo : hi;
        product <<= power;
        result = (product + (uint64_t)den / 2) / (uint64_t)den;
    } else {
        int k = -power;
        if (k > 62) return clamp(0, lo, hi);
        if (k > 31) {   /* keep den << k inside 64 bits: drop the low bits first, rounded */
            const int drop = k - 31;
            product = (product + ((uint64_t)1 << (drop - 1))) >> drop;
            k = 31;
        }
        const uint64_t divisor = (uint64_t)den << k;
        result = (product + divisor / 2) / divisor;
    }
    if (result > (uint64_t)INT64_TOP) return negative ? lo : hi;
    const int64_t value = negative ? -(int64_t)result : (int64_t)result;
    return clamp(value, lo, hi);
}

uint32_t sfx_float_from_ratio(uint64_t num, uint64_t den)
{
    if (!num || !den || num >> 32 || den >> 32) return 0;
    int e = 0;
    while (num < (den << 23)) {num <<= 1; e--;}
    while (num >= (den << 24)) {den <<= 1; e++;}
    uint64_t q = num / den;
    const uint64_t r = num % den;
    if (2 * r > den || (2 * r == den && (q & 1))) q++;
    if (q == (1u << 24)) {q >>= 1; e++;}
    const int biased = e + 23 + 127;
    if (biased >= 0xff) return 0x7f800000u;
    if (biased <= 0) return 0;   /* far below anything a rate can be */
    return (uint32_t)biased << 23 | (uint32_t)(q & 0x7fffff);
}

int32_t sfx_milli(uint32_t bits)
{
    return (int32_t)sfx_scale(bits, 1000, 1, 0, -0x7fffffffll - 1, 0x7fffffffll);
}

int sfx_positive(uint32_t bits)
{
    return !(bits >> 31) && ((bits >> 23) & 0xff) != 0xff && (bits & 0x7fffffffu) != 0;
}

int32_t sfx_accel_ios(uint32_t bits)
{
    return (int32_t)sfx_scale(bits, -100000, SFX_STANDARD_GRAVITY_X100000, 16, -0x7fffffffll - 1, 0x7fffffffll);
}

int32_t sfx_fixed(uint32_t bits)
{
    return (int32_t)sfx_scale(bits, 1, 1, 16, -0x7fffffffll - 1, 0x7fffffffll);
}

int32_t sfx_pressure_kpa(uint32_t bits)
{
    return (int32_t)sfx_scale(bits, 1, 10, 16, -0x7fffffffll - 1, 0x7fffffffll);
}

uint32_t sfx_lux(uint32_t bits, uint32_t permille)
{
    return (uint32_t)sfx_scale(bits, permille, 1000, 0, 0, 0x7fffffffll);
}

/* ---- axis maps ---- */
void sfx_axes_identity(SfxAxes *a)
{
    for (uint8_t i = 0; i < 3; ++i) {a->src[i] = i; a->neg[i] = 0;}
}

int sfx_axes_valid(const SfxAxes *a)
{
    unsigned seen = 0;
    for (unsigned i = 0; i < 3; ++i) {
        if (a->src[i] > 2 || a->neg[i] > 1) return 0;
        seen |= 1u << a->src[i];
    }
    return seen == 7;
}

void sfx_axes_apply(const SfxAxes *a, const int32_t in[3], int32_t out[3])
{
    for (unsigned i = 0; i < 3; ++i) {
        const int32_t v = in[a->src[i] % 3];
        /* the most negative value has no opposite: saturate */
        out[i] = a->neg[i] ? (v == -0x7fffffff - 1 ? 0x7fffffff : -v) : v;
    }
}

int sfx_axes_parse(const char *text, SfxAxes *accel, SfxAxes *gyro, SfxAxes *mag)
{
    SfxAxes maps[3];
    int given[3] = {0, 0, 0};
    unsigned at = 0;
    if (!text) return -1;
    for (unsigned i = 0; i < 3; ++i) maps[i] = *(i == 0 ? accel : i == 1 ? gyro : mag);
    while (text[at]) {
        const char which = text[at++];
        const int k = which == 'a' ? 0 : which == 'g' ? 1 : which == 'm' ? 2 : -1;
        if (k < 0 || given[k]) return -1;
        SfxAxes m;
        sfx_axes_identity(&m);
        for (unsigned i = 0; i < 3; ++i) {
            const char sign = text[at], axis = sign ? text[at + 1] : 0;
            if ((sign != '+' && sign != '-') || axis < 'x' || axis > 'z') return -1;
            m.neg[i] = sign == '-';
            m.src[i] = (uint8_t)(axis - 'x');
            at += 2;
        }
        if (!sfx_axes_valid(&m)) return -1;
        maps[k] = m;
        given[k] = 1;
        if (text[at] == ',' || text[at] == '.') {
            at++;
            if (!text[at]) return -1;
        } else if (text[at]) {
            return -1;
        }
    }
    if (!given[0] && !given[1] && !given[2]) return -1;
    *accel = maps[0];
    *gyro = maps[1];
    *mag = maps[2];
    return 0;
}

void sfx_axes_text(const SfxAxes *a, char out[7])
{
    for (unsigned i = 0; i < 3; ++i) {
        out[2 * i] = a->neg[i] ? '-' : '+';
        out[2 * i + 1] = (char)('x' + (a->src[i] % 3));
    }
    out[6] = 0;
}
