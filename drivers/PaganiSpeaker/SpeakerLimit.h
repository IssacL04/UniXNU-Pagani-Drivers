/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The speaker path's digital ceiling. Android protects these speakers with NXP's SpeakerBoost running in the DSP graph its audio HAL
 * loads (excursion and temperature models fed by the amplifiers' current/voltage sense); this path has none of that (the amplifiers'
 * sense output is off and the graph is ours), so every period that reaches the DSP, the boot tone and the iOS ring alike, passes this
 * limiter first and no sample ever leaves it above the ceiling.
 *
 * Shape: a stereo-linked peak limiter with one block (48 frames, 1 ms) of look-ahead. Each incoming block's peak p gives the gain the
 * block needs: unity up to the knee (ceiling + 3 dB below full scale), above it the output peak follows the line from (knee, knee) to
 * (full scale, ceiling), so a full-scale peak comes out exactly at the ceiling (about 11:1 above the knee at the default -12 dBFS).
 * The block held back is written with a gain ramp from the last gain to the smallest of the held block's need, the incoming block's
 * need and the released gain: the ramp never exceeds the held block's need, so the ceiling holds sample for sample, and the gain is
 * already down when a loud block arrives (no clipping, no step). The gain recovers at about 24 dB/s. Below the knee and at rest the
 * output is the input, bit for bit, one block later. Integer only (the kernel runs without floating point): Q16 gains, truncation
 * toward zero, a final clamp that is counted and never expected to act.
 *
 * The level. This limits peaks, not power or excursion, and nothing watches the coil: no SpeakerBoost, no Re/F0 calibration, no sense
 * data. The 13T's speakers are 6 ohm (devicetree tfa_default_mohm, 5.3..7.5). The container's boost (DCVOS 0x2f) is about 11.4 V if its
 * step is the family's 0.125 V from 5.5 V [inference], so a full-scale sine could be about 10 W, -6 dBFS about 2.5 W, -12 dBFS about
 * 0.6 W. A phone speaker lives on far less than 2.5 W of continuous sine near its resonance, which Android's model allows only for
 * moments; 0.6 W can be held. Hence -12 dBFS by default: quieter than Android, and safe for any signal for any time. A louder ceiling
 * (pagani-speaker-ceiling=6) is a choice to make after listening, with music, not with sine sweeps. */
#ifndef PAGANI_SPEAKER_LIMIT_H
#define PAGANI_SPEAKER_LIMIT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define SPEAKER_LIMIT_BLOCK 48u          /* frames: 1 ms at 48 kHz, the look-ahead and the attack ramp */
#define SPEAKER_LIMIT_CHANNELS 2u        /* interleaved stereo, linked */
#define SPEAKER_LIMIT_UNITY 65536u       /* Q16 */
#define SPEAKER_LIMIT_DEFAULT_DB 12u     /* the ceiling: 12 dB below full scale (8231); until a calibration exists (see below) */
#define SPEAKER_LIMIT_MIN_DB 3u          /* the ceiling cannot be set closer to full scale than this, */
#define SPEAKER_LIMIT_MAX_DB 40u         /* nor further below it than this */
#define SPEAKER_LIMIT_KNEE_DB 3u         /* the knee sits this far below the ceiling */
#define SPEAKER_LIMIT_RELEASE_Q16 181u   /* the gain recovers by 181/65536 per block: 0.024 dB per ms, about 24 dB/s */

typedef struct {
    uint32_t ceiling_db;                 /* as set (clamped) */
    int32_t ceiling, knee;               /* sample magnitudes */
    uint32_t gain;                       /* Q16, at the end of the last block written */
    uint32_t held_need;                  /* Q16, the gain the held block needs */
    int16_t held[SPEAKER_LIMIT_BLOCK * SPEAKER_LIMIT_CHANNELS];
    /* since the last reset */
    uint32_t blocks;                     /* blocks written */
    uint32_t limited;                    /* blocks written with a gain below unity */
    uint32_t min_gain;                   /* Q16, the lowest gain applied */
    uint32_t peak_in, peak_out;          /* largest magnitudes seen and written */
    uint32_t clamps;                     /* samples the final clamp had to cut (0 unless the arithmetic is wrong) */
} SpeakerLimit;

/* ceiling_db: dB below full scale; 0 means SPEAKER_LIMIT_DEFAULT_DB, the rest is clamped to MIN_DB..MAX_DB. Then as reset. */
void speaker_limit_init(SpeakerLimit *l, uint32_t ceiling_db);
/* A new run: the held block silent, the gain at unity, the statistics cleared (the ceiling stays). */
void speaker_limit_reset(SpeakerLimit *l);
/* frames (a multiple of SPEAKER_LIMIT_BLOCK) interleaved stereo frames from in to out, one block later: out gets the held block and
 * the first frames - BLOCK frames of in; in's last block is held for the next call. in and out must not overlap. */
void speaker_limit_run(SpeakerLimit *l, const int16_t *in, int16_t *out, uint32_t frames);
/* The Q16 gain a block with this peak magnitude (0..32768) needs: unity up to the knee, then the line to the ceiling. */
uint32_t speaker_limit_need(const SpeakerLimit *l, uint32_t peak);
/* A Q16 gain in thousandths of unity (for the log). */
static inline uint32_t speaker_limit_permille(uint32_t gain) {return (uint32_t)(((uint64_t)gain * 1000u) >> 16);}

#ifdef __cplusplus
}
#endif
#endif
