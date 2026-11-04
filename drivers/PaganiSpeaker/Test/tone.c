/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * Host tests of the boot tone (SpeakerTone.c) against libm: the interpolated sine, the dB gains, the frequency (zero crossings), the
 * amplitude, the fades at both ends, the length, the silence after it and the clamping of its arguments. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "SpeakerTone.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

static int16_t samples[2 * 48000 * 3];

int main(void)
{
    /* The table sine within 4 LSB of libm over a dense sweep of the phase (256 points, linear interpolation: below -78 dB). */
    int worst = 0;
    for (uint64_t p = 0; p < (1ull << 32); p += 0x10001ull * 7) {
        const double want = 32767.0 * sin(2.0 * acos(-1.0) * (double)p / 4294967296.0);
        const int err = abs(speaker_tone_sine((uint32_t)p) - (int)lround(want));
        if (err > worst) worst = err;
    }
    CHECK(worst <= 4);
    CHECK(speaker_tone_sine(0) == 0 && speaker_tone_sine(0x40000000u) == 32767 && speaker_tone_sine(0xc0000000u) == -32767);
    /* Gains: every dB from 0 to 90 within 1 LSB of 32767 * 10^(-dB/20); clamped above 90. */
    for (uint32_t db = 0; db <= 90; ++db) {
        const double want = 32767.0 * pow(10.0, -(double)db / 20.0);
        CHECK(fabs(speaker_tone_gain(db) - want) <= 1.0);
        ++checks;
    }
    CHECK(speaker_tone_gain(30) == 1036 && speaker_tone_gain(0) == 32767 && speaker_tone_gain(200) == speaker_tone_gain(90));

    /* 440 Hz, 500 ms, 30 dB down: 24000 frames, stereo with equal channels, then silence. */
    SpeakerTone t;
    speaker_tone_init(&t, 440, 500, 30, 48000);
    CHECK(t.frames == 24000 && t.fade == 480 && t.step == 39370534u && t.gain == 1036);
    memset(samples, 0x55, sizeof samples);
    for (unsigned i = 0; i < 26000 / 480 + 1; ++i) speaker_tone_fill(&t, samples + (uint64_t)i * 480 * 2, 480, 2);
    CHECK(speaker_tone_done(&t));
    unsigned crossings = 0, peak = 0;
    for (unsigned i = 0; i < 24000; ++i) {
        CHECK(samples[2 * i] == samples[2 * i + 1]);
        const unsigned m = (unsigned)abs(samples[2 * i]);
        if (m > peak) peak = m;
        if (i && (samples[2 * (i - 1)] < 0) != (samples[2 * i] < 0)) ++crossings;
    }
    CHECK(crossings >= 438 && crossings <= 442);            /* 440 cycles in 0.5 s: two crossings per cycle, over 0.5 s */
    CHECK(peak >= 1030 && peak <= 1036);
    for (unsigned i = 24000; i < 26000; ++i) CHECK(samples[2 * i] == 0 && samples[2 * i + 1] == 0);
    /* The fades: the first and the last 10 ms rise and fall; full level in between. */
    CHECK(samples[0] == 0 && samples[2 * 23999] == 0);
    unsigned early = 0, mid = 0;
    for (unsigned i = 0; i < 120; ++i) if ((unsigned)abs(samples[2 * i]) > early) early = (unsigned)abs(samples[2 * i]);
    for (unsigned i = 12000; i < 12120; ++i) if ((unsigned)abs(samples[2 * i]) > mid) mid = (unsigned)abs(samples[2 * i]);
    CHECK(early <= 1036 / 4 + 2 && mid >= 1000);
    for (unsigned i = 0; i < 480; ++i) {
        const double envelope = (double)i / 480.0;
        CHECK(fabs((double)samples[2 * i]) <= 1036.0 * envelope + 2.0);
    }

    /* Mono, other rates, and the clamped arguments. */
    speaker_tone_init(&t, 5, 0, 0, 0);
    CHECK(t.step == (uint32_t)((20ull << 32) / 48000 + 1) || t.step == (uint32_t)((20ull << 32) / 48000));
    CHECK(t.frames == 48 && t.fade == 24 && t.gain == 32767);
    speaker_tone_init(&t, 99999, 99999, 0, 48000);
    CHECK(t.frames == 480000 && t.step == (uint32_t)(((20000ull << 32) + 24000) / 48000));
    speaker_tone_init(&t, 1000, 10, 6, 16000);
    int16_t mono[200];
    speaker_tone_fill(&t, mono, 200, 1);
    CHECK(speaker_tone_done(&t) && t.frames == 160 && mono[160] == 0 && mono[199] == 0);
    printf("PASS tone: %u checks\n", checks);
    return 0;
}
