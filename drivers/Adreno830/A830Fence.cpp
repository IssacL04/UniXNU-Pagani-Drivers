/* SPDX-License-Identifier: MIT */
#include "A830Fence.h"
#include "A830Irq.h"
#include "A830Regs.h"
#include "Adreno830Status.h"
#include "NativeAdrenoRing.h"

namespace {
A830Fence *gInstance;
constexpr uint32_t kWatchdogPeriodMs=500,kHangMs=2000;   /* ARCHITECTURE 4.13.4 */
constexpr uint32_t kSliceUs=A830_FENCE_WAIT_SLICE_US;   /* 16 ms, ARCHITECTURE 4.13.3 */
/* Queue meter stamps (4.13.5): the low 40 bits of mach time (about 15 h at 19.2 MHz: differences stay exact across the
 * wrap), whose work it is (NativeAdrenoRingSource*, 2 bits), the depth it saw (7 bits, saturating: only depths below
 * A830FenceQueueStamps are ever looked back at), the seqno's low 14 bits (a slot holds seqno - 64 k otherwise) and the valid
 * bit. */
constexpr unsigned kStampTimeBits=40,kStampSourceShift=40,kStampDepthShift=42,kStampTagShift=49;
constexpr uint64_t kStampTimeMask=(uint64_t(1)<<kStampTimeBits)-1,kStampValid=uint64_t(1)<<63,kStampTagMask=0x3fff;
constexpr uint32_t kStampDepthMax=127,kJobUnknown=NativeAdrenoRingSourceMask+1;   /* a job accounted without its stamp */
static_assert((A830FenceQueueStamps&(A830FenceQueueStamps-1))==0 && A830FenceQueueStamps<=kStampDepthMax,
              "stamp slots are a power of two and the depths that matter fit the stamp");
constexpr uint64_t stampOf(uint32_t seq,uint64_t now,uint32_t flags,uint32_t depth)
{
    return (now&kStampTimeMask)|uint64_t(flags&NativeAdrenoRingSourceMask)<<kStampSourceShift|
           uint64_t(depth<kStampDepthMax?depth:kStampDepthMax)<<kStampDepthShift|uint64_t(seq&kStampTagMask)<<kStampTagShift|kStampValid;
}
constexpr bool stampFor(uint64_t stamp,uint32_t seq) {return (stamp&kStampValid) && ((stamp>>kStampTagShift)&kStampTagMask)==(seq&kStampTagMask);}
constexpr uint32_t sourceOf(uint64_t stamp) {return uint32_t(stamp>>kStampSourceShift)&NativeAdrenoRingSourceMask;}
constexpr uint32_t depthOf(uint64_t stamp) {return uint32_t(stamp>>kStampDepthShift)&kStampDepthMax;}
/* The period sum: measured completions in the top 16 bits, the latencies (absolute time) in the low 48. One latency is held
 * below 2^31 (beyond the 9999 ms the row shows at any counter rate), so 65535 of them cannot carry into the count. */
constexpr unsigned kSumBits=48;
constexpr uint64_t kSumMask=(uint64_t(1)<<kSumBits)-1,kLatencyMax=(uint64_t(1)<<31)-1;
template<typename T> void atomicMax(T *word,T value)
{
    T seen=__atomic_load_n(word,__ATOMIC_RELAXED);
    while(value>seen && !__atomic_compare_exchange_n(word,&seen,value,true,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
}
}

#ifdef ADRENO830_HOST_TEST
void (*a830FencePassHook)(A830Fence *);
#endif

A830Fence *A830Fence::instance() {return __atomic_load_n(&gInstance,__ATOMIC_ACQUIRE);}

IOReturn A830Fence::start(const A830FenceConfig &config)
{
    if(started_)return kIOReturnBusy;
    if(!config.memptrs)return kIOReturnBadArgument;
    config_=config;
    if(!config_.watchdog_period_ms)config_.watchdog_period_ms=kWatchdogPeriodMs;
    if(!config_.hang_ms)config_.hang_ms=kHangMs;
    if(!config_.slice_ms)config_.slice_ms=kSliceUs/1000;
    watchdog_call_=a830CallAllocate(watchdogCall,this);
    if(!watchdog_call_)return kIOReturnNoMemory;
    /* The meter accounts only seqnos completed from here on. */
    q_slow_abs_=a830NsToAbs(uint64_t(A830FenceQueueSlowUs)*1000);
    q_fw_abs_=a830NsToAbs(uint64_t(A830FenceQueueFwUs)*1000);
    q_done_at_=0;
    __atomic_store_n(&q_skip_set_,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&q_seen_,completed(),__ATOMIC_RELEASE);
    started_=true;
    __atomic_store_n(&gInstance,this,__ATOMIC_RELEASE);
    return kIOReturnSuccess;
}

/* With 332 attached and on, the slice only covers a lost interrupt. While the waits poll (332 never attached, turned off by
 * the storm guard, or fallBack()) nothing else wakes a waiter, and a 16 ms slice rounded every wait up to whole slices (the
 * annibale M8 run: about 55 late wakeups a second while scrolling); the poll slice keeps a wait within a millisecond. */
bool A830Fence::polling() const
{
    const A830Irq *irq=config_.irq;
    return !irq || !irq->attached() || irq->off() || irq->polled();
}

uint64_t A830Fence::sliceAbs() const
{
    const uint64_t us=polling()?A830_FENCE_POLL_SLICE_US:uint64_t(config_.slice_ms)*1000;
    return a830NsToAbs(us*1000);
}

/* ARCHITECTURE 4.13.3. assert_wait before the second look at the fence, so a wakeup between the look and thread_block is not
 * lost; the slice only covers a lost interrupt. No lock and no MMIO anywhere on this path. */
IOReturn A830Fence::wait(uint32_t seq,uint64_t deadline_abs,A830WaitStats *stats,const volatile uint32_t *closing)
{
    A830WaitStats local{};
    A830WaitStats &s=stats?*stats:local;
    s={};
    if(!started_)return kIOReturnNotReady;
    /* (The publish shell counts WaitFence calls and their timeouts in the status block; only late wakeups are counted here.)
     * A seqno somebody waits for has been handed to the CP: the watchdog learns about it here even when the submit path does
     * not report it with submitted(). */
    if(!passed(seq) && (!any() || static_cast<int32_t>(seq-lastSubmitted())>0))submitted(seq);
    const uint64_t slice=sliceAbs();
    bool after_slice=false;
    IOReturn result;
    for(;;) {
        if(passed(seq)) {
            result=kIOReturnSuccess;
            if(after_slice) {s.late=true;adreno830_status_add64(&adreno830_status.late_wakeups,1);}
            break;
        }
        if(lost()) {result=kA830NotResponding;break;}
        if(closing && *closing) {result=kA830Aborted;break;}
        const uint64_t now=a830Now();
        if(now>=deadline_abs) {result=kIOReturnTimeout;break;}
        const uint64_t until=deadline_abs-now>slice?now+slice:deadline_abs;
        int wait=a830AssertWait(&event_,until);
        if(passed(seq) || lost() || (closing && *closing)) {
            if(wait==A830WaitWaiting)a830ClearWait();
            continue;
        }
        if(wait==A830WaitWaiting) {
            ++s.blocks;
            wait=a830Block();
        }
        after_slice=false;
        if(wait==A830WaitAwakened) {++s.woken;continue;}
        if(wait==A830WaitTimedOut) {++s.slices;after_slice=true;continue;}
        /* Interrupted (a signal, thread_abort_safely) or restarted: "not yet" unless it is done (never kIOReturnAborted:
         * Turnip would declare the device lost). */
        s.interrupted=wait==A830WaitInterrupted;
        result=passed(seq)?kIOReturnSuccess:lost()?kA830NotResponding:kIOReturnTimeout;
        break;
    }
    /* With 332 off (storm guard) nothing kicks retirement: the waiter that saw the fence pass does. */
    if(result==kIOReturnSuccess && config_.irq && config_.irq->off() && retirePending())config_.irq->kickRetire();
    s.returned=a830Now();
    /* A waiter that saw its fence pass before the interrupt's top half measures those completions itself (while polling, the
     * only ones measured before the DCVS thread's take). */
    if(result==kIOReturnSuccess)queueCompleted(s.returned,polling());
    return result;
}

IOReturn A830Fence::waitNs(uint32_t seq,uint64_t timeout_ns,A830WaitStats *stats,const volatile uint32_t *closing)
{
    return wait(seq,timeout_ns?a830DeadlineAfterNs(timeout_ns):0,stats,closing);
}

IOReturn A830Fence::ringWaiter(void *cookie,uint32_t seq,uint32_t timeout_us,uint32_t *observed)
{
    A830Fence *fence=static_cast<A830Fence *>(cookie);
    if(!fence)return kIOReturnBadArgument;
    const IOReturn result=fence->waitNs(seq,uint64_t(timeout_us)*1000);
    if(observed)*observed=fence->completed();
    return result;
}

void A830Fence::signal()
{
    __atomic_fetch_add(&irq_seq_,1u,__ATOMIC_RELAXED);
    queueCompleted(a830Now(),false);
    a830Wakeup(&event_);
}

void A830Fence::markLost()
{
    __atomic_store_n(&lost_,true,__ATOMIC_RELEASE);
    stopWatchdog();
    wakeAll();
}

void A830Fence::clearLost()
{
    __atomic_store_n(&lost_,false,__ATOMIC_RELEASE);
    startWatchdog();
}

void A830Fence::submitted(uint32_t seq)
{
    /* Seqnos only grow (one ring); a stale caller never moves it back. */
    uint32_t old=__atomic_load_n(&submitted_,__ATOMIC_RELAXED);
    while((!any() || static_cast<int32_t>(seq-old)>0) &&
          !__atomic_compare_exchange_n(&submitted_,&old,seq,true,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED)) {}
    __atomic_store_n(&any_submitted_,true,__ATOMIC_RELEASE);
    armWatchdog();
}

void A830Fence::forceComplete(uint32_t seq)
{
    if(!config_.memptrs)return;
    queueSkip(seq);   /* failed jobs, not measured ones */
    __atomic_store_n(&config_.memptrs[0],0u,__ATOMIC_RELAXED);
    __atomic_store_n(&config_.memptrs[1],seq,__ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    wakeAll();
}

/* ---- software watchdog (ARCHITECTURE 4.13.4) ---- */

void A830Fence::armWatchdog()
{
    if(!started_ || lost() || __atomic_load_n(&watchdog_stopped_,__ATOMIC_ACQUIRE))return;
    if(__atomic_exchange_n(&watchdog_armed_,true,__ATOMIC_ACQ_REL))return;
    while(__atomic_exchange_n(&wd_lock_,1u,__ATOMIC_ACQUIRE)) {}
    /* A fresh run: the signature restarts from the first look. */
    wd_stalled_ms_=0;wd_fence_=completed();wd_rptr_shadow_=__atomic_load_n(&config_.memptrs[0],__ATOMIC_RELAXED);
    wd_rptr_=~0u;wd_ib1_base_=~0ull;wd_ib1_rem_=~0u;
    ++wd_stats_.armed;
    __atomic_store_n(&wd_lock_,0u,__ATOMIC_RELEASE);
    a830CallEnterDelayed(watchdog_call_,a830DeadlineAfterNs(uint64_t(config_.watchdog_period_ms)*1000000));
}

void A830Fence::watchdogCall(void *param0,void *) {static_cast<A830Fence *>(param0)->watchdogTick();}

void A830Fence::watchdogTick()
{
    if(!started_)return;
    while(__atomic_exchange_n(&wd_lock_,1u,__ATOMIC_ACQUIRE)) {}
    ++wd_stats_.ticks;
    bool rearm=false,hang=false;
    uint32_t stalled=0;
    if(!lost() && !__atomic_load_n(&watchdog_stopped_,__ATOMIC_ACQUIRE) && inFlight()) {
        const uint32_t fence=completed(),shadow=__atomic_load_n(&config_.memptrs[0],__ATOMIC_RELAXED);
        uint32_t rptr=wd_rptr_,rem=wd_ib1_rem_;
        uint64_t base=wd_ib1_base_;
        const bool probed=config_.irq && config_.irq->probe(&rptr,&base,&rem);
        /* Any change of fence, RPTR (shadow or register), IB1 base or IB1 remaining size is progress. */
        const bool progressed=fence!=wd_fence_ || shadow!=wd_rptr_shadow_ ||
                              (probed && (rptr!=wd_rptr_ || base!=wd_ib1_base_ || rem!=wd_ib1_rem_));
        wd_fence_=fence;wd_rptr_shadow_=shadow;
        if(probed) {wd_rptr_=rptr;wd_ib1_base_=base;wd_ib1_rem_=rem;}
        wd_stalled_ms_=progressed?0:wd_stalled_ms_+config_.watchdog_period_ms;
        stalled=wd_stalled_ms_;
        wd_stats_.stalled_ms=stalled;wd_stats_.last_fence=fence;wd_stats_.last_rptr=probed?rptr:shadow;
        wd_stats_.last_ib1_base=base;wd_stats_.last_ib1_rem=rem;
        if(stalled>=config_.hang_ms) {hang=true;++wd_stats_.hangs;}
        else rearm=true;
    }
    if(!rearm)__atomic_store_n(&watchdog_armed_,false,__ATOMIC_RELEASE);
    __atomic_store_n(&wd_lock_,0u,__ATOMIC_RELEASE);
    if(rearm) {
        a830CallEnterDelayed(watchdog_call_,a830DeadlineAfterNs(uint64_t(config_.watchdog_period_ms)*1000000));
        return;
    }
    if(hang) {
        if(config_.irq)config_.irq->escalate(A830IrqFaultWatchdog,A830ReasonHang,stalled);
        else {markLost();adreno830_status_add32(&adreno830_status.hangs,1);
              adreno830_status_lost(adreno830_status.stage,A830IrqFaultWatchdog,A830ReasonHang,kA830NotResponding);}
        return;
    }
    /* Disarmed with work submitted after the decision: look again. */
    if(inFlight())armWatchdog();
}

void A830Fence::stopWatchdog()
{
    __atomic_store_n(&watchdog_stopped_,true,__ATOMIC_RELEASE);
    if(watchdog_call_ && a830CallCancel(watchdog_call_))__atomic_store_n(&watchdog_armed_,false,__ATOMIC_RELEASE);
}

void A830Fence::startWatchdog()
{
    __atomic_store_n(&watchdog_stopped_,false,__ATOMIC_RELEASE);
    if(inFlight())armWatchdog();
}

A830WatchdogStats A830Fence::watchdogStats() const
{
    A830Fence *self=const_cast<A830Fence *>(this);
    while(__atomic_exchange_n(&self->wd_lock_,1u,__ATOMIC_ACQUIRE)) {}
    const A830WatchdogStats stats=wd_stats_;
    __atomic_store_n(&self->wd_lock_,0u,__ATOMIC_RELEASE);
    return stats;
}

/* ---- queue meter (ARCHITECTURE 4.13.5) ---- */

void A830Fence::ringStamp(void *cookie,uint32_t seq,uint32_t flags)
{
    if(A830Fence *fence=static_cast<A830Fence *>(cookie))fence->queueSubmitted(seq,a830Now(),flags);
}

void A830Fence::queueSubmitted(uint32_t seq,uint64_t now,uint32_t flags)
{
    /* The jobs this one waits behind, itself included (seq is not visible to the CP yet: at least 1). The stamp keeps the depth: the
     * jobs from seq - depth + 1 on were still in the ring when this one was handed over. */
    const int32_t ahead=static_cast<int32_t>(seq-completed());
    const uint32_t depth=ahead>0?uint32_t(ahead):1u;
    __atomic_store_n(&q_stamps_[seq&(A830FenceQueueStamps-1)],stampOf(seq,now,flags,depth),__ATOMIC_RELEASE);
    atomicMax(&q_depth_,depth);
    __atomic_fetch_add(&q_submitted_,1u,__ATOMIC_RELAXED);
    if(flags&NativeAdrenoRingStampContext)__atomic_fetch_add(&q_contexts_,1u,__ATOMIC_RELAXED);
}

void A830Fence::queueCompleted(uint64_t now,bool polled)
{
    if(!config_.memptrs)return;
    if(polled)__atomic_store_n(&q_polled_call_,1u,__ATOMIC_RELAXED);
    /* One accountant at a time (the attribution takes the jobs in order), and nobody waits for it: a caller counts its call, then
     * either becomes the accountant or returns. The accountant reads the count before its pass and again after it let go: a call
     * in between (a completion it may not have seen) gets another pass, with a fresh clock (that completion may be younger than
     * the accountant's time). */
    __atomic_add_fetch(&q_calls_,1u,__ATOMIC_SEQ_CST);
    for(;;) {
        if(__atomic_exchange_n(&q_busy_,1u,__ATOMIC_SEQ_CST))return;
        const uint32_t calls=__atomic_load_n(&q_calls_,__ATOMIC_SEQ_CST);
        queuePass(now);
        __atomic_store_n(&q_busy_,0u,__ATOMIC_SEQ_CST);
        if(__atomic_load_n(&q_calls_,__ATOMIC_SEQ_CST)==calls)return;
        now=a830Now();
    }
}

void A830Fence::queuePass(uint64_t now)
{
    /* The fence before the skip: a pass that sees a recovery's forced fence also sees the skip queueSkip stored before it. */
    const uint32_t done=completed();
#ifdef ADRENO830_HOST_TEST
    if(a830FencePassHook)a830FencePassHook(this);
#endif
    const bool polled=__atomic_exchange_n(&q_polled_call_,0u,__ATOMIC_RELAXED)!=0;
    uint32_t seen=__atomic_load_n(&q_seen_,__ATOMIC_RELAXED);   /* the accountant's own: written by the passes only */
    if(__atomic_exchange_n(&q_skip_set_,0u,__ATOMIC_ACQUIRE)) {
        const uint32_t skip=__atomic_load_n(&q_skip_,__ATOMIC_RELAXED);
        if(static_cast<int32_t>(skip-seen)>0) {seen=skip;q_done_at_=0;}   /* never backwards; the failed jobs end unknown */
    }
    /* (seen, done] is this pass's; a fence behind the accounted one (recovery's cleared memptrs) is nothing. */
    if(static_cast<int32_t>(done-seen)<=0) {__atomic_store_n(&q_seen_,seen,__ATOMIC_RELEASE);return;}
    /* Only the newest A830FenceQueueStamps seqnos can still have their stamp. */
    uint32_t first=seen+1,missed=0;
    if(done-seen>A830FenceQueueStamps) {missed=done-seen-A830FenceQueueStamps;first=done-A830FenceQueueStamps+1;}
    q_missed_total_+=missed;
    /* This pass's completion time, never before the previous pass's (a caller's clock may have been read before it). */
    const uint64_t at=now<q_done_at_?q_done_at_:now;
    uint64_t measured=0,sum=0,longest=0,rs=0,rsSum=0,rsLongest=0,fw=0,fwSum=0,fwLongest=0,ext=0,extSum=0,extLongest=0;
    uint32_t slow=0,fwOver=0;
    for(uint32_t seq=first;;++seq) {
        const uint64_t stamp=__atomic_load_n(&q_stamps_[seq&(A830FenceQueueStamps-1)],__ATOMIC_ACQUIRE);
        QueueJob &job=q_jobs_[seq&(A830FenceQueueStamps-1)];
        job.seq=seq;job.ext_before=q_ext_total_;job.missed_before=q_missed_total_;job.start=job.service=0;job.source=kJobUnknown;
        if(!stampFor(stamp,seq)) {++missed;++q_missed_total_;}
        else {
            uint64_t latency=(now-stamp)&kStampTimeMask;
            if(latency>kStampTimeMask/2)latency=0;   /* stamped by a CPU whose clock read a hair later: no wait */
            if(latency>kLatencyMax)latency=kLatencyMax;
            ++measured;sum+=latency;
            if(latency>longest)longest=latency;
            if(latency>q_slow_abs_)++slow;
            /* The service: from the later of its stamp and the previous completion to this one. */
            uint64_t waited=(at-stamp)&kStampTimeMask;
            if(waited>kStampTimeMask/2)waited=0;
            const uint64_t stamped=at-waited;
            job.start=q_done_at_>stamped?q_done_at_:stamped;
            job.service=at-job.start;
            job.source=sourceOf(stamp);
            const uint64_t service=job.service<kLatencyMax?job.service:kLatencyMax;
            if(job.source==NativeAdrenoRingSourceExternal) {
                ++ext;extSum+=service;q_ext_total_+=job.service;
                if(service>extLongest)extLongest=service;
            }
            else if(job.source==NativeAdrenoRingSourceRender) {
                ++rs;rsSum+=service;
                if(service>rsLongest)rsLongest=service;
                uint64_t ahead=0;
                if(externalAhead(seq,depthOf(stamp),stamped,&ahead)) {
                    if(ahead>kLatencyMax)ahead=kLatencyMax;
                    ++fw;fwSum+=ahead;
                    if(ahead>fwLongest)fwLongest=ahead;
                    if(ahead>q_fw_abs_)++fwOver;
                }
            }
        }
        q_done_at_=at;
        if(seq==done)break;
    }
    __atomic_store_n(&q_seen_,done,__ATOMIC_RELEASE);
    if(polled)__atomic_store_n(&q_polled_,1u,__ATOMIC_RELAXED);   /* seen without the fence interrupt: batched passes (4.13.5) */
    if(measured)__atomic_fetch_add(&q_sum_,measured<<kSumBits|sum,__ATOMIC_RELAXED);
    if(longest)atomicMax(&q_max_,longest);
    if(slow)__atomic_fetch_add(&q_slow_,slow,__ATOMIC_RELAXED);
    if(missed)__atomic_fetch_add(&q_missed_,missed,__ATOMIC_RELAXED);
    if(rs)__atomic_fetch_add(&q_rs_,rs<<kSumBits|rsSum,__ATOMIC_RELAXED);
    if(rsLongest)atomicMax(&q_rs_max_,rsLongest);
    if(fw)__atomic_fetch_add(&q_fw_,fw<<kSumBits|fwSum,__ATOMIC_RELAXED);
    if(fwLongest)atomicMax(&q_fw_max_,fwLongest);
    if(fwOver)__atomic_fetch_add(&q_fw_over_,fwOver,__ATOMIC_RELAXED);
    if(ext)__atomic_fetch_add(&q_ext_,ext<<kSumBits|extSum,__ATOMIC_RELAXED);
    if(extLongest)atomicMax(&q_ext_max_,extLongest);
}

bool A830Fence::externalAhead(uint32_t seq,uint32_t depth,uint64_t stamped,uint64_t *ahead) const
{
    *ahead=0;
    if(depth<=1)return true;                        /* nothing was ahead of it */
    if(depth>=A830FenceQueueStamps)return false;    /* the oldest one's entry is gone (and depths saturate) */
    /* The jobs from oldest on were in the ring at the stamp: their external service since then, without what the oldest one had
     * already run by the stamp. Unknown when the oldest was not accounted here (skipped, or before start()) or a job from it on
     * completed without its stamp. */
    const uint32_t first=seq-depth+1;
    const QueueJob &oldest=q_jobs_[first&(A830FenceQueueStamps-1)];
    if(oldest.seq!=first || oldest.missed_before!=q_missed_total_)return false;
    uint64_t total=q_ext_total_-oldest.ext_before;
    if(oldest.source==NativeAdrenoRingSourceExternal && stamped>oldest.start) {
        const uint64_t before=stamped-oldest.start;
        total-=before<oldest.service?before:oldest.service;
    }
    *ahead=total;
    return true;
}

void A830Fence::queueSkip(uint32_t seq)
{
    /* Applied by the next pass before it accounts anything: this call's, or (when an accountant is at work) its next one. */
    __atomic_store_n(&q_skip_,seq,__ATOMIC_RELAXED);
    __atomic_store_n(&q_skip_set_,1u,__ATOMIC_RELEASE);
    queueCompleted(a830Now(),false);
}

void A830Fence::takeQueue(Adreno830StatusQueue *out) {takeQueueAt(out,a830Now());}

void A830Fence::takeQueueAt(Adreno830StatusQueue *out,uint64_t now)
{
    if(!out)return;
    /* No fence interrupt can arrive: what completed since the last waiter is measured now (latencies as upper bounds, flagged
     * polled). */
    if(started_ && polling())queueCompleted(now,true);
    /* Averages to the nearest microsecond, maxima up. */
    auto averageUs=[](uint64_t word) {
        const uint64_t ns=a830AbsToNs((word&kSumMask)/(word>>kSumBits));
        return uint32_t((ns+500)/1000);
    };
    auto ceilingUs=[](uint64_t abs) {
        const uint64_t us=(a830AbsToNs(abs)+999)/1000;
        return us<UINT32_MAX?uint32_t(us):UINT32_MAX;
    };
    const uint64_t sum=__atomic_exchange_n(&q_sum_,uint64_t(0),__ATOMIC_ACQ_REL);
    const uint64_t longest=__atomic_exchange_n(&q_max_,uint64_t(0),__ATOMIC_ACQ_REL);
    Adreno830StatusQueue q{};
    q.completed=uint32_t(sum>>kSumBits);
    if(q.completed) {q.avg_us=averageUs(sum);q.max_us=ceilingUs(longest);}
    q.slow=__atomic_exchange_n(&q_slow_,0u,__ATOMIC_ACQ_REL);
    q.depth=__atomic_exchange_n(&q_depth_,0u,__ATOMIC_ACQ_REL);
    q.submitted=__atomic_exchange_n(&q_submitted_,0u,__ATOMIC_ACQ_REL);
    q.missed=__atomic_exchange_n(&q_missed_,0u,__ATOMIC_ACQ_REL);
    q.polled=__atomic_exchange_n(&q_polled_,0u,__ATOMIC_ACQ_REL);
    /* The attribution (preemption phase 0). */
    const uint64_t rs=__atomic_exchange_n(&q_rs_,uint64_t(0),__ATOMIC_ACQ_REL);
    const uint64_t rsLongest=__atomic_exchange_n(&q_rs_max_,uint64_t(0),__ATOMIC_ACQ_REL);
    const uint64_t fw=__atomic_exchange_n(&q_fw_,uint64_t(0),__ATOMIC_ACQ_REL);
    const uint64_t fwLongest=__atomic_exchange_n(&q_fw_max_,uint64_t(0),__ATOMIC_ACQ_REL);
    const uint64_t ext=__atomic_exchange_n(&q_ext_,uint64_t(0),__ATOMIC_ACQ_REL);
    const uint64_t extLongest=__atomic_exchange_n(&q_ext_max_,uint64_t(0),__ATOMIC_ACQ_REL);
    q.rs_jobs=uint32_t(rs>>kSumBits);
    if(q.rs_jobs) {q.rs_avg_us=averageUs(rs);q.rs_max_us=ceilingUs(rsLongest);}
    q.fw_jobs=uint32_t(fw>>kSumBits);
    if(q.fw_jobs) {q.fw_avg_us=averageUs(fw);q.fw_max_us=ceilingUs(fwLongest);}
    q.fw_over4=__atomic_exchange_n(&q_fw_over_,0u,__ATOMIC_ACQ_REL);
    q.ext_jobs=uint32_t(ext>>kSumBits);
    q.ext_busy_us=uint32_t((a830AbsToNs(ext&kSumMask)+500)/1000);
    if(q.ext_jobs)q.ext_max_us=ceilingUs(extLongest);
    q.contexts=__atomic_exchange_n(&q_contexts_,0u,__ATOMIC_ACQ_REL);
    *out=q;
}
