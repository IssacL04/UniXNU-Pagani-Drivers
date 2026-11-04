// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// Host tests of SensorsOrient.c: each state from the gravity vector, the hold, the hysteresis around the diagonal, the dead zones
// (nothing reported), shaking and free fall ignored, noise, the extremes of the input, and a sweep of every direction of the sphere
// against the floating point geometry (the angles the header promises).
#include <math.h>
#include "check.h"
#include "../SensorsOrient.h"

static int32_t fx(double g) {return (int32_t)lround(g * 65536.0);}

/* Feeds a vector every 20 ms for ms; the last state reported in that time, or -1. */
static int hold(SnoState *s, uint64_t *t, double x, double y, double z, unsigned ms)
{
    int reported = -1;
    for (unsigned i = 0; i < ms / 20; ++i) {
        const int r = sno_update(s, fx(x), fx(y), fx(z), *t);
        if (r >= 0) reported = r;
        *t += 20;
    }
    return reported;
}

static void states(void)
{
    static const struct {double x, y, z; int state;} table[] = {
        {0, -1, 0, SNO_PORTRAIT}, {0, 1, 0, SNO_UPSIDE_DOWN}, {-1, 0, 0, SNO_LANDSCAPE_LEFT}, {1, 0, 0, SNO_LANDSCAPE_RIGHT},
        {0, 0, -1, SNO_FACE_UP}, {0, 0, 1, SNO_FACE_DOWN},
    };
    for (unsigned a = 0; a < 6; ++a) {
        SnoState s;
        uint64_t t = 1000;
        sno_init(&s);
        CHECK(s.state == SNO_UNKNOWN);
        CHECK(hold(&s, &t, table[a].x, table[a].y, table[a].z, 200) == -1);       /* not yet */
        CHECK(hold(&s, &t, table[a].x, table[a].y, table[a].z, 200) == table[a].state);
        CHECK(hold(&s, &t, table[a].x, table[a].y, table[a].z, 2000) == -1);      /* once */
        for (unsigned b = 0; b < 6; ++b) {
            if (a == b) continue;
            CHECK(hold(&s, &t, table[b].x, table[b].y, table[b].z, 400) == table[b].state);
            CHECK(hold(&s, &t, table[a].x, table[a].y, table[a].z, 400) == table[a].state);
        }
    }
}

static void hold_and_flicker(void)
{
    SnoState s;
    uint64_t t = 0;
    sno_init(&s);
    CHECK(hold(&s, &t, 0, -1, 0, 400) == SNO_PORTRAIT);
    /* shorter than the hold, again and again: no change; each return to the state restarts the count */
    for (unsigned i = 0; i < 50; ++i) {
        CHECK(hold(&s, &t, 1, 0, 0, 200) == -1);
        CHECK(hold(&s, &t, 0, -1, 0, 60) == -1);
    }
    CHECK(s.state == SNO_PORTRAIT);
    /* a candidate replaced by another restarts too */
    CHECK(hold(&s, &t, 1, 0, 0, 200) == -1);
    CHECK(hold(&s, &t, -1, 0, 0, 200) == -1);
    CHECK(hold(&s, &t, -1, 0, 0, 100) == SNO_LANDSCAPE_LEFT);
}

static void hysteresis(void)
{
    SnoState s;
    uint64_t t = 0;
    const double r = M_PI / 180.0;
    sno_init(&s);
    CHECK(hold(&s, &t, 0, -1, 0, 400) == SNO_PORTRAIT);
    /* tilting toward landscape: portrait holds to the diagonal and 7 degrees beyond it */
    for (int degrees = 0; degrees <= 51; ++degrees) CHECK(hold(&s, &t, sin(degrees * r), -cos(degrees * r), 0, 400) == -1);
    CHECK(hold(&s, &t, sin(54 * r), -cos(54 * r), 0, 400) == SNO_LANDSCAPE_RIGHT);
    /* and back: landscape holds past the diagonal */
    for (int degrees = 54; degrees >= 39; --degrees) CHECK(hold(&s, &t, sin(degrees * r), -cos(degrees * r), 0, 400) == -1);
    CHECK(hold(&s, &t, sin(36 * r), -cos(36 * r), 0, 400) == SNO_PORTRAIT);
    /* flipping within an axis has no margin: straight through the bottom */
    CHECK(hold(&s, &t, 0, 1, 0, 400) == SNO_UPSIDE_DOWN);
}

static void dead_zones(void)
{
    SnoState s;
    uint64_t t = 0;
    sno_init(&s);
    CHECK(hold(&s, &t, 0, -1, 0, 400) == SNO_PORTRAIT);
    /* shaken, falling: not one sample counts */
    CHECK(hold(&s, &t, 2.5, 0, 0, 2000) == -1);
    CHECK(hold(&s, &t, 0, 0.1, 0, 2000) == -1);
    CHECK(hold(&s, &t, 0, 0, 0, 2000) == -1);
    CHECK(hold(&s, &t, 0, 0, 7.9, 2000) == -1);
    /* between flat and upright (60 degrees from flat is still flat enough to be neither, at a diagonal in x/y): no change */
    const double r = M_PI / 180.0;
    CHECK(hold(&s, &t, sin(66 * r) * sin(45 * r), sin(66 * r) * cos(45 * r), -cos(66 * r), 2000) == -1);
    /* the extremes of the input do not overflow anything */
    static const int32_t edge[] = {INT32_MIN, INT32_MAX, -1, 0, 1, 1 << 19, -(1 << 19), (1 << 19) + 1};
    for (unsigned a = 0; a < 8; ++a)
        for (unsigned b = 0; b < 8; ++b)
            for (unsigned c = 0; c < 8; ++c) (void)sno_update(&s, edge[a], edge[b], edge[c], t);
    CHECK(s.state >= 0 && s.state < SNO_STATES);
    /* a vector of no state before any state was reported: still unknown */
    sno_init(&s);
    CHECK(hold(&s, &t, 0.05, 0.02, 0.1, 5000) == -1 && s.state == SNO_UNKNOWN);
}

/* Noise around a state does not change it, and it comes out through noise. */
static void noise(void)
{
    SnoState s;
    uint64_t t = 0;
    unsigned seed = 12345;
    sno_init(&s);
    int reported = -1, changes = 0;
    for (unsigned i = 0; i < 100000; ++i) {
        seed = seed * 1103515245u + 12345u;
        const double nx = ((seed >> 8) & 1023) / 1023.0 - 0.5;
        seed = seed * 1103515245u + 12345u;
        const double ny = ((seed >> 8) & 1023) / 1023.0 - 0.5;
        seed = seed * 1103515245u + 12345u;
        const double nz = ((seed >> 8) & 1023) / 1023.0 - 0.5;
        const int r = sno_update(&s, fx(0.2 * nx), fx(-1 + 0.2 * ny), fx(0.2 * nz), t);
        if (r >= 0) {reported = r; changes++;}
        t += 20;
    }
    CHECK(reported == SNO_PORTRAIT && changes == 1);
}

/* Every direction of the sphere from a fresh state, against the geometry the header promises. */
static void sphere(void)
{
    const double r = M_PI / 180.0;
    unsigned shown = 0;
    for (int polar = 0; polar <= 180; polar += 3) {
        for (int azimuth = 0; azimuth < 360; azimuth += 3) {
            const double x = sin(polar * r) * cos(azimuth * r), y = sin(polar * r) * sin(azimuth * r), z = cos(polar * r);
            SnoState s;
            uint64_t t = 0;
            sno_init(&s);
            const int got = hold(&s, &t, x, y, z, 400);
            const double ax = fabs(x), ay = fabs(y), az = fabs(z);
            if (az >= 0.8501) {
                CHECK(got == (z < 0 ? SNO_FACE_UP : SNO_FACE_DOWN));
            } else if (az < 0.848 && ax * ax + ay * ay > 0.165 && (ay >= 1.302 * ax || ax >= 1.302 * ay)) {
                const int expected = ay > ax ? (y < 0 ? SNO_PORTRAIT : SNO_UPSIDE_DOWN) : (x < 0 ? SNO_LANDSCAPE_LEFT : SNO_LANDSCAPE_RIGHT);
                CHECK(got == expected);
                shown++;
            } else if (az < 0.848 && ay < 1.298 * ax && ax < 1.298 * ay) {
                CHECK(got == -1);                /* the diagonal: no first state */
            }
        }
    }
    CHECK(shown > 1000);
}

int main(void)
{
    states();
    hold_and_flicker();
    hysteresis();
    dead_zones();
    noise();
    sphere();
    printf("PASS orientation: states, hold, hysteresis, dead zones, noise, the sphere: %u checks\n", checks);
    return 0;
}
