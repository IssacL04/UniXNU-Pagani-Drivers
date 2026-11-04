/* SPDX-License-Identifier: MIT */
/* Host model of the secure world behind SMC #0: each request is checked register by register against
 * a script, answered from it, and every register the script does not return is clobbered. The model
 * also checks the lock discipline (yielding SMCs and WAITQ waits under the lock, EBUSY sleeps outside),
 * can deliver the TZ wait-queue interrupt while the caller is parked (or never deliver it), and can
 * refuse to issue an SMC. */
#include "../platform/sm8750/Sm8750Scm.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...)                                                                                       \
    do {                                                                                                       \
        if (!(cond)) {                                                                                         \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                    \
            fprintf(stderr, __VA_ARGS__);                                                                      \
            fputc('\n', stderr);                                                                               \
            abort();                                                                                           \
        }                                                                                                      \
    } while (0)

enum { MaxSteps = 64, AllRegs = 0xff };

typedef struct Model Model;
typedef struct {
    uint64_t in[8];
    uint32_t in_mask;        /* registers that must match in[] */
    uint64_t out[8];
    uint32_t out_mask;       /* registers returned from out[]; the rest are clobbered */
    int repeat;              /* answer every further SMC with this step */
    int fast;                /* must be a fast call made without the caller's lock (interrupt path) */
    void (*during)(Model *); /* runs inside this SMC, after it was matched */
} Step;

struct Model {
    Step steps[MaxSteps];
    unsigned count, next, smcs;
    uint64_t now;
    uint32_t smc_cost_us;
    int held, max_held, locks;
    int lockless;            /* ops without lock/unlock: the caller serialises, nothing to check */
    int refuse_at;           /* the smc op fails (cannot issue) at this SMC (1-based), 0 = never */
    unsigned busy_sleeps, poll_sleeps, waits, wakes_signalled;
    unsigned irq_on_wait;    /* deliver the wait-queue interrupt from this wait() (1-based), 0 = never */
    uint64_t slept_us;
    Sm8750Scm *scm;
};

static int model_smc(void *cookie, uint64_t regs[8])
{
    Model *m = cookie;
    if (m->refuse_at && (int)m->smcs + 1 == m->refuse_at) {
        m->smcs++;
        return -1;                       /* nothing reached the secure world */
    }
    CHECK(m->next < m->count, "unexpected SMC #%u x0=%#llx", m->smcs, (unsigned long long)regs[0]);
    Step *s = &m->steps[m->next];
    if (!s->repeat)
        m->next++;
    m->smcs++;
    for (unsigned i = 0; i < 8; i++)
        CHECK(!(s->in_mask & 1u << i) || regs[i] == s->in[i], "SMC #%u (step %u): x%u = %#llx, expected %#llx", m->smcs,
              (unsigned)(s - m->steps), i, (unsigned long long)regs[i], (unsigned long long)s->in[i]);
    const int fast = (regs[0] & 0x80000000u) != 0;
    CHECK(fast == s->fast, "SMC #%u: call type", m->smcs);
    /* Yielding calls run under the SCM lock; interrupt-path fast calls never take it. */
    CHECK(fast || m->lockless || m->held == 1, "SMC #%u issued without the lock", m->smcs);
    m->now += m->smc_cost_us;
    if (s->during)
        s->during(m);
    for (unsigned i = 0; i < 8; i++)
        regs[i] = (s->out_mask & 1u << i) ? s->out[i] : 0xbad0000000000000ull | (uint64_t)i << 8 | m->smcs;
    return 0;
}

static uint64_t model_now(void *cookie)
{
    return ((Model *)cookie)->now;
}

static void model_sleep(void *cookie, uint32_t us)
{
    Model *m = cookie;
    if (us == Sm8750ScmBusySleepUs) {
        CHECK(m->held == 0, "EBUSY sleep with the lock held");
        m->busy_sleeps++;
    } else {
        CHECK(us <= Sm8750ScmWaitqPollUs && (m->lockless || m->held == 1), "WAITQ poll sleep of %u us, lock %d", us,
              m->held);
        m->poll_sleeps++;
    }
    m->slept_us += us;
    m->now += us;
}

static void model_lock(void *cookie)
{
    Model *m = cookie;
    CHECK(m->held == 0, "recursive SCM lock");
    m->held++;
    m->locks++;
    if (m->held > m->max_held)
        m->max_held = m->held;
}

static void model_unlock(void *cookie)
{
    Model *m = cookie;
    CHECK(m->held == 1, "unlock without lock");
    m->held--;
}

static void model_wait(void *cookie, const void *event, uint32_t timeout_us)
{
    Model *m = cookie;
    CHECK(event == (const void *)m->scm->wakes, "wait event");
    CHECK(m->held == 1, "WAITQ wait without the lock (Linux holds qcom_scm_lock here)");
    CHECK(timeout_us > 0 && timeout_us <= Sm8750ScmWaitqPollUs, "wait slice %u", timeout_us);
    m->waits++;
    if (m->irq_on_wait && m->waits == m->irq_on_wait) {
        Sm8750ScmWaitqIrq(m->scm);       /* the interrupt arrives while we are blocked */
        return;
    }
    m->now += timeout_us;
}

static void model_wake(void *cookie, const void *event)
{
    Model *m = cookie;
    CHECK(event == (const void *)m->scm->wakes, "wake event");
    m->wakes_signalled++;
}

static const Sm8750ScmOps ops_template = {NULL, model_smc, model_now, model_sleep, model_lock, model_unlock, model_wait,
                                          model_wake};

static Sm8750Scm *g_other;

static void irq_other(Model *m)
{
    (void)m;
    Sm8750ScmWaitqIrq(g_other);
}

static Sm8750ScmOps g_ops;
static Sm8750Scm g_scm;

static Model *reset(Model *m)
{
    memset(m, 0, sizeof(*m));
    m->now = 1000000;
    m->smc_cost_us = 5;
    g_ops = ops_template;
    g_ops.cookie = m;
    Sm8750ScmInit(&g_scm, &g_ops);
    m->scm = &g_scm;
    return m;
}

/* A request as the secure world must see it: x0..x5 as given, x6 = x7 = 0. */
static Step *expect(Model *m, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t x4, uint64_t x5)
{
    CHECK(m->count < MaxSteps, "script too long");
    Step *s = &m->steps[m->count++];
    memset(s, 0, sizeof(*s));
    const uint64_t in[8] = {x0, x1, x2, x3, x4, x5, 0, 0};
    memcpy(s->in, in, sizeof(in));
    s->in_mask = AllRegs;
    s->fast = (x0 & 0x80000000u) != 0;
    return s;
}

static Step *answer(Step *s, int64_t x0, uint64_t x1, uint64_t x2, uint64_t x3)
{
    s->out[0] = (uint64_t)x0;
    s->out[1] = x1;
    s->out[2] = x2;
    s->out[3] = x3;
    s->out_mask = 0xf;
    return s;
}

static Step *with_x6(Step *s, uint64_t x6)
{
    s->out[6] = x6;
    s->out_mask |= 1u << 6;
    return s;
}

/* INTERRUPTED continuation of the request in step 'of': x0 = 1, x6 = session, everything else unchanged. */
static Step *resume_of(Model *m, unsigned of, uint64_t session)
{
    Step *s = expect(m, 1, 0, 0, 0, 0, 0);
    memcpy(s->in, m->steps[of].in, sizeof(s->in));
    s->in[0] = 1;
    s->in[6] = session;
    s->fast = m->steps[of].fast;
    return s;
}

static void done(const Model *m)
{
    CHECK(m->next == m->count, "script not consumed: %u of %u steps", m->next, m->count);
    CHECK(m->held == 0 && m->max_held <= 1, "lock state");
}

static void irq_now(Model *m)
{
    Sm8750ScmWaitqIrq(m->scm);
}

int main(void)
{
    Model model, *m;
    Sm8750SmcResult r;
    Sm8750ScmStatus s;

    /* Function ids and argument words as Linux builds them. */
    CHECK(SM8750_SCM_GPU_INIT_REGS == 0x42002801u && SM8750_SCM_IS_CALL_AVAIL == 0x42000601u &&
              SM8750_SCM_CP_SMMU_APERTURE == 0x42000c1bu && SM8750_SCM_PAS_INIT_IMAGE == 0x42000201u &&
              SM8750_SCM_PAS_MEM_SETUP == 0x42000202u && SM8750_SCM_PAS_AUTH_AND_RESET == 0x42000205u &&
              SM8750_SCM_PAS_IS_SUPPORTED == 0x42000207u && SM8750_SCM_WAITQ_RESUME == 0x42002402u &&
              SM8750_SCM_WAITQ_GET_WQ_CTX == 0xc2002403u && SM8750_SCM_WAITQ_GET_INFO == 0xc2002404u,
          "function ids");
    CHECK(SM8750_SCM_ARGS(2, Sm8750ScmVal, Sm8750ScmRw, 0, 0) == 0x82 && SM8750_SCM_ARGS(4, 0, 0, 0, 0) == 4, "arginfo");
    CHECK((Sm8750ScmGpuAlwaysEn | Sm8750ScmGpuTsenseEn) == 0x9, "gpu_req");

    /* 1. GPU_INIT_REGS(ALWAYS_EN | TSENSE_EN): x0 == 0 is success whatever x1 holds. */
    m = reset(&model);
    answer(expect(m, 0x42002801, 1, 9, 0, 0, 0), 0, 0x1234, 0, 0);
    s = Sm8750ScmGpuInitRegs(&g_scm, Sm8750ScmGpuAlwaysEn | Sm8750ScmGpuTsenseEn, &r);
    CHECK(s == Sm8750ScmOk && r.x0 == 0 && r.x1 == 0x1234 && r.calls == 1 && r.interrupted == 0 && r.busy == 0,
          "init regs: status %d x0 %lld calls %u", s, (long long)r.x0, r.calls);
    CHECK(r.fid == 0x42002801u && r.arginfo == 1 && r.args[0] == 9 && r.status == Sm8750ScmOk, "result record");
    CHECK(g_scm.last.fid == 0x42002801u && g_scm.history_count == 1 && g_scm.requests == 1 && g_scm.smcs == 1, "history");
    done(m);

    /* 2. EOPNOTSUPP (-4) from GPU_INIT_REGS is not an error (KGSL); -1 is. */
    m = reset(&model);
    answer(expect(m, 0x42002801, 1, 9, 0, 0, 0), Sm8750ScmX0NotSupported, 0, 0, 0);
    s = Sm8750ScmGpuInitRegs(&g_scm, 9, &r);
    CHECK(s == Sm8750ScmOk && r.x0 == -4 && g_scm.last.status == Sm8750ScmOk, "init regs -4: %d", s);
    answer(expect(m, 0x42002801, 1, 9, 0, 0, 0), Sm8750ScmX0Error, 0, 0, 0);
    s = Sm8750ScmGpuInitRegs(&g_scm, 9, &r);
    CHECK(s == Sm8750ScmError && r.x0 == -1, "init regs -1: %d", s);
    /* A plain call treats -4 as the error it is. */
    answer(expect(m, 0x42002801, 1, 9, 0, 0, 0), Sm8750ScmX0NotSupported, 0, 0, 0);
    const uint64_t nine[4] = {9};
    s = Sm8750ScmCall(&g_scm, SM8750_SCM_GPU_INIT_REGS, 1, nine, &r);
    CHECK(s == Sm8750ScmError && r.x0 == -4, "plain call -4: %d", s);
    done(m);

    /* 3. INTERRUPTED: resume with x0 = 1, the request's x1..x5 and the returned x6, until done. */
    m = reset(&model);
    with_x6(answer(expect(m, 0x42000205, 1, 13, 0, 0, 0), Sm8750ScmX0Interrupted, 0, 0, 0), 0xabc);
    with_x6(answer(resume_of(m, 0, 0xabc), Sm8750ScmX0Interrupted, 0, 0, 0), 0xdef);
    answer(resume_of(m, 0, 0xdef), 0, 0, 0, 0);
    s = Sm8750ScmPasAuthAndReset(&g_scm, Sm8750ScmGpuPasId, &r);
    CHECK(s == Sm8750ScmOk && r.calls == 3 && r.interrupted == 2 && r.x0 == 0 && r.x1 == 0, "interrupted: %d calls %u", s,
          r.calls);
    done(m);

    /* 4. INTERRUPTED forever: bounded by the 1 s budget, not a hang. */
    m = reset(&model);
    m->smc_cost_us = 1000;
    with_x6(answer(expect(m, 0x42000205, 1, 13, 0, 0, 0), Sm8750ScmX0Interrupted, 0, 0, 0), 0x77);
    Step *again = with_x6(answer(resume_of(m, 0, 0x77), Sm8750ScmX0Interrupted, 0, 0, 0), 0x77);
    again->repeat = 1;
    s = Sm8750ScmPasAuthAndReset(&g_scm, 13, &r);
    CHECK(s == Sm8750ScmTimeout && r.calls >= 999 && r.calls <= 1001 && r.elapsed_us >= 1000000 &&
              r.elapsed_us < 1010000 && m->held == 0,
          "interrupted forever: %d calls %u elapsed %u", s, r.calls, r.elapsed_us);

    /* 5. EBUSY: 30 ms sleeps outside the lock, 20 retries, then Busy (21 SMCs in all). */
    m = reset(&model);
    answer(expect(m, 0x42002801, 1, 9, 0, 0, 0), Sm8750ScmX0Busy, 0, 0, 0)->repeat = 1;
    s = Sm8750ScmGpuInitRegs(&g_scm, 9, &r);
    CHECK(s == Sm8750ScmBusy && r.calls == 21 && r.busy == 20 && m->busy_sleeps == 20 && m->smcs == 21 &&
              m->locks == 21 + 1 && r.x0 == -12 && r.elapsed_us >= 600000,
          "ebusy: %d calls %u busy %u sleeps %u locks %d", s, r.calls, r.busy, m->busy_sleeps, m->locks);
    CHECK(g_scm.last.status == Sm8750ScmBusy, "ebusy recorded");

    /* 6. EBUSY twice, then success: the original request is reissued from scratch. */
    m = reset(&model);
    answer(expect(m, 0x42000202, 3, 13, 0x9b09a000, 0x1000, 0), Sm8750ScmX0Busy, 0, 0, 0);
    answer(expect(m, 0x42000202, 3, 13, 0x9b09a000, 0x1000, 0), Sm8750ScmX0Busy, 0, 0, 0);
    answer(expect(m, 0x42000202, 3, 13, 0x9b09a000, 0x1000, 0), 0, 0, 0, 0);
    s = Sm8750ScmPasMemSetup(&g_scm, 13, 0x9b09a000, 0x1000, &r);
    CHECK(s == Sm8750ScmOk && r.calls == 3 && r.busy == 2 && m->busy_sleeps == 2, "ebusy then ok: %d", s);
    done(m);

    /* 7. WAITQ without the interrupt: poll GET_WQ_CTX every millisecond (lock held, as Linux holds it while
     *    parked), ignore bad flags and other contexts, then WAITQ_RESUME(call context). */
    m = reset(&model);
    answer(expect(m, 0x42000201, 0x82, 13, 0x8f000000, 0, 0), Sm8750ScmX0WaitqSleep, 1, 0x55, 0);
    answer(expect(m, 0xc2002403, 0, 0, 0, 0, 0), Sm8750ScmX0Error, 0, 0, 0);        /* nothing pending yet */
    answer(expect(m, 0xc2002403, 0, 0, 0, 0, 0), 0, 1, 0, 0);                       /* flags 0: not a wake */
    answer(expect(m, 0xc2002403, 0, 0, 0, 0, 0), 0, 0, Sm8750ScmWakeOne, 1);        /* someone else's, more */
    answer(expect(m, 0xc2002403, 0, 0, 0, 0, 0), 0, 1, Sm8750ScmWakeOne, 0);        /* ours */
    answer(expect(m, 0x42002402, 1, 0x55, 0, 0, 0), 0, 0, 0, 0);
    s = Sm8750ScmPasInitImage(&g_scm, 13, 0x8f000000, &r);
    CHECK(s == Sm8750ScmOk && r.waitq == 1 && r.wq_ctx == 1 && r.call_ctx == 0x55 && r.calls == 6, "waitq poll: %d calls %u",
          s, r.calls);
    CHECK(g_scm.wakes[1] == 1 && g_scm.wakes[0] == 1 && g_scm.wq_bad_flags == 1 && g_scm.wq_errors == 1 &&
              g_scm.wq_last_x0 == -1 && g_scm.polls == 3 && m->poll_sleeps == 2 && m->waits == 0,
          "waitq poll bookkeeping: polls %u sleeps %u", g_scm.polls, m->poll_sleeps);
    done(m);

    /* 8. WAITQ with the interrupt attached: park in wait(); the interrupt's GET_WQ_CTX wakes us; the
     *    resume itself is INTERRUPTED once. The waiter never polls. */
    m = reset(&model);
    Sm8750ScmSetIrqAttached(&g_scm, 1);
    m->irq_on_wait = 3;
    answer(expect(m, 0x42000205, 1, 13, 0, 0, 0), Sm8750ScmX0WaitqSleep, 0, 7, 0);
    answer(expect(m, 0xc2002403, 0, 0, 0, 0, 0), 0, 0, Sm8750ScmWakeAll, 0);
    with_x6(answer(expect(m, 0x42002402, 1, 7, 0, 0, 0), Sm8750ScmX0Interrupted, 0, 0, 0), 0x99);
    answer(resume_of(m, 2, 0x99), 0, 0, 0, 0);
    s = Sm8750ScmPasAuthAndReset(&g_scm, 13, &r);
    CHECK(s == Sm8750ScmOk && r.waitq == 1 && r.interrupted == 1 && r.calls == 3 && g_scm.irqs == 1 &&
              g_scm.wakes[0] == 1 && g_scm.polls == 0 && m->waits == 3 && m->wakes_signalled == 1,
          "waitq irq: %d calls %u waits %u", s, r.calls, m->waits);
    done(m);

    /* 9. The interrupt fires before the caller parks (inside the SMC that returns WAITQ_SLEEP): the wake
     *    must not be lost, and nobody waits. */
    m = reset(&model);
    Sm8750ScmSetIrqAttached(&g_scm, 1);
    answer(expect(m, 0x42000201, 0x82, 13, 0x8f000000, 0, 0), Sm8750ScmX0WaitqSleep, 1, 0x31, 0)->during = irq_now;
    answer(expect(m, 0xc2002403, 0, 0, 0, 0, 0), 0, 1, Sm8750ScmWakeOne, 0);
    answer(expect(m, 0x42002402, 1, 0x31, 0, 0, 0), 0, 0, 0, 0);
    s = Sm8750ScmPasInitImage(&g_scm, 13, 0x8f000000, &r);
    CHECK(s == Sm8750ScmOk && m->waits == 0 && g_scm.irqs == 1 && r.waitq == 1, "early wake: %d waits %u", s, m->waits);
    done(m);

    /* 10. A stale wake from before the request does not end a new wait early. */
    m = reset(&model);
    Sm8750ScmSetIrqAttached(&g_scm, 1);
    answer(expect(m, 0xc2002403, 0, 0, 0, 0, 0), 0, 0, Sm8750ScmWakeOne, 0);
    irq_now(m);                                                                    /* stale wake of context 0 */
    m->irq_on_wait = 2;
    answer(expect(m, 0x42000205, 1, 13, 0, 0, 0), Sm8750ScmX0WaitqSleep, 0, 0x12, 0);
    answer(expect(m, 0xc2002403, 0, 0, 0, 0, 0), 0, 0, Sm8750ScmWakeOne, 0);
    answer(expect(m, 0x42002402, 1, 0x12, 0, 0, 0), 0, 0, 0, 0);
    s = Sm8750ScmPasAuthAndReset(&g_scm, 13, &r);
    CHECK(s == Sm8750ScmOk && m->waits == 2 && g_scm.wakes[0] == 2, "stale wake: %d waits %u", s, m->waits);
    done(m);

    /* 11. WAITQ that is never woken: the interrupt never comes, GET_WQ_CTX polls (from the 10 ms grace on) find
     *     nothing, the budget ends it and the lock is released. */
    m = reset(&model);
    Sm8750ScmSetIrqAttached(&g_scm, 1);
    answer(expect(m, 0x42000205, 1, 13, 0, 0, 0), Sm8750ScmX0WaitqSleep, 0, 3, 0);
    answer(expect(m, 0xc2002403, 0, 0, 0, 0, 0), Sm8750ScmX0Error, 0, 0, 0)->repeat = 1;
    s = Sm8750ScmPasAuthAndReset(&g_scm, 13, &r);
    CHECK(s == Sm8750ScmTimeout && r.waitq == 1 && r.x0 == Sm8750ScmX0WaitqSleep && m->held == 0 &&
              r.elapsed_us >= 1000000 && r.elapsed_us < 1001000 && m->waits >= 990 && g_scm.irq_fallbacks == 1 &&
              g_scm.polls >= 980 && g_scm.polls <= 991 && m->poll_sleeps == 0,
          "waitq timeout: %d elapsed %u waits %u polls %u", s, r.elapsed_us, m->waits, g_scm.polls);

    /* 11a. An attached interrupt that is never delivered: after the grace period the waiter's own GET_WQ_CTX
     *      finds the wake, and the call completes instead of timing out. */
    m = reset(&model);
    Sm8750ScmSetIrqAttached(&g_scm, 1);
    answer(expect(m, 0x42000201, 0x82, 13, 0x8f000000, 0, 0), Sm8750ScmX0WaitqSleep, 1, 0x44, 0);
    answer(expect(m, 0xc2002403, 0, 0, 0, 0, 0), 0, 1, Sm8750ScmWakeOne, 0);
    answer(expect(m, 0x42002402, 1, 0x44, 0, 0, 0), 0, 0, 0, 0);
    s = Sm8750ScmPasInitImage(&g_scm, 13, 0x8f000000, &r);
    CHECK(s == Sm8750ScmOk && m->waits == Sm8750ScmWaitqIrqGraceUs / Sm8750ScmWaitqPollUs && g_scm.polls == 1 &&
              g_scm.irq_fallbacks == 1 && g_scm.irqs == 0 && g_scm.wakes[1] == 1 && r.calls == 3,
          "silent interrupt: %d waits %u polls %u", s, m->waits, g_scm.polls);
    done(m);

    /* 12. A wait-queue context the driver cannot track is an error, not a wait. */
    m = reset(&model);
    answer(expect(m, 0x42000205, 1, 13, 0, 0, 0), Sm8750ScmX0WaitqSleep, 99, 3, 0);
    s = Sm8750ScmPasAuthAndReset(&g_scm, 13, &r);
    CHECK(s == Sm8750ScmWaitqError && r.wq_ctx == 99, "bad context: %d", s);
    done(m);

    /* 13. PAS results live in x1: x0 == 0 with x1 != 0 is a failure. Arguments as the ZAP loader passes them. */
    m = reset(&model);
    answer(expect(m, 0x42000201, 0x82, 13, 0x8f000000, 0, 0), 0, 0x5, 0, 0);
    s = Sm8750ScmPasInitImage(&g_scm, 13, 0x8f000000, &r);
    CHECK(s == Sm8750ScmResultError && r.x1 == 5, "pas x1: %d", s);
    answer(expect(m, 0x42000201, 0x82, 13, 0x8f000000, 0, 0), 0, 0, 0, 0);
    answer(expect(m, 0x42000202, 3, 13, 0x9b09a000, 0x1000, 0), 0, 0, 0, 0);
    answer(expect(m, 0x42000205, 1, 13, 0, 0, 0), 0, 0, 0, 0);
    CHECK(Sm8750ScmPasInitImage(&g_scm, 13, 0x8f000000, &r) == Sm8750ScmOk, "init image");
    CHECK(Sm8750ScmPasMemSetup(&g_scm, 13, 0x9b09a000, 0x1000, &r) == Sm8750ScmOk, "mem setup");
    CHECK(Sm8750ScmPasAuthAndReset(&g_scm, 13, &r) == Sm8750ScmOk, "auth and reset");
    /* Metadata that TZ could not protect with an XPU is refused before any SMC. */
    CHECK(Sm8750ScmPasInitImage(&g_scm, 13, 0x8f000100, &r) == Sm8750ScmArgument && r.status == Sm8750ScmArgument,
          "unaligned metadata");
    CHECK(Sm8750ScmPasMemSetup(&g_scm, 13, 0x9b09a000, 0, &r) == Sm8750ScmArgument, "empty region");
    CHECK(g_scm.history_count == 4 && g_scm.last.fid == 0x42000205u &&
              g_scm.history[(g_scm.history_count - 1) % Sm8750ScmHistoryDepth].fid == 0x42000205u,
          "history after PAS");
    done(m);

    /* 14. is_available and the GPU probe: x2 = SiP owner | svc << 8 | cmd, available when x1 != 0. */
    m = reset(&model);
    answer(expect(m, 0x42000601, 1, 0x02002801, 0, 0, 0), 0, 1, 0, 0);
    answer(expect(m, 0x42000601, 1, 0x02000c1b, 0, 0, 0), 0, 0, 0, 0);
    answer(expect(m, 0x42000601, 1, 0x02000207, 0, 0, 0), 0, 1, 0, 0);
    uint32_t mask = 0;
    s = Sm8750ScmProbeGpu(&g_scm, &mask);
    CHECK(s == Sm8750ScmOk && mask == (Sm8750ScmHasGpuInitRegs | Sm8750ScmHasPasIsSupported | Sm8750ScmProbed) &&
              g_scm.available == mask,
          "probe: %d mask %#x", s, mask);
    answer(expect(m, 0x42000601, 1, 0x02002801, 0, 0, 0), Sm8750ScmX0Error, 1, 0, 0);
    int available = 1;
    s = Sm8750ScmIsAvailable(&g_scm, Sm8750ScmSvcGpu, Sm8750ScmGpuInitRegsCmd, &available, &r);
    CHECK(s == Sm8750ScmError && available == 0, "is_available error: %d", s);
    answer(expect(m, 0x42000207, 1, 13, 0, 0, 0), 0, 1, 0, 0);
    int supported = 0;
    CHECK(Sm8750ScmPasIsSupported(&g_scm, 13, &supported, &r) == Sm8750ScmOk && supported == 1, "pas supported");
    done(m);

    /* 15. CP_SMMU_APERTURE_ID for context bank 0. */
    m = reset(&model);
    answer(expect(m, 0x42000c1b, 4, 0xffff0000, 0xffffffff, 0xffffffff, 0xffffffff), 0, 0x42, 0, 0);
    CHECK(Sm8750ScmSetGpuSmmuAperture(&g_scm, 0, &r) == Sm8750ScmOk, "aperture");
    done(m);

    /* 16. Fast calls: no lock, no budget; WAITQ_SLEEP is illegal there; the call type must match the entry. */
    m = reset(&model);
    answer(expect(m, 0xc2002404, 0, 0, 0, 0, 0), 0, 0x102, 994, 0);
    uint32_t queues = 0, intid = 0;
    s = Sm8750ScmWaitqInfo(&g_scm, &queues, &intid, &r);
    CHECK(s == Sm8750ScmOk && queues == 2 && intid == 994 && m->locks == 0, "waitq info: %d q %u irq %u", s, queues, intid);
    answer(expect(m, 0xc2002404, 0, 0, 0, 0, 0), Sm8750ScmX0WaitqSleep, 0, 0, 0);
    CHECK(Sm8750ScmFastCall(&g_scm, SM8750_SCM_WAITQ_GET_INFO, 0, NULL, &r) == Sm8750ScmWaitqError, "fast waitq");
    CHECK(Sm8750ScmFastCall(&g_scm, SM8750_SCM_GPU_INIT_REGS, 1, nine, &r) == Sm8750ScmArgument, "yielding id as fast");
    CHECK(Sm8750ScmCall(&g_scm, SM8750_SCM_WAITQ_GET_INFO, 0, NULL, &r) == Sm8750ScmArgument, "fast id as yielding");
    CHECK(Sm8750ScmCall(&g_scm, 0x02002801, 1, nine, &r) == Sm8750ScmArgument, "SMC32 id");
    CHECK(Sm8750ScmCall(&g_scm, SM8750_SCM_GPU_INIT_REGS, 5, nine, &r) == Sm8750ScmArgument, "five arguments");
    CHECK(Sm8750ScmCall(&g_scm, SM8750_SCM_GPU_INIT_REGS, 1, NULL, &r) == Sm8750ScmArgument, "missing arguments");
    CHECK(g_scm.history_count == 0, "fast calls and refused requests are not recorded");
    done(m);

    /* 17. Missing ops are refused, not dereferenced; a NULL result pointer is fine. */
    m = reset(&model);
    Sm8750ScmOps broken = g_ops;
    broken.now_us = NULL;
    Sm8750Scm scm2;
    Sm8750ScmInit(&scm2, &broken);
    CHECK(Sm8750ScmGpuInitRegs(&scm2, 9, &r) == Sm8750ScmArgument && m->smcs == 0, "missing now_us");
    broken = g_ops;
    broken.unlock = NULL;
    Sm8750ScmInit(&scm2, &broken);
    CHECK(Sm8750ScmGpuInitRegs(&scm2, 9, NULL) == Sm8750ScmArgument, "lock without unlock");
    answer(expect(m, 0x42002801, 1, 9, 0, 0, 0), 0, 0, 0, 0);
    CHECK(Sm8750ScmGpuInitRegs(&g_scm, 9, NULL) == Sm8750ScmOk && g_scm.last.calls == 1, "NULL result");
    Sm8750ScmWaitqIrq(NULL);
    done(m);

    /* 18. The history keeps the last eight requests. */
    m = reset(&model);
    for (unsigned i = 0; i < 11; i++) {
        answer(expect(m, 0x42002801, 1, i, 0, 0, 0), 0, 0, 0, 0);
        CHECK(Sm8750ScmGpuInitRegs(&g_scm, i, NULL) == Sm8750ScmOk, "loop %u", i);
    }
    CHECK(g_scm.history_count == 11 && g_scm.history[10 % Sm8750ScmHistoryDepth].args[0] == 10 &&
              g_scm.history[0].args[0] == Sm8750ScmHistoryDepth && g_scm.history[3].args[0] == 3 &&
              g_scm.last.args[0] == 10,
          "history ring");
    done(m);

    /* 19. Final status as Linux derives it: qcom_scm_remap_error((int)x0), PAS "ret ? : (int)res.result[0]".
     *     A -4 written as a 32-bit register is still EOPNOTSUPP for GPU_INIT_REGS; a zero-extended -12 is not
     *     retried (Linux compares EBUSY as 64 bits) but is reported Busy; a nonzero x0 with zero low bits is an
     *     error; a PAS result with only upper bits set is a success. */
    m = reset(&model);
    answer(expect(m, 0x42002801, 1, 9, 0, 0, 0), 0x00000000fffffffcll, 0, 0, 0);
    CHECK(Sm8750ScmGpuInitRegs(&g_scm, 9, &r) == Sm8750ScmOk && r.x0 == 0xfffffffcll, "32-bit -4");
    answer(expect(m, 0x42002801, 1, 9, 0, 0, 0), 0x00000000fffffffcll, 0, 0, 0);
    CHECK(Sm8750ScmCall(&g_scm, SM8750_SCM_GPU_INIT_REGS, 1, nine, &r) == Sm8750ScmError, "32-bit -4, plain call");
    answer(expect(m, 0x42002801, 1, 9, 0, 0, 0), 0x00000000fffffff4ll, 0, 0, 0);
    CHECK(Sm8750ScmGpuInitRegs(&g_scm, 9, &r) == Sm8750ScmBusy && r.calls == 1 && r.busy == 0, "32-bit -12");
    answer(expect(m, 0x42002801, 1, 9, 0, 0, 0), (int64_t)0xffffffff00000000ull, 0, 0, 0);
    CHECK(Sm8750ScmGpuInitRegs(&g_scm, 9, &r) == Sm8750ScmError, "upper bits only");
    answer(expect(m, 0x42000205, 1, 13, 0, 0, 0), 0, 0x100000000ull, 0, 0);
    CHECK(Sm8750ScmPasAuthAndReset(&g_scm, 13, &r) == Sm8750ScmOk && r.x1 == 0x100000000ull, "PAS result upper bits");
    done(m);

    /* 20. A probe that is refused leaves the "probed" bit clear and reports the first failure. */
    m = reset(&model);
    answer(expect(m, 0x42000601, 1, 0x02002801, 0, 0, 0), 0, 1, 0, 0);
    answer(expect(m, 0x42000601, 1, 0x02000c1b, 0, 0, 0), Sm8750ScmX0InvalidArg, 0, 0, 0);
    answer(expect(m, 0x42000601, 1, 0x02000207, 0, 0, 0), 0, 1, 0, 0);
    mask = 0;
    s = Sm8750ScmProbeGpu(&g_scm, &mask);
    CHECK(s == Sm8750ScmError && mask == (Sm8750ScmHasGpuInitRegs | Sm8750ScmHasPasIsSupported) &&
              g_scm.available == mask,
          "partial probe: %d mask %#x", s, mask);
    done(m);

    /* 21. GET_WQ_CTX naming a context outside the table is counted, not indexed; the right wake still ends the
     *     wait (polling: no interrupt attached). */
    m = reset(&model);
    answer(expect(m, 0x42000205, 1, 13, 0, 0, 0), Sm8750ScmX0WaitqSleep, 1, 0x66, 0);
    answer(expect(m, 0xc2002403, 0, 0, 0, 0, 0), 0, 40, Sm8750ScmWakeOne, 1);
    answer(expect(m, 0xc2002403, 0, 0, 0, 0, 0), 0, 1, Sm8750ScmWakeAll, 0);
    answer(expect(m, 0x42002402, 1, 0x66, 0, 0, 0), 0, 0, 0, 0);
    CHECK(Sm8750ScmPasAuthAndReset(&g_scm, 13, &r) == Sm8750ScmOk && g_scm.wq_bad_ctx == 1 && g_scm.wakes[1] == 1 &&
              g_scm.polls == 1,
          "bad context from GET_WQ_CTX");
    done(m);

    /* 22. An SMC that cannot be issued fails the request at once, without retries, and is recorded; the same in
     *     a WAITQ poll. */
    m = reset(&model);
    m->refuse_at = 1;
    CHECK(Sm8750ScmGpuInitRegs(&g_scm, 9, &r) == Sm8750ScmIoError && r.calls == 1 && g_scm.last.status == Sm8750ScmIoError &&
              g_scm.history_count == 1,
          "refused SMC");
    done(m);
    m = reset(&model);
    m->refuse_at = 2;
    answer(expect(m, 0x42000205, 1, 13, 0, 0, 0), Sm8750ScmX0WaitqSleep, 0, 0x12, 0);
    CHECK(Sm8750ScmPasAuthAndReset(&g_scm, 13, &r) == Sm8750ScmIoError && r.waitq == 1 && m->held == 0,
          "refused GET_WQ_CTX poll");
    done(m);

    /* 23. Without lock ops (the caller serialises) and without wait/wake (attached interrupt or not, the waiter
     *     polls from the start). */
    m = reset(&model);
    m->lockless = 1;
    g_ops.lock = NULL;
    g_ops.unlock = NULL;
    g_ops.wait = NULL;
    g_ops.wake = NULL;
    Sm8750ScmSetIrqAttached(&g_scm, 1);
    answer(expect(m, 0x42000201, 0x82, 13, 0x8f000000, 0, 0), Sm8750ScmX0WaitqSleep, 0, 0x21, 0);
    answer(expect(m, 0xc2002403, 0, 0, 0, 0, 0), 0, 0, Sm8750ScmWakeOne, 0);
    answer(expect(m, 0x42002402, 1, 0x21, 0, 0, 0), 0, 0, 0, 0);
    CHECK(Sm8750ScmPasInitImage(&g_scm, 13, 0x8f000000, &r) == Sm8750ScmOk && m->locks == 0 && g_scm.polls == 1 &&
              g_scm.irq_fallbacks == 0 && m->waits == 0,
          "lock-less, no wait op");
    answer(expect(m, 0xc2002403, 0, 0, 0, 0, 0), 0, 1, Sm8750ScmWakeOne, 0);
    Sm8750ScmWaitqIrq(&g_scm);                                  /* no wake op: counts only */
    CHECK(g_scm.irqs == 1 && g_scm.wakes[1] == 1 && m->wakes_signalled == 0, "interrupt without wake op");
    done(m);

    /* 24. A budget too short for 20 EBUSY sleeps ends with Busy before the next sleep would cross it. */
    m = reset(&model);
    g_scm.budget_us = 100000;
    answer(expect(m, 0x42002801, 1, 9, 0, 0, 0), Sm8750ScmX0Busy, 0, 0, 0)->repeat = 1;
    s = Sm8750ScmGpuInitRegs(&g_scm, 9, &r);
    CHECK(s == Sm8750ScmBusy && r.calls == 4 && r.busy == 3 && r.elapsed_us < 100000 && m->held == 0,
          "short budget: %d calls %u busy %u", s, r.calls, r.busy);

    /* 25. In interrupt context the handler is bounded: at most 16 GET_WQ_CTX per interrupt however long TZ keeps
     *     reporting more pending, then one wake. A fast call passes its arguments in x2.. like a yielding one. */
    m = reset(&model);
    answer(expect(m, 0xc2002403, 0, 0, 0, 0, 0), 0, 0, Sm8750ScmWakeOne, 1)->repeat = 1;
    Sm8750ScmWaitqIrq(&g_scm);
    CHECK(m->smcs == Sm8750ScmIrqMaxContexts && g_scm.wakes[0] == Sm8750ScmIrqMaxContexts && m->wakes_signalled == 1 &&
              m->locks == 0,
          "bounded interrupt: %u SMCs", m->smcs);
    m = reset(&model);
    const uint64_t fast_args[4] = {0x11, 0x22};
    answer(expect(m, 0xc2002404, 2, 0x11, 0x22, 0, 0), 0, 5, 0, 0);
    CHECK(Sm8750ScmFastCall(&g_scm, SM8750_SCM_WAITQ_GET_INFO, 2, fast_args, &r) == Sm8750ScmOk && r.x1 == 5 &&
              r.args[1] == 0x22 && m->locks == 0,
          "fast call arguments");
    done(m);

    /* 26. WAITQ values are 32-bit (Linux keeps them in u32): upper bits in WAITQ_SLEEP's x1/x2 and in
     *     GET_WQ_CTX's x1..x3 are ignored, so neither the context nor more_pending is misread. */
    m = reset(&model);
    answer(expect(m, 0x42000205, 1, 13, 0, 0, 0), Sm8750ScmX0WaitqSleep, 0xabcd00000001ull, 0x9900000033ull, 0);
    answer(expect(m, 0xc2002403, 0, 0, 0, 0, 0), 0, 0x7700000001ull, 0x5500000001ull, 0x3300000000ull);
    answer(expect(m, 0x42002402, 1, 0x33, 0, 0, 0), 0, 0, 0, 0);
    CHECK(Sm8750ScmPasAuthAndReset(&g_scm, 13, &r) == Sm8750ScmOk && r.wq_ctx == 1 && r.call_ctx == 0x33 &&
              g_scm.wakes[1] == 1 && g_scm.wq_bad_flags == 0 && m->smcs == 3,
          "32-bit WAITQ values");
    done(m);

    /* 27. SET_REMOTE_STATE(ZAP resume, 13) as qcom_scm_pas_set_remote_state issues it: BOOT svc, ARGS(2) = {state, id};
     *     like the PAS calls it fails on x1[31:0] != 0, and it takes the WAITQ path like any yielding call (polling,
     *     then the interrupt path). */
    CHECK(SM8750_SCM_SET_REMOTE_STATE == 0x4200010au && Sm8750ScmZapShaderResume == 0, "set_remote_state id");
    m = reset(&model);
    answer(expect(m, 0x4200010a, 2, 0, 13, 0, 0), 0, 0, 0, 0);
    CHECK(Sm8750ScmPasSetRemoteState(&g_scm, Sm8750ScmZapShaderResume, Sm8750ScmGpuPasId, &r) == Sm8750ScmOk &&
              r.fid == 0x4200010au && r.arginfo == 2 && r.args[0] == 0 && r.args[1] == 13 && r.calls == 1,
          "set_remote_state");
    answer(expect(m, 0x4200010a, 2, 0, 13, 0, 0), 0, 0xffffffea, 0, 0);
    CHECK(Sm8750ScmPasSetRemoteState(&g_scm, 0, 13, &r) == Sm8750ScmResultError && r.x1 == 0xffffffea,
          "set_remote_state result in x1");
    answer(expect(m, 0x4200010a, 2, 0, 13, 0, 0), Sm8750ScmX0NotSupported, 0, 0, 0);
    CHECK(Sm8750ScmPasSetRemoteState(&g_scm, 0, 13, &r) == Sm8750ScmError && r.x0 == -4,
          "set_remote_state not supported is an error");
    answer(expect(m, 0x4200010a, 2, 0, 13, 0, 0), Sm8750ScmX0WaitqSleep, 1, 0x5a, 0);
    answer(expect(m, 0xc2002403, 0, 0, 0, 0, 0), 0, 1, Sm8750ScmWakeOne, 0);
    answer(expect(m, 0x42002402, 1, 0x5a, 0, 0, 0), 0, 0, 0, 0);
    CHECK(Sm8750ScmPasSetRemoteState(&g_scm, 0, 13, &r) == Sm8750ScmOk && r.waitq == 1 && r.calls == 3 &&
              g_scm.wakes[1] == 1 && g_scm.polls == 1,
          "set_remote_state through WAITQ");
    CHECK(g_scm.last.fid == 0x4200010au && g_scm.history_count == 4, "set_remote_state recorded");
    done(m);

    /* The kernel has two Sm8750Scm: the interrupt handler of the A830 driver's fetches a wake the other one (PaganiAdsp's, polling) waits
     * for. GET_WQ_CTX gives a wake to one caller only: the poller must still see it, without a poll that would find nothing. */
    {
        static Sm8750Scm other;
        static Sm8750ScmOps other_ops;
        m = reset(&model);
        other_ops = (Sm8750ScmOps){m, model_smc, model_now, model_sleep, NULL, NULL, NULL, NULL};
        Sm8750ScmInit(&other, &other_ops);
        g_other = &other;
        answer(expect(m, 0x42000205, 1, 13, 0, 0, 0), Sm8750ScmX0WaitqSleep, 1, 0x77, 0)->during = irq_other;
        answer(expect(m, 0xc2002403, 0, 0, 0, 0, 0), 0, 1, Sm8750ScmWakeOne, 0);
        answer(expect(m, 0x42002402, 1, 0x77, 0, 0, 0), 0, 0, 0, 0);
        s = Sm8750ScmPasAuthAndReset(&g_scm, 13, &r);
        CHECK(s == Sm8750ScmOk && r.waitq == 1 && g_scm.polls == 0 && g_scm.wakes[1] == 0 && other.wakes[1] == 1 && other.irqs == 1,
              "a wake the other Sm8750Scm fetched: %d polls %u", s, g_scm.polls);
        done(m);
    }

#if defined(__aarch64__)
    /* The real instruction is linked (never executed on the host: SMC traps at EL0). */
    int (*volatile instruction)(void *, uint64_t[8]) = Sm8750ScmSmcInstruction;
    CHECK(instruction != NULL, "Sm8750ScmSmcInstruction");
#endif

    puts("PASS: SM8750 SCM follows qcom_scm: INTERRUPTED resumes with x6, WAITQ sleep/wake (interrupt, poll, poll after "
         "a silent interrupt), EBUSY 20x30 ms, GPU_INIT_REGS accepts -4, PAS and SET_REMOTE_STATE check x1[31:0]");
    return 0;
}
