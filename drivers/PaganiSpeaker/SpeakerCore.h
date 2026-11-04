/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * The stage order, the command/answer handling and the period flow follow UniXNU-sheng package/AudioProbe.cpp (commit 08cde20e:
 * audioThread, command, nextAnswer, dspReady, openGraphs, startOutput, stopOutput, submit, handle, playTone), moved into portable C over
 * a table of platform operations so the host tests run the whole path against a simulated SoC, amplifiers and DSP. */
/* The OnePlus 13T speaker path above PaganiAdsp (boot argument pagani-speaker=1; pagani-audio-tone only together with it):
 *   1 Pins      I2C hub pins (function 1, pull-up, strong I2C pull), MI2S pins in their sleep state
 *   2 Rail      L15B (ldob15) 1.8 V voted through apps RSC DRV-2 (failure is logged; the rail may already be on)
 *   3 Bus       I2C hub clocks (GCC votes and halts, the SE4 RCG on the XO), serial engine 4 at 400 kHz (FIFO, polled)
 *   4 Amps      both resets pulsed; TFA9873 identified by REV (0x34 top, 0x35 bottom)
 *   5 AmpConfig the init program (keys, the revision's table, the container's TDM/board fields, "speaker" profile, no I/V feedback),
 *               read back and verified; the amplifiers stay powered down
 *   6 Dsp       GLINK "adsp_apps" open (PaganiAdsp boots the DSP on its own thread; up to 120 s)
 *   7 Apm       the audio framework answers ready (APM_CMD_GET_SPF_STATE)
 *   8 Smmu      the audio stream ids in pass-through on the storage stream's bank of the apps SMMU (0x1001 required; SpeakerSmmu.h)
 *   9 Buffer    64 KiB of low memory for the period buffers (the DSP takes 32-bit addresses, stream index in the upper word)
 *  10 Graphs    device graph (I2S sink, secondary MI2S, SD line, 48 kHz x 2 x 32 bit), stream graph (WR_SHMEM -> PCM_DEC 16-bit
 *               interleaved -> PCM_CNV device width, deinterleaved), shared memory media format, MEM_MAP
 *  11 Tone      the boot tone played (pagani-audio-tone=1: once on both speakers; =2: both, then 0x34 alone, then 0x35 alone)
 *  12 Serving   the ADSP backend handed to the PaganiAudio service
 * A playback run (the tone, or the iOS service through SpeakerBackend.c): MI2S pins active, PRM bit clock request (secondary MI2S
 * IBIT = rate x 2 x bits), PREPARE and START (device, then stream), the amplifiers powered up (clocks running), then periods: four in
 * flight at the DSP, each refilled when the DSP answers the one before. Every period passes the digital ceiling (SpeakerLimit.h:
 * 12 dB below full scale by default, one 1 ms block of look-ahead) on its way into the DSP buffer, so nothing the source gives (the
 * tone at any level, iOS at full volume) reaches the amplifiers above it. Stop: amplifiers down, STOP (stream, then device), clock
 * released, pins to sleep. Waiting happens only on the calling thread; the DSP thread only queues answers (speaker_core_receive).
 * Nothing panics: a failure records the stage (negative) and its code and logs one "[spk] FAIL" line. */
#ifndef PAGANI_SPEAKER_CORE_H
#define PAGANI_SPEAKER_CORE_H
#include <stdint.h>
#include "PaganiGpr.h"
#include "PaganiReach.h"
#include "SpeakerBoard.h"
#include "SpeakerHw.h"
#include "SpeakerLimit.h"
#include "SpeakerTone.h"
#ifdef __cplusplus
extern "C" {
#endif

enum {
    SpeakerStOff = 0, SpeakerStPins = 1, SpeakerStRail = 2, SpeakerStBus = 3, SpeakerStAmps = 4, SpeakerStAmpConfig = 5,
    SpeakerStDsp = 6, SpeakerStApm = 7, SpeakerStSmmu = 8, SpeakerStBuffer = 9, SpeakerStGraphs = 10, SpeakerStTone = 11,
    SpeakerStServing = 12,
};
/* Error codes of the core's own steps (the operations' codes pass through). */
enum {
    SpeakerErrNoDevice = -19,      /* no amplifier answered, the engine is not I2C, the DSP driver is off */
    SpeakerErrIo = -5,             /* a register did not read back, a transfer failed */
    SpeakerErrTimeout = -60,       /* no answer in time */
    SpeakerErrRefused = -71,       /* the DSP answered with an error status (in last_status) */
    SpeakerErrDown = -32,          /* the DSP channel closed (the DSP crashed) */
    SpeakerErrNoMemory = -12,
    SpeakerErrArgument = -22,
    SpeakerErrStopped = -125,      /* stopped by pagani-speaker-stop (not a failure) */
};
enum {SpeakerFaultNone = 0, SpeakerFaultStall = 1, SpeakerFaultDown = 2};

typedef struct SpeakerOps {
    void *context;
    uint64_t (*now_ms)(void *context);
    /* Sleeps until speaker_core_receive or _state ran (the glue wakes the waiter) or ms pass. */
    void (*wait)(void *context, uint32_t ms);
    void (*trace)(void *context, const char *line);
    /* The GLINK channel "adsp_apps" through PaganiAdsp: register it (0, or an error when the DSP driver is off), send one packet
     * (PaganiAdspOk = 0 or a PaganiAdsp error), and the DSP's state (0 with *stage and *crashed, or an error when off). */
    int (*channel)(void *context);
    int (*send)(void *context, const uint8_t *packet, uint32_t length);
    int (*dsp_state)(void *context, int32_t *stage, uint32_t *crashed);
    /* L15B at millivolts: 0 or an error; *detail for the log. Optional. */
    int (*rail)(void *context, uint32_t millivolts, uint32_t *detail);
    /* One audio stream id in pass-through for DMA to [pa, pa + bytes): 0 or an error; *detail for the log. */
    int (*smmu)(void *context, uint16_t sid, uint64_t pa, uint32_t bytes, uint32_t *detail);
    /* The period buffer: a write-combined mapping of the physical range, or null. */
    uint8_t *(*buffer)(void *context, uint64_t pa, uint32_t bytes);
    /* After CPU writes to the buffer and before the DSP is told (a full barrier). */
    void (*publish)(void *context);
    const SpeakerIo *io;
    /* Holds the UFS log's writer (1) or lets it go (0), by this driver's bit of pagani_panel_probe_quiet. Optional (null: no stretches). */
    void (*quiet)(void *context, unsigned on);
} SpeakerOps;

typedef struct {
    uint32_t stop_after;           /* 0: all; N: stop after stage N */
    uint32_t amps;                 /* bit 0: 0x34 (top), bit 1: 0x35 (bottom); 0 means both */
    uint32_t sd_line;              /* AR_I2S_SD0 (data0, gpio122) or AR_I2S_SD1 (data1, gpio124); 0 means SD0 */
    uint32_t device_bits;          /* 32 (backend_conf.xml) or 16; 0 means 32 */
    uint32_t prm_port;             /* the port PRM commands go from: AR_PRM_IID (2) or AR_PRM_PORT_ASM (7); 0 means 2 */
    uint32_t prm_clock;            /* 1: request the secondary MI2S bit clock from the PRM around a run; 0: do not */
    uint32_t feedback;             /* 1: keep the container's current-sense output on (TDMCSE); 0 (default): off */
    uint32_t i2c_pull;             /* 1: the strong I2C pull-up on gpio72/73 (qcom,i2c_pull) */
    uint32_t rail;                 /* 1: vote L15B */
    uint32_t sids;                 /* bit per SPEAKER_SIDS entry; bit 0 (0x1001) is always set */
    uint64_t buffer_pa;            /* 0 means SPEAKER_BUFFER_PA */
    uint32_t dsp_wait_ms;          /* 0 means 120000 */
    uint32_t apm_queries;          /* 0 means 100 (100 ms apart) */
    uint32_t ceiling_db;           /* the digital ceiling, dB below full scale: 0 means 12; clamped to 3..40 (it cannot be switched off) */
    uint32_t crumb_ms;             /* how long a line takes to reach the UFS log; 0: no UFS log, so nothing to hold */
} SpeakerConfig;

/* What a playback run plays: the next period into dst (SpeakerPeriodFrames x SpeakerChannels samples), one more period played, and
 * "the hardware runs now" (called once, after the graphs started and the amplifiers came up, before the first period is taken). */
typedef struct SpeakerSource {
    void *context;
    void (*align)(void *context);
    void (*take)(void *context, int16_t *dst);
    void (*played)(void *context);
} SpeakerSource;

#define SPEAKER_ANSWERS 32u
#define SPEAKER_ANSWER_BYTES 128u

typedef struct SpeakerCore {
    const SpeakerOps *ops;
    SpeakerConfig config;
    int32_t stage, error;          /* the last stage reached; negative: failed there with error */
    SpeakerBus bus;
    SpeakerClocks clocks;
    SpeakerSe se;
    uint32_t amps_found, amps_ready, amps_on;     /* bit per amplifier */
    uint16_t rev[SpeakerAmps];
    uint16_t amp_status[SpeakerAmps][Tfa9873StCount];
    uint32_t clks_failed;          /* power ups whose CLKS wait ran out */
    /* Answers: the DSP thread writes slots and head, the owner reads them and moves tail. */
    struct {uint32_t length; uint8_t data[SPEAKER_ANSWER_BYTES];} ring[SPEAKER_ANSWERS];
    volatile uint32_t head, tail, dropped;
    volatile uint32_t up, downs;
    uint32_t token;
    uint8_t packet[GPR_MAX_PACKET];
    uint32_t handle;               /* MEM_MAP */
    uint8_t *buffer;
    uint64_t buffer_pa, dsp_address;
    uint8_t graphs;                /* bit 0 device graph open, bit 1 stream graph open */
    uint8_t ready, dead, fault;
    uint32_t last_status, last_opcode, answers, stray, bad, sid_mask;
    /* playback */
    const SpeakerSource *source;
    uint8_t running, clock_on, status_read;
    uint8_t held;                  /* the UFS log's writer is held */
    uint32_t runs, in_flight, sent, played, write_errors, send_failures, stale, stalls, prm_status;
    uint64_t last_played_ms, run_start_ms;
    SpeakerLimit limit;            /* every period passes it on its way into the DSP buffer */
    int16_t period[SpeakerPeriodFrames * SpeakerChannels];   /* the source's period before the limiter */
    char line[200];
} SpeakerCore;

void speaker_core_init(SpeakerCore *c, const SpeakerOps *ops, const SpeakerConfig *config);
/* Stages 1..10. 1: the path is ready for playback; 0: stopped as configured or failed (stage / error). */
int speaker_core_bringup(SpeakerCore *c);
/* The boot tone on a ready path, on both speakers; 0, or the failing step's code. */
int speaker_core_tone(SpeakerCore *c, uint32_t hz, uint32_t ms, uint32_t atten_db);
/* The same on the speakers in channels (bit 0: slot 0, amplifier 0x34; bit 1: slot 1, amplifier 0x35; the others get silence). */
int speaker_core_tone_on(SpeakerCore *c, uint32_t hz, uint32_t ms, uint32_t atten_db, uint32_t channels);
/* pagani-audio-tone: mode 1 one tone on both speakers; mode 2 the walk: both, then 0x34 alone, then 0x35 alone, a second apart (one
 * line first says which is which). 0 when every tone played, else the first failing code. */
int speaker_core_tones(SpeakerCore *c, uint32_t mode, uint32_t hz, uint32_t ms, uint32_t atten_db);
/* Records a stage reached outside the core (SpeakerStServing). */
void speaker_core_reached(SpeakerCore *c, int32_t stage);

/* Playback runs. start: 0 when the run plays (the source is aligned and the first periods are queued); stop: back to idle. */
int speaker_core_start(SpeakerCore *c, const SpeakerSource *source);
void speaker_core_stop(SpeakerCore *c);
/* Handles the answers that arrived and refills the DSP's queue; returns how long the caller may wait (ms). Sets fault when the DSP
 * stops answering a running stream (1 s) or its channel closed. */
uint32_t speaker_core_pump(SpeakerCore *c);

/* The DSP thread (PaganiAdsp client callbacks): one packet from adsp_apps (copied; never waits), and the channel state. */
void speaker_core_receive(SpeakerCore *c, const uint8_t *data, uint32_t length);
void speaker_core_state(SpeakerCore *c, int up);

#ifdef __cplusplus
}
#endif
#endif
