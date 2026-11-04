/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * The ring and timeline engine of the PaganiAudio service, in portable C (the kernel archive and the host tests compile the same file).
 * The period flow, the measured lead and the drift resyncs follow UniXNU-sheng package/AudioProbe.cpp (revision 08cde20: setupShared,
 * framesNow, syncPlayback, syncCapture, submit, startOutput, stopOutput, startInput, captured, serve); the backend interface, the clocked
 * silent backend and the per-client requests are new.
 *
 * The engine owns the control page and the two rings of PaganiAudioAbi.h and one timeline anchored on the host clock. A backend carries
 * the rings to hardware: the silent backend (this file) takes each playback period by the clock and clears it, and captures zeros, so the
 * audio server sees a running device with no hardware at all; a DSP backend takes each period when it queues it to the DSP and reports it
 * played when the DSP answers (pagani_audio_engine_take / _played / _captured). All engine calls except the requests and the backend switch
 * come from one service thread. Nothing the plug-in can write is trusted: the engine keeps its own copy of every constant of the control
 * page and only counts what it reads back (the plug-in's write position). */
#ifndef PAGANI_AUDIO_ENGINE_H
#define PAGANI_AUDIO_ENGINE_H
#include <stdint.h>
#include "PaganiAudioAbi.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct PaganiAudioEngine PaganiAudioEngine;

/* A backend. Every call comes from the service thread, never two at once. */
typedef struct PaganiAudioBackend {
    const char *name;                        /* for the log: "silent", "adsp" */
    uint32_t id;                             /* PaganiAudioBackend*, published in the control page */
    /* 1: the backend follows the timeline exactly (it moves periods by the host clock), so the engine never resyncs its positions. */
    uint32_t clocked;
    void *context;
    /* Optional: brings the path up before the backend is first used (a switch calls it); 0 on success. It may take time: only the service
     * thread waits. */
    int (*open)(PaganiAudioEngine *engine, void *context);
    /* Playback starts: the engine has cleared the ring and set next = done = first to the period that plays now. A backend whose start
     * takes time calls pagani_audio_engine_align_output() once its hardware runs. 0 on success; on failure the engine stays stopped. */
    int (*start_output)(PaganiAudioEngine *engine, void *context, uint64_t now);
    void (*stop_output)(PaganiAudioEngine *engine, void *context);
    /* Capture starts: the engine has cleared the ring and set the capture position to the frame that plays now. 0 on success. */
    int (*start_input)(PaganiAudioEngine *engine, void *context, uint64_t now);
    void (*stop_input)(PaganiAudioEngine *engine, void *context);
    /* Periodic work at host time now. Returns the longest the service thread may sleep before the next call (ms). Optional. */
    uint32_t (*pump)(PaganiAudioEngine *engine, void *context, uint64_t now);
} PaganiAudioBackend;

/* One opener of the service (a user client): which directions it wants running. Zero-initialised. */
typedef struct {
    volatile uint32_t bits;                  /* PaganiAudioOutputRunning | PaganiAudioInputRunning | PaganiAudioActuatorRunning |
                                                PaganiAudioClientReleased */
} PaganiAudioClientState;
/* Set by pagani_audio_engine_release: the client closed; its requests are refused from then on. */
enum {PaganiAudioClientReleased = 1u << 31};

typedef struct {
    uint32_t out_starts, out_stops, out_refused, in_starts, in_stops, in_refused;
    uint64_t taken, played, captured;        /* playback periods taken and played, capture frames delivered */
    uint32_t late, holes;                    /* the service's own counts (the control page copies can be overwritten by the plug-in) */
    uint32_t skipped;                        /* playback periods the service cleared without taking them (a resync or a stall) */
    uint32_t play_resyncs, cap_resyncs, cap_short, cap_skipped;
    int32_t drift_min, drift_max, base_lead;
    uint32_t peak_out, peak_in;              /* largest sample magnitude the backends carried */
    uint32_t switches, switch_failed;
    uint32_t requests, refused_requests;
} PaganiAudioEngineStats;

struct PaganiAudioEngine {
    PaganiAudioControl *ctl;
    int16_t *out, *in;
    /* The engine's own copies of the timeline (never read back from the shared page). One frame lasts tick_num / tick_den host ticks
     * (400 / 1 on 19.2 MHz); ticks_100ms is the plug-in's write recency window. */
    uint64_t anchor, tick_num, tick_den, ticks_100ms;
    uint32_t period, ring, out_channels, in_channels, in_flight, keep_ms, lead;
    const PaganiAudioBackend *backend;
    const PaganiAudioBackend *volatile pending;   /* a backend to switch to at the next poll */
    volatile uint32_t out_users, in_users;        /* clients that want each direction */
    volatile uint32_t act_users;                  /* clients that want the haptic actuator (revision 2; PaganiAudioHaptics.c serves it) */
    /* Playback: periods are absolute (period n holds timeline frames n * period ..); next to take, played so far, the first of the run. */
    int running, stop_pending;
    uint64_t next, done, first, stop_asked_ms;
    int64_t base_lead;
    /* Capture: the timeline frame after the last one delivered, periods and frames delivered in this run. */
    int capturing;
    uint64_t cap_pos, cap_done, cap_frames;
    int64_t base_capture;
    /* When the backend last refused to start each direction (ms, 0 never): it is not asked again within a second. */
    uint64_t out_refused_ms, in_refused_ms;
    PaganiAudioEngineStats stats;
};

/* Fills the control page (PAGANI_AUDIO_CONTROL_BYTES) and clears both rings (PAGANI_AUDIO_RING_FRAMES frames of
 * PAGANI_AUDIO_OUT_CHANNELS and PAGANI_AUDIO_IN_CHANNELS samples), anchors the timeline at host time now and selects backend. numer and
 * denom are the host timebase (nanoseconds = ticks * numer / denom). options goes to the control page. 0 on success; -1 when the
 * timebase cannot express a frame as a ratio of 32-bit integers or an argument is missing. (The revision 2 block, PaganiAudioRevision,
 * is the service's to write: PaganiAudio.cpp, after this.) */
int pagani_audio_engine_init(PaganiAudioEngine *engine, void *control, int16_t *out, int16_t *in, uint64_t now, uint32_t numer,
                             uint32_t denom, const PaganiAudioBackend *backend, uint32_t options);
/* The timeline frame that plays at host time now (0 before the anchor). */
uint64_t pagani_audio_engine_frame_at(const PaganiAudioEngine *engine, uint64_t now);
/* The host time at which timeline frame frame plays. */
uint64_t pagani_audio_engine_host_at(const PaganiAudioEngine *engine, uint64_t frame);

/* A client's request (any thread): PaganiAudioStart/StopOutput/Input, and PaganiAudioStart/StopActuator (revision 2: counted in
 * act_users, which the kernel's haptics bridge follows; the user client passes them only when the actuator is offered). 0 accepted, -1 an
 * unknown selector or a released client. A direction runs while any client wants it. */
int pagani_audio_engine_request(PaganiAudioEngine *engine, PaganiAudioClientState *client, uint32_t selector);
/* The client went away (any thread): whatever it wanted is released, and a request that races the close (another thread of the dying
 * process) is refused instead of counting a direction that no close would release any more. Releasing twice changes nothing. */
void pagani_audio_engine_release(PaganiAudioEngine *engine, PaganiAudioClientState *client);
/* Switch to another backend at the next poll (any thread); the running directions stop and start again on it. */
void pagani_audio_engine_set_backend(PaganiAudioEngine *engine, const PaganiAudioBackend *backend);

/* One round of the service thread at host time now (now_ms the same instant in milliseconds, for the keep timer): applies a pending
 * backend switch, starts and stops each direction as the clients want (output stays up keep_ms after the last client stops it) and runs
 * the backend's pump. Returns how long the thread may sleep (ms). */
uint32_t pagani_audio_engine_poll(PaganiAudioEngine *engine, uint64_t now, uint64_t now_ms);

/* For backends. Takes playback period n out of the ring (copied to dst when not null, period * out_channels samples) and clears it there;
 * counts it late when the plug-in, writing recently, had not reached its end, and a hole when it was written but holds a silent run of 64
 * frames between sound. Returns PaganiAudioTakeLate / PaganiAudioTakeHole bits. */
enum {PaganiAudioTakeLate = 1u, PaganiAudioTakeHole = 2u};
uint32_t pagani_audio_engine_take(PaganiAudioEngine *engine, uint64_t n, int16_t *dst, uint64_t now);
/* One more queued period played (a DSP answered it, or its time passed). An unclocked backend's queue lead is measured once per run
 * (published as lead_frames with two periods of margin) and next is set back to it when it drifts three periods away. */
void pagani_audio_engine_played(PaganiAudioEngine *engine, uint64_t now);
/* count captured frames (mono samples) for the capture position; src null delivers silence. An unclocked backend's capture position
 * follows the play position the same way; frames a resync skips read as silence. */
void pagani_audio_engine_captured(PaganiAudioEngine *engine, const int16_t *src, uint32_t count, uint64_t now);
/* Playback hardware runs from now: the period that plays now becomes next = done = first; the periods the start took meanwhile are
 * cleared (counted as skipped). */
void pagani_audio_engine_align_output(PaganiAudioEngine *engine, uint64_t now);

/* The silent backend: no hardware; playback periods are taken in_flight periods ahead of the play position and cleared, capture
 * delivers zeros as the timeline passes. */
extern const PaganiAudioBackend pagani_audio_silent_backend;

/* The kernel service (PaganiAudio.cpp), for a hardware backend such as the ADSP's: switch to it (0 accepted; it is opened on the
 * service thread, and the old backend stays when it cannot open), and wake the service thread from an answer callback so the backend's
 * pump runs at once. */
int pagani_audio_set_backend(const PaganiAudioBackend *backend);
void pagani_audio_wake(void);

#ifdef __cplusplus
}
#endif
#endif
