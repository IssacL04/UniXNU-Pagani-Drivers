/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
#include "AdspPlatform.h"
#include "AdspUtil.h"

/* The shared lock words (AdspPlatform.h): weak, so the static kernel link keeps one copy whichever drivers define them. */
__attribute__((weak)) volatile uint32_t sm8750_scm_owner;
__attribute__((weak)) volatile uint32_t sm8750_aoss_qmp_owner;

static int take(PaganiAdspPlatform *p, volatile uint32_t *word, uint32_t timeout_us, uint32_t *waits)
{
    for (uint32_t waited = 0;; waited += PaganiAdspLockStepUs) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(word, &expected, (uint32_t)PaganiAdspLockOwner, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
        if (waited >= timeout_us) return PaganiAdspPlatLock;
        if (waits) (*waits)++;
        p->config.sleep_us(p->config.context, PaganiAdspLockStepUs);
    }
}
static void give(volatile uint32_t *word)
{
    uint32_t expected = PaganiAdspLockOwner;
    __atomic_compare_exchange_n(word, &expected, 0u, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

/* ---- Sm8750ScmOps: every SMC of ours runs with the SCM word held ---- */
static void scm_lock(void *cookie)
{
    PaganiAdspPlatform *p = (PaganiAdspPlatform *)cookie;
    p->scm_held = take(p, &sm8750_scm_owner, PaganiAdspScmLockUs, &p->scm_waits) == 0;
    if (!p->scm_held) p->scm_lock_failures++;
}
static void scm_unlock(void *cookie)
{
    PaganiAdspPlatform *p = (PaganiAdspPlatform *)cookie;
    if (p->scm_held) give(&sm8750_scm_owner);
    p->scm_held = 0;
}
static int scm_smc(void *cookie, uint64_t regs[8])
{
    PaganiAdspPlatform *p = (PaganiAdspPlatform *)cookie;
    if (!p->scm_held) return 1;   /* the lock was not had: refused (Sm8750ScmIoError), nothing reaches the secure world */
    return p->config.smc(p->config.context, regs);
}
static uint64_t scm_now(void *cookie)
{
    PaganiAdspPlatform *p = (PaganiAdspPlatform *)cookie;
    return p->config.now_us(p->config.context);
}
static void scm_sleep(void *cookie, uint32_t microseconds)
{
    PaganiAdspPlatform *p = (PaganiAdspPlatform *)cookie;
    p->config.sleep_us(p->config.context, microseconds);
}

void pagani_adsp_platform_init(PaganiAdspPlatform *p, const PaganiAdspPlatformConfig *config)
{
    pagani_zero(p, sizeof *p);
    p->config = *config;
    p->scm_ops.cookie = p;
    p->scm_ops.smc = scm_smc;
    p->scm_ops.now_us = scm_now;
    p->scm_ops.sleep_us = scm_sleep;
    p->scm_ops.lock = scm_lock;
    p->scm_ops.unlock = scm_unlock;
    Sm8750ScmInit(&p->scm, &p->scm_ops);
    p->scm.budget_us = PaganiAdspScmBudgetUs;
    Sm8750AossQmpInit(&p->qmp, config->qmp_ram, Sm8750AossQmpMsgramBytes, config->ipcc);
}

static int scm_result(Sm8750ScmStatus status)
{
    return status == Sm8750ScmOk ? 0 : PaganiAdspPlatScm - (int)status;
}

int pagani_adsp_platform_pas_init(PaganiAdspPlatform *p, uint32_t pas, const uint8_t *metadata, uint64_t bytes)
{
    unsigned slot = 0;
    while (slot < 2 && p->meta[slot].handle) ++slot;
    if (slot == 2 || !metadata || !bytes || !p->config.meta_alloc) return PaganiAdspPlatArgument;
    uint64_t phys = 0;
    void *handle = 0;
    if (p->config.meta_alloc(p->config.context, metadata, bytes, &phys, &handle) || !handle) return PaganiAdspPlatNoMemory;
    p->meta[slot].handle = handle;
    p->meta[slot].phys = phys;
    p->meta[slot].pas = pas;
    return scm_result(Sm8750ScmPasInitImage(&p->scm, pas, phys, &p->last));
}

int pagani_adsp_platform_pas_mem_setup(PaganiAdspPlatform *p, uint32_t pas, uint64_t base, uint64_t bytes)
{
    return scm_result(Sm8750ScmPasMemSetup(&p->scm, pas, base, bytes, &p->last));
}

int pagani_adsp_platform_pas_auth(PaganiAdspPlatform *p, uint32_t pas)
{
    return scm_result(Sm8750ScmPasAuthAndReset(&p->scm, pas, &p->last));
}

int pagani_adsp_platform_pas_shutdown(PaganiAdspPlatform *p, uint32_t pas)
{
    /* qcom_scm_pas_shutdown: PIL command 6, one value argument, "ret ? : res.result[0]". */
    const uint64_t args[4] = {pas};
    Sm8750ScmStatus status = Sm8750ScmCall(&p->scm, SM8750_SCM_FID(0, Sm8750ScmSvcPil, PaganiAdspPasShutdownCmd),
                                           SM8750_SCM_ARGS(1, Sm8750ScmVal, 0, 0, 0), args, &p->last);
    if (status == Sm8750ScmOk && (uint32_t)p->last.x1) status = Sm8750ScmResultError;
    return scm_result(status);
}

void pagani_adsp_platform_pas_release(PaganiAdspPlatform *p, uint32_t pas, int authenticated)
{
    for (unsigned slot = 0; slot < 2; ++slot) {
        if (!p->meta[slot].handle || p->meta[slot].pas != pas) continue;
        if (!authenticated) return;     /* kept for good: the secure world may still hold it */
        if (p->config.meta_free) p->config.meta_free(p->config.context, p->meta[slot].handle);
        p->meta[slot].handle = 0;
        p->meta[slot].phys = 0;
        return;
    }
}

int pagani_adsp_platform_qmp(PaganiAdspPlatform *p, const char *message)
{
    if (take(p, &sm8750_aoss_qmp_owner, PaganiAdspQmpLockUs, &p->qmp_waits)) return p->qmp_last = PaganiAdspPlatLock;
    const int rc = Sm8750AossQmpSend(&p->qmp, message);
    give(&sm8750_aoss_qmp_owner);
    return p->qmp_last = rc;
}

/* ---- rails ---- */
static int arc(PaganiAdspPlatform *p, const char *name, Sm8750RpmhArc *out)
{
    unsigned count = 0;
    pagani_zero(out, sizeof *out);
    const int rc = Sm8750CmdDbArc(p->config.db, name, &out->addr, out->vlvl, &count);
    out->count = (uint8_t)count;
    out->present = rc == Sm8750RpmhOk;
    return rc;
}
static int level(const PaganiAdspPlatform *p, const Sm8750RpmhArc *rail, uint32_t *hlvl)
{
    if (p->config.rail_level) {
        const int rc = Sm8750RpmhArcLevel(rail, p->config.rail_level, hlvl);
        if (rc != Sm8750RpmhRange) return rc;
    }
    return Sm8750RpmhArcTop(rail, hlvl);
}
static int lookup(PaganiAdspPlatform *p)
{
    if (p->rails_known) return p->rails_lookup;
    p->rails_known = 1;
    int rc = p->config.db && p->config.cmd_db ? Sm8750CmdDbCopy(p->config.cmd_db, p->config.db) : Sm8750RpmhArgument;
    if (!rc) rc = arc(p, "lcx.lvl", &p->lcx);
    if (!rc) rc = arc(p, "lmx.lvl", &p->lmx);
    if (!rc) rc = level(p, &p->lcx, &p->lcx_hlvl);
    if (!rc) rc = level(p, &p->lmx, &p->lmx_hlvl);
    return p->rails_lookup = rc;
}

int pagani_adsp_platform_rails(PaganiAdspPlatform *p, int on)
{
    const int rc = lookup(p);
    if (rc) return PaganiAdspPlatRails + rc;
    Sm8750RpmhCmd cmds[2];
    if (on) {   /* memory rail first when raising, last when lowering (as the GPU driver orders mx/cx) */
        cmds[0] = (Sm8750RpmhCmd){p->lmx.addr, p->lmx_hlvl};
        cmds[1] = (Sm8750RpmhCmd){p->lcx.addr, p->lcx_hlvl};
    } else {
        cmds[0] = (Sm8750RpmhCmd){p->lcx.addr, 0};
        cmds[1] = (Sm8750RpmhCmd){p->lmx.addr, 0};
    }
    return Sm8750RpmhWriteActive(p->config.drv2, cmds, 2, 0x3, &p->rpmh);
}
