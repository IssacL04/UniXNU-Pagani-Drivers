/* SPDX-License-Identifier: MIT */
/* host-test: A830SelfTest.cpp A830Irq.cpp A830Fence.cpp Adreno830Status.cpp tests/A830HostSched.cpp */
/* The S8 checks (ARCHITECTURE 3.4 S8, 4.14.2, 8.4; PLAN M6/M10) against a fake CP that executes the PM4 streams on its own
 * thread and raises the fence interrupt through the fake GIC, over a memory model with separate CPU-cache and DRAM copies of
 * each probe page (coherent reads snoop the cache, coherent writes update it, a non-coherent table walk faults). Covered: the
 * default sequence (CP_MEM_WRITE + IRQ fence, V4, V9a; V9b/V9c not run), the cohtest sequence, every failure reason of S8
 * (19 wiring, 5 timeout, 6 readback, 18 hang), the M10 recovery of a V9b/V9c hang, the S8 page words and coh_limit, the VM
 * policy, and the probe streams word by word. */
#include "A830FakeGpu.h"
#include "A830HostSched.h"
#include "A830SelfTest.h"
#include "AdrenoPM4.h"
#include "Adreno830Status.h"
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <thread>

namespace {
unsigned checks;
#define EXPECT(x) do {CHECK(x);++checks;} while(0)

/* register offsets the V9c stream programs, spelled independently of A830Regs.h section 21 */
constexpr uint32_t kCcuCacheCntl=0x088E5,kRbBlt=0x08C00,kRbPixel=0x08C01,kGrasBlt=0x08500,kDestInfo=0x08C17,kDestBase=0x08C18,
                   kDestPitch=0x08C1A,kClear0=0x08C2C,kDestTl=0x08505,kDestBr=0x08506,kSpOut=0x0A9BF,kTpl1Blt=0x0B2D2;
constexpr uint32_t kCpBlit=0x2C,kWaitMemWrites=0x12;

struct Page {
    alignas(64) uint32_t cache[1024];
    alignas(64) uint32_t dram[1024];
    unsigned mode=0;
    uint64_t iova=0;
    bool mapped=false;
};

struct World;
struct FakeCp {
    World *world=nullptr;
    std::mutex m;
    std::condition_variable cv;
    struct Job {std::vector<uint32_t> words;uint32_t seq;};
    std::deque<Job> queue;
    std::thread worker;
    bool stop=false;
    uint32_t seq=0;
    std::vector<std::vector<uint32_t>> streams;   /* every stream submitted */
    std::map<uint32_t,uint32_t> regs;
    std::vector<uint32_t> events;
    unsigned blits=0,unknown=0;
};

struct World {
    FakeGpu gpu;
    uint32_t memptrs[1024]{};
    alignas(64) uint32_t scratch[1024]{};
    A830Fence fence;
    A830Irq irq;
    FakeCp cp;
    std::vector<Page *> pages;
    uint64_t next_iova=0x30000000;
    /* the coherence model */
    bool coherent_reads=true,coherent_writes=true,coherent_walk=true;
    bool hang_all=false,no_irq=false,corrupt_scratch=false,hang_coherent_blit=false,hang_coherent_write=false;
    uint32_t pending_fsr=0;uint64_t pending_far=0;
    unsigned policies=0,recoveries=0,allocated=0,released=0,unmaps=0;
    A830CoherenceResult policy{};

    Page *pageAt(uint64_t iova)
    {
        for(Page *p:pages)if(p->mapped && iova>=p->iova && iova<p->iova+4096)return p;
        return nullptr;
    }
    bool coherent(const Page *p) const {return p->mode==A830SelfTestMapCoherentRo || p->mode==A830SelfTestMapCoherentRw;}
    bool readGpu(uint64_t iova,uint32_t *value)
    {
        if(iova>=A830_GPU_VA_SCRATCH && iova<A830_GPU_VA_SCRATCH+4096) {*value=__atomic_load_n(&scratch[(iova-A830_GPU_VA_SCRATCH)/4],__ATOMIC_ACQUIRE);return true;}
        Page *p=pageAt(iova);
        if(!p)return false;
        const unsigned i=unsigned((iova-p->iova)/4);
        if(p->mode==A830SelfTestMapCachedTable && !coherent_walk) {pending_fsr|=0x2;pending_far=iova;*value=0;return true;}
        *value=coherent(p) && coherent_reads?p->cache[i]:p->dram[i];
        return true;
    }
    bool writeGpu(uint64_t iova,uint32_t value,bool *coherent_target)
    {
        if(iova>=A830_GPU_VA_SCRATCH && iova<A830_GPU_VA_SCRATCH+4096) {
            __atomic_store_n(&scratch[(iova-A830_GPU_VA_SCRATCH)/4],corrupt_scratch?value^1:value,__ATOMIC_RELEASE);
            return true;
        }
        Page *p=pageAt(iova);
        if(!p)return false;
        const unsigned i=unsigned((iova-p->iova)/4);
        p->dram[i]=value;
        if(coherent(p)) {*coherent_target=true;if(coherent_writes)p->cache[i]=value;}
        return true;
    }
    /* One stream: decode with the kit, apply. Returns false when the job hangs. */
    bool execute(const std::vector<uint32_t> &words)
    {
        AdrenoPM4Packet packets[512];
        size_t count=0;
        CHECK(AdrenoPM4Plan(words.data(),words.size(),packets,512,&count)==AdrenoPM4Success);
        bool coherent_write=false,coherent_blit=false;
        for(size_t k=0;k<count;++k) {
            const AdrenoPM4Packet &p=packets[k];
            const uint32_t *v=&words[p.header_word+1];
            if(p.type==4) {for(uint32_t i=0;i<p.payload_words;++i)cp.regs[p.identifier+i]=v[i];continue;}
            switch(p.identifier) {
            case A8XX_PM4_CP_MEM_WRITE: {
                const uint64_t addr=v[0]|uint64_t(v[1])<<32;
                for(uint32_t i=2;i<p.payload_words;++i)CHECK(writeGpu(addr+4*(i-2),v[i],&coherent_write));
                break;
            }
            case A8XX_PM4_CP_MEM_TO_MEM: {
                CHECK(p.payload_words==5 && v[0]==0);
                uint32_t value=0;
                CHECK(readGpu(v[3]|uint64_t(v[4])<<32,&value));
                CHECK(writeGpu(v[1]|uint64_t(v[2])<<32,value,&coherent_write));
                break;
            }
            case A8XX_PM4_CP_EVENT_WRITE7: cp.events.push_back(v[0]);break;
            case kCpBlit: {
                CHECK(p.payload_words==1 && v[0]==3);
                ++cp.blits;
                /* the 2D engine: solid R32_UINT fill of [TL.x, BR.x] on row 0 at the destination base */
                CHECK((cp.regs[kRbBlt]&0x80) && ((cp.regs[kRbBlt]>>8)&0xFF)==0x4B && (cp.regs[kDestInfo]&0xFF)==0x4B);
                CHECK(cp.regs[kGrasBlt]==cp.regs[kRbBlt] && cp.regs[kCcuCacheCntl]==0x087009F0u);
                const uint64_t base=cp.regs[kDestBase]|uint64_t(cp.regs[kDestBase+1])<<32;
                const uint32_t x0=cp.regs[kDestTl]&0x3FFF,x1=cp.regs[kDestBr]&0x3FFF;
                for(uint32_t x=x0;x<=x1;++x)CHECK(writeGpu(base+4*x,cp.regs[kClear0],&coherent_blit));
                break;
            }
            case A8XX_PM4_CP_THREAD_CONTROL: case A8XX_PM4_CP_WAIT_FOR_IDLE: case kWaitMemWrites: break;
            default: ++cp.unknown;break;
            }
        }
        if(hang_all)return false;
        if(hang_coherent_blit && coherent_blit)return false;
        if(hang_coherent_write && coherent_write)return false;
        return true;
    }
};

void cpWork(World *w)
{
    std::unique_lock<std::mutex> g(w->cp.m);
    for(;;) {
        w->cp.cv.wait(g,[w] {return w->cp.stop || !w->cp.queue.empty();});
        if(w->cp.stop)return;
        FakeCp::Job job=w->cp.queue.front();
        w->cp.queue.pop_front();
        g.unlock();
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        if(w->execute(job.words)) {
            __atomic_store_n(&w->memptrs[1],job.seq,__ATOMIC_RELEASE);
            if(!w->no_irq) {w->gpu.raise(A8XX_RBBM_INT_0_CP_CACHE_FLUSH_TS);gGic.deliver(A830_IRQ_GPU);}
        }
        g.lock();
    }
}

IOReturn submit(void *ring,const uint32_t *words,size_t count,uint32_t *fence)
{
    World *w=static_cast<World *>(ring);
    std::lock_guard<std::mutex> g(w->cp.m);
    const uint32_t seq=++w->cp.seq;
    w->cp.streams.emplace_back(words,words+count);
    w->cp.queue.push_back({std::vector<uint32_t>(words,words+count),seq});
    w->cp.cv.notify_all();
    *fence=seq;
    return kIOReturnSuccess;
}

A830SelfTestVm vm(World *w)
{
    A830SelfTestVm v{};
    v.cookie=w;
    v.allocate=[](void *c,A830SelfTestPage *page) {
        World *w=static_cast<World *>(c);
        Page *p=new Page;
        std::memset(p->cache,0,sizeof p->cache);std::memset(p->dram,0,sizeof p->dram);
        w->pages.push_back(p);++w->allocated;
        page->cpu=p->cache;page->physical=0x900000000ull+w->allocated*0x1000;page->owner=p;
        return kIOReturnSuccess;
    };
    v.release=[](void *c,A830SelfTestPage *page) {
        World *w=static_cast<World *>(c);
        Page *p=static_cast<Page *>(page->owner);
        w->pages.erase(std::find(w->pages.begin(),w->pages.end(),p));
        delete p;++w->released;
    };
    v.map=[](void *c,A830SelfTestPage *page,unsigned mode) {
        World *w=static_cast<World *>(c);
        Page *p=static_cast<Page *>(page->owner);
        p->mode=mode;p->iova=w->next_iova;p->mapped=true;w->next_iova+=0x1000;
        page->iova=p->iova;page->handle=p->iova>>12;
        return kIOReturnSuccess;
    };
    v.unmap=[](void *c,A830SelfTestPage *page) {
        ++static_cast<World *>(c)->unmaps;
        static_cast<Page *>(page->owner)->mapped=false;
        return kIOReturnSuccess;
    };
    v.maintain=[](void *,A830SelfTestPage *page,unsigned op) {
        Page *p=static_cast<Page *>(page->owner);
        if(op==A830SelfTestClean || op==A830SelfTestCleanInvalidate)std::memcpy(p->dram,p->cache,sizeof p->dram);
        if(op==A830SelfTestInvalidate || op==A830SelfTestCleanInvalidate)std::memcpy(p->cache,p->dram,sizeof p->cache);
    };
    v.faults=[](void *c,uint64_t *far) {
        World *w=static_cast<World *>(c);
        const uint32_t fsr=w->pending_fsr;
        *far=w->pending_far;w->pending_fsr=0;
        return fsr;
    };
    v.policy=[](void *c,const A830CoherenceResult *r) {World *w=static_cast<World *>(c);++w->policies;w->policy=*r;};
    return v;
}

IOReturn recover(void *c,uint16_t reason)
{
    World *w=static_cast<World *>(c);
    CHECK(reason==A830ReasonHang);
    ++w->recoveries;
    w->hang_all=w->hang_coherent_blit=w->hang_coherent_write=false;
    w->fence.forceComplete(w->cp.seq);
    return kIOReturnSuccess;
}

uint64_t aoCounter(void *) {return 0x123456789ull;}

/* Everything of one S8 run lives for the whole program (thread calls and the CP worker may still reference it). */
World *previousWorld;
World *world(bool cohtest=false,bool with_vm=true,bool with_recover=false,uint32_t coherent=0,bool coherent_given=false)
{
    if(previousWorld)previousWorld->fence.stopWatchdog();
    a830HostCallsIdle(1000);
    gGic.reset();
    adreno830_status_reset();
    World *w=new World;
    w->gpu.mask=A8XX_INT_MASK;
    w->gpu.set(A8XX_CP_ALWAYS_ON_COUNTER,0x89ABCDEF);w->gpu.set(A8XX_CP_ALWAYS_ON_COUNTER_HI,0x12);
    gGic.level=[w](uint32_t) {return w->gpu.asserted();};
    A830FenceConfig fc{};
    fc.memptrs=w->memptrs;fc.irq=&w->irq;
    CHECK(w->fence.start(fc)==kIOReturnSuccess);
    A830IrqConfig ic{};
    ic.gpu=w->gpu.mmio();ic.fence=&w->fence;
    CHECK(w->irq.start(ic)==kIOReturnSuccess);
    w->cp.world=w;
    w->cp.worker=std::thread(cpWork,w);
    previousWorld=w;
    (void)cohtest;(void)with_vm;(void)with_recover;(void)coherent;(void)coherent_given;
    return w;
}

A830SelfTestEnv env(World *w,bool cohtest=false,bool with_vm=true,bool with_recover=false,uint32_t coherent=0,bool given=false)
{
    A830SelfTestEnv e{};
    e.irq=&w->irq;e.fence=&w->fence;e.ring=w;e.submit=submit;
    e.scratch=w->scratch;e.scratch_iova=A830_GPU_VA_SCRATCH;
    if(with_vm)e.vm=vm(w);
    e.timer=w;e.ao_counter=aoCounter;
    if(with_recover) {e.recovery=w;e.recover=recover;}
    e.coherent=coherent;e.coherent_given=given;e.cohtest=cohtest;
    return e;
}

void finish(World *w)
{
    {std::lock_guard<std::mutex> g(w->cp.m);w->cp.stop=true;w->cp.cv.notify_all();}
    w->cp.worker.join();
    CHECK(w->cp.unknown==0);
}

void defaultSequence()
{
    World *w=world();
    A830SelfTest test(env(w));
    A830SelfTestReport r{};A830SelfTestFailure f{};
    EXPECT(test.run(A830_IRQ_GPU,&r,&f)==kIOReturnSuccess);
    EXPECT(f.step==0 && f.reason==0);
    EXPECT(r.attach_result==kIOReturnSuccess && r.readback==A830IrqReadbackLevel && gGic.enabled(A830_IRQ_GPU));
    EXPECT(r.scratch==A830SelfTestScratchValue && r.fence==1 && r.fence_irqs>=1 && r.irq_count>=1);
    EXPECT(r.cp_counter==0x1289ABCDEFull && r.ao_counter==0x123456789ull);
    EXPECT(r.coherence.v4==A830CohPass && r.coherence.v9a==A830CohPass && r.coherence.v9a_rounds==A830SelfTestV9aRounds);
    EXPECT(r.coherence.v9b==A830CohNotRun && r.coherence.v9c==A830CohNotRun);   /* the default image never runs them */
    EXPECT(r.coherence.limit==A830CoherenceReadOnly && r.coherence.level==A830CoherenceReadOnly && r.coherence.tables_cacheable);
    /* the S8 page (Adreno830Status.h) */
    const Adreno830Status &s=adreno830_status;
    EXPECT((s.aux[0]&0xFFFF)==A830IrqReadbackLevel && (s.aux[0]>>32)==r.wake_us && s.aux[1]==r.irq_count);
    EXPECT(s.aux[2]==(A830CohPass|A830CohPass<<2) && s.aux[3]==0x1289ABCDEFull && s.aux[4]==0x123456789ull);
    EXPECT(s.aux[5]==(A830SelfTestScratchValue|uint64_t(1)<<32) && s.coh_limit==1);
    EXPECT(w->policies==1 && w->policy.level==1 && w->policy.tables_cacheable);
    EXPECT(std::memcmp(&a830_selftest_last,&r,sizeof r)==0);
    /* the first stream is exactly the CP_MEM_WRITE (the ring adds its own fence) */
    uint32_t mw[4];
    EXPECT(A830SelfTest::memWriteWords(A830_GPU_VA_SCRATCH,A830SelfTestScratchValue,mw,4)==4);
    EXPECT(w->cp.streams[0]==std::vector<uint32_t>(mw,mw+4));
    /* V4 + 8 V9a rounds, one probe page each, all returned */
    EXPECT(w->cp.streams.size()==1+1+A830SelfTestV9aRounds && w->allocated==2 && w->released==2 && w->unmaps==2);
    /* every V9a round starts with a UCHE invalidate */
    unsigned invalidates=0;
    for(uint32_t e:w->cp.events)invalidates+=e==A8XX_EVENT_CACHE_INVALIDATE7;
    EXPECT(invalidates==A830SelfTestV9aRounds);
    finish(w);
}

void cohtestSequence()
{
    World *w=world();
    A830SelfTest test(env(w,true,true,false,1,true));
    A830SelfTestReport r{};A830SelfTestFailure f{};
    EXPECT(test.run(A830_IRQ_GPU,&r,&f)==kIOReturnSuccess);
    EXPECT(r.coherence.v9b==A830CohPass && r.coherence.v9c==A830CohPass && r.coherence.limit==A830CoherenceFull);
    EXPECT(r.coherence.level==1);   /* pagani-gpu-coherent=1 given: lowered */
    EXPECT(adreno830_status.aux[2]==0x55 && adreno830_status.coh_limit==2 && w->cp.blits==1);
    EXPECT(w->policy.limit==2 && w->policy.level==1);
    finish(w);
    World *full=world();
    A830SelfTest test2(env(full,true));
    EXPECT(test2.run(A830_IRQ_GPU,&r,&f)==kIOReturnSuccess && r.coherence.level==A830CoherenceFull);
    finish(full);
}

void notCoherent()
{
    World *w=world();
    w->coherent_reads=false;w->coherent_walk=false;w->coherent_writes=false;
    A830SelfTest test(env(w,true));
    A830SelfTestReport r{};A830SelfTestFailure f{};
    /* record only: S8 still succeeds */
    EXPECT(test.run(A830_IRQ_GPU,&r,&f)==kIOReturnSuccess);
    EXPECT(r.coherence.v4==A830CohFail && r.coherence.v4_fsr==0x2 && r.coherence.v4_far!=0 && r.coherence.v4_mismatch==16);
    EXPECT(r.coherence.v9a==A830CohFail && r.coherence.v9a_rounds==0);
    EXPECT(r.coherence.v9b==A830CohFail && r.coherence.v9b_mismatch==A830SelfTestV9Lines);
    EXPECT(r.coherence.v9c==A830CohPass);   /* the fill landed (seen after an invalidate) and the clean completed */
    EXPECT(r.coherence.limit==0 && r.coherence.level==0 && !r.coherence.tables_cacheable && adreno830_status.coh_limit==0);
    EXPECT(adreno830_status.aux[2]==(A830CohFail|A830CohFail<<2|A830CohFail<<4|A830CohPass<<6));
    finish(w);
    /* coherent reads only: level 1, V9b fails */
    World *ro=world();
    ro->coherent_writes=false;
    A830SelfTest test2(env(ro,true));
    EXPECT(test2.run(A830_IRQ_GPU,&r,&f)==kIOReturnSuccess);
    EXPECT(r.coherence.v9a==A830CohPass && r.coherence.v9b==A830CohFail && r.coherence.limit==1);
    finish(ro);
}

void interruptNotWired()
{
    /* the fence completes but no interrupt arrives: the waiter only sees it after a 16 ms slice */
    World *w=world();
    w->no_irq=true;
    A830SelfTest test(env(w));
    A830SelfTestReport r{};A830SelfTestFailure f{};
    EXPECT(test.run(A830_IRQ_GPU,&r,&f)!=kIOReturnSuccess);
    EXPECT(f.step==A830S8StepIrq && f.reason==A830ReasonIrqWiring && f.reg==A830_IRQ_GPU && f.expect==f.value+1);
    EXPECT(r.scratch==A830SelfTestScratchValue && r.coherence.limit==0 && adreno830_status.coh_limit==0);
    EXPECT(w->cp.streams.size()==1);   /* nothing after the failed check */
    finish(w);
    /* the GIC refuses the line */
    World *refused=world();
    gGic.attach_result=kIOReturnNoResources;
    A830SelfTest test2(env(refused));
    EXPECT(test2.run(A830_IRQ_GPU,&r,&f)==kIOReturnNoResources);
    EXPECT(f.step==A830S8StepAttach && f.reason==A830ReasonIrqWiring && r.attach_result==kIOReturnNoResources);
    EXPECT(refused->cp.streams.empty() && (adreno830_status.irq_off&A830IrqOffGpu));
    finish(refused);
}

void fenceAndReadbackFailures()
{
    World *w=world();
    w->hang_all=true;
    A830SelfTest test(env(w));
    A830SelfTestReport r{};A830SelfTestFailure f{};
    const auto began=std::chrono::steady_clock::now();
    EXPECT(test.run(A830_IRQ_GPU,&r,&f)==kIOReturnTimeout);
    EXPECT(std::chrono::steady_clock::now()-began>=std::chrono::milliseconds(100));
    EXPECT(f.step==A830S8StepMemWrite && f.reason==A830ReasonTimeout && f.reg==A830_GPU_VA_SCRATCH && f.expect==1 && f.value==0);
    w->fence.stopWatchdog();
    finish(w);
    World *bad=world();
    bad->corrupt_scratch=true;
    A830SelfTest test2(env(bad));
    EXPECT(test2.run(A830_IRQ_GPU,&r,&f)==kIOReturnIOError);
    EXPECT(f.step==A830S8StepReadback && f.reason==A830ReasonUnexpectedState && f.value==(A830SelfTestScratchValue^1) &&
           f.expect==A830SelfTestScratchValue);
    finish(bad);
}

void cohtestHang()
{
    /* M6: a V9c hang stops S8 with reason 18; the ceiling is 1 (V9a passed); nothing is published */
    World *w=world();
    w->hang_coherent_blit=true;
    A830SelfTest test(env(w,true));
    A830SelfTestReport r{};A830SelfTestFailure f{};
    EXPECT(test.run(A830_IRQ_GPU,&r,&f)==kIOReturnTimeout);
    EXPECT(f.step==A830S8StepV9c && f.reason==A830ReasonHang);
    EXPECT(r.coherence.v9b==A830CohPass && r.coherence.v9c==A830CohFail && r.coherence.timed_out && r.coherence.limit==1);
    EXPECT(adreno830_status.coh_limit==1 && w->policies==0 && !r.recovered);
    EXPECT(w->fence.watchdogStats().armed>=1 && a830HostCallsIdle(1000));   /* the watchdog was stopped: no late loss */
    finish(w);
    /* M10: recovered, S8 continues with coherence <= 1 */
    World *m10=world();
    m10->hang_coherent_blit=true;
    A830SelfTest test2(env(m10,true,true,true));
    EXPECT(test2.run(A830_IRQ_GPU,&r,&f)==kIOReturnSuccess);
    EXPECT(r.recovered && m10->recoveries==1 && r.coherence.v9c==A830CohFail && r.coherence.limit==1 && m10->policy.level==1);
    finish(m10);
    /* a V9b hang ends the series: V9c is not run */
    World *v9b=world();
    v9b->hang_coherent_write=true;
    A830SelfTest test3(env(v9b,true,true,true));
    EXPECT(test3.run(A830_IRQ_GPU,&r,&f)==kIOReturnSuccess);
    EXPECT(r.recovered && r.coherence.v9b==A830CohFail && r.coherence.v9c==A830CohNotRun && r.coherence.limit==1);
    EXPECT(v9b->cp.blits==0);
    finish(v9b);
}

void withoutVm()
{
    World *w=world();
    A830SelfTest test(env(w,true,false));
    A830SelfTestReport r{};A830SelfTestFailure f{};
    EXPECT(test.run(A830_IRQ_GPU,&r,&f)==kIOReturnSuccess);
    EXPECT(r.coherence.v4==A830CohNotRun && r.coherence.v9a==A830CohNotRun && r.coherence.limit==0 && adreno830_status.aux[2]==0);
    finish(w);
}

uint32_t regIn(const uint32_t *words,size_t count,uint32_t reg)
{
    AdrenoPM4Packet packets[64];
    size_t n=0;
    CHECK(AdrenoPM4Plan(words,count,packets,64,&n)==AdrenoPM4Success);
    for(size_t k=0;k<n;++k)
        if(packets[k].type==4 && reg>=packets[k].identifier && reg<packets[k].identifier+packets[k].payload_words)
            return words[packets[k].header_word+1+(reg-packets[k].identifier)];
    return 0xFFFFFFFF;
}

void streams()
{
    uint32_t out[512];
    uint32_t header=0;
    /* CP_MEM_WRITE: header, address, value */
    EXPECT(A830SelfTest::memWriteWords(0x1234567890ull,0x55,out,4)==4);
    EXPECT(AdrenoPM4Type7(A8XX_PM4_CP_MEM_WRITE,3,&header)==AdrenoPM4Success && out[0]==header && out[1]==0x34567890 &&
           out[2]==0x12 && out[3]==0x55);
    EXPECT(A830SelfTest::memWriteWords(0,0,out,3)==0);   /* never a partial stream */
    /* CP_MEM_TO_MEM per dword, with and without the UCHE invalidate; CP_WAIT_MEM_WRITES at the end */
    const uint64_t src[2]={0x10000,0x10040},dst[2]={0x302100,0x302104};
    EXPECT(A830SelfTest::copyWords(src,dst,2,false,out,512)==13);
    EXPECT(AdrenoPM4Type7(A8XX_PM4_CP_MEM_TO_MEM,5,&header)==AdrenoPM4Success && out[0]==header && out[1]==0 &&
           out[2]==0x302100 && out[4]==0x10000 && out[8]==0x302104 && out[10]==0x10040);
    EXPECT(A830SelfTest::copyWords(src,dst,2,true,out,512)==15 && out[1]==A8XX_EVENT_CACHE_INVALIDATE7);
    const uint32_t values[2]={7,8};
    EXPECT(A830SelfTest::fillWords(dst,values,2,out,512)==9 && out[3]==7 && out[7]==8);
    /* the 2D fill of V9c */
    const size_t n=A830SelfTest::blitWords(0x30001000,4096,0xC9C9C9C9u,out,512);
    EXPECT(n==38);
    EXPECT(regIn(out,n,kCcuCacheCntl)==0x087009F0u && regIn(out,n,kRbPixel)==0);
    EXPECT(regIn(out,n,kRbBlt)==0x07F04B80u && regIn(out,n,kGrasBlt)==0x07F04B80u);
    EXPECT(regIn(out,n,kTpl1Blt)==0x20000000u && regIn(out,n,kSpOut)==0xF25Cu);
    EXPECT(regIn(out,n,kClear0)==0xC9C9C9C9u && regIn(out,n,kClear0+1)==0);
    EXPECT(regIn(out,n,kDestInfo)==0x4B && regIn(out,n,kDestBase)==0x30001000 && regIn(out,n,kDestBase+1)==0 &&
           regIn(out,n,kDestPitch)==0);
    EXPECT(regIn(out,n,kDestTl)==0 && regIn(out,n,kDestBr)==1023);
    EXPECT(AdrenoPM4Type7(A8XX_PM4_CP_THREAD_CONTROL,1,&header)==AdrenoPM4Success && out[0]==header && out[1]==0x80000001u);
    EXPECT(out[n-1]==0x21);   /* CCU_CLEAN_COLOR closes the pass; the ring's CACHE_CLEAN fence follows */
    EXPECT(A830SelfTest::blitWords(0x30001010,4096,0,out,512)==0 && A830SelfTest::blitWords(0x30001000,4096,0,out,37)==0);
    EXPECT(A830SelfTest::blitWords(0x30001000,0x10004*4,0,out,512)==0);
}

void levels()
{
    A830CoherenceResult r{};
    EXPECT(A830SelfTest::limitOf(r)==0);
    r.v9a=A830CohPass;
    EXPECT(A830SelfTest::limitOf(r)==1);
    r.v9b=A830CohPass;
    EXPECT(A830SelfTest::limitOf(r)==1);
    r.v9c=A830CohPass;
    EXPECT(A830SelfTest::limitOf(r)==2);
    EXPECT(A830SelfTest::levelOf(r,0,true)==0 && A830SelfTest::levelOf(r,1,true)==1 && A830SelfTest::levelOf(r,2,true)==2);
    EXPECT(A830SelfTest::levelOf(r,0,false)==2);   /* not given: the tests decide */
    r.timed_out=true;
    EXPECT(A830SelfTest::limitOf(r)==1);
    r={};r.v9b=r.v9c=A830CohPass;
    EXPECT(A830SelfTest::limitOf(r)==0 && A830SelfTest::levelOf(r,2,true)==0);   /* an explicit 2 cannot exceed the tests */
}
}

int main()
{
    streams();
    levels();
    defaultSequence();
    cohtestSequence();
    notCoherent();
    interruptNotWired();
    fenceAndReadbackFailures();
    cohtestHang();
    withoutVm();
    a830HostCallsIdle(2000);
    a830HostShutdown();
    std::printf("PASS: A830SelfTest %u checks: S8 default (CP_MEM_WRITE + IRQ fence, V4, V9a) and cohtest (V9b, V9c) sequences, "
                "reasons 19/5/6/18, M10 recovery of a probe hang, S8 page and coh_limit, VM policy, probe streams\n",checks);
    return 0;
}
