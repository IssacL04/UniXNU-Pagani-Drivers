// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Host tests of SensorsFixed.c: the integer-only float conversions (the kernel code has no floating point registers) checked against
// the host's double arithmetic over special values and a million random floats, the float encoder against the host's own rounding, the
// iOS unit conversions (the accelerometer's sign: a phone lying face up reads +9.80665 m/s^2 on z from the hub and -1 g on iOS), and the
// axis maps (parse, refuse, apply).
#include <math.h>
#include "check.h"
#include "../SensorsFixed.h"

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t next(void) {rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng;}

/* The reference: round half away from zero of v * num / den * 2^shift, saturated. */
static int64_t reference(float f, int64_t num, int64_t den, int shift, int64_t lo, int64_t hi)
{
    if (isnan(f)) return 0 < lo ? lo : 0 > hi ? hi : 0;
    const long double v = (long double)f * (long double)num / (long double)den * ldexpl(1.0L, shift);
    if (v >= (long double)hi) return hi;
    if (v <= (long double)lo) return lo;
    const long double r = v < 0 ? -floorl(-v + 0.5L) : floorl(v + 0.5L);
    return (int64_t)r;
}

static void check_scale(float f, int64_t num, int64_t den, int shift, int64_t lo, int64_t hi)
{
    const int64_t got = sfx_scale(float_bits(f), num, den, shift, lo, hi), want = reference(f, num, den, shift, lo, hi);
    /* exact except at a rounding tie the long double could not see: one unit */
    if (llabs(got - want) > 1) {
        fprintf(stderr, "sfx_scale(%a=%g, %lld/%lld, 2^%d) = %lld, want %lld\n", (double)f, (double)f, (long long)num, (long long)den, shift,
                (long long)got, (long long)want);
        exit(1);
    }
    ++checks;
}

static void scales(void)
{
    const float specials[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 1.5f, 2.5f, -2.5f, 9.80665f, -9.80665f, 1e-45f, 1e-38f, 3.4e38f,
                              -3.4e38f, 65535.5f, 0.000015258789f, 123456.78f, 1e10f, -1e10f, 156.9064f, 34.906585f, 4912.0f, 1013.25f};
    const int64_t I32_MIN = -0x7fffffffll - 1, I32_MAX = 0x7fffffffll;
    for (unsigned i = 0; i < sizeof specials / sizeof specials[0]; ++i) {
        check_scale(specials[i], 1, 1, 16, I32_MIN, I32_MAX);
        check_scale(specials[i], -100000, 980665, 16, I32_MIN, I32_MAX);
        check_scale(specials[i], 1, 10, 16, I32_MIN, I32_MAX);
        check_scale(specials[i], 1000, 1, 0, I32_MIN, I32_MAX);
        check_scale(specials[i], 750, 1000, 0, 0, I32_MAX);
    }
    /* infinities saturate, NaN is 0 */
    CHECK(sfx_scale(0x7f800000u, 1, 1, 16, -5, 5) == 5 && sfx_scale(0xff800000u, 1, 1, 16, -5, 5) == -5);
    CHECK(sfx_scale(0xff800000u, -1, 1, 16, -5, 5) == 5 && sfx_scale(0x7fc00000u, 1, 1, 16, -5, 5) == 0);
    CHECK(sfx_scale(float_bits(1.0f), 1, 0, 0, -5, 5) == 0);   /* no division by zero */
    /* a million random floats across all exponents, in the conversions the driver makes */
    for (unsigned round = 0; round < 1000000; ++round) {
        uint32_t bits = (uint32_t)next();
        if (round % 2) bits = (bits & 0x807fffffu) | ((uint32_t)(100 + next() % 60) << 23);   /* half of them near useful magnitudes */
        const float f = bits_float(bits);
        if (isnan(f)) continue;
        switch (round % 4) {
        case 0: check_scale(f, -100000, 980665, 16, I32_MIN, I32_MAX); break;
        case 1: check_scale(f, 1, 1, 16, I32_MIN, I32_MAX); break;
        case 2: check_scale(f, 1, 10, 16, I32_MIN, I32_MAX); break;
        default: check_scale(f, (int64_t)(next() % 5000), 1000, 0, 0, I32_MAX); break;
        }
    }
}

static void ratios(void)
{
    /* the rates the driver asks for are num/den with den 1000; compare with the host's correctly rounded division */
    for (uint64_t num = 1; num < 600000; num += 1 + num / 997) {
        const uint32_t got = sfx_float_from_ratio(num, 1000), want = float_bits((float)((double)num / 1000.0));
        if (got != want) {
            /* (float)(double) rounds twice; check the exact nearest instead */
            const long double exact = (long double)num / 1000.0L;
            const float a = bits_float(got);
            const float b = bits_float(want);
            CHECK(fabsl((long double)a - exact) <= fabsl((long double)b - exact));
        }
        ++checks;
    }
    for (unsigned round = 0; round < 200000; ++round) {
        const uint64_t num = next() >> 32, den = (next() >> 32) | 1;
        if (!num) continue;
        const uint32_t got = sfx_float_from_ratio(num, den);
        const long double exact = (long double)num / (long double)den;
        const float g = bits_float(got);
        const float below = nextafterf(g, 0.0f), above = nextafterf(g, INFINITY);
        /* the result is the nearest float: neighbours are not nearer */
        CHECK(fabsl((long double)g - exact) <= fabsl((long double)below - exact) &&
              fabsl((long double)g - exact) <= fabsl((long double)above - exact));
    }
    CHECK(sfx_float_from_ratio(50, 1) == float_bits(50.0f) && sfx_float_from_ratio(100000, 1000) == float_bits(100.0f));
    CHECK(sfx_float_from_ratio(0, 5) == 0 && sfx_float_from_ratio(5, 0) == 0 && sfx_float_from_ratio(1ull << 33, 1) == 0);
    CHECK(sfx_positive(float_bits(25.0f)) && !sfx_positive(float_bits(-25.0f)) && !sfx_positive(0) && !sfx_positive(0x7f800000u) &&
          !sfx_positive(0x7fc00000u));
    /* positive floats order like their bits */
    CHECK(float_bits(12.5f) < float_bits(13.0f) && float_bits(0.1f) < float_bits(400.0f));
    CHECK(sfx_milli(float_bits(-1.2345f)) == -1235 && sfx_milli(float_bits(9.80665f)) == 9807);
}

static void units(void)
{
    /* lying face up: the hub says +9.80665 on z, iOS wants -1 g */
    CHECK(sfx_accel_ios(float_bits(9.80665f)) == -65536 && sfx_accel_ios(float_bits(-9.80665f)) == 65536);
    CHECK(sfx_accel_ios(float_bits(0.0f)) == 0 && abs(sfx_accel_ios(float_bits(4.903325f)) + 32768) <= 1);
    CHECK(sfx_fixed(float_bits(1.0f)) == 65536 && sfx_fixed(float_bits(-0.5f)) == -32768 && sfx_fixed(float_bits(45.25f)) == 2965504);
    CHECK(sfx_pressure_kpa(float_bits(1013.25f)) == 6640435);   /* 101.325 kPa */
    CHECK(sfx_lux(float_bits(312.6f), 1000) == 313 && sfx_lux(float_bits(312.4f), 1000) == 312 && sfx_lux(float_bits(-4.0f), 1000) == 0);
    CHECK(sfx_lux(float_bits(1000.0f), 500) == 500 && sfx_lux(float_bits(1e12f), 1000) == 0x7fffffff);
}

static void axes(void)
{
    SfxAxes a, g, m, id;
    char text[7];
    sfx_axes_identity(&id);
    sfx_axes_text(&id, text);
    CHECK(!strcmp(text, "+x+y+z") && sfx_axes_valid(&id));
    a = g = m = id;
    CHECK(sfx_axes_parse("a-y+x+z", &a, &g, &m) == 0);
    sfx_axes_text(&a, text);
    CHECK(!strcmp(text, "-y+x+z") && !memcmp(&g, &id, sizeof id) && !memcmp(&m, &id, sizeof id));
    const int32_t in[3] = {100, 200, 300};
    int32_t out[3];
    sfx_axes_apply(&a, in, out);
    CHECK(out[0] == -200 && out[1] == 100 && out[2] == 300);
    CHECK(sfx_axes_parse("g+z+y-x,m-x-y+z.a+x+y+z", &a, &g, &m) == 0);
    sfx_axes_text(&g, text);
    CHECK(!strcmp(text, "+z+y-x"));
    sfx_axes_text(&m, text);
    CHECK(!strcmp(text, "-x-y+z"));
    sfx_axes_text(&a, text);
    CHECK(!strcmp(text, "+x+y+z"));
    const int32_t extreme[3] = {-0x7fffffff - 1, 5, 6};
    sfx_axes_apply(&m, extreme, out);
    CHECK(out[0] == 0x7fffffff && out[1] == -5 && out[2] == 6);
    /* refusals leave every map as it was */
    const SfxAxes before[3] = {a, g, m};
    static const char *const bad[] = {"", "a", "a+x+y", "a+x+x+z", "q+x+y+z", "a+x+y+w", "a*x+y+z", "a+x+y+z,", "a+x+y+z,a+x+y+z",
                                      "a+x+y+zg+x+y+z", "a+x+y+z;g+x+y+z"};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        CHECK(sfx_axes_parse(bad[i], &a, &g, &m) == -1);
        CHECK(!memcmp(&a, &before[0], sizeof a) && !memcmp(&g, &before[1], sizeof g) && !memcmp(&m, &before[2], sizeof m));
    }
    CHECK(sfx_axes_parse(0, &a, &g, &m) == -1);
    SfxAxes broken = {{0, 0, 2}, {0, 0, 0}};
    CHECK(!sfx_axes_valid(&broken));
}

int main(void)
{
    scales();
    ratios();
    units();
    axes();
    printf("PASS fixed point: float scaling (a million random floats), the float encoder, iOS units, axis maps: %u checks\n", checks);
    return 0;
}
