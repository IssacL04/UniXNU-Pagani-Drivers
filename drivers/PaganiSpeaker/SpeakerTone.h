/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The boot test tone (pagani-audio-tone=1): a sine of hz for ms, attenuated by atten dB, with 10 ms linear fades so the start and the end
 * do not click, then silence. Integer only (kernel code runs without floating point): a 32-bit phase accumulator, a 256-point Q15 sine
 * table with linear interpolation and a Q15 gain from a 1 dB table. sheng plays a 750 Hz tone at 30 dB below full scale; the 13T
 * default is 440 Hz for 0.5 s at 30 dB (about 1036 / 32767). */
#ifndef PAGANI_SPEAKER_TONE_H
#define PAGANI_SPEAKER_TONE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t phase, step;           /* 2^32 per cycle */
    int32_t gain;                   /* Q15 */
    uint32_t frames, frame, fade;   /* tone length, frames produced, fade length (frames) */
} SpeakerTone;

/* hz 20..20000, ms 1..10000, atten_db 0..90 (0 full scale); out-of-range values are clamped. */
void speaker_tone_init(SpeakerTone *t, uint32_t hz, uint32_t ms, uint32_t atten_db, uint32_t rate);
/* frames interleaved frames of channels 16-bit samples (the same value on every channel); silence after the tone. */
void speaker_tone_fill(SpeakerTone *t, int16_t *dst, uint32_t frames, uint32_t channels);
static inline int speaker_tone_done(const SpeakerTone *t) {return t->frame >= t->frames;}
/* The Q15 gain of atten_db (32767 at 0 dB). */
int32_t speaker_tone_gain(uint32_t atten_db);
/* One sine sample (Q15, -32767..32767) at phase (2^32 per cycle). */
int32_t speaker_tone_sine(uint32_t phase);

#ifdef __cplusplus
}
#endif
#endif
