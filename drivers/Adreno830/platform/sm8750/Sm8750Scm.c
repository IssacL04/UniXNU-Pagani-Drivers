/* SPDX-License-Identifier: MIT */
#include "Sm8750Scm.h"
#include <stddef.h>

/* Request policy: when is x0/x1 a success. */
typedef enum {
    NeedX0,                 /* qcom_scm_call(..., NULL) */
    NeedX0OrUnsupported,    /* KGSL: -EOPNOTSUPP from GPU_INIT_REGS is not an error */
    NeedX0AndX1,            /* "ret ? : res.result[0]" of the PAS calls */
} Policy;

#define CALL_FAST 0x80000000u     /* ARM_SMCCC_FAST_CALL */
#define CALL_SMC64 0x40000000u    /* ARM_SMCCC_SMC_64 */

static uint32_t load32(const uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void bump(uint32_t *p)
{
    __atomic_fetch_add(p, 1, __ATOMIC_RELEASE);
}

/* Every wake of the boot, whoever fetched it. GET_WQ_CTX hands a wake to one caller only, and the kernel has two Sm8750Scm: the A830
 * driver's, whose handler of INTID 994 fetches at once, and PaganiAdsp's, which polls (the line has one handler). A waiter looks here,
 * so a wake the other one fetched still ends its wait. */
static uint32_t all_wakes[Sm8750ScmWaitqContexts];

static int ops_valid(const Sm8750ScmOps *ops)
{
    return ops && ops->smc && ops->now_us && ops->sleep_us && !ops->lock == !ops->unlock && !ops->wait == !ops->wake;
}

static void lock(const Sm8750Scm *scm)
{
    if (scm->ops->lock)
        scm->ops->lock(scm->ops->cookie);
}

static void unlock(const Sm8750Scm *scm)
{
    if (scm->ops->unlock)
        scm->ops->unlock(scm->ops->cookie);
}

/* One request plus its INTERRUPTED continuations (Linux __scm_smc_do_quirk): x0 = 1 and the returned x6,
 * every other register as in the request. regs holds the first non-INTERRUPTED return. */
static Sm8750ScmStatus smc_quirk(const Sm8750Scm *scm, const uint64_t request[8], uint64_t regs[8],
                                 Sm8750SmcResult *r, uint64_t deadline, uint32_t max_calls)
{
    const Sm8750ScmOps *ops = scm->ops;
    uint64_t x0 = request[0], x6 = request[6];
    for (;;) {
        if (r->calls >= max_calls || (deadline && ops->now_us(ops->cookie) >= deadline))
            return Sm8750ScmTimeout;
        for (unsigned i = 0; i < 8; i++)
            regs[i] = request[i];
        regs[0] = x0;
        regs[6] = x6;
        r->calls++;
        if (ops->smc(ops->cookie, regs))
            return Sm8750ScmIoError;
        if ((int64_t)regs[0] != Sm8750ScmX0Interrupted)
            return Sm8750ScmOk;
        r->interrupted++;
        x0 = Sm8750ScmX0Interrupted;
        x6 = regs[6];
    }
}

/* A wake reported by GET_WQ_CTX (QCOM_SMC_WAITQ_FLAG_WAKE_ONE/_ALL for one context). Like every WAITQ value
 * (Linux scm_get_wq_ctx and __scm_smc_do_quirk_handle_waitq store them in u32), only the low 32 bits count. */
static void note_wake(Sm8750Scm *scm, uint32_t context, uint32_t flags)
{
    if (flags != Sm8750ScmWakeOne && flags != Sm8750ScmWakeAll)
        bump(&scm->wq_bad_flags);
    else if (context >= Sm8750ScmWaitqContexts)
        bump(&scm->wq_bad_ctx);
    else {
        bump(&all_wakes[context]);
        bump(&scm->wakes[context]);
    }
}

/* GET_WQ_CTX (fast) until nothing more is pending, bumping the woken contexts. A failing or empty
 * GET_WQ_CTX only counts as an error. Returns nonzero if an SMC could not be issued. */
static int drain_wq(Sm8750Scm *scm, Sm8750SmcResult *r)
{
    static const uint64_t get_wq_ctx[8] = {SM8750_SCM_WAITQ_GET_WQ_CTX};
    for (unsigned round = 0; round < Sm8750ScmIrqMaxContexts; round++) {
        uint64_t regs[8];
        const Sm8750ScmStatus s = smc_quirk(scm, get_wq_ctx, regs, r, 0, r->calls + Sm8750ScmFastMaxCalls);
        if (s || regs[0]) {
            bump(&scm->wq_errors);
            if (!s)
                __atomic_store_n(&scm->wq_last_x0, (int64_t)regs[0], __ATOMIC_RELAXED);
            return s == Sm8750ScmIoError;
        }
        note_wake(scm, (uint32_t)regs[1], (uint32_t)regs[2]);
        if (!(uint32_t)regs[3])             /* more_pending */
            return 0;
    }
    return 0;
}

/* Sleep until the wait-queue context we were parked on has been woken (Linux
 * qcom_scm_wait_for_wq_completion), by the interrupt handler or by polling GET_WQ_CTX ourselves. With the
 * interrupt attached we poll only once the wait is Sm8750ScmWaitqIrqGraceUs old: an attach that succeeded
 * does not prove that INTID 994 is delivered, and a lost edge is never repeated. GET_WQ_CTX from both sides
 * is safe: every wake it reports is counted once, by whoever fetched it, also in another Sm8750Scm (all_wakes). */
static Sm8750ScmStatus wait_wq(Sm8750Scm *scm, uint32_t context, const uint32_t seen[Sm8750ScmWaitqContexts],
                               Sm8750SmcResult *r, uint64_t deadline)
{
    const Sm8750ScmOps *ops = scm->ops;
    const int interrupt = load32(&scm->irq_attached) && ops->wait;
    const uint64_t start = ops->now_us(ops->cookie);
    int poll = !interrupt;
    for (;;) {
        if (load32(&all_wakes[context]) != seen[context])
            return Sm8750ScmOk;
        if (!poll && ops->now_us(ops->cookie) - start >= Sm8750ScmWaitqIrqGraceUs) {
            poll = 1;
            scm->irq_fallbacks++;
        }
        if (poll) {
            scm->polls++;
            if (r->calls >= Sm8750ScmMaxCalls)
                return Sm8750ScmTimeout;
            if (drain_wq(scm, r))
                return Sm8750ScmIoError;
            if (load32(&all_wakes[context]) != seen[context])
                return Sm8750ScmOk;
        }
        const uint64_t now = ops->now_us(ops->cookie);
        if (now >= deadline)
            return Sm8750ScmTimeout;
        const uint32_t slice = deadline - now < Sm8750ScmWaitqPollUs ? (uint32_t)(deadline - now) : Sm8750ScmWaitqPollUs;
        if (interrupt)
            ops->wait(ops->cookie, scm->wakes, slice);
        else
            ops->sleep_us(ops->cookie, slice);
    }
}

/* One locked attempt (Linux __scm_smc_do_quirk_handle_waitq): the request, its INTERRUPTED
 * continuations, and WAITQ_SLEEP -> wait -> WAITQ_RESUME(call context) until a final status. */
static Sm8750ScmStatus attempt(Sm8750Scm *scm, const uint64_t request[8], Sm8750SmcResult *r, uint64_t deadline)
{
    uint64_t current[8], regs[8];
    uint32_t seen[Sm8750ScmWaitqContexts];
    for (unsigned i = 0; i < 8; i++)
        current[i] = request[i];
    for (;;) {
        for (unsigned i = 0; i < Sm8750ScmWaitqContexts; i++)
            seen[i] = load32(&all_wakes[i]);
        Sm8750ScmStatus s = smc_quirk(scm, current, regs, r, deadline, Sm8750ScmMaxCalls);
        if (s)
            return s;
        r->x0 = (int64_t)regs[0];
        r->x1 = regs[1];
        r->x2 = regs[2];
        r->x3 = regs[3];
        if (r->x0 != Sm8750ScmX0WaitqSleep)
            return Sm8750ScmOk;
        r->waitq++;
        r->wq_ctx = (uint32_t)regs[1];
        r->call_ctx = (uint32_t)regs[2];
        if (r->wq_ctx >= Sm8750ScmWaitqContexts)
            return Sm8750ScmWaitqError;
        if ((s = wait_wq(scm, r->wq_ctx, seen, r, deadline)))
            return s;
        const uint64_t resume[8] = {SM8750_SCM_WAITQ_RESUME, SM8750_SCM_ARGS(1, 0, 0, 0, 0), r->call_ctx};
        for (unsigned i = 0; i < 8; i++)
            current[i] = resume[i];
    }
}

static void record(Sm8750Scm *scm, const Sm8750SmcResult *r)
{
    lock(scm);
    scm->requests++;
    scm->smcs += r->calls;
    scm->last = *r;
    scm->history[scm->history_count % Sm8750ScmHistoryDepth] = *r;
    scm->history_count++;
    unlock(scm);
}

/* Linux __scm_smc_call: "(long)a0 ? qcom_scm_remap_error(a0) : 0" with the remap taking an int, and the PAS
 * helpers' "ret ? : res.result[0]" returned as an int. So a nonzero x0 is an error whose code is x0[31:0]
 * (a status TZ wrote as a 32-bit register still maps to -EOPNOTSUPP), and a PAS result is x1[31:0]. */
static Sm8750ScmStatus judge(const Sm8750SmcResult *r, Policy policy)
{
    if (r->x0 == Sm8750ScmX0Ok)
        return policy == NeedX0AndX1 && (uint32_t)r->x1 ? Sm8750ScmResultError : Sm8750ScmOk;
    const int32_t code = (int32_t)(uint32_t)r->x0;
    if (policy == NeedX0OrUnsupported && code == Sm8750ScmX0NotSupported)
        return Sm8750ScmOk;
    return code == Sm8750ScmX0Busy ? Sm8750ScmBusy : Sm8750ScmError;
}

/* Linux __scm_smc_do: locked attempts; EBUSY sleeps 30 ms outside the lock and retries, 20 times. */
static Sm8750ScmStatus request(Sm8750Scm *scm, uint32_t fid, uint32_t arginfo, const uint64_t args[4],
                               Sm8750SmcResult *out, Policy policy)
{
    Sm8750SmcResult local, *r = out ? out : &local;
    const unsigned count = arginfo & 0xf;
    *r = (Sm8750SmcResult){.fid = fid, .arginfo = arginfo};
    if (!scm || !ops_valid(scm->ops) || (fid & CALL_FAST) || !(fid & CALL_SMC64) || count > 4 || (count && !args)) {
        r->status = Sm8750ScmArgument;
        return Sm8750ScmArgument;
    }
    for (unsigned i = 0; i < count; i++)
        r->args[i] = args[i];
    const Sm8750ScmOps *ops = scm->ops;
    const uint64_t start = ops->now_us(ops->cookie);
    const uint64_t deadline = start + (scm->budget_us ? scm->budget_us : (uint32_t)Sm8750ScmDefaultBudgetUs);
    const uint64_t req[8] = {fid, arginfo, r->args[0], r->args[1], r->args[2], r->args[3], 0, 0};
    Sm8750ScmStatus s;
    for (;;) {
        lock(scm);
        s = attempt(scm, req, r, deadline);
        unlock(scm);
        if (s || r->x0 != Sm8750ScmX0Busy)
            break;
        if (r->busy >= Sm8750ScmBusyRetries || ops->now_us(ops->cookie) + Sm8750ScmBusySleepUs > deadline)
            break;
        r->busy++;
        ops->sleep_us(ops->cookie, Sm8750ScmBusySleepUs);
    }
    if (!s)
        s = judge(r, policy);
    const uint64_t elapsed = ops->now_us(ops->cookie) - start;
    r->elapsed_us = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
    r->status = (int32_t)s;
    record(scm, r);
    return s;
}

void Sm8750ScmInit(Sm8750Scm *scm, const Sm8750ScmOps *ops)
{
    if (!scm)
        return;
    *scm = (Sm8750Scm){0};
    scm->ops = ops;
    scm->budget_us = Sm8750ScmDefaultBudgetUs;
}

void Sm8750ScmSetIrqAttached(Sm8750Scm *scm, int attached)
{
    if (scm)
        __atomic_store_n(&scm->irq_attached, attached ? 1u : 0u, __ATOMIC_RELEASE);
}

Sm8750ScmStatus Sm8750ScmCall(Sm8750Scm *scm, uint32_t fid, uint32_t arginfo, const uint64_t args[4],
                              Sm8750SmcResult *out)
{
    return request(scm, fid, arginfo, args, out, NeedX0);
}

Sm8750ScmStatus Sm8750ScmFastCall(Sm8750Scm *scm, uint32_t fid, uint32_t arginfo, const uint64_t args[4],
                                  Sm8750SmcResult *out)
{
    Sm8750SmcResult local, *r = out ? out : &local;
    const unsigned count = arginfo & 0xf;
    uint64_t regs[8];
    *r = (Sm8750SmcResult){.fid = fid, .arginfo = arginfo};
    if (!scm || !scm->ops || !scm->ops->smc || !(fid & CALL_FAST) || !(fid & CALL_SMC64) || count > 4 || (count && !args)) {
        r->status = Sm8750ScmArgument;
        return Sm8750ScmArgument;
    }
    for (unsigned i = 0; i < count; i++)
        r->args[i] = args[i];
    const uint64_t req[8] = {fid, arginfo, r->args[0], r->args[1], r->args[2], r->args[3], 0, 0};
    Sm8750ScmStatus s = smc_quirk(scm, req, regs, r, 0, Sm8750ScmFastMaxCalls);
    if (!s) {
        r->x0 = (int64_t)regs[0];
        r->x1 = regs[1];
        r->x2 = regs[2];
        r->x3 = regs[3];
        s = r->x0 == Sm8750ScmX0WaitqSleep ? Sm8750ScmWaitqError : judge(r, NeedX0);
    }
    r->status = (int32_t)s;
    return s;
}

Sm8750ScmStatus Sm8750ScmIsAvailable(Sm8750Scm *scm, uint32_t svc, uint32_t cmd, int *available, Sm8750SmcResult *out)
{
    Sm8750SmcResult local, *r = out ? out : &local;
    /* SCM_SMC_FNID(svc, cmd) | ARM_SMCCC_OWNER_SIP << ARM_SMCCC_OWNER_SHIFT */
    const uint64_t args[4] = {0x02000000u | (svc & 0xffu) << 8 | (cmd & 0xffu)};
    if (available)
        *available = 0;
    const Sm8750ScmStatus s = request(scm, SM8750_SCM_IS_CALL_AVAIL, SM8750_SCM_ARGS(1, 0, 0, 0, 0), args, r, NeedX0);
    if (!s && available)
        *available = r->x1 != 0;
    return s;
}

Sm8750ScmStatus Sm8750ScmProbeGpu(Sm8750Scm *scm, uint32_t *mask)
{
    static const struct {
        uint8_t svc, cmd;
        uint32_t bit;
    } probes[] = {
        {Sm8750ScmSvcGpu, Sm8750ScmGpuInitRegsCmd, Sm8750ScmHasGpuInitRegs},
        {Sm8750ScmSvcMp, Sm8750ScmCpSmmuApertureCmd, Sm8750ScmHasCpAperture},
        {Sm8750ScmSvcPil, Sm8750ScmPasIsSupportedCmd, Sm8750ScmHasPasIsSupported},
    };
    Sm8750ScmStatus first = Sm8750ScmOk;
    uint32_t found = 0;
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        int available = 0;
        const Sm8750ScmStatus s = Sm8750ScmIsAvailable(scm, probes[i].svc, probes[i].cmd, &available, NULL);
        if (s && !first)
            first = s;
        if (available)
            found |= probes[i].bit;
    }
    if (!first)
        found |= Sm8750ScmProbed;           /* every probe got an answer */
    if (scm)
        scm->available = found;
    if (mask)
        *mask = found;
    return first;
}

Sm8750ScmStatus Sm8750ScmWaitqInfo(Sm8750Scm *scm, uint32_t *queues, uint32_t *intid, Sm8750SmcResult *out)
{
    Sm8750SmcResult local, *r = out ? out : &local;
    const Sm8750ScmStatus s = Sm8750ScmFastCall(scm, SM8750_SCM_WAITQ_GET_INFO, 0, NULL, r);
    if (!s) {
        if (queues)
            *queues = (uint32_t)(r->x1 & 0xff);
        if (intid)
            *intid = (uint32_t)(r->x2 & 0xffff);
    }
    return s;
}

Sm8750ScmStatus Sm8750ScmGpuInitRegs(Sm8750Scm *scm, uint32_t gpu_req, Sm8750SmcResult *out)
{
    const uint64_t args[4] = {gpu_req};
    return request(scm, SM8750_SCM_GPU_INIT_REGS, SM8750_SCM_ARGS(1, 0, 0, 0, 0), args, out, NeedX0OrUnsupported);
}

Sm8750ScmStatus Sm8750ScmSetGpuSmmuAperture(Sm8750Scm *scm, uint32_t context_bank, Sm8750SmcResult *out)
{
    const uint64_t args[4] = {0xffff0000u | (context_bank & 0xffu), 0xffffffffu, 0xffffffffu, 0xffffffffu};
    return request(scm, SM8750_SCM_CP_SMMU_APERTURE, SM8750_SCM_ARGS(4, 0, 0, 0, 0), args, out, NeedX0);
}

Sm8750ScmStatus Sm8750ScmPasIsSupported(Sm8750Scm *scm, uint32_t pas_id, int *supported, Sm8750SmcResult *out)
{
    Sm8750SmcResult local, *r = out ? out : &local;
    const uint64_t args[4] = {pas_id};
    if (supported)
        *supported = 0;
    const Sm8750ScmStatus s = request(scm, SM8750_SCM_PAS_IS_SUPPORTED, SM8750_SCM_ARGS(1, 0, 0, 0, 0), args, r, NeedX0);
    if (!s && supported)
        *supported = r->x1 != 0;
    return s;
}

Sm8750ScmStatus Sm8750ScmPasInitImage(Sm8750Scm *scm, uint32_t pas_id, uint64_t metadata_phys, Sm8750SmcResult *out)
{
    /* The metadata must be physically contiguous and 4 KiB aligned (TZ puts an XPU over it). */
    if (!metadata_phys || (metadata_phys & 0xfff)) {
        if (out)
            *out = (Sm8750SmcResult){.fid = SM8750_SCM_PAS_INIT_IMAGE, .status = Sm8750ScmArgument};
        return Sm8750ScmArgument;
    }
    const uint64_t args[4] = {pas_id, metadata_phys};
    return request(scm, SM8750_SCM_PAS_INIT_IMAGE, SM8750_SCM_ARGS(2, Sm8750ScmVal, Sm8750ScmRw, 0, 0), args, out,
                   NeedX0AndX1);
}

Sm8750ScmStatus Sm8750ScmPasMemSetup(Sm8750Scm *scm, uint32_t pas_id, uint64_t phys, uint64_t bytes,
                                     Sm8750SmcResult *out)
{
    if (!phys || !bytes || (phys & 0xfff)) {
        if (out)
            *out = (Sm8750SmcResult){.fid = SM8750_SCM_PAS_MEM_SETUP, .status = Sm8750ScmArgument};
        return Sm8750ScmArgument;
    }
    const uint64_t args[4] = {pas_id, phys, bytes};
    return request(scm, SM8750_SCM_PAS_MEM_SETUP, SM8750_SCM_ARGS(3, 0, 0, 0, 0), args, out, NeedX0AndX1);
}

Sm8750ScmStatus Sm8750ScmPasAuthAndReset(Sm8750Scm *scm, uint32_t pas_id, Sm8750SmcResult *out)
{
    const uint64_t args[4] = {pas_id};
    return request(scm, SM8750_SCM_PAS_AUTH_AND_RESET, SM8750_SCM_ARGS(1, 0, 0, 0, 0), args, out, NeedX0AndX1);
}

Sm8750ScmStatus Sm8750ScmPasSetRemoteState(Sm8750Scm *scm, uint32_t state, uint32_t pas_id, Sm8750SmcResult *out)
{
    /* qcom_scm_pas_set_remote_state: QCOM_SCM_ARGS(2), args {state, id}, "ret ? : res.result[0]". */
    const uint64_t args[4] = {state, pas_id};
    return request(scm, SM8750_SCM_SET_REMOTE_STATE, SM8750_SCM_ARGS(2, 0, 0, 0, 0), args, out, NeedX0AndX1);
}

void Sm8750ScmWaitqIrq(void *context)
{
    Sm8750Scm *scm = (Sm8750Scm *)context;
    Sm8750SmcResult scratch = {0};
    if (!scm || !scm->ops || !scm->ops->smc)
        return;
    bump(&scm->irqs);
    (void)drain_wq(scm, &scratch);
    if (scm->ops->wake)
        scm->ops->wake(scm->ops->cookie, scm->wakes);
}

#if defined(__aarch64__)
int Sm8750ScmSmcInstruction(void *cookie, uint64_t regs[8])
{
    register uint64_t x0 __asm__("x0") = regs[0];
    register uint64_t x1 __asm__("x1") = regs[1];
    register uint64_t x2 __asm__("x2") = regs[2];
    register uint64_t x3 __asm__("x3") = regs[3];
    register uint64_t x4 __asm__("x4") = regs[4];
    register uint64_t x5 __asm__("x5") = regs[5];
    register uint64_t x6 __asm__("x6") = regs[6];
    register uint64_t x7 __asm__("x7") = regs[7];
    (void)cookie;
    __asm__ volatile("smc #0"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
                     :
                     : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    regs[0] = x0;
    regs[1] = x1;
    regs[2] = x2;
    regs[3] = x3;
    regs[4] = x4;
    regs[5] = x5;
    regs[6] = x6;
    regs[7] = x7;
    return 0;
}
#endif
