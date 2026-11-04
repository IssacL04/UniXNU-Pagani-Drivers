/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * The haptic actuator bridge of the PaganiAudio service (ABI revision 2), in portable C like the engine (the kernel archive and the host
 * tests compile the same file). New here; no sheng counterpart.
 *
 * iOS plays every haptic (the haptic server of CoreHaptics and UIKit feedback, the vibe engine of system vibrations) into the HAL device
 * whose UID is "Actuator": 48 kHz mono Float32 in IO cycles of 256 frames. The plug-in (hal/PaganiAudioPlugIn.c) offers that device and
 * mixes what the audio server writes into the actuator ring of memory type 3 (PaganiAudioAbi.h, PaganiAudioActuatorControl), at most a
 * safety offset plus an IO cycle (384 frames, 8 ms) ahead of the play position. This bridge runs on the service thread: while a client
 * wants the actuator it takes each frame PAGANI_AUDIO_ACT_TAKE_FRAMES before it is due and clears it, low-passes and decimates the 48 kHz
 * samples to the PMIC FIFO rate (an average of 48000 / rate samples), scales them to the vibrator's signed 8-bit FIFO samples and feeds
 * the vibrator's stream API (drivers/PaganiHaptics, PaganiHaptics.h):
 *   - scale: a sample of 1.0 is the board's FIFO full scale (7450 mV, PAGANI_HAPTICS_FIFO_MV); the stream runs at the driver's runtime
 *     cap (pagani-haptics-vmax-mv, default 3600 mV, at most the full scale) as its VMAX, so int8 = round(x * 127 * full / cap), clipped to
 *     +-127 (counted): the drive voltage is x * 7450 mV up to the cap, whatever the cap;
 *   - sustained drive: the cap's default is the board's voltage for its short effects (qcom,wf-vmax-mv: at most 6 LRA periods, 35 ms);
 *     what the board drives for longer is 1260 mV (qcom,vmax-mv, DIRECT_PLAY). A stream can hold the cap for as long as a sound lasts (a
 *     ringer's vibration, a continuous CoreHaptics event), so drive above the sustained level (pagani-audio-haptics-sustain-mv, default
 *     1260) draws on a credit of 80 ms that comes back at a quarter of the speed; with the credit used up it is clipped to that level
 *     (counted): transients keep their strength, a long buzz runs at what the motor runs at on Android;
 *   - a stream begins at the first sample that is not silent after the scaling (nothing is sent while silent), carries the silences
 *     between sounds, and ends with a drain after 100 ms of silence, when the clients stop, or after 25 s (the driver's own limit is 30 s;
 *     the next sound begins a new stream);
 *   - a refused begin (the driver off: -19) is not asked again within a second; samples the driver's ring refuses are counted.
 * Nothing here waits: the stream calls only queue commands for the vibrator's thread (PaganiHaptics.cpp), so the service thread goes on,
 * and the plug-in's IO thread never meets the bridge at all (it only writes memory). */
#ifndef PAGANI_AUDIO_HAPTICS_H
#define PAGANI_AUDIO_HAPTICS_H
#include <stdint.h>
#include "PaganiAudioEngine.h"
#ifdef __cplusplus
extern "C" {
#endif

/* The vibrator's stream API as the bridge calls it (the kernel passes PaganiHaptics.h's functions; host tests a fake). begin, write and
 * end return what PaganiHaptics.h's calls return (write: samples taken, or negative); cap answers the runtime cap in mV (0: unknown). */
typedef struct {
    int (*begin)(void *context, uint32_t rate_hz, uint32_t vmax_mv);
    int (*write)(void *context, const int8_t *samples, uint32_t count);
    int (*end)(void *context, int drain);
    uint32_t (*cap)(void *context);
    void *context;
} PaganiAudioHapticsOps;

enum {
    PAGANI_AUDIO_HAPTICS_FULL_MV = 7450,     /* the board's FIFO full scale: what a sample of 1.0 drives */
    PAGANI_AUDIO_HAPTICS_CAP_MV = 3600,      /* the driver's default runtime cap, used when it does not say */
    PAGANI_AUDIO_HAPTICS_SUSTAIN_MV = 1260,  /* the board's sustained drive (DIRECT_PLAY): the default sustained level */
    PAGANI_AUDIO_HAPTICS_MIN_MV = 50,        /* the least level the driver can set (a VMAX step) */
    PAGANI_AUDIO_HAPTICS_BURST_MS = 80,      /* drive above the sustained level allowed at a stretch */
    PAGANI_AUDIO_HAPTICS_RECOVERY = 4,       /* samples at or below it that give one sample above it back */
    PAGANI_AUDIO_HAPTICS_RATE = 8000,        /* the default FIFO rate: 6 actuator frames a sample */
    PAGANI_AUDIO_HAPTICS_SILENCE_MS = 100,   /* a stream ends after this much silence */
    PAGANI_AUDIO_HAPTICS_STREAM_MS = 25000,  /* and at this length (the driver stops one at 30 s) */
    PAGANI_AUDIO_HAPTICS_RETRY_MS = 1000,    /* a refused begin is not asked again sooner */
    PAGANI_AUDIO_HAPTICS_KEEP_FRAMES = 512,  /* after the last client stops, frames still taken (the plug-in wrote up to 384 ahead) */
    PAGANI_AUDIO_HAPTICS_POLL_MS = 2,        /* the service thread's round while the actuator runs */
    PAGANI_AUDIO_HAPTICS_CHUNK = 256,        /* int8 samples handed over per write call at most */
};

typedef struct {
    uint64_t frames;             /* actuator frames taken (and cleared) */
    uint64_t loud_frames;        /* of those, frames that were not silent */
    uint64_t samples;            /* int8 samples the driver took */
    uint64_t refused;            /* int8 samples the driver did not take (its ring full, or it stopped) */
    uint64_t skipped;            /* frames passed over after a stall longer than the ring (cleared, never sent) */
    uint32_t runs, run_ends;     /* actuator runs (a client wanted it) and their ends */
    uint32_t streams;            /* stream begins the driver accepted */
    uint32_t stream_ends;        /* stream ends: silence, the clients' stop, or the length limit */
    uint32_t rollovers;          /* of those, at the length limit */
    uint32_t begin_failed;       /* begins the driver refused */
    int32_t last_rc;             /* the driver's last negative answer (0 none) */
    uint32_t peak_in;            /* the largest actuator sample taken (x 32767) */
    uint32_t peak_out;           /* the largest |int8| sent */
    uint32_t clips;              /* int8 samples clipped at the cap */
    uint32_t limited;            /* int8 samples clipped to the sustained level (the credit was used up) */
    uint32_t stale;              /* non-silent frames left in the ring when a run ended (written after its clients stopped), cleared */
    int32_t latency_us;          /* the last stream begin: microseconds from the plug-in's write of its first loud frame (-1 unknown) */
    int32_t latency_max_us;
    uint32_t cap_mv, full_mv, rate;   /* what the last begin used */
} PaganiAudioHapticsStats;

typedef struct {
    PaganiAudioActuatorControl *ctl;
    int16_t *ring;
    uint32_t ring_frames, take_frames, rate, decimate, full_mv, cap_mv, silence_samples, stream_samples;
    uint32_t sustain_mv;         /* the sustained level */
    uint32_t sustain_q;          /* the same as an int8 sample at the stream's VMAX (127: the cap is no higher) */
    uint32_t credit;             /* drive above the sustained level used up, in 1 / PAGANI_AUDIO_HAPTICS_RECOVERY samples */
    uint32_t credit_max;         /* the credit: PAGANI_AUDIO_HAPTICS_BURST_MS of samples */
    PaganiAudioHapticsOps ops;
    int running, stop_pending, streaming;
    uint64_t taken;              /* the bridge's own copy of the take position (the page's is only published) */
    uint64_t stop_frame;         /* with stop_pending: frames are taken up to here, then the run ends */
    uint64_t ended_frame;        /* where the last run ended (the credit recovers between runs too) */
    uint64_t retry_host;         /* no begin before this host time (after a refusal) */
    uint64_t quiet;              /* silent samples in a row inside the stream */
    uint64_t length;             /* samples of the stream so far */
    int32_t sum;                 /* the decimation's running sum and count */
    uint32_t summed;
    uint32_t pending;            /* int8 samples in out[] not handed over yet */
    int8_t out[PAGANI_AUDIO_HAPTICS_CHUNK];
    PaganiAudioHapticsStats stats;
} PaganiAudioHaptics;

/* Fills the actuator header (memory of PAGANI_AUDIO_ACT_BYTES: the header page, then the ring) on the engine's timeline and clears the
 * ring. rate: the FIFO rate (8000, 16000, 24000 or 48000; anything else is 8000); full_mv: what a sample of 1.0 drives (0: 7450; at most
 * 7450); sustain_mv: the sustained level (0: 1260; between 50 mV and the full scale). 0 on success, -1 on a missing argument or an engine
 * without a timeline. */
int pagani_audio_haptics_init(PaganiAudioHaptics *h, const PaganiAudioEngine *engine, void *memory, const PaganiAudioHapticsOps *ops,
                              uint32_t rate, uint32_t full_mv, uint32_t sustain_mv);
/* One round at host time now (the service thread's): starts a run when a client wants the actuator (engine->act_users), takes the frames
 * that came due, feeds the vibrator, ends the run PAGANI_AUDIO_HAPTICS_KEEP_FRAMES after the last client stopped. Returns how long the
 * thread may sleep (ms): PAGANI_AUDIO_HAPTICS_POLL_MS while running, else 1000. */
uint32_t pagani_audio_haptics_poll(PaganiAudioHaptics *h, const PaganiAudioEngine *engine, uint64_t now);
/* The int8 FIFO sample for the sum of count actuator samples (16-bit, x 32767): round(sum / count * 127 * full / cap), +-127 at most;
 * *clipped set when it was more. Exposed for the host tests. */
int8_t pagani_audio_haptics_scale(int32_t sum, uint32_t count, uint32_t full_mv, uint32_t cap_mv, int *clipped);

#ifdef __cplusplus
}
#endif
#endif
