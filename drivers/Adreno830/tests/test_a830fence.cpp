/* SPDX-License-Identifier: MIT */
/* host-test: A830Fence.cpp A830Irq.cpp Adreno830Status.cpp tests/A830HostSched.cpp */
/* A830Fence on real threads (ARCHITECTURE 4.13.3/4.13.4, PLAN M6/M10): the wakeup that races the sleep is never lost (no
 * wait needs the 16 ms safety slice), timeouts, an interrupted thread gets kIOReturnTimeout (never kIOReturnAborted), a
 * closing connection gets kIOReturnAborted, a lost device kIOReturnNotResponding; no lock and no MMIO while waiting; the
 * software watchdog (500 ms / 2 s) escalates a stalled ring through A830Irq and stays quiet while anything moves; recovery
 * completes every waiter; retirement is kicked while 332 is off. The queue meter (4.13.5): stamps, depth, the period's values,
 * missed stamps, skips, who measures, and racing accountants. */
#include "A830FakeGpu.h"
#include "A830Fence.h"
#include "A830HostSched.h"
#include "A830Irq.h"
#include "Adreno830Status.h"
#include "NativeAdrenoRing.h"
#include <atomic>
#include <chrono>
#include <random>
#include <thread>

namespace {
unsigned checks;
#define EXPECT(x) do {CHECK(x);++checks;} while(0)
using Clock=std::chrono::steady_clock;
uint64_t ms(Clock::duration d) {return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(d).count());}

struct Sink {unsigned lost=0,retires=0;A830FaultReport report{};};
Sink sink;

struct Rig {
    FakeGpu gpu;
    uint32_t memptrs[1024]{};
    A830Fence fence;
    A830Irq irq;
};

Rig *previous;
Rig *fresh(bool with_irq=true,uint32_t period_ms=0,uint32_t hang_ms=0,uint32_t slice_ms=0)
{
    /* the previous case's ring may still look busy: its watchdog must not fire into this case */
    if(previous)previous->fence.stopWatchdog();
    a830HostRunCalls(true);
    adreno830_status_reset();
    gGic.reset();
    sink=Sink{};
    Rig *rig=new Rig;
    rig->gpu.mask=A8XX_INT_MASK;
    gGic.level=[rig](uint32_t) {return rig->gpu.asserted();};
    A830FenceConfig fc{};
    /* cases that do not test the watchdog get one that never fires on its own */
    fc.memptrs=rig->memptrs;fc.irq=with_irq?&rig->irq:nullptr;
    fc.watchdog_period_ms=period_ms?period_ms:3600000;fc.hang_ms=hang_ms?hang_ms:3600000;
    fc.slice_ms=slice_ms;
    previous=rig;
    EXPECT(rig->fence.start(fc)==kIOReturnSuccess);
    EXPECT(rig->fence.start(fc)==kIOReturnBusy);
    if(with_irq) {
        A830IrqConfig config{};
        config.gpu=rig->gpu.mmio();config.fence=&rig->fence;
        config.hooks.cookie=&sink;
        config.hooks.deviceLost=[](void *c,const A830FaultReport *r) {Sink *s=static_cast<Sink *>(c);++s->lost;s->report=*r;};
        config.hooks.retire=[](void *c,uint32_t) {++static_cast<Sink *>(c)->retires;};
        EXPECT(rig->irq.start(config)==kIOReturnSuccess);
        EXPECT(rig->irq.attach(A830_IRQ_GPU)==kIOReturnSuccess);
    }
    return rig;
}

/* The CP: completes seq and raises the fence interrupt (bit20), which the GIC delivers to A830Irq::primary. */
void complete(Rig *rig,uint32_t seq)
{
    __atomic_store_n(&rig->memptrs[1],seq,__ATOMIC_RELEASE);
    rig->gpu.raise(A8XX_RBBM_INT_0_CP_CACHE_FLUSH_TS);
    gGic.deliver(A830_IRQ_GPU);
}

void waitUntilBlocked(A830Fence &fence)
{
    for(int i=0;i<5000 && !a830HostWaiting(fence.event());++i)std::this_thread::sleep_for(std::chrono::microseconds(200));
    CHECK(a830HostWaiting(fence.event())==1);
}

void basics()
{
    Rig *rig=fresh();
    A830Fence unstarted;
    EXPECT(unstarted.wait(1,0)==kIOReturnNotReady && unstarted.completed()==0 && !unstarted.inFlight());
    A830FenceConfig none{};
    EXPECT(unstarted.start(none)==kIOReturnBadArgument);
    rig->memptrs[1]=7;
    A830WaitStats s{};
    EXPECT(rig->fence.wait(7,0,&s)==kIOReturnSuccess && s.blocks==0);
    EXPECT(rig->fence.wait(5,0)==kIOReturnSuccess);           /* older seqnos have passed too */
    EXPECT(rig->fence.wait(8,0,&s)==kIOReturnTimeout && s.blocks==0);   /* timeout 0: one look */
    EXPECT(rig->fence.waitNs(8,0)==kIOReturnTimeout);
    /* the publish shell counts WaitFence calls; the fence only learns the outstanding seqno (watchdog) */
    EXPECT(adreno830_status.waits==0 && adreno830_status.wait_timeouts==0);
    EXPECT(rig->fence.lastSubmitted()==8 && rig->fence.inFlight() && rig->fence.watchdogStats().armed==1);
    rig->fence.stopWatchdog();
    /* seqno wrap: 0xFFFFFFF0 + 0x20 has passed 0xFFFFFFF8 */
    rig->memptrs[1]=0x10;
    EXPECT(rig->fence.passed(0xFFFFFFF8u) && !rig->fence.passed(0x11));
    uint32_t observed=0;
    EXPECT(A830Fence::ringWaiter(&rig->fence,0x10,0,&observed)==kIOReturnSuccess && observed==0x10);
    EXPECT(A830Fence::ringWaiter(&rig->fence,0x11,1000,&observed)==kIOReturnTimeout && observed==0x10);
    EXPECT(A830Fence::ringWaiter(nullptr,1,0,&observed)==kIOReturnBadArgument);
}

/* The race of 4.13.3: completion and interrupt at any point relative to the waiter's look, assert_wait and thread_block.
 * The safety slice is 5 s here and every deadline 3 s: a single lost wakeup would end its wait in kIOReturnTimeout, however
 * loaded the host is. */
void racingWakeups()
{
    Rig *rig=fresh(true,0,0,5000);
    constexpr uint32_t rounds=3000;
    std::atomic<uint32_t> next{0};
    std::thread gpu([&] {
        std::mt19937 random(830);
        for(uint32_t seq=1;seq<=rounds;++seq) {
            while(next.load(std::memory_order_acquire)<seq)std::this_thread::yield();
            const unsigned spin=random()%400;
            for(volatile unsigned i=0;i<spin;i=i+1) {}
            if(random()%3==0)std::this_thread::sleep_for(std::chrono::microseconds(random()%50));
            complete(rig,seq);
        }
    });
    const uint64_t accesses_before=rig->gpu.accessCount();
    (void)accesses_before;
    uint32_t late=0,blocked=0,failed=0;
    for(uint32_t seq=1;seq<=rounds;++seq) {
        next.store(seq,std::memory_order_release);
        A830WaitStats s{};
        failed+=rig->fence.waitNs(seq,3000000000ull,&s)!=kIOReturnSuccess;
        late+=s.late;blocked+=s.blocks!=0;
    }
    gpu.join();
    EXPECT(failed==0 && late==0);
    EXPECT(blocked>0);   /* the waiter did sleep and was woken by the interrupt */
    EXPECT(rig->fence.irqSeq()==rounds && adreno830_status.late_wakeups==0);
}

/* No MMIO and no lock while waiting: the fake GPU sees no access from the waiter (the GPU thread raises nothing). */
void noMmioWhileWaiting()
{
    Rig *rig=fresh();
    rig->memptrs[1]=1;
    const uint64_t before=rig->gpu.accessCount();
    A830WaitStats s{};
    const Clock::time_point began=Clock::now();
    EXPECT(rig->fence.waitNs(2,40000000ull,&s)==kIOReturnTimeout);
    /* 16 ms slices up to the 40 ms deadline (a loaded host may oversleep one): every block ended by its slice */
    EXPECT(ms(Clock::now()-began)>=40 && s.slices>=2 && s.blocks==s.slices && !s.woken);
    EXPECT(rig->gpu.accessCount()==before);
}

void lateWithoutInterrupt()
{
    Rig *rig=fresh();
    std::thread gpu([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        __atomic_store_n(&rig->memptrs[1],3u,__ATOMIC_RELEASE);   /* no interrupt: the slice finds it */
    });
    A830WaitStats s{};
    EXPECT(rig->fence.waitNs(3,1000000000ull,&s)==kIOReturnSuccess);
    gpu.join();
    EXPECT(s.late && s.slices>=1 && adreno830_status.late_wakeups==1);
}

/* No fence interrupt at all (no A830Irq, as when 332 is not attached): the waiter polls in 1 ms slices, so a fence completed
 * 5 ms after the wait began is found after several short slices, not at the end of a 16 ms one. */
void pollingWithoutInterrupt()
{
    Rig *rig=fresh(false);
    std::thread gpu([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        __atomic_store_n(&rig->memptrs[1],4u,__ATOMIC_RELEASE);
    });
    A830WaitStats s{};
    EXPECT(rig->fence.waitNs(4,1000000000ull,&s)==kIOReturnSuccess);
    gpu.join();
    EXPECT(s.late && s.slices>=3 && s.blocks==s.slices && !s.woken);
}

void interruptedReturnsTimeout()
{
    Rig *rig=fresh();
    std::atomic<IOReturn> result{kIOReturnSuccess};
    A830WaitStats s{};
    std::thread waiter([&] {result=rig->fence.waitNs(5,UINT64_MAX,&s);});
    waitUntilBlocked(rig->fence);
    a830HostAbort(waiter.get_id());
    waiter.join();
    /* "not yet": Turnip retries a timeout and declares the device lost on anything else (ARCHITECTURE 4.13.3) */
    EXPECT(result==kIOReturnTimeout && s.interrupted && result!=kA830Aborted);
    /* interrupted after the fence passed: success */
    std::thread waiter2([&] {result=rig->fence.waitNs(6,UINT64_MAX,&s);});
    waitUntilBlocked(rig->fence);
    __atomic_store_n(&rig->memptrs[1],6u,__ATOMIC_RELEASE);
    a830HostAbort(waiter2.get_id());
    waiter2.join();
    EXPECT(result==kIOReturnSuccess);
    /* an abort pending before the wait (assert_wait answers INTERRUPTED at once) */
    std::thread waiter3([&] {
        a830HostAbort(std::this_thread::get_id());
        result=rig->fence.waitNs(9,UINT64_MAX,&s);
    });
    waiter3.join();
    EXPECT(result==kIOReturnTimeout && s.interrupted && s.blocks==0);
}

void closingAndLost()
{
    Rig *rig=fresh();
    volatile uint32_t closing=0;
    std::atomic<IOReturn> result{kIOReturnSuccess};
    std::thread waiter([&] {result=rig->fence.waitNs(4,UINT64_MAX,nullptr,&closing);});
    waitUntilBlocked(rig->fence);
    closing=1;
    rig->fence.wakeAll();
    waiter.join();
    EXPECT(result==kA830Aborted);
    std::thread lost([&] {result=rig->fence.waitNs(4,UINT64_MAX);});
    waitUntilBlocked(rig->fence);
    rig->fence.markLost();
    lost.join();
    EXPECT(result==kA830NotResponding && rig->fence.lost());
    EXPECT(rig->fence.waitNs(4,0)==kA830NotResponding);
    rig->memptrs[1]=4;
    EXPECT(rig->fence.waitNs(4,0)==kIOReturnSuccess);   /* a passed fence is still reported done */
    rig->fence.clearLost();
    EXPECT(!rig->fence.lost() && rig->fence.waitNs(5,0)==kIOReturnTimeout);
}

void recoveryCompletesWaiters()
{
    Rig *rig=fresh();
    rig->memptrs[0]=0x40;rig->memptrs[1]=10;
    rig->fence.submitted(14);
    std::atomic<IOReturn> result[3]={kIOReturnIOError,kIOReturnIOError,kIOReturnIOError};
    std::thread waiters[3];
    for(int i=0;i<3;++i)waiters[i]=std::thread([&,i] {result[i]=rig->fence.waitNs(12+i,UINT64_MAX);});
    for(int i=0;i<5000 && a830HostWaiting(rig->fence.event())<3;++i)std::this_thread::sleep_for(std::chrono::microseconds(200));
    EXPECT(a830HostWaiting(rig->fence.event())==3);
    rig->fence.forceComplete(14);
    for(auto &t:waiters)t.join();
    EXPECT(result[0]==kIOReturnSuccess && result[1]==kIOReturnSuccess && result[2]==kIOReturnSuccess);
    EXPECT(rig->memptrs[0]==0 && rig->fence.completed()==14 && !rig->fence.inFlight());
}

/* ---- watchdog (manual thread calls) ---- */

void watchdogHang()
{
    a830HostManualCalls(true);
    Rig *rig=fresh(true,500,2000);
    rig->memptrs[1]=9;
    rig->fence.submitted(10);
    EXPECT(rig->fence.inFlight() && rig->fence.watchdogStats().armed==1);
    rig->gpu.set(A8XX_CP_RB_RPTR,0x30);
    rig->gpu.setPipe(A8XX_PIPE_BR,A8XX_CP_IB1_BASE,0x21000000);rig->gpu.setPipe(A8XX_PIPE_BR,A8XX_CP_IB1_REM_SIZE,12);
    /* the first look sets the signature; four more 500 ms looks without any change make 2 s: a hang */
    EXPECT(a830HostRunCalls(true)==1 && rig->fence.watchdogStats().stalled_ms==0 && sink.lost==0);
    for(int i=1;i<4;++i) {EXPECT(a830HostRunCalls(true)==1 && sink.lost==0);}
    EXPECT(rig->fence.watchdogStats().stalled_ms==1500);
    EXPECT(a830HostRunCalls(true)>=1);
    EXPECT(sink.lost==1 && sink.report.kind==A830IrqFaultWatchdog && sink.report.reason==A830ReasonHang && sink.report.detail==2000);
    EXPECT(sink.report.rptr==0x30 && sink.report.ib1_base==0x21000000 && sink.report.ib1_rem==12 && sink.report.submitted==10);
    EXPECT(rig->fence.watchdogStats().hangs==1 && adreno830_status.hangs==1);
    EXPECT(rig->gpu.apertureNow()==0 && a830HostRunCalls(true)==0);   /* not re-armed after a hang */
    a830HostManualCalls(false);
}

void watchdogProgress()
{
    a830HostManualCalls(true);
    Rig *rig=fresh(true,500,2000);
    rig->memptrs[1]=20;
    rig->fence.submitted(21);
    /* one long IB: neither fence nor RPTR moves, but IB1_REM_SIZE shrinks -> progress */
    for(uint32_t i=0;i<12;++i) {
        rig->gpu.setPipe(A8XX_PIPE_BR,A8XX_CP_IB1_REM_SIZE,1000-i);
        EXPECT(a830HostRunCalls(true)==1 && sink.lost==0);
    }
    /* a moving fence (more work submitted meanwhile) is progress as well */
    rig->fence.submitted(30);
    for(uint32_t i=0;i<6;++i) {
        rig->memptrs[1]=21+i;
        EXPECT(a830HostRunCalls(true)==1 && sink.lost==0);
    }
    /* everything completed: the watchdog disarms and stays off until the next submission */
    rig->memptrs[1]=30;
    EXPECT(a830HostRunCalls(true)==1 && a830HostRunCalls(true)==0 && sink.lost==0);
    rig->fence.submitted(31);
    EXPECT(rig->fence.watchdogStats().armed==2 && a830HostRunCalls(true)==1);
    /* stopped (slumber entry): no look, no re-arm; started again it resumes */
    rig->fence.stopWatchdog();
    EXPECT(a830HostRunCalls(true)==0);
    rig->fence.startWatchdog();
    EXPECT(a830HostRunCalls(true)==1 && sink.lost==0);
    rig->fence.stopWatchdog();   /* this ring stays busy: keep its watchdog out of the next cases */
    a830HostManualCalls(false);
}

void watchdogWithoutIrq()
{
    a830HostManualCalls(true);
    Rig *rig=fresh(false,500,1000);
    adreno830_status.stage=A830SRunning;
    rig->fence.submitted(3);
    for(int i=0;i<3;++i)a830HostRunCalls(true);
    EXPECT(rig->fence.lost() && adreno830_status.state==A830Lost && adreno830_status.hangs==1);
    EXPECT(rig->fence.waitNs(3,0)==kA830NotResponding);
    a830HostManualCalls(false);
}

/* Threaded thread calls and real time: a 20 ms / 80 ms watchdog fires once on a stalled ring. */
void watchdogThreaded()
{
    Rig *rig=fresh(true,20,80);
    rig->fence.submitted(2);
    for(int i=0;i<200 && !sink.lost;++i)std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT(sink.lost==1 && sink.report.kind==A830IrqFaultWatchdog);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT(sink.lost==1);
}

/* ---- queue meter (ARCHITECTURE 4.13.5) ---- */
unsigned queueCases;

/* Deterministic times (host absolute time is ns): stamps, the depth each submission saw, completions accounted once, the
 * period's average/maximum/slow values and its reset, the 40-bit stamp clock, missed stamps, skips and forced fences, and
 * who measures: the fence interrupt's top half, a waiter, and the DCVS thread's take only while the waits poll. */
void queueMeter()
{
    Rig *rig=fresh();
    A830Fence &f=rig->fence;
    const uint64_t t0=uint64_t(1)<<41,ms=1000000;
    Adreno830StatusQueue q{};
    EXPECT(f.queueSeen()==0);
    f.takeQueueAt(&q,t0);
    EXPECT(q.submitted==0 && q.completed==0 && q.missed==0 && q.depth==0 && q.avg_us==0 && q.max_us==0 && q.slow==0 && !q.polled);
    /* Three submissions behind each other (depth 1, 2, 3); 1 and 2 complete under one interrupt (5 and 4 ms), 3 after 28 ms. */
    f.queueSubmitted(1,t0);f.queueSubmitted(2,t0+1*ms);f.queueSubmitted(3,t0+2*ms);
    rig->memptrs[1]=2;
    f.queueCompleted(t0+5*ms,false);
    f.queueCompleted(t0+6*ms,false);   /* nothing new: nothing counted twice */
    rig->memptrs[1]=3;
    f.queueCompleted(t0+30*ms,false);
    f.takeQueueAt(&q,t0+31*ms);
    EXPECT(q.submitted==3 && q.completed==3 && q.missed==0 && q.depth==3 && q.slow==1 && !q.polled);
    EXPECT(q.avg_us==12333 && q.max_us==28000 && f.queueSeen()==3);
    f.takeQueueAt(&q,t0+32*ms);        /* the next period starts empty */
    EXPECT(q.submitted==0 && q.completed==0 && q.depth==0 && q.slow==0 && q.max_us==0 && q.avg_us==0);
    ++queueCases;
    /* 16 ms is not slow, 16 ms + 1 ns is (and its maximum rounds up to 16001 us); depth counts from the completed fence. */
    f.queueSubmitted(4,t0);f.queueSubmitted(5,t0);
    rig->memptrs[1]=4;f.queueCompleted(t0+16*ms,false);
    rig->memptrs[1]=5;f.queueCompleted(t0+16*ms+1,false);
    f.takeQueueAt(&q,t0);
    EXPECT(q.completed==2 && q.slow==1 && q.max_us==16001 && q.depth==2 && q.avg_us==16000);
    ++queueCases;
    /* The stamp keeps 40 bits of the clock: a latency across their wrap is exact; a stamp read a hair after the completion
     * (another CPU's clock) is no wait at all. */
    const uint64_t wrap=(uint64_t(1)<<40)-1000;
    f.queueSubmitted(6,wrap);rig->memptrs[1]=6;f.queueCompleted(wrap+10000,false);
    f.queueSubmitted(7,t0+5*ms);rig->memptrs[1]=7;f.queueCompleted(t0+4*ms,false);
    f.takeQueueAt(&q,t0);
    EXPECT(q.completed==2 && q.max_us==10 && q.avg_us==5 && q.slow==0);
    ++queueCases;
    /* 100 in flight: only the newest 64 still have their stamp; the depth is the deepest a submission saw. */
    for(uint32_t seq=8;seq<108;++seq)f.queueSubmitted(seq,t0);
    rig->memptrs[1]=107;f.queueCompleted(t0+2*ms,false);
    f.takeQueueAt(&q,t0+3*ms);
    EXPECT(q.submitted==100 && q.depth==100 && q.completed==A830FenceQueueStamps && q.missed==100-A830FenceQueueStamps &&
           q.max_us==2000 && q.avg_us==2000);
    /* A seqno never stamped (its slot holds an older seqno): missed, not measured. */
    rig->memptrs[1]=108;f.queueCompleted(t0,false);
    f.takeQueueAt(&q,t0);
    EXPECT(q.completed==0 && q.missed==1 && q.submitted==0);
    ++queueCases;
    /* The recovery's forced fences: skipped (never backwards), not measured; a cleared memptrs fence measures nothing. */
    f.queueSubmitted(109,t0);f.queueSubmitted(110,t0);
    f.queueSkip(110);
    rig->memptrs[1]=110;f.queueCompleted(t0+ms,false);
    f.queueSkip(105);
    EXPECT(f.queueSeen()==110);
    f.queueSubmitted(111,t0);
    f.forceComplete(111);
    f.queueCompleted(t0+ms,false);
    f.takeQueueAt(&q,t0+ms);
    EXPECT(q.submitted==3 && q.completed==0 && q.missed==0 && f.queueSeen()==111 && f.completed()==111);
    rig->memptrs[1]=0;f.queueCompleted(t0,false);
    EXPECT(f.queueSeen()==111);
    rig->memptrs[1]=111;
    ++queueCases;
    /* Real time: the ring's stamp hook, then the CP completes and interrupts: the top half measures. */
    A830Fence::ringStamp(&f,112,NativeAdrenoRingSourceKernel);
    A830Fence::ringStamp(nullptr,112,NativeAdrenoRingSourceKernel);   /* no fence: nothing */
    complete(rig,112);
    f.takeQueue(&q);
    EXPECT(q.submitted==1 && q.completed==1 && q.missed==0 && !q.polled && q.max_us<1000000);
    /* A waiter that sees its fence before the interrupt measures it itself (not polled: 332 is live). */
    A830Fence::ringStamp(&f,113,NativeAdrenoRingSourceKernel);
    rig->memptrs[1]=113;
    EXPECT(f.wait(113,0)==kIOReturnSuccess);
    f.takeQueue(&q);
    EXPECT(q.completed==1 && !q.polled && f.queueSeen()==113);
    /* With 332 live the take leaves a completion to its interrupt. */
    f.queueSubmitted(114,t0);rig->memptrs[1]=114;
    f.takeQueueAt(&q,t0+ms);
    EXPECT(q.submitted==1 && q.completed==0);
    f.queueCompleted(t0+2*ms,false);
    f.takeQueueAt(&q,t0+3*ms);
    EXPECT(q.completed==1 && q.max_us==2000);
    ++queueCases;
    /* No fence interrupt (332 not attached): waiters and the DCVS thread's take measure, the period is flagged polled. */
    Rig *polled=fresh(false);
    A830Fence &p=polled->fence;
    const uint64_t r=a830Now();
    p.queueSubmitted(1,r);p.queueSubmitted(2,r);
    polled->memptrs[1]=1;
    EXPECT(p.wait(1,0)==kIOReturnSuccess && p.queueSeen()==1);
    polled->memptrs[1]=2;                 /* nobody waits for 2: the take measures it */
    p.takeQueueAt(&q,r+50*ms);
    EXPECT(q.submitted==2 && q.completed==2 && q.polled && q.max_us==50000 && q.slow==1);
    p.takeQueueAt(&q,r+60*ms);
    EXPECT(!q.polled && q.completed==0);
    ++queueCases;
    /* The meter starts at the fence the CP reached before start(). */
    static uint32_t early[16]={0,50};
    A830Fence *late=new A830Fence;   /* kept, like the rigs: its watchdog call stays allocated */
    A830FenceConfig config{};config.memptrs=early;config.watchdog_period_ms=3600000;config.hang_ms=3600000;
    EXPECT(late->start(config)==kIOReturnSuccess && late->queueSeen()==50);
    late->stopWatchdog();
    ++queueCases;
}

/* Accounting contexts racing (two waiters, the CP side's interrupt after each completion, the take): every completion is counted
 * exactly once, and the last one is accounted when the last call returns, though that call may have found another at work (its
 * accountant passes again): nobody calls after it. */
void queueRacing()
{
    Rig *rig=fresh(false);
    A830Fence &f=rig->fence;
    constexpr uint32_t n=20000;
    std::atomic<bool> done{false};
    auto account=[&] {while(!done.load(std::memory_order_acquire))f.queueCompleted(a830Now(),false);};
    std::thread a(account),b(account);
    uint64_t submitted=0,completed=0,missed=0;
    Adreno830StatusQueue q{};
    for(uint32_t seq=1;seq<=n;++seq) {
        f.queueSubmitted(seq,a830Now());
        __atomic_store_n(&rig->memptrs[1],seq,__ATOMIC_RELEASE);
        f.queueCompleted(a830Now(),false);
        if(seq%997==0) {f.takeQueue(&q);submitted+=q.submitted;completed+=q.completed;missed+=q.missed;}
    }
    done.store(true,std::memory_order_release);
    a.join();b.join();
    const uint32_t seen=f.queueSeen();
    f.takeQueue(&q);submitted+=q.submitted;completed+=q.completed;missed+=q.missed;
    EXPECT(seen==n && submitted==n && completed+missed==n && f.queueSeen()==n);
    ++queueCases;
}

/* The accountant's second look: a completion that arrives in the middle of a pass (its interrupt on another CPU finds the accountant at
 * work and returns at once) is accounted before the accountant returns, with a clock read after that call, not the accountant's own
 * older one. */
Rig *hooked;
unsigned hookPasses;
void queueRecheck()
{
    Rig *rig=fresh(false);
    A830Fence &f=rig->fence;
    const uint64_t ms=1000000,t1=a830Now();
    f.queueSubmitted(1,t1,NativeAdrenoRingSourceExternal);
    f.queueSubmitted(2,t1,NativeAdrenoRingSourceRender);
    rig->memptrs[1]=1;
    hooked=rig;hookPasses=0;
    a830FencePassHook=[](A830Fence *fence) {
        if(hookPasses++)return;
        /* 2 completes 2 ms after its stamp, during this first pass, and its top half runs: it finds the pass at work. */
        const uint64_t until=a830Now()+2*1000000;
        while(a830Now()<until) {}
        __atomic_store_n(&hooked->memptrs[1],2u,__ATOMIC_RELEASE);
        fence->queueCompleted(a830Now(),false);
        CHECK(fence->queueSeen()==0);
    };
    f.queueCompleted(t1+ms,false);   /* the accountant's clock: 1 ms after the stamps, older than 2's completion */
    a830FencePassHook=nullptr;
    EXPECT(hookPasses==2 && f.queueSeen()==2);
    Adreno830StatusQueue q{};
    f.takeQueueAt(&q,a830Now());
    /* 1 waited 1 ms by the accountant's clock; 2 at least 2 ms by the fresh one, and ran at least 1 ms after 1 (the old clock would
     * give it 1 ms and no service). */
    EXPECT(q.completed==2 && q.max_us>=2000 && q.ext_jobs==1 && q.ext_busy_us==1000 && q.rs_jobs==1 && q.rs_max_us>=1000);
    ++queueCases;
}

/* ---- job attribution (preemption phase 0, ARCHITECTURE 4.13.5/4.17) ---- */
unsigned attributionCases;
constexpr uint32_t kRender=NativeAdrenoRingSourceRender,kExternal=NativeAdrenoRingSourceExternal,kKernel=NativeAdrenoRingSourceKernel;

/* Deterministic times (host absolute time is ns): each job's service time (from the later of its stamp and the previous completion),
 * the external service time ahead of each render-server job (fw, without what the oldest external job had run by its stamp), n4,
 * the external jobs and their busy time; several completions under one interrupt; kernel work ahead; more than 63 jobs ahead and a
 * stampless job ahead (fw unknown); a recovery's skip; context entries; and the polled upper bound. */
void queueAttribution()
{
    Rig *rig=fresh();
    A830Fence &f=rig->fence;
    const uint64_t t0=uint64_t(1)<<41,ms=1000000;
    auto at=[&](uint32_t done,uint64_t when) {rig->memptrs[1]=done;f.queueCompleted(when,false);};
    Adreno830StatusQueue q{};
    /* Interleaved: E1 runs 0-10 ms; R1 stamped at 2 waits behind it (fw 8: E1's 2 ms before R1's stamp delayed nothing) and runs
     * 10-13; R2 stamped at 14 with nothing ahead (fw 0) runs 14-16; E2, R3, E3, R4 stamped at 20-23 run 20-25, 25-26, 26-30, 30-31:
     * R3 had E2 ahead (5 ms, 1 of them before its stamp: fw 4, not above 4), R4 had E2, R3 and E3 (9 ms, 3 before: fw 6). */
    f.queueSubmitted(1,t0,kExternal);f.queueSubmitted(2,t0+2*ms,kRender);
    at(1,t0+10*ms);at(2,t0+13*ms);
    f.queueSubmitted(3,t0+14*ms,kRender);
    at(3,t0+16*ms);
    f.queueSubmitted(4,t0+20*ms,kExternal);f.queueSubmitted(5,t0+21*ms,kRender);
    f.queueSubmitted(6,t0+22*ms,kExternal);f.queueSubmitted(7,t0+23*ms,kRender);
    at(4,t0+25*ms);at(5,t0+26*ms);at(6,t0+30*ms);at(7,t0+31*ms);
    f.takeQueueAt(&q,t0+32*ms);
    EXPECT(q.submitted==7 && q.completed==7 && q.missed==0 && q.depth==4 && !q.polled);
    EXPECT(q.avg_us==7000 && q.max_us==11000 && q.slow==0);                        /* the latencies, as before */
    EXPECT(q.rs_jobs==4 && q.rs_avg_us==1750 && q.rs_max_us==3000);                /* services 3, 2, 1, 1 */
    EXPECT(q.fw_jobs==4 && q.fw_avg_us==4500 && q.fw_max_us==8000 && q.fw_over4==2);  /* fw 8, 0, 4, 6 */
    EXPECT(q.ext_jobs==3 && q.ext_busy_us==19000 && q.contexts==0);                /* 10 + 5 + 4 */
    f.takeQueueAt(&q,t0+33*ms);
    EXPECT(!q.rs_jobs && !q.rs_max_us && !q.fw_jobs && !q.fw_max_us && !q.fw_over4 && !q.ext_jobs && !q.ext_busy_us);
    ++attributionCases;
    /* One interrupt settles three: the first of them takes the whole time (E 9 ms), the others none; each render-server job's fw is
     * E's service after its own stamp (8 and 7.5 ms: upper bounds, both above 4). */
    f.queueSubmitted(8,t0+40*ms,kExternal);f.queueSubmitted(9,t0+41*ms,kRender);f.queueSubmitted(10,t0+41*ms+ms/2,kRender);
    at(10,t0+49*ms);
    f.takeQueueAt(&q,t0+50*ms);
    EXPECT(q.completed==3 && q.rs_jobs==2 && q.rs_avg_us==0 && q.rs_max_us==0 && q.ext_jobs==1 && q.ext_busy_us==9000);
    EXPECT(q.fw_jobs==2 && q.fw_avg_us==7750 && q.fw_max_us==8000 && q.fw_over4==2);
    ++attributionCases;
    /* The kernel's own work ahead is not external: fw 0, known. */
    f.queueSubmitted(11,t0+50*ms,kKernel);f.queueSubmitted(12,t0+51*ms,kRender);
    at(11,t0+56*ms);at(12,t0+57*ms);
    f.takeQueueAt(&q,t0+58*ms);
    EXPECT(q.completed==2 && q.rs_jobs==1 && q.rs_max_us==1000 && q.fw_jobs==1 && q.fw_max_us==0 && !q.fw_over4 && !q.ext_jobs);
    ++attributionCases;
    /* 68 external jobs, then a render-server job 69 deep: one pass over 69 completions measures the newest 64 (5 missed) and cannot
     * know that job's fw (its oldest job ahead is beyond the 64). */
    for(uint32_t seq=13;seq<=80;++seq)f.queueSubmitted(seq,t0+60*ms,kExternal);
    f.queueSubmitted(81,t0+61*ms,kRender);
    at(81,t0+70*ms);
    f.takeQueueAt(&q,t0+71*ms);
    EXPECT(q.submitted==69 && q.depth==69 && q.completed==64 && q.missed==5 && q.ext_jobs==63 && q.rs_jobs==1 && q.fw_jobs==0);
    ++attributionCases;
    /* The stamp ring wraps: E 82 and R 83 (one ahead), then 63 kernel jobs, the last one's stamp in E's slot. E completes (at 85)
     * without its stamp (missed), so R's fw is unknown, while R itself is measured (85-90). */
    f.queueSubmitted(82,t0+80*ms,kExternal);f.queueSubmitted(83,t0+81*ms,kRender);
    for(uint32_t seq=84;seq<=146;++seq)f.queueSubmitted(seq,t0+82*ms,kKernel);
    at(82,t0+85*ms);at(83,t0+90*ms);
    f.takeQueueAt(&q,t0+91*ms);
    EXPECT(q.completed==1 && q.missed==1 && q.rs_jobs==1 && q.rs_max_us==5000 && q.fw_jobs==0 && q.ext_jobs==0);
    at(146,t0+95*ms);
    f.takeQueueAt(&q,t0+96*ms);
    EXPECT(q.completed==63 && q.missed==0 && !q.rs_jobs && !q.ext_jobs);
    ++attributionCases;
    /* A recovery's skip over E 147: R 148's job ahead was never accounted, so its fw is unknown; its service runs from its stamp. */
    f.queueSubmitted(147,t0+100*ms,kExternal);f.queueSubmitted(148,t0+101*ms,kRender);
    f.queueSkip(147);
    EXPECT(f.queueSeen()==147);
    at(148,t0+110*ms);
    f.takeQueueAt(&q,t0+111*ms);
    EXPECT(q.completed==1 && q.rs_jobs==1 && q.rs_max_us==9000 && q.fw_jobs==0 && f.queueSeen()==148);
    ++attributionCases;
    /* Context entries (client switches) are counted per period, whatever the source. */
    f.queueSubmitted(149,t0+120*ms,kRender|NativeAdrenoRingStampContext);
    f.queueSubmitted(150,t0+121*ms,kExternal|NativeAdrenoRingStampContext);
    f.queueSubmitted(151,t0+122*ms,kExternal);
    at(151,t0+130*ms);
    f.takeQueueAt(&q,t0+131*ms);
    EXPECT(q.submitted==3 && q.contexts==2 && q.rs_jobs==1 && q.ext_jobs==2 && q.fw_jobs==1 && q.fw_max_us==0);
    f.takeQueueAt(&q,t0+132*ms);
    EXPECT(q.contexts==0);
    ++attributionCases;
    /* Without the fence interrupt the DCVS thread's take sees both at once, 50 ms on: E takes it all, R's fw is 49 ms. Upper bounds,
     * flagged polled. */
    Rig *polled=fresh(false);
    A830Fence &p=polled->fence;
    const uint64_t r=a830Now();
    p.queueSubmitted(1,r,kExternal);p.queueSubmitted(2,r+ms,kRender);
    polled->memptrs[1]=2;
    p.takeQueueAt(&q,r+50*ms);
    EXPECT(q.polled && q.completed==2 && q.ext_busy_us==50000 && q.rs_max_us==0 && q.fw_jobs==1 && q.fw_max_us==49000 && q.fw_over4==1);
    ++attributionCases;
}

/* Three accountants race over 20000 completions of alternating render-server and external jobs: each completion is accounted once,
 * by kind, and the attribution never claims more than it measured. (A period's fields are taken one by one, so a pass between two
 * of them may land in the next period: the counts hold over all periods.) */
void queueAttributionRacing()
{
    Rig *rig=fresh(false);
    A830Fence &f=rig->fence;
    constexpr uint32_t n=20000;
    std::atomic<bool> done{false};
    auto account=[&] {while(!done.load(std::memory_order_acquire))f.queueCompleted(a830Now(),false);};
    std::thread a(account),b(account);
    uint64_t completed=0,missed=0,rs=0,ext=0,fw=0,over=0;
    Adreno830StatusQueue q{};
    auto take=[&] {
        f.takeQueue(&q);
        completed+=q.completed;missed+=q.missed;rs+=q.rs_jobs;ext+=q.ext_jobs;fw+=q.fw_jobs;over+=q.fw_over4;
    };
    for(uint32_t seq=1;seq<=n;++seq) {
        f.queueSubmitted(seq,a830Now(),(seq&1)?kExternal:kRender);
        __atomic_store_n(&rig->memptrs[1],seq,__ATOMIC_RELEASE);
        f.queueCompleted(a830Now(),false);   /* the CP side's interrupt */
        if(seq%997==0)take();
    }
    done.store(true,std::memory_order_release);
    a.join();b.join();
    const uint32_t seen=f.queueSeen();   /* before the take accounts anything itself */
    take();
    EXPECT(seen==n && completed+missed==n && rs+ext==completed && fw<=rs && over<=fw && f.queueSeen()==n);
    ++attributionCases;
}

/* The edges of the attribution. Several completions in one pass: the first takes the whole time, so with the render server's job first
 * the external job behind it counts none and the fw of the render-server job behind that is low (known, 0 here for 23 ms), while the
 * same jobs seen one by one (the fence interrupt) give 23 ms; that is why the phase 0 gate reads only periods without polling (4.17).
 * A caller's clock older than the last pass (a waiter that read it before an interrupt's pass) ends at that pass: no negative service.
 * A ring 128 or more deep saturates the stamp's depth, so the fw of that job stays unknown (a wrapped depth would claim it known). */
void queueAttributionEdges()
{
    const uint64_t t0=uint64_t(1)<<41,ms=1000000;
    Adreno830StatusQueue q{};
    for(const bool together:{true,false}) {
        Rig *rig=fresh();
        A830Fence &f=rig->fence;
        /* R0 runs 0-1 ms, E1 1-24 ms, R1 24-25 ms (stamped at 0, 0.1 and 0.2 ms). */
        f.queueSubmitted(1,t0,kRender);f.queueSubmitted(2,t0+ms/10,kExternal);f.queueSubmitted(3,t0+ms/5,kRender);
        if(together) {rig->memptrs[1]=3;f.queueCompleted(t0+25*ms,false);}
        else {
            rig->memptrs[1]=1;f.queueCompleted(t0+ms,false);
            rig->memptrs[1]=2;f.queueCompleted(t0+24*ms,false);
            rig->memptrs[1]=3;f.queueCompleted(t0+25*ms,false);
        }
        f.takeQueueAt(&q,t0+26*ms);
        if(together)EXPECT(q.rs_jobs==2 && q.rs_max_us==25000 && q.ext_jobs==1 && q.ext_busy_us==0 && q.fw_jobs==2 && q.fw_max_us==0 &&
                           q.fw_over4==0 && q.max_us==25000);
        else EXPECT(q.rs_jobs==2 && q.rs_max_us==1000 && q.ext_jobs==1 && q.ext_busy_us==23000 && q.fw_jobs==2 && q.fw_max_us==23000 &&
                    q.fw_over4==1);
        ++attributionCases;
    }
    {
        Rig *rig=fresh();
        A830Fence &f=rig->fence;
        /* E stamped at 0 completes at 10 ms; R stamped at 1 ms completes next, seen with a clock read at 5 ms: it ends at 10 ms with
         * no service, its fw is E's 10 ms without the 1 ms E ran before R's stamp, and the next job starts from 10 ms. */
        f.queueSubmitted(1,t0,kExternal);f.queueSubmitted(2,t0+ms,kRender);
        rig->memptrs[1]=1;f.queueCompleted(t0+10*ms,false);
        rig->memptrs[1]=2;f.queueCompleted(t0+5*ms,false);
        f.queueSubmitted(3,t0+8*ms,kRender);
        rig->memptrs[1]=3;f.queueCompleted(t0+12*ms,false);
        f.takeQueueAt(&q,t0+13*ms);
        EXPECT(q.completed==3 && q.rs_jobs==2 && q.rs_max_us==2000 && q.rs_avg_us==1000 && q.fw_jobs==2 && q.fw_max_us==9000 &&
               q.ext_jobs==1 && q.ext_busy_us==10000);
        ++attributionCases;
    }
    {
        Rig *rig=fresh();
        A830Fence &f=rig->fence;
        /* 130 external jobs, then a render-server job 131 deep: one pass measures the newest 64. */
        for(uint32_t seq=1;seq<=130;++seq)f.queueSubmitted(seq,t0,kExternal);
        f.queueSubmitted(131,t0+ms,kRender);
        rig->memptrs[1]=131;f.queueCompleted(t0+10*ms,false);
        f.takeQueueAt(&q,t0+11*ms);
        EXPECT(q.submitted==131 && q.depth==131 && q.completed==64 && q.missed==67 && q.rs_jobs==1 && q.fw_jobs==0);
        ++attributionCases;
    }
}

void retireKickWhileIrqOff()
{
    a830HostManualCalls(true);
    Rig *rig=fresh();
    rig->gpu.stuck=A8XX_RBBM_INT_0_UCHE_TRAP;
    gGic.deliver(A830_IRQ_GPU);
    EXPECT(rig->irq.off());
    a830HostRunCalls(true);
    sink.retires=0;
    rig->fence.submitted(1);
    rig->memptrs[1]=1;   /* completed while 332 is off: no interrupt kicks retirement */
    EXPECT(rig->fence.waitNs(1,0)==kIOReturnSuccess);
    a830HostRunCalls(true);
    EXPECT(sink.retires==1 && !rig->fence.retirePending());
    a830HostManualCalls(false);
}
}

int main()
{
    basics();
    racingWakeups();
    noMmioWhileWaiting();
    lateWithoutInterrupt();
    pollingWithoutInterrupt();
    interruptedReturnsTimeout();
    closingAndLost();
    recoveryCompletesWaiters();
    watchdogHang();
    watchdogProgress();
    watchdogWithoutIrq();
    watchdogThreaded();
    retireKickWhileIrqOff();
    queueMeter();
    queueRacing();
    queueRecheck();
    queueAttribution();
    queueAttributionRacing();
    queueAttributionEdges();
    a830HostRunCalls(true);
    a830HostShutdown();
    std::printf("PASS: job attribution: %u cases (service from the later of stamp and previous completion, external time ahead of "
                "each render-server job without what the oldest had run by its stamp, n4 above 4 ms, external jobs and busy time, "
                "several completions under one interrupt (fw high with the external job first, low behind the render server's), kernel "
                "work ahead, more than 63 ahead, 128 or more ahead and a stampless job ahead unknown, a skip, context entries, polled "
                "periods, a caller's clock older than the last pass, 20000 completions by kind raced by three accountants)\n",
                attributionCases);
    std::printf("PASS: queue meter: %u cases (stamps and ring depth, completions accounted once from the top half, a waiter "
                "or the polled take, average/maximum/16 ms counts per period, 40-bit stamp clock, 64 stamps with missed "
                "ones counted, recovery skips, the accountant's second pass with a fresh clock for a completion that came during "
                "its pass, 20000 completions raced by three accountants with nobody calling after the last)\n",queueCases);
    std::printf("PASS: A830Fence %u checks: 3000 racing interrupt wakeups without a lost one, timeout, interrupted -> Timeout, "
                "closing -> Aborted, lost -> NotResponding, no MMIO while waiting, watchdog hang/progress/disarm, recovery\n",checks);
    return 0;
}
