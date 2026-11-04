/* SPDX-License-Identifier: MIT */
#ifndef A830_FENCE_H
#define A830_FENCE_H
#include "A830Sched.h"
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>

/* Ring fences, waits and hang detection (ARCHITECTURE 4.13, PLAN M6/M10/M13).
 *
 * The CP writes the completed seqno to memptrs+4 (CP_EVENT_WRITE7 0x88000031: CACHE_CLEAN | WRITE | IRQ, so the GPU's writes
 * are visible before the seqno, and RBBM_INT_0 bit20 fires). The memptrs page is Normal-NC on both sides: a load-acquire is
 * all a reader needs. Seqnos of the one ring complete in order; a seqno has passed when (int32_t)(fence - seq) >= 0.
 *
 * wait() is the algorithm of 4.13.3: no lock, no MMIO; assert_wait on the fence event with a 16 ms safety slice (against a
 * lost interrupt, never relied on for progress), re-check, thread_block. A thread interrupted before the deadline returns
 * kIOReturnTimeout ("not yet": Turnip retries a timeout and turns every other error into a lost device), kIOReturnNotResponding
 * means the device is lost, kIOReturnAborted only that the caller's connection is closing.
 *
 * The software watchdog (4.13.4): while work is in flight a thread call looks every 500 ms at the fence, the RPTR shadow, and
 * the BR CP_RB_RPTR / CP_IB1_BASE / CP_IB1_REM_SIZE (A830Irq::probe); 2 s without any change is a hang, escalated through
 * A830Irq::escalate (A830IrqFaultWatchdog, reason 18) to the recovery entry.
 *
 * The queue meter (4.13.5): how long each submission waits in the one ring (no priorities: the display's scanout blit queues
 * behind every job submitted before it). The ring stamps every kernel and framed submission right before the barrier and the
 * WPTR write that hand it to the CP (NativeAdrenoRingWaiter::stamp = ringStamp): the time, whose work it is (the kernel's, the
 * render server's or an external process's: NativeAdrenoRingSource*), the ring depth seqno - completed and the seqno go into
 * stamps[seqno % A830FenceQueueStamps]; the depth is also kept as the period's maximum, and a submission that enters a new
 * context (a client switch) is counted. Whoever first sees the completed fence move accounts every seqno between the last
 * accounted one and the new fence, in order (latency = now - stamp): the fence interrupt's top half (signal()), a waiter that
 * finds its fence passed, and, only while no fence interrupt can arrive (polling()), the DCVS thread's takeQueue. One of them
 * accounts at a time (q_busy_); a caller that finds another at work leaves it its call and returns at once, and the accountant
 * passes again until no call came during its pass, so each completion is accounted exactly once and nobody waits: no lock, no
 * MMIO, no allocation, at most A830FenceQueueStamps stamps read per pass.
 * Job attribution (preemption phase 0, ARCHITECTURE 4.17): a job's service time is its completion minus the later of its stamp
 * and the previous job's completion. Several completions seen in one pass give the first one the whole time and the rest none:
 * the render-server service and the latencies are then upper bounds, but fw, n4 and the external busy time can come out low as
 * well as high (an external job behind the first counts none, and so does the fw of a render-server job behind that one). With
 * the fence interrupt a pass spans at most the interrupt's latency; while polling() the passes span up to a poll period and those
 * figures say little (the phase 0 gate reads only periods without polling, 4.17). For each render-server job the external
 * service time ahead of it is the service of the external jobs still in the ring when it was stamped, without the part the
 * oldest of them had already run by then (fw); unknown when a job in between lost its stamp or more than 63 were ahead (the
 * stamp's depth saturates at 127). takeQueue hands the period
 * (submitted, completed, missed, average and longest latency, completions above 16 ms, deepest ring; render-server service and
 * fw average and maximum, fw above 4 ms, external jobs and their service time, context entries) to the status rows once a second
 * and starts the next one. A recovery's forced fence (forceComplete, the orchestrator's CP restart) skips its seqnos (queueSkip)
 * instead of measuring the jobs it failed. */

class A830Irq;
struct Adreno830StatusQueue;
enum : uint32_t {A830FenceQueueStamps=64,A830FenceQueueSlowUs=16000,A830FenceQueueFwUs=4000};

struct A830FenceConfig {
    volatile uint32_t *memptrs;   /* CPU view of the memptrs page: +0 BR RPTR shadow, +4 BR fence, +8 BV RPTR, +12 BV fence */
    A830Irq *irq;                 /* watchdog probe and escalation; retire kick while 332 is off; may be null (no watchdog MMIO) */
    uint32_t watchdog_period_ms;  /* 0: 500 */
    uint32_t hang_ms;             /* 0: 2000 */
    uint32_t slice_ms;            /* 0: 16 (A830_FENCE_WAIT_SLICE_US) */
};
/* How one wait went (optional output). */
struct A830WaitStats {
    uint32_t blocks;      /* thread_block calls */
    uint32_t woken;       /* blocks ended by a wakeup (the fence interrupt or a wakeAll) */
    uint32_t slices;      /* blocks ended by the safety slice */
    bool interrupted;     /* the thread was interrupted (signal, thread_abort_safely) */
    bool late;            /* completed only after a safety slice: the interrupt did not wake the waiter */
    uint64_t returned;    /* a830Now() at return */
};
/* Watchdog record (host tools, status page). */
struct A830WatchdogStats {
    uint64_t ticks,armed,hangs;
    uint32_t stalled_ms;   /* current stall */
    uint32_t last_fence,last_rptr,last_ib1_rem;
    uint64_t last_ib1_base;
};

class A830Fence final {
    A830FenceConfig config_{};
    A830Call *watchdog_call_=nullptr;
    char event_=0;               /* the wait channel: thread_wakeup(&event_) */
    uint32_t irq_seq_=0;         /* fence interrupts seen (the top half's fence_irq_seq) */
    uint32_t submitted_=0;       /* newest seqno handed to the CP */
    uint32_t retired_=0;         /* newest seqno the retire hook was given */
    bool started_=false,lost_=false,watchdog_armed_=false,watchdog_stopped_=false,any_submitted_=false;
    /* watchdog progress signature */
    uint32_t wd_fence_=0,wd_rptr_shadow_=0,wd_rptr_=0,wd_ib1_rem_=0;
    uint64_t wd_ib1_base_=0;
    uint32_t wd_stalled_ms_=0;
    A830WatchdogStats wd_stats_{};
    uint32_t wd_lock_=0;
    /* queue meter (4.13.5) */
    uint64_t q_stamps_[A830FenceQueueStamps]={};   /* mach time (low 40 bits) | source << 40 | depth (7 bits) << 42 | seqno (14 bits)
                                                      << 49 | 1 << 63; 0: none */
    /* The accountant's own (one at a time: q_busy_): each accounted job's start (the later of its stamp and the previous completion),
     * service time, the external service and the stampless completions accounted before it, by seqno % A830FenceQueueStamps. */
    struct QueueJob {uint64_t start,service,ext_before;uint32_t seq,missed_before,source;};
    QueueJob q_jobs_[A830FenceQueueStamps]={};
    uint32_t q_seen_=0;          /* newest seqno accounted (or skipped); written by the accountant, read lock-free */
    uint64_t q_done_at_=0;       /* when q_seen_ was seen completed (absolute time; 0: not known, after start() or a skip) */
    uint64_t q_ext_total_=0;     /* external service time accounted so far (absolute time) */
    uint32_t q_missed_total_=0;  /* completions accounted without their stamp so far */
    uint32_t q_busy_=0;          /* an accountant is at work */
    uint32_t q_calls_=0;         /* accounting calls so far: the accountant passes again when one came during its pass */
    uint32_t q_polled_call_=0;   /* a call came without the fence interrupt: the next pass that accounts anything marks the period */
    uint32_t q_skip_=0,q_skip_set_=0;   /* queueSkip's seqno, applied by the next pass */
    /* the period (relaxed atomics; takeQueue takes them) */
    uint64_t q_sum_=0;           /* the period: measured completions << 48 | their latency sum (absolute time, 48 bits) */
    uint64_t q_max_=0;           /* the period: longest latency (absolute time) */
    uint32_t q_slow_=0,q_depth_=0,q_submitted_=0,q_missed_=0,q_polled_=0,q_contexts_=0;
    uint64_t q_rs_=0,q_rs_max_=0;   /* render-server jobs measured << 48 | their service sum; the longest service */
    uint64_t q_fw_=0,q_fw_max_=0;   /* of them with the external time ahead known << 48 | its sum; the longest */
    uint64_t q_ext_max_=0;          /* the longest service of an external job */
    uint32_t q_fw_over_=0;          /* that time above A830FenceQueueFwUs */
    uint64_t q_ext_=0;              /* external jobs measured << 48 | their service sum */
    uint64_t q_slow_abs_=0,q_fw_abs_=0;   /* A830FenceQueueSlowUs and A830FenceQueueFwUs as absolute time (start()) */
    static void watchdogCall(void *param0,void *param1);
    bool polling() const;      /* no fence interrupt can wake a waiter: no A830Irq, 332 not attached, off, or polled */
    uint64_t sliceAbs() const; /* the safety slice with 332 on; A830_FENCE_POLL_SLICE_US while polling() */
    void armWatchdog();
    /* One accounting pass (q_busy_ held): queueSkip's seqno first, then every seqno up to the completed fence, in order. */
    void queuePass(uint64_t now);
    /* The external service time ahead of a render-server job (stamped at stamped with depth), from the jobs accounted before it;
     * false when it cannot be known. */
    bool externalAhead(uint32_t seq,uint32_t depth,uint64_t stamped,uint64_t *ahead) const;
public:
    A830Fence()=default;
    A830Fence(const A830Fence &)=delete;
    A830Fence &operator=(const A830Fence &)=delete;
    /* Stores the memptrs view and allocates the watchdog thread call; registers this as instance(). Once. */
    IOReturn start(const A830FenceConfig &config);
    /* ARCHITECTURE 4.13.3. deadline_abs is mach absolute time (0 or a past deadline: check once). closing (optional) is the
     * caller's connection state: non-zero makes the wait return kIOReturnAborted (wakeAll() wakes the sleepers to see it). */
    IOReturn wait(uint32_t seq,uint64_t deadline_abs,A830WaitStats *stats=nullptr,const volatile uint32_t *closing=nullptr);
    /* The same with a relative timeout (WaitFence's timeout_ns; 0 = check once, UINT64_MAX = no deadline). */
    IOReturn waitNs(uint32_t seq,uint64_t timeout_ns,A830WaitStats *stats=nullptr,const volatile uint32_t *closing=nullptr);
    /* NativeAdrenoRingWaiter::wait (NativeAdrenoRing.h): cookie = the fence; observed = the completed fence. */
    static IOReturn ringWaiter(void *cookie,uint32_t seq,uint32_t timeout_us,uint32_t *observed);
    /* Primary interrupt context (A830Irq on bit20): fence_irq_seq += 1, the queue meter's completions, thread_wakeup. */
    void signal();
    /* Wake every sleeper (connection closing, recovery). Any context. */
    void wakeAll() {a830Wakeup(&event_);}
    /* Global device loss: every wait returns kIOReturnNotResponding from now on; the watchdog stops. */
    void markLost();
    /* After a successful recovery: waits work again and the watchdog may run. */
    void clearLost();
    bool lost() const {return __atomic_load_n(&lost_,__ATOMIC_ACQUIRE);}
    /* The submit path, after the WPTR write that made seq visible to the CP: arms the watchdog. A wait for a seqno beyond the
     * newest one known does the same, so the watchdog runs whenever anybody waits for work. */
    void submitted(uint32_t seq);
    /* Recovery (ARCHITECTURE 4.13.4): the CP is halted; memptrs fence = seq (the last issued seqno, so every waiter sees its
     * work done; the jobs were failed separately) and the BR RPTR shadow = 0, then every waiter is woken. The queue meter
     * skips those seqnos first (queueSkip). */
    void forceComplete(uint32_t seq);
    uint32_t completed() const {return config_.memptrs?__atomic_load_n(&config_.memptrs[1],__ATOMIC_ACQUIRE):0;}
    bool passed(uint32_t seq) const {return static_cast<int32_t>(completed()-seq)>=0;}
    uint32_t lastSubmitted() const {return __atomic_load_n(&submitted_,__ATOMIC_ACQUIRE);}
    bool inFlight() const {return any() && !passed(lastSubmitted());}
    bool any() const {return __atomic_load_n(&any_submitted_,__ATOMIC_ACQUIRE);}
    /* Retirement bookkeeping of retireWork: a completion not yet handed to the retire hook (no submitted() needed). */
    bool retirePending() const {return config_.memptrs && __atomic_load_n(&retired_,__ATOMIC_ACQUIRE)!=completed();}
    void noteRetired(uint32_t seq) {__atomic_store_n(&retired_,seq,__ATOMIC_RELEASE);}
    uint32_t irqSeq() const {return __atomic_load_n(&irq_seq_,__ATOMIC_RELAXED);}
    /* One watchdog look (the thread call body; host tests call it directly). */
    void watchdogTick();
    /* Stop the watchdog for good (shutdown, slumber entry); startWatchdog() resumes it (slumber exit). */
    void stopWatchdog();
    void startWatchdog();
    A830WatchdogStats watchdogStats() const;
    /* Queue meter (4.13.5). ringStamp: NativeAdrenoRingWaiter::stamp (cookie = the fence), under the ring lock right before
     * the barrier and WPTR write of seq: queueSubmitted(seq, now, flags) (NativeAdrenoRingSource* | NativeAdrenoRingStampContext).
     * queueCompleted: accounts the seqnos completed since the last call, any context, never waiting (polled: they were seen
     * without the fence interrupt: the period's latencies and render-server service are upper bounds, its fw and n4 may be low);
     * a call that finds another accountant at work returns at once and that accountant covers it with a fresh clock. queueSkip:
     * the seqnos up to seq are taken as accounted, unmeasured (a recovery forces the fence over failed jobs; call it before the
     * forced fence is stored: a pass that sees the forced fence sees the skip). takeQueue: the period since the last take into
     * *out, then a new period (the DCVS thread, once a second); while polling() it first accounts what completed. */
    static void ringStamp(void *cookie,uint32_t seq,uint32_t flags);
    void queueSubmitted(uint32_t seq,uint64_t now,uint32_t flags=0);
    void queueCompleted(uint64_t now,bool polled);
    void queueSkip(uint32_t seq);
    void takeQueue(Adreno830StatusQueue *out);
    void takeQueueAt(Adreno830StatusQueue *out,uint64_t now);
    uint32_t queueSeen() const {return __atomic_load_n(&q_seen_,__ATOMIC_ACQUIRE);}
    void *event() {return &event_;}
    /* The device's fence (set by start(); null before): for the publish shell, the submit path and recovery. */
    static A830Fence *instance();
};
#ifdef ADRENO830_HOST_TEST
/* Host tests only: called inside every accounting pass right after it read the completed fence, the accountant's q_busy_ held (a
 * completion and its interrupt on another CPU in the middle of a pass: tests/test_a830fence.cpp). */
extern void (*a830FencePassHook)(A830Fence *fence);
#endif
#endif
