/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The secure world, the AOSS mailbox and RPMh for the ADSP bring-up, through the SM8750 platform layer of the Adreno830 driver
 * (platform/sm8750 Sm8750Scm, Sm8750AossQmp, Sm8750Rpmh; used as they are, linked from libAdreno830.a). Portable C: the kernel gives
 * mapped windows and the SMC instruction, the host tests models of each block.
 *
 * Sharing with the GPU driver:
 *  - RPMh DRV-2: Sm8750RpmhWriteActive takes the existing lock word sm8750_rpmh_drv2_owner itself.
 *  - SCM: this driver's own Sm8750Scm (a 20 s budget: authenticating the 38 MiB adsp image takes longer than the GPU's 1 s). Its
 *    lock is the weak word sm8750_scm_owner ('ADSP' while held). Linux serializes all yielding SCM calls under one mutex; the A830
 *    driver does not take this word: all its yielding calls are in its synchronous bring-up, before this driver runs (README, "Sharing").
 *    Our wait-queue wakes are polled (GET_WQ_CTX); the TZ wait-queue interrupt 994 stays the GPU driver's, and a wake its handler fetches
 *    reaches this driver through the wakes the two Sm8750Scm share (integrate/Sm8750Scm-shared-wakes.diff).
 *  - AOSS QMP: one message at a time under the weak word sm8750_aoss_qmp_owner; the channel the GPU driver opened is reused as it is.
 * A lock that stays taken past its deadline makes the call fail without touching the hardware. */
#ifndef PAGANI_ADSP_PLATFORM_H
#define PAGANI_ADSP_PLATFORM_H
#include <stdint.h>
#include "Sm8750AossQmp.h"
#include "Sm8750Rpmh.h"
#include "Sm8750Scm.h"
#ifdef __cplusplus
extern "C" {
#endif

extern volatile uint32_t sm8750_scm_owner;
extern volatile uint32_t sm8750_aoss_qmp_owner;

enum {
    PaganiAdspLockOwner = 0x41445350,           /* 'ADSP' */
    PaganiAdspScmBudgetUs = 20000000,
    PaganiAdspScmLockUs = 10000000,             /* wait for the SCM word at most this long (inside the 20 s budget) */
    PaganiAdspQmpLockUs = 3000000,
    PaganiAdspLockStepUs = 1000,                /* a sleeping wait in the kernel (IOSleep 1 ms), never a spin */
    PaganiAdspRailLevel = 384,                  /* vlvl of the 13T devicetree's cx-uV-uA/mx-uV-uA (RPMH_REGULATOR_LEVEL_TURBO) */
    PaganiAdspPasShutdownCmd = 0x06,
    /* Error codes (the platform layer's own pass through). */
    PaganiAdspPlatLock = -16,                   /* a shared lock word stayed taken */
    PaganiAdspPlatNoMemory = -12,
    PaganiAdspPlatArgument = -22,
    PaganiAdspPlatScm = -1000,                  /* -1000 - Sm8750ScmStatus */
    PaganiAdspPlatRails = -2000,                /* -2000 + the Sm8750Rpmh code of the cmd-db lookup */
};

typedef struct {
    void *context;
    uint64_t (*now_us)(void *context);
    void (*sleep_us)(void *context, uint32_t microseconds);
    /* SMC #0 with x0..x7 in and out (Sm8750ScmSmcInstruction in the kernel). */
    int (*smc)(void *context, uint64_t regs[8]);
    /* Contiguous memory the secure world can take: copies `bytes` of data, cleans it to memory and leaves no cacheable alias that
     * could write back over it; *phys 4 KiB aligned. 0 or an error. free returns it (only after a successful authentication). */
    int (*meta_alloc)(void *context, const uint8_t *data, uint64_t bytes, uint64_t *phys, void **handle);
    void (*meta_free)(void *context, void *handle);
    const Sm8750AossQmpIo *qmp_ram, *ipcc;     /* AOSS message RAM (0x400) and the IPCC window (SEND_ID +0x0c) */
    const Sm8750RpmhIo *drv2, *cmd_db;         /* apps RSC DRV-2 and the command database window */
    Sm8750CmdDb *db;                           /* 128 KiB for the snapshot (allocated by the caller) */
    uint32_t rail_level;                       /* vlvl; 0: the top level */
} PaganiAdspPlatformConfig;

typedef struct {
    PaganiAdspPlatformConfig config;
    Sm8750ScmOps scm_ops;
    Sm8750Scm scm;
    Sm8750SmcResult last;
    uint32_t scm_held, scm_waits, scm_lock_failures;
    Sm8750AossQmp qmp;
    int qmp_last;
    uint32_t qmp_waits;
    uint8_t rails_known;
    Sm8750RpmhArc lcx, lmx;
    uint32_t lcx_hlvl, lmx_hlvl;
    int rails_lookup;                           /* cmd-db result of the lookup */
    Sm8750RpmhResult rpmh;
    struct {void *handle; uint64_t phys; uint32_t pas;} meta[2];
} PaganiAdspPlatform;

void pagani_adsp_platform_init(PaganiAdspPlatform *p, const PaganiAdspPlatformConfig *config);
/* One AOSS QMP message under the QMP lock word. */
int pagani_adsp_platform_qmp(PaganiAdspPlatform *p, const char *message);
/* The lcx/lmx proxy votes: on raises lmx then lcx to the rail level, off drops lcx then lmx to level 0. The command database is read
 * on the first call. */
int pagani_adsp_platform_rails(PaganiAdspPlatform *p, int on);
/* PAS through Sm8750Scm; 0 or PaganiAdspPlatScm - status (x0/x1 in p->last). */
int pagani_adsp_platform_pas_init(PaganiAdspPlatform *p, uint32_t pas, const uint8_t *metadata, uint64_t bytes);
int pagani_adsp_platform_pas_mem_setup(PaganiAdspPlatform *p, uint32_t pas, uint64_t base, uint64_t bytes);
int pagani_adsp_platform_pas_auth(PaganiAdspPlatform *p, uint32_t pas);
int pagani_adsp_platform_pas_shutdown(PaganiAdspPlatform *p, uint32_t pas);
/* Returns the metadata memory of `pas` when the image authenticated; otherwise the memory stays allocated (the secure world may still
 * hold it). */
void pagani_adsp_platform_pas_release(PaganiAdspPlatform *p, uint32_t pas, int authenticated);

#ifdef __cplusplus
}
#endif
#endif
