/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The portable torch core (no kernel or C library calls; built for XNU and for the host tests): discovery, identity and a register
 * dump of the PM8550 flash LED module, then torch mode on the board's channels: on at a level (permille of the runtime cap), off,
 * the auto-off deadline, fault polling, all through the gate of TorchSpmi.c. One thread calls it (the kernel glue's), so it has no
 * lock. Every step it takes is a "[torch]" line through PgtOps.trace, rationed (TorchCore.c). */
#ifndef PAGANI_TORCH_CORE_H
#define PAGANI_TORCH_CORE_H
#include <stdint.h>
#include "TorchSpmi.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { PGT_MODE_ON = 1, PGT_MODE_DRY = 2 };
enum { PGT_ST_NONE = 0, PGT_ST_OFF = 1, PGT_ST_ON = 2, PGT_ST_FAULT = 3, PGT_ST_REFUSED = 4, PGT_ST_DRY = 5 };
enum { PGT_LEVEL_MAX = 1000, PGT_DEFAULT_MA = 50, PGT_AUTO_OFF_MS = 300000, PGT_TEST_MAX_MS = 1000 };

typedef struct {
    unsigned mode;                  /* PGT_MODE_ON or PGT_MODE_DRY */
    unsigned ee;
    uint16_t ppid;                  /* SID << 8 | PID of the flash module */
    uint8_t chan_mask;              /* the channels of the board's LED, all driven at the same current */
    uint16_t hard_ma;               /* per channel: the gate's limit, fixed by the board (at most PGT_TORCH_POLICY_MA, 200) */
    uint16_t max_ma;                /* per channel at level 1000: the runtime cap (at most hard_ma) */
    uint32_t auto_off_ms;           /* an "on" ends after this long (at most 5 min: the core caps it) */
    uint32_t poll_ms;               /* fault polling interval while on */
    uint8_t log;                    /* 0 failures only, 1 rationed (default), 2 every change */
} PgtConfig;

typedef struct {
    void *ctx;
    uint64_t (*now_us)(void *ctx);
    void (*trace)(void *ctx, const char *line);
} PgtOps;

typedef struct {
    PgtConfig cfg;
    const PgtOps *ops;
    PgtSpmi spmi;
    int state;                      /* PGT_ST_* */
    int stage;                      /* 1 started, 2 discovered, 3 identified, 4 ready, 5 dry run done; negative: where it stopped */
    unsigned level;                 /* 0..1000 now */
    unsigned ma;                    /* per channel now (what the code drives) */
    uint8_t code;                   /* the ITARGET code now */
    uint8_t code_max;               /* the runtime cap's code */
    uint8_t hard_code;              /* the hard cap's code (the gate's limit) */
    uint64_t on_us, deadline_us;    /* when it went on; when it goes off by itself */
    uint32_t ons, offs, auto_offs, faults;
    uint8_t sts[3];                 /* STATUS1, STATUS2, STATUS3 at the last check */
    uint8_t boot_sts[3];            /* the same at discovery (before anything was written) */
    uint8_t chan_en, en_ctl, ires;  /* at discovery */
    int last_rc;
    const char *why;                /* the refusal or fault */
    /* the log ration (TorchCore.c) */
    uint32_t event_lines, dropped, fault_lines;
    uint64_t event_line_us;
} PgtCore;

/* The ITARGET code of a per-channel torch current (the formula of [L]): (mA x 1000 + 1) / 5000 - 1, 0 below one step. */
unsigned pgt_ma_code(unsigned ma);
/* The current a code drives: (code + 1) x 5 mA. */
unsigned pgt_code_ma(unsigned code);
/* The per-channel current of a level (permille of max_ma, rounded; at least one step when level > 0). */
unsigned pgt_level_ma(unsigned level, unsigned max_ma);

/* The configuration of the boot arguments (the glue's only logic, here so the host tests run it): mode 1 or 2, else 0 is returned and
 * nothing runs; max_ma_arg 0 (absent): the default 50 mA; anything else capped to [5, hard_ma]; hard_ma capped to 200; log above 2 is 2. */
int pgt_config_make(PgtConfig *c, unsigned mode, unsigned ee, uint16_t ppid, uint8_t chan_mask, unsigned hard_ma, unsigned max_ma_arg,
                    unsigned log);
/* pagani-torch-test: at most 1000 ms; 0 no test. */
uint32_t pgt_test_ms(unsigned arg);

/* Discovery, identity, the dump; mode 1: open the channel, every channel off. 0 ready; 1 the dry run is done (nothing written);
 * negative: refused or failed (logged; nothing more may be written). */
int pgt_core_init(PgtCore *t, const PgtConfig *c, const PgtOps *ops, const PgtSpmiIo *io);
/* Level 0: off; 1..1000: on at that permille of max_ma, for at most auto_off_ms (or hold_ms when nonzero and shorter: the bring-up
 * test). `why` names the caller in the log. 0, -19 not ready, -22 bad level, -5 the driver stopped after a fault. */
int pgt_core_set(PgtCore *t, unsigned level, uint32_t hold_ms, const char *why);
/* While on: the status registers (any fault: emergency stop, off until reboot) and the deadline. Returns the ms until the next call
 * is needed (0: nothing to poll). */
uint32_t pgt_core_poll(PgtCore *t);
/* Status words (pagani_torch_status): stage, state, level, mA, code, ons, offs, faults, status bits (s1 << 16 | s2 << 8 | s3), writes,
 * refused, APID, owner, type << 8 | subtype, auto-offs, last rc. */
void pgt_core_export(const PgtCore *t, int32_t words[16]);

#ifdef __cplusplus
}
#endif
#endif
