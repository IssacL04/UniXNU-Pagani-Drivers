/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The haptics module's driver logic (portable C, no C library): identity and ownership checks, the HAP530 initialization, DIRECT_PLAY
 * (constant amplitude for a time), FIFO effects at the LRA period, PCM-like streaming into the FIFO, stop with brake, and the safety
 * rules: a hard and a runtime cap on the drive voltage, a cap on each play's length, an on-time budget, a stop on any bus error or
 * short circuit after which the driver stays off, and bounded waits only.
 *
 * Threading: every pgh_core_* function except pgh_ring_push runs on one thread (the driver's own kernel thread; the glue posts
 * commands to it). pgh_ring_push is the stream producer's side of a single-producer/single-consumer ring and may run on another
 * thread (one producer at a time). */
#ifndef PAGANI_HAPTICS_CORE_H
#define PAGANI_HAPTICS_CORE_H
#include <stddef.h>
#include <stdint.h>
#include "HapticsSpmi.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { PGH_MODE_OFF = 0, PGH_MODE_ON = 1, PGH_MODE_DRY = 2 };
enum { PGH_EFFECTS_MAX = 16 };

typedef struct {
    unsigned mode;                       /* PGH_MODE_ON or PGH_MODE_DRY */
    /* board (PaganiHapticsBoard.h) */
    unsigned ee;
    uint16_t ppid[PGH_PERIPHS];          /* CFG, PTN, HBST, SDAM: SID << 8 | PID */
    uint32_t t_lra_us;                   /* nominal LRA period */
    uint32_t vmax_mv, effect_mv, fifo_mv;/* DIRECT_PLAY, effects, FIFO full scale */
    uint8_t brake[8], brake_len, brake_mode, brake_gain, drv_shape, brake_shape;
    uint8_t drv_duty, autores_cfg;       /* DRV_DUTY_CFG and AUTORES_CFG (auto resonance bit 0) after the init */
    uint32_t f0_min_dhz, f0_max_dhz;     /* accepted measured resonance (0.1 Hz) */
    uint8_t effect_cycles[PGH_EFFECTS_MAX];
    unsigned effects;
    uint32_t vbat_low_mv;
    int vbat_low_soc, vbat_cold_soc, vbat_cold_dc;
    /* identity pins of the PATTERN and HBOOST peripherals (0: not pinned, only logged; the CFG identity is fixed) */
    uint8_t ptn_type, ptn_subtype, hbst_type, hbst_subtype;
    /* policy */
    uint32_t hard_mv;                    /* never exceeded (VMAX register values above it are refused by HapticsSpmi too) */
    uint32_t cap_mv;                     /* runtime cap, <= hard_mv */
    uint32_t max_play_ms;                /* one DIRECT_PLAY or effect */
    uint32_t max_stream_ms;              /* one stream */
    uint32_t budget_ms;                  /* on-time bucket */
    uint32_t refill_permille;            /* bucket refill per elapsed ms (sustained duty) */
    uint8_t autores;                     /* closed loop (auto resonance) in DIRECT_PLAY */
    uint8_t log;                         /* 0 failures, 1 rationed (default), 2 every play */
    uint8_t spmi_retry;                  /* a transaction the arbiter dropped goes again (HapticsSpmi.h); default 0 */
} PghConfig;

typedef struct {
    void *ctx;
    uint64_t (*now_us)(void *ctx);
    void (*wait_us)(void *ctx, unsigned us);                   /* sleeps or spins; waits of the stop and VMAX sequences */
    void (*trace)(void *ctx, const char *line);
    void (*battery)(void *ctx, int *percent, int *deci_c);     /* may be null: percent -1 unknown */
} PghOps;

enum { PGH_CMD_STOP = 1, PGH_CMD_CONSTANT, PGH_CMD_EFFECT, PGH_CMD_STREAM_BEGIN, PGH_CMD_STREAM_END };
typedef struct {
    uint32_t op;
    uint32_t a, b, c;   /* CONSTANT: amplitude permille, ms, mV (0: board); EFFECT: id, strength permille;
                           STREAM_BEGIN: rate Hz, mV (0: the cap), ring index of its first sample; STREAM_END: drain, end index */
} PghCmd;

enum { PGH_RING = 16384u };
typedef struct {
    uint32_t head, tail;                 /* free-running sample counts; head is the producer's, tail the consumer's */
    int8_t data[PGH_RING];
} PghRing;
/* Producer: copies up to n samples, returns how many (0 when full). */
unsigned pgh_ring_push(PghRing *r, const int8_t *samples, unsigned n);
static inline uint32_t pgh_ring_head(const PghRing *r) {return __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);}

enum { PGH_ST_OFF, PGH_ST_IDLE, PGH_ST_DIRECT, PGH_ST_EFFECT, PGH_ST_STREAM_WAIT, PGH_ST_STREAM, PGH_ST_DRY, PGH_ST_FAULT };

typedef struct {
    PghConfig cfg;
    PghOps ops;
    PghSpmi spmi;
    PghRing ring;
    int state, stage, fault_rc;
    uint16_t sel;                        /* MOD_STATUS selection written last (0xffff unknown) */
    uint8_t vmax_code, en_on, autores_ok, clamp_5v;
    uint32_t hw_max_mv, fifo_len;
    /* the play under way */
    uint8_t src, brake_on, rate, ending;
    uint64_t t_start, deadline, t_last, t_empty;
    uint32_t stream_end, stream_rate_hz, stream_mv;
    /* measured LRA period (closed loop); written as the open-loop period after the play that measured it */
    uint32_t cl_t_lra_us, f0_dhz;
    uint8_t tlra_pending;
    /* budget */
    int64_t budget_us;
    uint64_t t_budget;
    /* counters */
    uint32_t plays, streams, stops, clips, refused, underruns, busy, faults_seen, hpwr_timeouts;
    uint64_t samples;
    uint32_t log_lines;
} PghCore;

/* Discovery, identity, the register log; with PGH_MODE_ON the initialization. 0 ready (ON), 1 dry run done (DRY: nothing written),
 * negative: refused or failed (nothing written unless the failure came during the initialization's writes). */
int pgh_core_init(PghCore *c, const PghConfig *cfg, const PghOps *ops, const PghSpmiIo *io);
/* One command (thread). 0 accepted; -16 busy (budget, boost in use), -19 not ready, -22 bad argument, -5/-110 bus error (faulted). */
int pgh_core_command(PghCore *c, const PghCmd *cmd);
/* Periodic work (thread): FIFO refill, end of plays, deadlines, faults. Returns the ms until the next call is needed (0: none until a
 * command). */
uint32_t pgh_core_poll(PghCore *c);
/* [0] stage, [1] state, [2] plays, [3] streams, [4] SPMI writes, [5] reads, [6] refused | failures << 8 | timeouts << 16,
 * [7] f0 (0.1 Hz, 0 unknown), [8] VMAX code, [9] faults seen, [10] underruns, [11] budget ms, [12] closed-loop period us,
 * [13] last error, [14] clips (cap or budget), [15] stream samples (low 32 bits). */
void pgh_core_export(const PghCore *c, int32_t words[16]);
/* The drive voltage a request gets (mV): the caps, the boost clamp and the low-battery rule applied. */
uint32_t pgh_core_vmax(PghCore *c, uint32_t request_mv);

#ifdef __cplusplus
}
#endif
#endif
