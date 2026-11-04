/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The "adsp" backend of the PaganiAudio service (drivers/PaganiAudio PaganiAudioEngine.h): the iOS audio server's playback ring goes to
 * the speakers through the core's playback runs. The service calls every function on its own thread:
 *   open          the path is ready (stage 10) and the engine's period and channels are the speaker path's (480 frames, stereo);
 *   start_output  a run whose source is the ring: the engine's next period is taken (pagani_audio_engine_take) into a DSP slot for
 *                 every period the DSP answers (pagani_audio_engine_played), four in flight; once the graphs and the amplifiers run,
 *                 the engine is aligned to the period that plays now (pagani_audio_engine_align_output);
 *   stop_output   the run stops (amplifiers down, graphs stopped, clock released);
 *   start_input   accepted: capture delivers silence by the clock until the microphone path exists;
 *   pump          the DSP's answers (the PaganiAdsp callback wakes the service through pagani_audio_wake), the refills, the silent
 *                 capture; a stalled DSP restarts the run (three times), a closed channel or more stalls hand the service back to
 *                 the silent backend.
 * The backend is unclocked (the DSP paces playback), so the engine measures the DSP's lead and resyncs a drift as with sheng. */
#ifndef PAGANI_SPEAKER_BACKEND_H
#define PAGANI_SPEAKER_BACKEND_H
#include <stdint.h>
#include "PaganiAudioEngine.h"
#include "SpeakerCore.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    SpeakerCore *core;
    uint64_t (*now_ticks)(void *context);   /* the host clock of the engine's timeline (mach_absolute_time) */
    void *context;
    PaganiAudioEngine *engine;
    SpeakerSource source;
    uint32_t restarts, refused, fallbacks;
    uint8_t fallback;                       /* the silent backend was asked for */
} SpeakerBackend;

/* Fills out as the "adsp" backend (PaganiAudioBackendAdsp, unclocked) of the core; b and out must live as long as the service. */
void speaker_backend_init(SpeakerBackend *b, PaganiAudioBackend *out, SpeakerCore *core, uint64_t (*now_ticks)(void *context),
                          void *context);

#ifdef __cplusplus
}
#endif
#endif
