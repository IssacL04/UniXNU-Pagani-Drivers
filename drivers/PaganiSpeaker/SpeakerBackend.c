/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* See SpeakerBackend.h. The period flow is sheng's (UniXNU-sheng package/AudioProbe.cpp, commit 08cde20e: startOutput, submit, handle)
 * split along PaganiAudio's backend interface: the ring, the timeline, the late/hole accounting and the drift resync are the engine's. */
#include "SpeakerBackend.h"

enum {kRestarts = 3};

static uint64_t ticks(const SpeakerBackend *b) {return b->now_ticks(b->context);}

/* ---- the ring as the source of a playback run ---- */
static void ring_align(void *context)
{
    SpeakerBackend *b = (SpeakerBackend *)context;
    pagani_audio_engine_align_output(b->engine, ticks(b));
}
static void ring_take(void *context, int16_t *dst)
{
    SpeakerBackend *b = (SpeakerBackend *)context;
    PaganiAudioEngine *e = b->engine;
    pagani_audio_engine_take(e, e->next, dst, ticks(b));
    e->next++;
}
static void ring_played(void *context)
{
    SpeakerBackend *b = (SpeakerBackend *)context;
    pagani_audio_engine_played(b->engine, ticks(b));
}

static void fall_back(SpeakerBackend *b)
{
    if (b->fallback) return;
    b->fallback = 1;
    b->fallbacks++;
    pagani_audio_set_backend(&pagani_audio_silent_backend);
}

static int adsp_open(PaganiAudioEngine *e, void *context)
{
    SpeakerBackend *b = (SpeakerBackend *)context;
    b->engine = e;
    if (e->period != SpeakerPeriodFrames || e->out_channels != SpeakerChannels) return -1;
    if (!b->core->ready || b->core->dead) return -1;
    b->fallback = 0;
    return 0;
}

static int adsp_start_output(PaganiAudioEngine *e, void *context, uint64_t now)
{
    (void)now;
    SpeakerBackend *b = (SpeakerBackend *)context;
    b->engine = e;
    if (b->core->dead) {
        fall_back(b);
        return -1;
    }
    if (speaker_core_start(b->core, &b->source)) {
        b->refused++;
        return -1;
    }
    return 0;
}

static void adsp_stop_output(PaganiAudioEngine *e, void *context)
{
    (void)e;
    speaker_core_stop(((SpeakerBackend *)context)->core);
}

static int adsp_start_input(PaganiAudioEngine *e, void *context, uint64_t now)
{
    (void)e; (void)context; (void)now;
    return 0;   /* silence by the clock (pump) */
}

static void adsp_stop_input(PaganiAudioEngine *e, void *context) {(void)e; (void)context;}

static uint32_t ms_for(uint64_t frames)
{
    const uint64_t ms = (frames * 1000u + PAGANI_AUDIO_RATE - 1u) / PAGANI_AUDIO_RATE;
    return ms ? (uint32_t)(ms < 1000u ? ms : 1000u) : 1u;
}

static uint32_t adsp_pump(PaganiAudioEngine *e, void *context, uint64_t now)
{
    SpeakerBackend *b = (SpeakerBackend *)context;
    SpeakerCore *c = b->core;
    b->engine = e;
    uint32_t wait = speaker_core_pump(c);
    if (c->fault == SpeakerFaultStall && c->running && b->restarts < kRestarts) {
        /* a stalled DSP queue: the run starts again (graphs stopped and started, the engine aligned anew) */
        b->restarts++;
        speaker_core_stop(c);
        if (speaker_core_start(c, &b->source)) fall_back(b);
    } else if (c->fault || c->dead) {
        if (c->running) speaker_core_stop(c);
        fall_back(b);
    }
    if (e->capturing) {
        const uint64_t frame = pagani_audio_engine_frame_at(e, now), period = e->period;
        if (frame > e->cap_pos + e->ring) {
            /* a stall longer than the ring: the ring holds silence anyway; go on one ring behind the timeline */
            e->cap_pos += (frame - e->cap_pos) / period * period - (e->ring / period - 1u) * period;
        }
        while (e->cap_pos + period <= frame) pagani_audio_engine_captured(e, 0, (uint32_t)period, now);
        const uint32_t w = ms_for(e->cap_pos + period - frame);
        if (w < wait) wait = w;
    }
    return wait;
}

void speaker_backend_init(SpeakerBackend *b, PaganiAudioBackend *out, SpeakerCore *core, uint64_t (*now_ticks)(void *context),
                          void *context)
{
    for (unsigned i = 0; i < sizeof *b; ++i) ((uint8_t *)b)[i] = 0;
    b->core = core;
    b->now_ticks = now_ticks;
    b->context = context;
    b->source.context = b;
    b->source.align = ring_align;
    b->source.take = ring_take;
    b->source.played = ring_played;
    out->name = "adsp";
    out->id = PaganiAudioBackendAdsp;
    out->clocked = 0;
    out->context = b;
    out->open = adsp_open;
    out->start_output = adsp_start_output;
    out->stop_output = adsp_stop_output;
    out->start_input = adsp_start_input;
    out->stop_input = adsp_stop_input;
    out->pump = adsp_pump;
}
