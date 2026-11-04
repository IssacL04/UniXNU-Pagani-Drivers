/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* See SpeakerLimit.h. New code; the dB table is the boot tone's (SpeakerTone.c). */
#include "SpeakerLimit.h"
#include "SpeakerTone.h"

#define FULL 32768u                       /* the largest magnitude a 16-bit sample has */
#define SAMPLES (SPEAKER_LIMIT_BLOCK * SPEAKER_LIMIT_CHANNELS)

void speaker_limit_reset(SpeakerLimit *l)
{
    for (uint32_t i = 0; i < SAMPLES; ++i) l->held[i] = 0;
    l->gain = l->held_need = SPEAKER_LIMIT_UNITY;
    l->blocks = l->limited = l->clamps = 0;
    l->min_gain = SPEAKER_LIMIT_UNITY;
    l->peak_in = l->peak_out = 0;
}

void speaker_limit_init(SpeakerLimit *l, uint32_t ceiling_db)
{
    if (!ceiling_db) ceiling_db = SPEAKER_LIMIT_DEFAULT_DB;
    if (ceiling_db < SPEAKER_LIMIT_MIN_DB) ceiling_db = SPEAKER_LIMIT_MIN_DB;
    if (ceiling_db > SPEAKER_LIMIT_MAX_DB) ceiling_db = SPEAKER_LIMIT_MAX_DB;
    l->ceiling_db = ceiling_db;
    l->ceiling = speaker_tone_gain(ceiling_db);                          /* round(32767 * 10^(-dB / 20)) */
    l->knee = speaker_tone_gain(ceiling_db + SPEAKER_LIMIT_KNEE_DB);
    speaker_limit_reset(l);
}

uint32_t speaker_limit_need(const SpeakerLimit *l, uint32_t peak)
{
    const uint64_t knee = (uint32_t)l->knee, ceiling = (uint32_t)l->ceiling, span = FULL - knee;
    if (peak <= knee) return SPEAKER_LIMIT_UNITY;
    if (peak > FULL) peak = FULL;
    /* The output level on the line from (knee, knee) to (full scale, ceiling) as an exact fraction, level / span (below the peak, so
     * the gain stays under unity); the gain is its ratio to the peak rounded down: it never rises with the peak, and the peak never
     * comes out above the line (exactly the ceiling at full scale). At most 2^47: no overflow. */
    const uint64_t level = knee * span + (uint64_t)(peak - knee) * (ceiling - knee);
    return (uint32_t)((level << 16) / ((uint64_t)peak * span));
}

static uint32_t block_peak(const int16_t *p)
{
    uint32_t peak = 0;
    for (uint32_t i = 0; i < SAMPLES; ++i) {
        const int32_t v = p[i];
        const uint32_t m = (uint32_t)(v < 0 ? -v : v);
        if (m > peak) peak = m;
    }
    return peak;
}

/* One sample at gain g (Q16), truncated toward zero so the magnitude never rounds up; the clamp only counts a broken invariant. */
static int16_t scale(SpeakerLimit *l, int16_t v, uint32_t g)
{
    const int64_t m = v < 0 ? -(int64_t)v : (int64_t)v;
    int64_t y = g >= SPEAKER_LIMIT_UNITY ? m : (m * (int64_t)g) >> 16;
    if (y > l->ceiling) {
        l->clamps++;
        y = l->ceiling;
    }
    if ((uint32_t)y > l->peak_out) l->peak_out = (uint32_t)y;
    return (int16_t)(v < 0 ? -y : y);
}

/* The held block from gain g0 to g1 in a straight line (the last frame at g1): every frame's gain lies between the two. */
static void write_block(SpeakerLimit *l, int16_t *dst, uint32_t g0, uint32_t g1)
{
    l->blocks++;
    if (g0 < SPEAKER_LIMIT_UNITY || g1 < SPEAKER_LIMIT_UNITY) l->limited++;
    if (g1 < l->min_gain) l->min_gain = g1;
    for (uint32_t f = 0; f < SPEAKER_LIMIT_BLOCK; ++f) {
        const int64_t step = ((int64_t)g1 - (int64_t)g0) * (int64_t)(f + 1u) / (int64_t)SPEAKER_LIMIT_BLOCK;
        const uint32_t g = (uint32_t)((int64_t)g0 + step);
        for (uint32_t c = 0; c < SPEAKER_LIMIT_CHANNELS; ++c) {
            const uint32_t i = f * SPEAKER_LIMIT_CHANNELS + c;
            dst[i] = scale(l, l->held[i], g);
        }
    }
}

void speaker_limit_run(SpeakerLimit *l, const int16_t *in, int16_t *out, uint32_t frames)
{
    uint32_t at = 0;
    for (; at + SPEAKER_LIMIT_BLOCK <= frames; at += SPEAKER_LIMIT_BLOCK) {
        const int16_t *src = in + (uint64_t)at * SPEAKER_LIMIT_CHANNELS;
        const uint32_t peak = block_peak(src), need = speaker_limit_need(l, peak);
        if (peak > l->peak_in) l->peak_in = peak;
        /* where the held block's ramp ends: the gain released from the last block, within the held and the incoming block's needs
         * (the ramp starts at the last block's end gain, which was within the held block's need already) */
        const uint32_t g0 = l->gain;
        uint32_t g1 = g0 + (uint32_t)(((uint64_t)g0 * SPEAKER_LIMIT_RELEASE_Q16 + 65535u) >> 16);
        if (g1 > SPEAKER_LIMIT_UNITY) g1 = SPEAKER_LIMIT_UNITY;
        if (g1 > l->held_need) g1 = l->held_need;
        if (g1 > need) g1 = need;
        write_block(l, out + (uint64_t)at * SPEAKER_LIMIT_CHANNELS, g0, g1);
        for (uint32_t i = 0; i < SAMPLES; ++i) l->held[i] = src[i];
        l->held_need = need;
        l->gain = g1;
    }
    /* a partial block is not part of the stream: written as silence, never left as it was */
    for (uint64_t i = (uint64_t)at * SPEAKER_LIMIT_CHANNELS; i < (uint64_t)frames * SPEAKER_LIMIT_CHANNELS; ++i) out[i] = 0;
}
