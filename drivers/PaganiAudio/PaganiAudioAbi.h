/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * Ported from UniXNU-sheng package/ShengAudioAbi.h (revision 08cde20): names changed (Sheng -> Pagani, magic 'PGAU'), the reserved
 * word names the backend and the former tail padding carries the options. Layout and protocol are otherwise sheng's.
 *
 * New here: the service publishes how far it has taken playback (out_taken), and the plug-in drops what the audio server mixes behind
 * it: a late IO cycle's frames would otherwise wait in the ring for a whole turn and play 1.37 s late.
 *
 * The interface between the PaganiAudio kernel service (PaganiAudio.cpp, IOKit class PaganiAudio) and the audio server plug-in that iOS
 * loads (hal/PaganiAudioPlugIn.c). The plug-in opens the service, maps three memory types and starts and stops each direction.
 *
 * Memory type 0 is the control page; 1 the playback ring; 2 the capture ring. Rings hold 16-bit little-endian interleaved samples,
 * ring_frames frames each. Sample time t lives at frame t % ring_frames. Sample time 0 of a direction is the frame the backend plays (or
 * captures) at host time anchor_host of that direction; one frame lasts ticks_per_frame_q32 / 2^32 host ticks (400 on the 19.2 MHz timer
 * of the SM8750: computed in 128 bits, 1e9 * 12 << 32 does not fit 64). The anchors are set once, when the service starts, so one
 * timeline holds for the whole boot. The service takes each playback period lead_frames or less before it plays and clears it, and
 * fills capture frames behind the play position, so the plug-in writes at least lead_frames ahead and reads at least lead_frames behind.
 * The plug-in adds each IO context's mix to the ring (several contexts of the audio server write the same frames); the clearing keeps a
 * frame from being added to what the ring held one turn earlier. */
#ifndef PAGANI_AUDIO_ABI_H
#define PAGANI_AUDIO_ABI_H
#include <stdint.h>

#define PAGANI_AUDIO_CLASS "PaganiAudio"
#define PAGANI_AUDIO_USER_CLIENT_CLASS "PaganiAudioUserClient"
#define PAGANI_AUDIO_MAGIC 0x50474155u        /* 'PGAU' */
#define PAGANI_AUDIO_VERSION 1u
#define PAGANI_AUDIO_RATE 48000u
#define PAGANI_AUDIO_OUT_CHANNELS 2u
#define PAGANI_AUDIO_IN_CHANNELS 1u
#define PAGANI_AUDIO_PERIOD_FRAMES 480u        /* 10 ms */
#define PAGANI_AUDIO_RING_FRAMES 65536u        /* 1.37 s: room for the 300 ms lead and the audio server's late IO cycles */
#define PAGANI_AUDIO_LEAD_FRAMES (30u * PAGANI_AUDIO_PERIOD_FRAMES)   /* the least lead the service publishes (300 ms) */
#define PAGANI_AUDIO_CONTROL_BYTES 4096u

enum {PaganiAudioMemoryControl = 0, PaganiAudioMemoryOutput = 1, PaganiAudioMemoryInput = 2};
enum {PaganiAudioStartOutput = 0, PaganiAudioStopOutput = 1, PaganiAudioStartInput = 2, PaganiAudioStopInput = 3};
/* Scalar input: the address of CoreAudio's host clock frequency constant in the caller (24 MHz, the timer of Apple's chips); the service
 * sets it to this board's timer frequency. */
enum {PaganiAudioSetHostClock = 4};
enum {PaganiAudioOutputRunning = 1u << 0, PaganiAudioInputRunning = 1u << 1, PaganiAudioOutputReady = 1u << 2, PaganiAudioInputReady = 1u << 3};
/* PaganiAudioControl.backend: what carries the rings. Silent consumes playback by the clock and captures zeros (no hardware). */
enum {PaganiAudioBackendNone = 0, PaganiAudioBackendSilent = 1, PaganiAudioBackendAdsp = 2};
/* PaganiAudioControl.options (boot argument pagani-audio-trace): bit 0 lets the plug-in trace the audio server's routing layer
 * (VirtualAudio.plugin's failing property calls and C++ throws, rebound through its symbol pointers) into the trace section.
 * Bit 1 (set unless boot argument pagani-audio-probe=0) lets it probe, read only, what the audio server makes of the device: the
 * probe section at the end of the page. */
enum {PaganiAudioOptionTraceRouting = 1u << 0, PaganiAudioOptionProbeRoute = 1u << 1};

typedef struct {
    uint32_t magic, version;
    uint32_t rate, out_channels, in_channels, period_frames, ring_frames, lead_frames;
    volatile uint32_t state;                 /* PaganiAudio*Running / *Ready bits */
    uint32_t backend;                        /* PaganiAudioBackend* (sheng: reserved0) */
    uint64_t ticks_per_frame_q32;            /* host ticks per frame, 32.32 fixed point */
    volatile uint64_t out_anchor_host, in_anchor_host;
    volatile uint64_t out_frames, in_frames; /* frames the service consumed / produced so far in this run */
    volatile uint32_t out_late, in_late;     /* periods the service had to take or deliver without fresh data */
    volatile uint32_t out_seq, in_seq;       /* incremented around anchor updates (odd while writing) */
    volatile uint64_t out_written;           /* the plug-in: the sample time after the last frame it wrote */
    volatile uint64_t in_written;            /* the service: the sample time after the last captured frame in the ring */
    volatile uint64_t out_written_host;      /* the plug-in: host time of its last write */
    volatile uint32_t out_holes;             /* the service: written periods that held a run of 64 or more silent frames */
    uint32_t options;                        /* PaganiAudioOption* (sheng: tail padding) */
    volatile uint64_t out_taken;             /* the service: the sample time after the last frame it took (cleared) in this run */
} PaganiAudioControl;

/* Second half of the control page: the plug-in records what the audio server asked for and it does not answer (selector, object id
 * << 24 | scope fourcc's first byte), so the kernel log names them without the unified log. */
#define PAGANI_AUDIO_DIAG_OFFSET 2048u
#define PAGANI_AUDIO_DIAG_SLOTS 32u
typedef struct {
    volatile uint32_t count, loads;
    volatile uint32_t selector[PAGANI_AUDIO_DIAG_SLOTS], where[PAGANI_AUDIO_DIAG_SLOTS];
    volatile uint32_t volume_milli, volume_sets;  /* the output volume (scalar x 1000) and how often the audio server set it */
    volatile uint32_t mix_peak;                   /* the largest output sample before the volume, x 32767 */
    volatile uint32_t in_misses;                  /* input reads that asked for frames the service had not captured yet */
    /* How far ahead of the play position the audio server writes (output time minus now, frames) and how far behind it reads, the
     * least and most of each, and the frames of the last write. */
    volatile int32_t ahead_min, ahead_max, behind_min, behind_max;
    volatile uint32_t io_frames, writes;
    /* Writes that began inside frames already written (another IO context of the audio server mixing into the same ring), and the most
     * frames such a write went back. */
    volatile uint32_t overlaps, overlap_max;
    /* Every frame written; writes that began past the end of the one before (a skipped stretch) and the frames skipped; the time from
     * one write to the next in buckets below 15, 25, 40, 80 and 160 ms and above (one IO cycle of 1024 frames is 21.3 ms). */
    volatile uint32_t frames_total, gaps, gap_frames;
    volatile uint32_t interval[6];
    volatile uint64_t last_end, last_host;
    volatile int32_t skew_min, skew_max;          /* the audio server's current sample time minus the timeline's, least and most */
    /* The audio server's IO thread as the plug-in sees it: current and base priority, scheduling policy, whether it has a time constraint
     * and that constraint's period, computation and constraint (host ticks); the lowest current priority seen. */
    volatile int32_t io_pri, io_base, io_policy, io_tc, io_pri_min;
    volatile uint32_t io_period, io_computation, io_constraint;
    /* CoreAudio's host clock frequency (Hz) after the plug-in asked the service to set it, and that request's result (0 done, -1 the
     * constant was not found, else the kernel's return). */
    volatile uint32_t host_clock;
    volatile int32_t host_clock_result;
    /* Frames the audio server mixed behind out_taken (too late: the service had taken them) and the writes that held any; dropped. */
    volatile uint32_t dropped_frames, dropped_writes;
} PaganiAudioDiag;
/* After it, every distinct property access in the order the audio server made it: selector, then object << 24 | scope's first byte << 16 |
 * kind (1 set, 0 get, 4 a failing call of the routing layer, 8 a throw there) | 2 if it failed. */
#define PAGANI_AUDIO_TRACE_OFFSET 2560u
#define PAGANI_AUDIO_TRACE_SLOTS 128u
typedef struct {
    volatile uint32_t count;
    volatile uint32_t entry[PAGANI_AUDIO_TRACE_SLOTS][2];
} PaganiAudioTrace;

/* After it, the route probe (option PaganiAudioOptionProbeRoute). The volume HUD is decided around the plug-in, inside the audio
 * server (iOS 21H461): SpringBoard changes the volume only while MediaExperience reports that the current route has a volume control
 * (+[SBVolumeControl _isVolumeChangeAllowedForState:error:]), which MediaExperience reads from the routing layer's default virtual
 * device, property 'vcsp' in the output scope (vaemCurrentRouteHasVolumeControl); and the routing layer (VirtualAudio.plugin,
 * Device_Thunderbolt_Aspen.cpp) gives this device's Thunderbolt port a volume control only when the HAL answers {'vold', output, main}
 * for the device, which it translates to the plug-in's volume control. The plug-in reads those from a queue of its own a while after
 * it loads and after volume changes and IO starts, and writes them here; the kernel puts them in the boot log ("audio route"). The
 * plug-in makes seq odd while it writes and even (non-zero) when a probe is complete, a seqlock: the writer stores the odd seq, a
 * release fence, the fields, then the even seq with release; the reader loads seq with acquire, the fields, an acquire fence, seq again. */
#define PAGANI_AUDIO_PROBE_OFFSET 3648u
enum {PaganiAudioProbeTimer = 1, PaganiAudioProbeVolume = 2, PaganiAudioProbeStart = 3};
typedef struct {
    volatile uint32_t seq;
    volatile uint32_t reason;               /* PaganiAudioProbe*: what made the last probe run */
    volatile uint32_t devices;              /* audio devices the HAL lists ('dev#') */
    volatile uint32_t va_plugin;            /* the routing layer's plug-in object (bundle com.apple.audio.CoreAudio.VirtualAudio), 0 none */
    volatile uint32_t vad;                  /* its default virtual device (UID VirtualAudioDevice_Default), 0 none */
    volatile uint32_t ours;                 /* this device's HAL object (UID PaganiAudioDevice), 0 none */
    volatile int32_t ours_vold;             /* the HAL has {'vold', output, main} on this device: 1 or 0; -1 not asked */
    volatile int32_t ours_running;          /* 'gone' of this device (running in any process): 1 or 0; -1 unread */
    volatile int32_t vcsp_status;           /* reading 'vcsp' (output) of the default virtual device: 0 or the error */
    volatile uint32_t vcsp;                 /* its value; MediaExperience's CurrentRouteHasVolumeControl is (vcsp_status | vcsp) != 0 */
    volatile uint32_t category, mode;       /* the routing layer's category ('rctx') and mode ('inpm'), four-character codes, 0 unread */
    volatile int32_t ports_status;          /* reading the routing layer's active ports ('aprc', output): 0 or the error */
    volatile uint32_t out_ports;            /* the default virtual device's active output ports */
    volatile uint32_t port_type[2];         /* 'ptyp' of the first two (this device's output port: 'ptbo'), 0 unread. With no active
                                               output port (out_ports 0) they say why instead: [0] the first connected output port's type
                                               ('prts'), [1] four letters: MediaExperience's serialization listener ('srzl') S/s/?, this
                                               device hogged by the audio server ('oink', the routing layer took it) H/h/x/?, the
                                               connected output ports 0-9/+/?, a Thunderbolt one among them T/t (hal/PaganiAudioPlugIn.c) */
    volatile uint32_t vad_volume_milli;     /* the default virtual device's output volume scalar ('volm') x 1000, 0xffffffff unread */
    volatile uint32_t vol_queries;          /* property reads of the plug-in's volume control (the HAL and the routing layer asking) */
} PaganiAudioProbe;

/* ---- ABI revision 2: additions only (everything above keeps its place and meaning; PAGANI_AUDIO_VERSION stays 1) ----------------------
 * A revision 2 service writes the revision block below into the free part of the control page (an older one leaves it zero), so a newer
 * plug-in never uses an addition an older service lacks, and an older plug-in never looks at them. The additions:
 *  - options bits 2..4 (boot arguments pagani-audio-mx=1, pagani-audio-srzl=1, and the actuator: pagani-haptics-ios=1 or 2 with
 *    pagani-haptics=1), each default off;
 *  - the MediaExperience progress block (PaganiAudioMx), written by the plug-in with option PaganiAudioOptionMxProbe;
 *  - the haptic actuator: memory type 3 (a header page and a mono ring) and selectors 5/6, only with PaganiAudioOptionActuator. */
#define PAGANI_AUDIO_REVISION 2u
#define PAGANI_AUDIO_REVISION_MAGIC 0x50474152u   /* 'PGAR' */
#define PAGANI_AUDIO_REVISION_OFFSET 1024u        /* between the control block (136 bytes) and the diagnostics (2048) */
typedef struct {
    uint32_t magic, revision;
    uint32_t actuator_frames;                 /* the actuator ring's frames when the actuator is offered (option bit 4), else 0 */
    uint32_t actuator_rate;
} PaganiAudioRevision;

/* Option bits of revision 2 (PaganiAudioControl.options). MxProbe (pagani-audio-mx=1): the plug-in also looks, read only, at how far
 * MediaExperience's start got inside the audio server (PaganiAudioMx below). SrzlRepair (pagani-audio-srzl=1): the plug-in installs
 * MediaExperience's own serialization listener on the routing layer once, when MediaExperience's one attempt is known to have been made
 * and lost (README.md, route-re 2.3). Actuator: the plug-in offers the haptic actuator device (UID "Actuator") on memory type 3.
 * VaSettings (pagani-audio-srzl bit 1): with the actuator offered, the plug-in gives VirtualAudio's actuator settings the values of a
 * product without I/V sense before VirtualAudio's deferred initialisation asks for them (README.md: on the J120 product they throw). */
enum {PaganiAudioOptionMxProbe = 1u << 2, PaganiAudioOptionSrzlRepair = 1u << 3, PaganiAudioOptionActuator = 1u << 4,
      PaganiAudioOptionVaSettings = 1u << 5};

/* MediaExperience's progress inside the audio server (option PaganiAudioOptionMxProbe; route-re 6 R1), written by the plug-in a while after
 * it loads (with the route probe), read by the kernel for the boot log ("audio mx"). Plain words: the plug-in writes the fields, then
 * gen; the kernel prints a row only when gen held still for one of its rounds (no seqlock: a diagnostic, and the route probe's reader
 * keeps the only acquire fence of the kernel glue). progress bits (PaganiAudioMx*): MediaExperience's zero-initialised globals in the
 * order its start sets them, the audio control queue (ACQ) set and alive, and the routing layer's device filter mode; Unverified when
 * the code at MXInitialize was not 21H461's (then nothing else was read). */
#define PAGANI_AUDIO_MX_OFFSET 3728u              /* after the route probe (3648 + 72) */
enum {
    PaganiAudioMxStarted = 1u << 0,           /* MXInitialize.onceToken set */
    PaganiAudioMxDone = 1u << 1,              /* MXInitialize finished (onceToken ~0, or libdispatch's generation form: low bits 2) */
    PaganiAudioMxSession = 1u << 2,           /* gCMSM: the session manager's mutex made */
    PaganiAudioMxScreen = 1u << 3,            /* gCMScreen: CMScreenInitialize done */
    PaganiAudioMxIds = 1u << 4,               /* gIDSServer: the IDS calls done */
    PaganiAudioMxRoutes = 1u << 5,            /* the current routes vector: device state, IOKit, silent mode done */
    PaganiAudioMxContext = 1u << 6,           /* FigRoutingManagerCopySystemContext done (right before FigVAEndpointManagerCreate) */
    PaganiAudioMxEndpoint = 1u << 7,          /* A: FigVAEndpointManagerCreate entered */
    PaganiAudioMxVaPlugin = 1u << 8,          /* B: the routing layer's plug-in id cached ('srzl' is next) */
    PaganiAudioMxSerial = 1u << 9,            /* C: the 'srzl'/'rwbu' calls returned (whatever their result) */
    PaganiAudioMxQueue = 1u << 10,            /* AudioToolbox handed MediaExperience its control queue (ACQ) */
    PaganiAudioMxQueueAlive = 1u << 11,       /* an empty block sent to the ACQ ran within 2 s */
    PaganiAudioMxFilterShift = 12,            /* bits 12-13: the routing layer's device filter mode (2: six UIDs only, 3: all), 0 unread */
    PaganiAudioMxNoSymbol = 1u << 30,         /* MXInitialize not found */
    PaganiAudioMxUnverified = 1u << 31,       /* MXInitialize's code is not 21H461's: nothing read */
};
/* srzl_state: what the repair (option PaganiAudioOptionSrzlRepair) did: 0 nothing yet, 1 not needed (the listener is there), 2 waiting
 * (MediaExperience has not made its attempt: C not set, or the routing layer not initialised), 3 done (both sets succeeded), 4 failed
 * (srzl_status / rwbu_status hold the HAL's results), 5 refused (the listener code is not 21H461's). */
enum {PaganiAudioSrzlNone = 0, PaganiAudioSrzlPresent = 1, PaganiAudioSrzlWaiting = 2, PaganiAudioSrzlDone = 3, PaganiAudioSrzlFailed = 4,
      PaganiAudioSrzlRefused = 5};
/* va_settings: what became of VirtualAudio's actuator settings (option PaganiAudioOptionVaSettings): 0 nothing yet, 1 given before its
 * deferred initialisation, 2 given, but its device manager already existed (the initialisation may have thrown before them), 3 nothing
 * written: they were there already, 4 refused: not 21H461's VirtualAudio, 5 refused: not the product they are meant for. */
enum {PaganiAudioVaSettingsNone = 0, PaganiAudioVaSettingsGiven = 1, PaganiAudioVaSettingsLate = 2, PaganiAudioVaSettingsPresent = 3,
      PaganiAudioVaSettingsOtherBuild = 4, PaganiAudioVaSettingsOtherProduct = 5};
typedef struct {
    volatile uint32_t gen;                    /* incremented after every write of the block */
    volatile uint32_t progress;               /* PaganiAudioMx* bits */
    volatile int32_t acq_ms;                  /* the ACQ's answer to the empty block (ms), -1 none within 2 s, -2 not asked */
    volatile uint32_t srzl_state;             /* PaganiAudioSrzl* */
    volatile int32_t srzl_status, rwbu_status;/* the repair's two AudioObjectSetPropertyData results */
    volatile uint32_t serial_after;           /* 'S'/'s'/'?': the listener read back after the repair, 0 not done */
    volatile uint32_t filter_before, filter_after;   /* the device filter mode before and after the repair, 0 unread */
    volatile uint32_t attempts;               /* repair attempts (at most one per boot) */
    volatile uint32_t va_settings;            /* PaganiAudioVaSettings* */
} PaganiAudioMx;

/* The haptic actuator (option PaganiAudioOptionActuator, memory type 3, selectors 5/6). The audio server's haptic server and vibe engine
 * play only to the HAL device whose UID is "Actuator" (VirtualAudio's Device_Actuator_Aspen and its port 'pact'): 48 kHz mono Float32,
 * an IO cycle of 256 frames. The plug-in offers that device next to the speaker device and mixes what the audio server writes into the
 * actuator ring (16-bit samples, sample time t at frame t % ring_frames, the main timeline's host clock: one frame lasts
 * ticks_per_frame_q32 / 2^32 ticks from anchor_host); it writes at most a safety offset plus an IO cycle ahead (1.5 cycles), adds each
 * IO context's mix and drops what lies behind taken. The kernel (PaganiAudioHaptics.c) takes each frame shortly before it is due, clears
 * it and feeds the vibrator (PaganiHaptics stream API). Memory type 3: this header page, then the ring. */
#define PAGANI_AUDIO_ACT_MAGIC 0x50474148u        /* 'PGAH' */
#define PAGANI_AUDIO_ACT_VERSION 1u
#define PAGANI_AUDIO_ACT_RATE 48000u
#define PAGANI_AUDIO_ACT_IO_FRAMES 256u           /* the haptic server's IO cycle */
#define PAGANI_AUDIO_ACT_SAFETY_FRAMES 128u       /* the device's safety offset: the plug-in writes about 384 frames (8 ms) ahead */
#define PAGANI_AUDIO_ACT_TAKE_FRAMES 64u          /* the kernel takes a frame this long (1.3 ms) before it is due */
#define PAGANI_AUDIO_ACT_RING_FRAMES 16384u       /* 341 ms */
#define PAGANI_AUDIO_ACT_HEADER_BYTES 4096u
#define PAGANI_AUDIO_ACT_BYTES (PAGANI_AUDIO_ACT_HEADER_BYTES + PAGANI_AUDIO_ACT_RING_FRAMES * 2u)
enum {PaganiAudioMemoryActuator = 3};
enum {PaganiAudioStartActuator = 5, PaganiAudioStopActuator = 6};
/* A client's wish for the actuator (PaganiAudioClientState bits, next to the output and input bits) and the header's running state. */
enum {PaganiAudioActuatorRunning = 1u << 4};
typedef struct {
    uint32_t magic, version, rate, ring_frames, io_frames, safety_frames, take_frames, reserved0;
    uint64_t ticks_per_frame_q32, anchor_host;           /* the main timeline (PaganiAudioControl's) */
    volatile uint32_t state;                             /* the kernel: PaganiAudioActuatorRunning while it takes frames */
    volatile uint32_t reserved1;
    volatile uint64_t written;                           /* the plug-in: the sample time after the last frame it wrote */
    volatile uint64_t written_host;                      /* the plug-in: host time of that write */
    volatile uint64_t taken;                             /* the kernel: the sample time after the last frame it took (cleared) */
    /* The plug-in's counters (the kernel only prints them). */
    volatile uint32_t loads, io_starts, io_stops, writes;
    volatile uint64_t frames, dropped_frames;            /* frames written; frames mixed behind taken (too late), dropped */
    volatile uint32_t dropped_writes, peak;              /* writes that held such frames; the largest sample written, x 32767 */
    volatile int32_t ahead_min, ahead_max;               /* output time minus now (frames), least and most */
    volatile uint32_t unknown_sets, unknown_selector, unknown_where;   /* property writes the plug-in does not know (accepted), the last */
    volatile uint32_t format_sets;                       /* stream format and sample rate writes accepted */
    volatile uint64_t sound_frame, sound_host;           /* the last write that began a sound after silence: its first loud frame */
    /* The plug-in's look at what the audio server made of the actuator (with the route probe): VirtualAudio's virtual device for it
     * (UID VirtualAudioDevice_Actuator, published once its port 'pact' exists; what the vibe engine opens), 0 none; and the hog mode
     * of the actuator device ('oink': 1 the audio server, 0 free, -1 another process, -2 unread). */
    volatile uint32_t va_device;
    volatile int32_t hog;
} PaganiAudioActuatorControl;

/* The layout both sides compile against (the kernel, the plug-in for iOS and the host tests). */
#ifdef __cplusplus
#define PAGANI_AUDIO_ASSERT(c, m) static_assert(c, m)
#else
#define PAGANI_AUDIO_ASSERT(c, m) _Static_assert(c, m)
#endif
PAGANI_AUDIO_ASSERT(sizeof(PaganiAudioControl) == 136, "control block size");
PAGANI_AUDIO_ASSERT(__builtin_offsetof(PaganiAudioControl, state) == 32 && __builtin_offsetof(PaganiAudioControl, backend) == 36,
                    "control state words");
PAGANI_AUDIO_ASSERT(__builtin_offsetof(PaganiAudioControl, ticks_per_frame_q32) == 40 &&
                    __builtin_offsetof(PaganiAudioControl, out_anchor_host) == 48 && __builtin_offsetof(PaganiAudioControl, in_anchor_host) == 56,
                    "control timeline words");
PAGANI_AUDIO_ASSERT(__builtin_offsetof(PaganiAudioControl, out_written) == 96 && __builtin_offsetof(PaganiAudioControl, in_written) == 104 &&
                    __builtin_offsetof(PaganiAudioControl, out_written_host) == 112 &&
                    __builtin_offsetof(PaganiAudioControl, out_holes) == 120 && __builtin_offsetof(PaganiAudioControl, options) == 124 &&
                    __builtin_offsetof(PaganiAudioControl, out_taken) == 128, "control write positions");
PAGANI_AUDIO_ASSERT(sizeof(PaganiAudioDiag) == 424 && __builtin_offsetof(PaganiAudioDiag, last_end) == 352 &&
                    __builtin_offsetof(PaganiAudioDiag, host_clock_result) == 412 && __builtin_offsetof(PaganiAudioDiag, dropped_writes) == 420,
                    "diagnostic block layout");
PAGANI_AUDIO_ASSERT(PAGANI_AUDIO_DIAG_OFFSET >= sizeof(PaganiAudioControl) &&
                    PAGANI_AUDIO_DIAG_OFFSET + sizeof(PaganiAudioDiag) <= PAGANI_AUDIO_TRACE_OFFSET &&
                    PAGANI_AUDIO_TRACE_OFFSET + sizeof(PaganiAudioTrace) <= PAGANI_AUDIO_PROBE_OFFSET &&
                    PAGANI_AUDIO_PROBE_OFFSET + sizeof(PaganiAudioProbe) <= PAGANI_AUDIO_CONTROL_BYTES, "control page sections");
PAGANI_AUDIO_ASSERT(sizeof(PaganiAudioProbe) == 72 && __builtin_offsetof(PaganiAudioProbe, vcsp) == 36 &&
                    __builtin_offsetof(PaganiAudioProbe, port_type) == 56 && __builtin_offsetof(PaganiAudioProbe, vol_queries) == 68 &&
                    PAGANI_AUDIO_PROBE_OFFSET % 8 == 0, "route probe layout");
PAGANI_AUDIO_ASSERT(PAGANI_AUDIO_LEAD_FRAMES < PAGANI_AUDIO_RING_FRAMES / 2 && PAGANI_AUDIO_RING_FRAMES >= 64 * PAGANI_AUDIO_PERIOD_FRAMES,
                    "the lead leaves half the ring; a period may still wrap at the ring's end (65536 is no multiple of 480)");
/* Revision 2: the additions sit in space revision 1 left unused (the engine clears the whole page). */
PAGANI_AUDIO_ASSERT(sizeof(PaganiAudioRevision) == 16 && PAGANI_AUDIO_REVISION_OFFSET >= sizeof(PaganiAudioControl) &&
                    PAGANI_AUDIO_REVISION_OFFSET + sizeof(PaganiAudioRevision) <= PAGANI_AUDIO_DIAG_OFFSET, "revision block");
PAGANI_AUDIO_ASSERT(sizeof(PaganiAudioMx) == 44 && PAGANI_AUDIO_MX_OFFSET >= PAGANI_AUDIO_PROBE_OFFSET + sizeof(PaganiAudioProbe) &&
                    PAGANI_AUDIO_MX_OFFSET + sizeof(PaganiAudioMx) <= PAGANI_AUDIO_CONTROL_BYTES && PAGANI_AUDIO_MX_OFFSET % 8 == 0,
                    "MediaExperience progress block");
PAGANI_AUDIO_ASSERT((PaganiAudioOptionMxProbe | PaganiAudioOptionSrzlRepair | PaganiAudioOptionActuator) == 0x1c &&
                    PaganiAudioMemoryActuator == PaganiAudioMemoryInput + 1 && PaganiAudioStartActuator == PaganiAudioSetHostClock + 1,
                    "revision 2 numbering continues revision 1's");
PAGANI_AUDIO_ASSERT(sizeof(PaganiAudioActuatorControl) == 168 && __builtin_offsetof(PaganiAudioActuatorControl, written) == 56 &&
                    __builtin_offsetof(PaganiAudioActuatorControl, taken) == 72 &&
                    __builtin_offsetof(PaganiAudioActuatorControl, sound_host) == 152 &&
                    sizeof(PaganiAudioActuatorControl) <= PAGANI_AUDIO_ACT_HEADER_BYTES, "actuator header layout");
PAGANI_AUDIO_ASSERT(PAGANI_AUDIO_ACT_SAFETY_FRAMES + PAGANI_AUDIO_ACT_IO_FRAMES <= 2 * PAGANI_AUDIO_ACT_IO_FRAMES &&
                    PAGANI_AUDIO_ACT_TAKE_FRAMES < PAGANI_AUDIO_ACT_SAFETY_FRAMES &&
                    (PAGANI_AUDIO_ACT_RING_FRAMES & (PAGANI_AUDIO_ACT_RING_FRAMES - 1)) == 0 &&
                    PAGANI_AUDIO_ACT_RING_FRAMES >= 32 * PAGANI_AUDIO_ACT_IO_FRAMES && PAGANI_AUDIO_ACT_BYTES % 4096 == 0,
                    "the actuator's lead stays within two IO cycles and the kernel takes inside it; the ring holds many cycles");
#undef PAGANI_AUDIO_ASSERT

#endif
