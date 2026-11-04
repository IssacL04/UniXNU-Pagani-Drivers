/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * Host tests of the digital ceiling (SpeakerLimit.c) against libm: the gain curve for every peak at every ceiling (never above the
 * ceiling, unity up to the knee, falling with the peak, the line to the ceiling within an LSB of the real-number one); the ceiling held
 * sample for sample over random signals of every level, length and shape (noise, sines, squares, -32768, single spikes, one channel
 * only) with the final clamp never acting; bit-exact delay of one block below the knee; the attack done inside the block before a loud
 * one (no step: a constant signal's output moves by a bounded amount per frame); the release (6 dB back in about 250 ms); steady-state
 * sine levels on the curve; stereo linking; reset; a partial block written as silence; the statistics. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "SpeakerLimit.h"
#include "SpeakerTone.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

enum {B = SPEAKER_LIMIT_BLOCK, P = 480};
static uint64_t rng = 0x243f6a8885a308d3ull;
static uint32_t rnd(void) {rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16);}

/* Runs n frames (a multiple of P) through l in periods, as the core does; out gets n frames. */
static void run(SpeakerLimit *l, const int16_t *in, int16_t *out, uint32_t n)
{
    for (uint32_t at = 0; at < n; at += P) speaker_limit_run(l, in + 2 * at, out + 2 * at, P);
}

static void curve(void)
{
    for (uint32_t db = SPEAKER_LIMIT_MIN_DB; db <= SPEAKER_LIMIT_MAX_DB; ++db) {
        SpeakerLimit l;
        speaker_limit_init(&l, db);
        const double c = 32767.0 * pow(10.0, -(double)db / 20.0), k = 32767.0 * pow(10.0, -(double)(db + 3) / 20.0);
        CHECK(fabs(l.ceiling - c) <= 1.0 && fabs(l.knee - k) <= 1.0 && l.knee < l.ceiling);
        uint32_t last = SPEAKER_LIMIT_UNITY;
        for (uint32_t p = 0; p <= 32768; ++p) {
            const uint32_t g = speaker_limit_need(&l, p);
            CHECK(g <= SPEAKER_LIMIT_UNITY && g <= last);
            CHECK(((uint64_t)p * g >> 16) <= (uint64_t)l.ceiling);
            if (p <= (uint32_t)l.knee) CHECK(g == SPEAKER_LIMIT_UNITY);
            else {
                CHECK(g < SPEAKER_LIMIT_UNITY);
                /* never above the real-number line, less than half an LSB below it (only the gain is rounded, down) */
                const double want = l.knee + (p - (double)l.knee) * (l.ceiling - (double)l.knee) / (32768.0 - l.knee);
                const double got = (double)p * g / 65536.0;
                CHECK(got <= want + 1e-9 && got > want - 0.5);
            }
            last = g;
        }
        CHECK(((uint64_t)32768 * speaker_limit_need(&l, 32768) >> 16) >= (uint64_t)l.ceiling - 1);
        CHECK(speaker_limit_need(&l, 40000) == speaker_limit_need(&l, 32768));
    }
    /* the argument: 0 is the default, the rest clamped */
    SpeakerLimit l;
    speaker_limit_init(&l, 0);
    CHECK(l.ceiling_db == 12 && l.ceiling == 8231 && l.knee == 5827 && l.gain == SPEAKER_LIMIT_UNITY);
    speaker_limit_init(&l, 2);
    CHECK(l.ceiling_db == 3 && l.ceiling == 23197);
    speaker_limit_init(&l, 99);
    CHECK(l.ceiling_db == 40 && l.ceiling == 328);
}

static int16_t in[2 * 48000 * 4], out[2 * 48000 * 4];

/* Below the knee the output is the input one block later, bit for bit (and silence first). */
static void transparent(void)
{
    SpeakerLimit l;
    speaker_limit_init(&l, 6);
    const uint32_t n = 48000 * 2;
    for (uint32_t i = 0; i < 2 * n; ++i) in[i] = (int16_t)((int32_t)(rnd() % (2u * 11626u + 1u)) - 11626);
    in[0] = 11626; in[1] = -11626;
    run(&l, in, out, n);
    for (uint32_t i = 0; i < 2 * B; ++i) CHECK(out[i] == 0);
    for (uint32_t i = 2 * B; i < 2 * n; ++i) CHECK(out[i] == in[i - 2 * B]);
    CHECK(l.limited == 0 && l.min_gain == SPEAKER_LIMIT_UNITY && l.clamps == 0 && l.peak_in == 11626 && l.peak_out == 11626);
    CHECK(l.blocks == n / B);
    /* an impulse at frame 1000 comes out at frame 1048 */
    speaker_limit_reset(&l);
    memset(in, 0, sizeof in);
    in[2 * 1000] = 5000; in[2 * 1000 + 1] = -5000;
    run(&l, in, out, 4800);
    for (uint32_t f = 0; f < 4800; ++f) {
        const int16_t want = f == 1000 + B ? 5000 : 0;
        CHECK(out[2 * f] == want && out[2 * f + 1] == -want);
    }
}

/* Random signals of every kind: the ceiling holds for every sample, the clamp never acts. */
static void ceiling(void)
{
    for (unsigned trial = 0; trial < 400; ++trial) {
        SpeakerLimit l;
        const uint32_t db = SPEAKER_LIMIT_MIN_DB + trial % (SPEAKER_LIMIT_MAX_DB - SPEAKER_LIMIT_MIN_DB + 1);
        speaker_limit_init(&l, db);
        const uint32_t n = P * (10 + rnd() % 40);
        uint32_t f = 0;
        while (f < n) {
            const uint32_t len = 1 + rnd() % 3000, kind = rnd() % 7, amp = rnd() % 32769;
            const uint32_t step = rnd();
            for (uint32_t i = 0; i < len && f < n; ++i, ++f) {
                int32_t a = 0, b = 0;
                switch (kind) {
                case 0: a = (int32_t)(rnd() % (2 * amp + 1)) - (int32_t)amp; b = (int32_t)(rnd() % (2 * amp + 1)) - (int32_t)amp; break;
                case 1: a = b = (int32_t)((int64_t)speaker_tone_sine(step * i) * (int64_t)amp / 32767); break;
                case 2: a = b = (i / (1 + step % 200)) & 1 ? -(int32_t)amp : (int32_t)amp; break;
                case 3: a = b = -32768; break;
                case 4: a = i == len / 2 ? 32767 : 0; b = 0; break;                                    /* a lone spike, left only */
                case 5: a = 0; b = (int32_t)(rnd() % (2 * amp + 1)) - (int32_t)amp; break;             /* right only */
                default: a = b = 0; break;
                }
                if (a > 32767) a = 32767;
                if (b > 32767) b = 32767;
                in[2 * f] = (int16_t)a;
                in[2 * f + 1] = (int16_t)b;
            }
        }
        run(&l, in, out, n);
        for (uint32_t i = 0; i < 2 * n; ++i) CHECK(abs(out[i]) <= l.ceiling);
        CHECK(l.clamps == 0 && l.peak_out <= (uint32_t)l.ceiling);
        /* where the input never left the knee for a whole neighbourhood and the gain was at rest, nothing changed: checked above */
    }
}

/* A constant level (the output shows the gain frame by frame): quiet, full scale, quiet. The attack happens in the block before the
 * loud one (the loud block starts already at its gain), no frame-to-frame jump larger than one ramp step, then the release. */
static void envelope(void)
{
    SpeakerLimit l;
    speaker_limit_init(&l, 6);
    const uint32_t n = 48000;
    for (uint32_t f = 0; f < n; ++f) {
        const int16_t v = f >= 9600 && f < 19200 ? 32767 : 8192;
        in[2 * f] = in[2 * f + 1] = v;
    }
    run(&l, in, out, n);
    const uint32_t need = speaker_limit_need(&l, 32767);
    /* the loud part comes out at the ceiling's level from its first frame (one block later) to its last */
    for (uint32_t f = 9600 + B; f < 19200 + B; ++f) CHECK(out[2 * f] == (int16_t)((32767ull * need) >> 16) && out[2 * f] <= 16422);
    /* the block before it ramps down: the quiet level, smoothly, to the loud block's gain */
    const int max_step = (int)((8192ull * (SPEAKER_LIMIT_UNITY - need) / B) >> 16) + 2;
    for (uint32_t f = B + 1; f < n; ++f) {
        if (f == 9600 + B || f == 19200 + B) continue;                    /* the input's own steps */
        CHECK(abs(out[2 * f] - out[2 * f - 2]) <= max_step);
    }
    CHECK(out[2 * (9600 + B - 1)] == (int16_t)((8192ull * need) >> 16));
    /* the release: back to unity within 240..270 ms after the loud part */
    uint32_t back = 0;
    for (uint32_t f = 19200 + B; f < n; ++f) if (out[2 * f] == 8192) {back = f; break;}
    CHECK(back && back - (19200 + B) >= 240 * 48 && back - (19200 + B) <= 270 * 48);
    for (uint32_t f = back; f < n; ++f) CHECK(out[2 * f] == 8192);
    CHECK(l.min_gain == need && l.limited >= 200 + 240 && l.clamps == 0);
}

/* Steady sines at levels from -20 dBFS to full scale: the output peak lies on the curve. */
static void levels(void)
{
    for (int db10 = -200; db10 <= 0; db10 += 5) {
        SpeakerLimit l;
        speaker_limit_init(&l, 6);
        const double amplitude = 32767.0 * pow(10.0, db10 / 200.0);
        const uint32_t n = 24000;
        for (uint32_t f = 0; f < n; ++f) in[2 * f] = in[2 * f + 1] = (int16_t)lround(amplitude * sin(2 * M_PI * 997.0 * f / 48000.0));
        run(&l, in, out, n);
        int peak_in = 0, peak = 0;
        for (uint32_t f = 12000; f < n; ++f) {
            if (abs(in[2 * f - 2 * B]) > peak_in) peak_in = abs(in[2 * f - 2 * B]);
            if (abs(out[2 * f]) > peak) peak = abs(out[2 * f]);
        }
        const double want = peak_in <= l.knee ? peak_in
                                               : l.knee + (peak_in - (double)l.knee) * (l.ceiling - (double)l.knee) / (32768.0 - l.knee);
        CHECK(fabs(peak - want) <= 2.0 && peak <= l.ceiling);
    }
}

/* Stereo linking: a loud left channel turns the quiet right one down by the same gain. */
static void linked(void)
{
    SpeakerLimit l;
    speaker_limit_init(&l, 6);
    const uint32_t n = 9600;
    for (uint32_t f = 0; f < n; ++f) {in[2 * f] = 32767; in[2 * f + 1] = 4000;}
    run(&l, in, out, n);
    const uint32_t need = speaker_limit_need(&l, 32767);
    for (uint32_t f = 4800; f < n; ++f) CHECK(out[2 * f] == (int16_t)((32767ull * need) >> 16) && out[2 * f + 1] == (int16_t)((4000ull * need) >> 16));
    /* a reset starts over: silence held, unity, statistics cleared, the ceiling kept */
    speaker_limit_reset(&l);
    CHECK(l.gain == SPEAKER_LIMIT_UNITY && l.held_need == SPEAKER_LIMIT_UNITY && l.limited == 0 && l.blocks == 0 && l.peak_in == 0 &&
          l.ceiling == 16422);
    for (uint32_t i = 0; i < 2 * B; ++i) CHECK(l.held[i] == 0);
    /* a partial block is written as silence (never left as it was) and does not enter the stream */
    for (uint32_t i = 0; i < 2 * 100; ++i) {in[i] = 1000; out[i] = 0x5555;}
    speaker_limit_run(&l, in, out, 100);
    for (uint32_t i = 0; i < 2 * 96; ++i) CHECK(out[i] == (i < 2 * B ? 0 : 1000));
    for (uint32_t i = 2 * 96; i < 2 * 100; ++i) CHECK(out[i] == 0);
    CHECK(l.blocks == 2 && speaker_limit_permille(SPEAKER_LIMIT_UNITY) == 1000 && speaker_limit_permille(32768) == 500);
}

int main(void)
{
    curve();
    transparent();
    ceiling();
    envelope();
    levels();
    linked();
    printf("PASS limit: %u checks\n", checks);
    return 0;
}
