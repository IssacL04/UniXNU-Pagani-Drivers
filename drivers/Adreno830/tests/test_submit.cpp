/* SPDX-License-Identifier: MIT */
/* Real VM, ring and execbuf code over two software register peers (ARCHITECTURE 5.4, IMPLEMENTATION_PLAN 2.1 test_submit).
 * host-test: NativeAdrenoMemory.cpp NativeAdrenoSmmuMemory.cpp NativeAdrenoVm.cpp NativeAdrenoRing.cpp NativeAdrenoSubmit.cpp */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoSubmit.h"
#include <cstdio>
#include <cstring>
#include <vector>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
#define SMMU_BYTES 0x40000
#define CB 0x22000
IOReturn NativeAdrenoQueryCpu4K(NativeAdrenoCpuState *out)
{CHECK(out);*out={};return kIOReturnSuccess;}
IOReturn NativeAdrenoMaintainOwnedPages(void *mapping,size_t bytes,NativeAdrenoCacheOperation op)
{
    CHECK(mapping && bytes && !(((uintptr_t)mapping|bytes)&4095));
    CHECK(op==NativeAdrenoClean || op==NativeAdrenoCleanInvalidate || op==NativeAdrenoInvalidate);
    ++fixture.cache_calls;
    return fixture.cache_calls==fixture.cache_fail_call?kIOReturnIOError:kIOReturnSuccess;
}
/* SMMUv2 register peer: 32- and 64-bit accesses (the kit writes TLBIVA and the 64-bit context registers whole). */
static uint32_t mmio[SMMU_BYTES/4];
static struct {unsigned calls,fail_call,after;} mf;
static ArmSmmu4KIo smmuio;
static ArmSmmu4KStreamState streams;
static ArmSmmu4KStream routes[2]={{2,0},{4,1}};
static ArmSmmu4KStreamResult route_result;
static ArmSmmu4KContextConfig context;
static ArmSmmu4KContextImage context_image;
static int mevent(void){return ++mf.calls==mf.fail_call;}
static int mread32(void *c,size_t at,uint32_t *v)
{CHECK(c==&mf && at%4==0 && at<=SMMU_BYTES-4);if(mevent())return 1;*v=mmio[at/4];return 0;}
static int mread64(void *c,size_t at,uint64_t *v)
{CHECK(c==&mf && at%8==0 && at<=SMMU_BYTES-8);if(mevent())return 1;std::memcpy(v,mmio+at/4,8);return 0;}
static int mwrite32(void *c,size_t at,uint32_t v)
{CHECK(c==&mf && at%4==0 && at<=SMMU_BYTES-4);int f=mevent();if(f && !mf.after)return 1;mmio[at/4]=v;return f;}
static int mwrite64(void *c,size_t at,uint64_t v)
{CHECK(c==&mf && at%8==0 && at<=SMMU_BYTES-8);int f=mevent();if(f && !mf.after)return 1;std::memcpy(mmio+at/4,&v,8);return f;}
static int mbarrier(void *c){CHECK(c==&mf);return mevent();}
/* CP register peer. The memptrs page is fencemem: [0] the BR read pointer shadow (ring room), [1] the completed fence. */
struct CpPeer {
    std::vector<uint32_t> regs;
    std::vector<uint32_t> writes;
    unsigned publish=0,acquire=0,barriers=0,fail_publish=0;
    CpPeer():regs(0x10000,0){}
};
static CpPeer cp;
static uint64_t clock_us=0;
static unsigned idles=0;
static int cwrite(void *c,uint32_t reg,uint32_t v)
{CHECK(c==&cp && reg<cp.regs.size());cp.regs[reg]=v;cp.writes.push_back(reg);return 0;}
static int cread(void *c,uint32_t reg,uint32_t *v)
{CHECK(c==&cp && v && reg<cp.regs.size());*v=cp.regs[reg];return 0;}
static int cbarrier(void *c){CHECK(c==&cp);++cp.barriers;return 0;}
static int cpublish(void *c,const volatile uint32_t *w,size_t bytes)
{CHECK(c==&cp && w && bytes && bytes<=32768 && !(bytes&3));++cp.publish;return cp.publish==cp.fail_publish;}
static int cacquire(void *c,volatile uint32_t *w,size_t bytes)
{CHECK(c==&cp && w && bytes>=8 && bytes<=4096);++cp.acquire;return 0;}
static uint64_t cnow(void *c){CHECK(c==&cp);return clock_us;}
static int cidle(void *c){CHECK(c==&cp);clock_us+=1000;++idles;return 0;}
struct Path {
    NativeAdrenoMemory tables,object;
    ArmSmmu4KLiveArena arena;
    ArmSmmu4KLiveState state{};
    ArmSmmu4KLiveVisibility visibility;
    NativeAdrenoVm vm;
    NativeAdrenoRing ring;
    NativeAdrenoSubmit submit;
    uint64_t handle=0;
};
static uint32_t ringmem[8192],fencemem[1024];
static NativeAdrenoRingIo ringio{&cp,cwrite,cread,cbarrier,nullptr,nullptr,nullptr};
static NativeAdrenoRingCache ringcache{&cp,cpublish,cacquire};
static NativeAdrenoRingClock ringclock{&cp,cnow,cidle};
static void smmuReset(void)
{
    std::memset(mmio,0,sizeof(mmio));mf={};streams=(ArmSmmu4KStreamState){};
    smmuio=(ArmSmmu4KIo){&mf,SMMU_BYTES,mread32,mread64,mwrite32,mwrite64,mbarrier};
    mmio[0x20/4]=(1u<<30)|(1u<<27)|(4u<<9)|8;
    mmio[0x24/4]=0x40000010;mmio[0x28/4]=0x1433;mmio[0]=0x436;
    for(unsigned i=0;i<8;++i)mmio[(0xc00+4*i)/4]=0x20000;
    mmio[0x800/4]=1u<<31|9;mmio[0xc00/4]=3;
}
static int exposed(Path &p,bool write_combined=true)
{
    smmuReset();cp=CpPeer{};clock_us=0;idles=0;
    std::memset(ringmem,0xef,sizeof(ringmem));std::memset(fencemem,0,sizeof(fencemem));
    if(p.tables.allocate(32*4096,40,true,32*4096))return kIOReturnNoMemory;
    if(NativeAdrenoPrepareSmmuMemory(&p.tables,32,40,0x800000,65536,0,16,&p.arena,&p.state,&p.visibility))
        return kIOReturnBadArgument;
    context=(ArmSmmu4KContextConfig){.lower=&p.arena.config,.lower_tables=&p.state.tables,
        .id0=mmio[0x20/4],.id2=mmio[0x28/4],.asid=1,.coherent_walk=1,
        .stall_on_fault=0,.continue_after_fault=1};
    ArmSmmu4KContextConfig actual=context;
    CHECK(ArmSmmu4KEncodeContext(&actual,&context_image)==ArmSmmu4KSuccess);
    mmio[0x1008/4]=context_image.cbar;mmio[0x1808/4]=context_image.cba2r;
    mmio[CB/4]=context_image.sctlr;mmio[(CB+0x10)/4]=context_image.tcr2;mmio[(CB+0x30)/4]=context_image.tcr;
    std::memcpy(mmio+(CB+0x20)/4,&context_image.ttbr0,8);
    std::memcpy(mmio+(CB+0x28)/4,&context_image.ttbr1,8);
    mmio[(CB+0x38)/4]=context_image.mair0;
    if(ArmSmmu4KAttachStreams(&smmuio,&streams,routes,2,2,&context,&route_result))return kIOReturnBadArgument;
    NativeAdrenoVmRegion lower={&p.tables,&p.arena,&p.state};
    if(p.vm.attach(&smmuio,&context,&streams,&lower,nullptr,32,1000))return kIOReturnBadArgument;
    p.vm.setFenceSource(&fencemem[1]);
    if(p.object.allocate(8192,40,false,8192))return kIOReturnNoMemory;
    if(write_combined && p.object.makeWriteCombined())return kIOReturnNoMemory;
    if(p.vm.map(&p.object,0,0x800000,3,0,2,&p.handle))return kIOReturnBadArgument;
    if(p.ring.attach(&ringio,&ringcache,ringmem,sizeof(ringmem),0x100000,fencemem,sizeof(fencemem),0x108000))
        return kIOReturnBadArgument;
    return p.submit.attach(&p.vm,&p.ring);
}
/* The ring index right after a submission of n IB packets, from the ring index before it (and whether it carried the context entry). */
static uint32_t after(uint32_t at,uint32_t n,bool reset)
{
    return at+NativeAdrenoRingPrologueWords+(reset?NativeAdrenoRingResetWords:0)+4*n+NativeAdrenoRingFramedFenceWords;
}
/* The CP finished everything up to fence (and consumed the ring up to rptr). */
static void complete(uint32_t fence,uint32_t rptr)
{
    fencemem[1]=fence;fencemem[0]=rptr;
}
static NativeAdrenoSubmitBuffer many[NativeAdrenoSubmitMaxBuffers];
int main()
{
    unsigned cases=0;
    /* One validated indirect-buffer submission: exact ring words, one WPTR write, a job reference, no SMMU access, no cache
     * maintenance, no TLB invalidation (ARCHITECTURE 5.4). */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,4096};
        uint32_t fence=0,retry=7;
        const unsigned smmu=mf.calls,cache=fixture.cache_calls,physical=fixture.physical_calls,wptr_writes=(unsigned)cp.writes.size();
        mmio[(CB+0x618)/4]=0xffffffffu;   /* TLBIALL: must stay untouched */
        CHECK(p.submit.submit(&buffer,1,0,&fence,&retry,5)==kIOReturnSuccess && fence==1 && retry==0);
        CHECK(mf.calls==smmu && fixture.cache_calls==cache && fixture.physical_calls==physical && mmio[(CB+0x618)/4]==0xffffffffu);
        CHECK(cp.writes.size()==wptr_writes+1 && cp.writes.back()==0x0807);
        const uint32_t ib=NativeAdrenoRingPrologueWords+NativeAdrenoRingResetWords;   /* the first submission enters a fresh context */
        CHECK(cp.regs[0x0807]==after(0,1,true));
        CHECK((ringmem[ib]>>16&0x7fu)==0x3f && (ringmem[ib]&0x3fffu)==3);
        CHECK(ringmem[ib+1]==0x800000 && ringmem[ib+2]==0 && ringmem[ib+3]==1024);
        CHECK(p.object.deviceReferences()==2); /* mapping + job */
        CHECK(p.submit.outstanding()==1 && p.submit.stats().references_held==1 && p.submit.issued(1) && !p.submit.issued(2));
        CHECK(p.vm.lastUse(p.handle)==1 && !p.vm.idle(p.handle) && p.vm.unmap(p.handle)==kIOReturnBusy);
        CHECK(p.submit.detach()==kIOReturnBusy);
        CHECK(p.submit.wait(1,&ringclock,1000)==kIOReturnTimeout && p.submit.stats().timeouts==1);
        CHECK(p.submit.wait(1,&ringclock,0)==kIOReturnTimeout && p.submit.stats().timeouts==2);   /* timeout 0 checks once */
        CHECK(!p.submit.completed(1));
        complete(1,cp.regs[0x0807]);
        CHECK(p.submit.completed(1) && p.submit.wait(1,&ringclock,1000)==kIOReturnSuccess);
        CHECK(p.submit.outstanding()==0 && p.submit.stats().completed==1);
        CHECK(p.object.deviceReferences()==1 && p.submit.stats().references_retired==1 && p.vm.idle(p.handle));
        CHECK(p.submit.wait(1,&ringclock,1000)==kIOReturnSuccess); /* retired job: the CP passed it */
        CHECK(p.submit.wait(2,&ringclock,1000)==kIOReturnBadArgument); /* never submitted */
        /* The same client again: no context entry; another client: the entry again. */
        const uint32_t second=cp.regs[0x0807];
        CHECK(p.submit.submit(&buffer,1,0,&fence,nullptr,5)==kIOReturnSuccess && fence==2 && cp.regs[0x0807]==after(second,1,false));
        complete(2,cp.regs[0x0807]);
        const uint32_t third=cp.regs[0x0807];
        CHECK(p.submit.submit(&buffer,1,8,&fence,nullptr,6)==kIOReturnSuccess && fence==3 && cp.regs[0x0807]==after(third,1,true));
        complete(3,cp.regs[0x0807]);
        CHECK(p.submit.retireCompleted()==kIOReturnSuccess && p.submit.outstanding()==0 && p.object.deviceReferences()==1);
        CHECK(p.submit.detach()==kIOReturnSuccess);
        ++cases;
    }
    /* Refusals precede references, ring bytes and WPTR updates. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        const uint32_t wptr=cp.regs[0x0807];
        uint32_t fence=0;
        CHECK(p.submit.submit(nullptr,1,0,&fence)==kIOReturnBadArgument);
        NativeAdrenoSubmitBuffer zero[1]={{p.handle,0x800000,0}};
        CHECK(p.submit.submit(zero,1,0,&fence)==kIOReturnBadArgument);
        NativeAdrenoSubmitBuffer unaligned[1]={{p.handle,0x800000,4098}};
        CHECK(p.submit.submit(unaligned,1,0,&fence)==kIOReturnBadArgument);
        NativeAdrenoSubmitBuffer wrong[1]={{p.handle,0x802000,4096}};
        CHECK(p.submit.submit(wrong,1,0,&fence)==kIOReturnBadArgument);
        NativeAdrenoSubmitBuffer overrun[1]={{p.handle,0x800000,8192+4}};
        CHECK(p.submit.submit(overrun,1,0,&fence)==kIOReturnBadArgument);
        NativeAdrenoSubmitBuffer unknown[1]={{p.handle+7,0x800000,4096}};
        CHECK(p.submit.submit(unknown,1,0,&fence)==kIOReturnBadArgument);
        NativeAdrenoSubmitBuffer good[1]={{p.handle,0x800000,4096}};
        CHECK(p.submit.submit(good,1,NativeAdrenoSubmitRanges*NativeAdrenoSubmitRangeQueues,&fence)==kIOReturnBadArgument);
        for(auto &b:many)b={p.handle,0x800000,4096};
        CHECK(p.submit.submit(many,NativeAdrenoSubmitMaxBuffers+1,0,&fence)==kIOReturnBadArgument);
        CHECK(p.submit.submit(good,1,0,nullptr)==kIOReturnBadArgument);
        CHECK(p.object.deviceReferences()==1 && cp.regs[0x0807]==wptr && p.submit.outstanding()==0 && !fence);
        /* Duplicate buffers share one device reference. */
        NativeAdrenoSubmitBuffer twice[2]={{p.handle,0x800000,4096},{p.handle,0x800000,4096}};
        CHECK(p.submit.submit(twice,2,0,&fence)==kIOReturnSuccess);
        CHECK(p.object.deviceReferences()==2 && cp.regs[0x0807]==after(0,2,true));
        complete(1,cp.regs[0x0807]);
        CHECK(p.submit.wait(1,&ringclock,100)==kIOReturnSuccess && p.object.deviceReferences()==1);
        CHECK(p.submit.detach()==kIOReturnSuccess);
        ++cases;
    }
    /* Job slots (ARCHITECTURE 5.4): 44, one reserved per client range (thirty-two, the publish shell's connection slots); the render
     * server's range 0 also takes the 12 shared ones; any other range has at most one job in flight; a range with nothing in flight
     * always finds its slot. Completed jobs free their slots on demand, without a wait. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        CHECK(NativeAdrenoSubmitRanges==32 && NativeAdrenoSubmitSharedJobs==12 && NativeAdrenoSubmitJobs==44);
        NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,256};
        uint32_t fence=0,last=0;
        for(uint32_t n=1;n<=1+NativeAdrenoSubmitSharedJobs;++n) {
            CHECK(p.submit.submit(&buffer,1,3,&fence,nullptr,5)==kIOReturnSuccess && fence==n);   /* range 0, queue 3 */
            last=fence;
        }
        const uint32_t busy=p.submit.stats().busy;
        CHECK(p.submit.submit(&buffer,1,0,&fence)==kIOReturnBusy && !fence && p.submit.stats().busy==busy+1);
        CHECK(p.submit.rangeOutstanding(0,8) && p.submit.queueOutstanding(3) && !p.submit.queueOutstanding(0));
        /* Every other range still has its reserved slot, while range 0 holds all the shared ones (any queue of the range). */
        for(uint32_t range=1;range<NativeAdrenoSubmitRanges;++range)
            CHECK(p.submit.submit(&buffer,1,range*NativeAdrenoSubmitRangeQueues+range%NativeAdrenoSubmitRangeQueues,&fence,nullptr,5+range)==
                  kIOReturnSuccess && fence==last+range);
        CHECK(p.submit.outstanding()==NativeAdrenoSubmitJobs);
        CHECK(p.submit.rangeOutstanding((NativeAdrenoSubmitRanges-1)*NativeAdrenoSubmitRangeQueues,NativeAdrenoSubmitRangeQueues));
        /* A range other than 0 never has two in flight. */
        CHECK(p.submit.submit(&buffer,1,8,&fence,nullptr,6)==kIOReturnBusy);
        /* The CP passes the first two fences: their slots come back at the next submission without any wait() (one sweep). */
        complete(2,0);
        const uint32_t sweeps=p.submit.stats().sweeps;
        CHECK(p.submit.submit(&buffer,1,0,&fence,nullptr,5)==kIOReturnSuccess && p.submit.stats().sweeps==sweeps+1);
        CHECK(p.submit.outstanding()==NativeAdrenoSubmitJobs-1 && p.object.deviceReferences()==1+NativeAdrenoSubmitJobs-1);
        complete(fence,cp.regs[0x0807]);
        CHECK(p.submit.waitAll(&ringclock,1000)==kIOReturnSuccess && p.submit.outstanding()==0 && p.object.deviceReferences()==1);
        CHECK(p.submit.detach()==kIOReturnSuccess);
        ++cases;
    }
    /* Ring room (ARCHITECTURE 5.4): the ring full for a caller with work in flight is Busy; for a caller with nothing in flight it
     * is NativeAdrenoSubmitRetry with the fence that frees room, and the bounded retry loop either gets in once the CP consumed
     * or turns into Busy after 200 ms; it is never kIOReturnNoResources. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        for(auto &b:many)b={p.handle,0x800000,256};
        uint32_t fence=0,retry=0;
        /* 13 frames of 128 IBs from range 0 (its reserved and the twelve shared slots), the read pointer still at 0. */
        uint32_t at=0;
        for(uint32_t n=1;n<=1+NativeAdrenoSubmitSharedJobs;++n) {
            CHECK(p.submit.submit(many,NativeAdrenoSubmitMaxBuffers,0,&fence,nullptr,5)==kIOReturnSuccess && fence==n);
            at=after(at,NativeAdrenoSubmitMaxBuffers,n==1);
        }
        CHECK(cp.regs[0x0807]==at);
        /* Range 1 fits once more; range 2 then finds no room, and has nothing in flight. */
        CHECK(p.submit.submit(many,NativeAdrenoSubmitMaxBuffers,8,&fence,nullptr,6)==kIOReturnSuccess && fence==14);
        const uint32_t wptr=cp.regs[0x0807];
        const unsigned references=p.object.deviceReferences();
        CHECK(p.submit.submit(many,NativeAdrenoSubmitMaxBuffers,16,&fence,&retry,7)==NativeAdrenoSubmitRetry && !fence && retry==1);
        CHECK(cp.regs[0x0807]==wptr && p.object.deviceReferences()==references && p.submit.stats().retries==1);
        /* Range 0 (work in flight) is told Busy for the same reason (here: no slot first). */
        CHECK(p.submit.submit(many,NativeAdrenoSubmitMaxBuffers,0,&fence,&retry)==kIOReturnBusy);
        /* The retry loop: the first sleep sees the CP finish fence 1 and consume its words; the submission then gets in. */
        uint64_t fake=0;unsigned sleeps=0,calls=0;
        IOReturn status=NativeAdrenoSubmitWithRetry(
            [&](uint32_t *r) {++calls;return p.submit.submit(many,NativeAdrenoSubmitMaxBuffers,16,&fence,r,7);},
            [&](uint32_t f,uint64_t deadline) {++sleeps;CHECK(f==1 && deadline==200000);complete(1,after(0,NativeAdrenoSubmitMaxBuffers,true));fake+=100;},
            [&] {return fake;});
        CHECK(status==kIOReturnSuccess && fence==15 && calls==2 && sleeps==1);
        /* Nothing frees room: after 200 ms the caller gets Busy (Turnip then retries), never an error. */
        fake=0;sleeps=0;calls=0;
        status=NativeAdrenoSubmitWithRetry(
            [&](uint32_t *r) {++calls;return p.submit.submit(many,NativeAdrenoSubmitMaxBuffers,24,&fence,r,8);},
            [&](uint32_t f,uint64_t) {++sleeps;CHECK(f==2);fake+=50000;},
            [&] {return fake;});
        CHECK(status==kIOReturnBusy && sleeps==4 && calls==5 && !fence);
        /* A loop that never sees the internal status: it passes every other answer through at once. */
        calls=0;
        status=NativeAdrenoSubmitWithRetry([&](uint32_t *) {++calls;return kIOReturnBadArgument;},[&](uint32_t,uint64_t) {CHECK(0);},
            [&] {return fake;});
        CHECK(status==kIOReturnBadArgument && calls==1);
        complete(15,cp.regs[0x0807]);
        CHECK(p.submit.waitAll(&ringclock,1000)==kIOReturnSuccess && p.submit.outstanding()==0);
        CHECK(p.submit.detach()==kIOReturnSuccess);
        ++cases;
    }
    /* Teardown waits poll with the clock's idle hook between zero-timeout checks: a range's own jobs only. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,256};
        uint32_t fence=0;
        CHECK(p.submit.submit(&buffer,1,0,&fence,nullptr,5)==kIOReturnSuccess && fence==1);
        CHECK(p.submit.submit(&buffer,1,8,&fence,nullptr,6)==kIOReturnSuccess && fence==2);
        idles=0;
        CHECK(p.submit.waitRange(8,8,&ringclock,3000)==kIOReturnTimeout && idles==3);
        complete(1,0);
        CHECK(p.submit.waitRange(0,8,&ringclock,3000)==kIOReturnSuccess && p.submit.outstanding()==1);
        CHECK(p.submit.waitRange(8,8,nullptr,3000)==kIOReturnTimeout);
        complete(2,cp.regs[0x0807]);
        CHECK(p.submit.waitRange(8,8,&ringclock,0)==kIOReturnSuccess && p.submit.outstanding()==0);
        CHECK(p.submit.detach()==kIOReturnSuccess);
        ++cases;
    }
    /* M10 recovery: halt snapshots the ranges with work in flight under the ring lock and stops new submissions (Busy for a range
     * with work in flight, the internal retry for the others); recover drops every job after the GPU restart; resume reopens. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,256};
        uint32_t fence=0,ranges=0,retry=9;
        CHECK(p.submit.submit(&buffer,1,0,&fence,nullptr,5)==kIOReturnSuccess);
        CHECK(p.submit.submit(&buffer,1,24,&fence,nullptr,8)==kIOReturnSuccess && fence==2);
        complete(1,0);   /* fence 1 passed before the hang: not lost */
        CHECK(p.submit.halt(&ranges)==kIOReturnSuccess && ranges==(1u<<3) && p.submit.halted());
        CHECK(p.submit.submit(&buffer,1,24,&fence,&retry)==kIOReturnBusy);
        CHECK(p.submit.submit(&buffer,1,8,&fence,&retry)==NativeAdrenoSubmitRetry && retry==0);
        const unsigned locks_before=cp.barriers;(void)locks_before;
        CHECK(p.object.deviceReferences()==2);
        uint32_t last=0;
        CHECK(p.ring.recover(&last)==kIOReturnSuccess && last==2);
        CHECK(p.submit.recover(&ranges)==kIOReturnSuccess && ranges==(1u<<3) && p.submit.stats().abandoned==1);
        CHECK(p.object.deviceReferences()==1 && p.submit.outstanding()==0);
        p.submit.resume();
        CHECK(!p.submit.halted() && p.submit.submit(&buffer,1,8,&fence,nullptr,6)==kIOReturnSuccess && fence==3);
        complete(3,cp.regs[0x0807]);
        CHECK(p.submit.waitAll(&ringclock,1000)==kIOReturnSuccess);
        CHECK(p.submit.detach()==kIOReturnSuccess);
        ++cases;
    }
    /* The hang's culprit (2026-10-30): halt names the range of the job the CP stands in, the lowest fence past the completed one, whatever
     * the slot order; ranges up to 31 fit the 32-bit masks; nothing in flight names none. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,256};
        uint32_t fence=0,ranges=0,oldest=0;
        CHECK(p.submit.submit(&buffer,1,0,&fence,nullptr,5)==kIOReturnSuccess && fence==1);      /* range 0, passed below */
        CHECK(p.submit.submit(&buffer,1,31*8+2,&fence,nullptr,9)==kIOReturnSuccess && fence==2); /* range 31: where the CP stands */
        CHECK(p.submit.submit(&buffer,1,5*8,&fence,nullptr,6)==kIOReturnSuccess && fence==3);    /* range 5, behind it */
        CHECK(p.submit.submit(&buffer,1,1,&fence,nullptr,5)==kIOReturnSuccess && fence==4);      /* range 0 again, a shared slot */
        complete(1,0);
        CHECK(p.submit.halt(&ranges,&oldest)==kIOReturnSuccess && ranges==(1u<<31|1u<<5|1u) && oldest==31);
        uint32_t last=0;
        CHECK(p.ring.recover(&last)==kIOReturnSuccess && p.submit.recover(&ranges)==kIOReturnSuccess && p.submit.outstanding()==0);
        p.submit.resume();
        CHECK(p.submit.halt(&ranges,&oldest)==kIOReturnSuccess && ranges==0 && oldest==UINT32_MAX);
        p.submit.resume();
        CHECK(p.submit.detach()==kIOReturnSuccess);
        ++cases;
    }
    /* A poisoned object retires nothing at the halt: the jobs the CP already passed and the poisoned commit's job (fence 0, never
     * issued) are not where the CP stands and are never named, so with nothing in flight after the completed fence there is no
     * culprit (it used to be the poisoned commit's range, or a passed one). They still count in the ranges, as recover() drops them. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,256};
        uint32_t fence=0,ranges=0,oldest=0,last=0;
        CHECK(p.submit.submit(&buffer,1,2*8,&fence,nullptr,5)==kIOReturnSuccess && fence==1);
        CHECK(p.submit.submit(&buffer,1,3*8,&fence,nullptr,6)==kIOReturnSuccess && fence==2);
        complete(2,cp.regs[0x0807]);   /* passed, not retired */
        cp.fail_publish=cp.publish+1;
        CHECK(p.submit.submit(&buffer,1,4*8,&fence,nullptr,7)==kIOReturnIOError && !fence && p.submit.poisoned());
        CHECK(p.submit.halt(&ranges,&oldest)==kIOReturnSuccess && ranges==(1u<<4|1u<<3|1u<<2) && oldest==UINT32_MAX);
        CHECK(p.ring.recover(&last)==kIOReturnSuccess && p.submit.recover(&ranges)==kIOReturnSuccess && ranges==(1u<<4|1u<<3|1u<<2));
        p.submit.resume();
        CHECK(p.submit.detach()==kIOReturnSuccess);
        ++cases;
    }
    /* The same with a job still in flight after the completed fence: the CP stands in that one, whatever was passed or poisoned. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,256};
        uint32_t fence=0,ranges=0,oldest=0,last=0;
        CHECK(p.submit.submit(&buffer,1,2*8,&fence,nullptr,5)==kIOReturnSuccess && fence==1);
        CHECK(p.submit.submit(&buffer,1,3*8,&fence,nullptr,6)==kIOReturnSuccess && fence==2);
        CHECK(p.submit.submit(&buffer,1,5*8,&fence,nullptr,8)==kIOReturnSuccess && fence==3);
        complete(2,cp.regs[0x0807]);
        cp.fail_publish=cp.publish+1;
        CHECK(p.submit.submit(&buffer,1,4*8,&fence,nullptr,7)==kIOReturnIOError && p.submit.poisoned());
        CHECK(p.submit.halt(&ranges,&oldest)==kIOReturnSuccess && ranges==(1u<<5|1u<<4|1u<<3|1u<<2) && oldest==5);
        CHECK(p.ring.recover(&last)==kIOReturnSuccess && p.submit.recover(&ranges)==kIOReturnSuccess);
        p.submit.resume();
        CHECK(p.submit.detach()==kIOReturnSuccess);
        ++cases;
    }
    /* A commit that poisons the ring retains every reference and disables execbuf until recover(). */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        cp.fail_publish=cp.publish+1;
        NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,4096};
        uint32_t fence=0;
        CHECK(p.submit.submit(&buffer,1,0,&fence)==kIOReturnIOError);
        CHECK(!fence && p.submit.poisoned() && p.ring.poisoned());
        CHECK(p.submit.outstanding()==1 && p.submit.stats().poisoned_jobs==1);
        CHECK(p.object.deviceReferences()==2); /* never retired */
        CHECK(p.submit.detach()==kIOReturnBusy && p.submit.wait(1,&ringclock,10)==kIOReturnNotReady);
        CHECK(p.submit.submit(&buffer,1,0,&fence)==kIOReturnNotReady && p.submit.retireCompleted()==kIOReturnNotReady);
        CHECK(p.ring.detach()==kIOReturnBusy && p.vm.detach()==kIOReturnBusy);
        CHECK(p.object.release()==kIOReturnBusy);
        uint32_t ranges=0,last=0;
        CHECK(p.ring.recover(&last)==kIOReturnSuccess && !p.ring.poisoned());
        CHECK(p.submit.recover(&ranges)==kIOReturnSuccess && ranges==1u && !p.submit.poisoned() && p.object.deviceReferences()==1);
        CHECK(p.submit.submit(&buffer,1,0,&fence)==kIOReturnSuccess && fence==1);
        complete(1,cp.regs[0x0807]);
        CHECK(p.submit.waitAll(&ringclock,1000)==kIOReturnSuccess && p.submit.detach()==kIOReturnSuccess);
        ++cases;
    }
    /* The M10 lock hooks: every public method holds Ring around its body; the Memory lock is never taken by the submit object. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        static int depth[2],taken[2],deepest;
        depth[0]=depth[1]=taken[0]=taken[1]=deepest=0;
        const NativeAdrenoSubmitLocks locks={nullptr,
            [](void *,NativeAdrenoSubmitLock w) {CHECK(!depth[w]);++depth[w];++taken[w];if(depth[0]+depth[1]>deepest)deepest=depth[0]+depth[1];},
            [](void *,NativeAdrenoSubmitLock w) {CHECK(depth[w]==1);--depth[w];}};
        CHECK(p.submit.attachLocks(&locks)==kIOReturnSuccess);
        NativeAdrenoSubmitLocks broken=locks;broken.unlock=nullptr;
        CHECK(p.submit.attachLocks(&broken)==kIOReturnBadArgument);
        NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,256};
        uint32_t fence=0;
        CHECK(p.submit.submit(&buffer,1,0,&fence,nullptr,5)==kIOReturnSuccess && taken[0]==1 && !taken[1]);
        CHECK(p.submit.attachLocks(nullptr)==kIOReturnBusy);   /* not while a job is outstanding */
        CHECK(p.submit.wait(fence,&ringclock,2000)==kIOReturnTimeout && taken[0]>=3 && !depth[0]);   /* no lock while idle */
        complete(1,cp.regs[0x0807]);
        CHECK(p.submit.rangeOutstanding(0,8) && p.submit.retireCompleted()==kIOReturnSuccess && !p.submit.rangeOutstanding(0,8));
        {NativeAdrenoSubmitGuard guard(&p.submit,NativeAdrenoSubmitLockMemory);CHECK(depth[1]==1);}
        CHECK(!depth[0] && !depth[1] && deepest==1);
        CHECK(p.submit.attachLocks(nullptr)==kIOReturnSuccess && p.submit.detach()==kIOReturnSuccess);
        ++cases;
    }
    /* A refused attach leaves both peers untouched. */
    {
        Path p;NativeAdrenoRing ring;NativeAdrenoSubmit submit;
        CHECK(submit.attach(nullptr,&ring)==kIOReturnBadArgument);
        CHECK(submit.attach(&p.vm,nullptr)==kIOReturnBadArgument);
        CHECK(submit.detach()==kIOReturnNotReady && submit.recover(nullptr)==kIOReturnNotReady && submit.halt(nullptr)==kIOReturnNotReady);
        CHECK(submit.submit(nullptr,1,0,nullptr)==kIOReturnBadArgument && !submit.issued(1) && !submit.lastSubmitted());
        ++cases;
    }
    /* M14 P0: the client range of every fence the object issued (rangesInFlight, the page-fault attribution of ARCHITECTURE 4.12.3),
     * kept after the job retires, forgotten 64 fences later; a fence it did not issue is unknown. The source goes through to the
     * ring's stamp (the queue meter's job attribution), external unless the caller names the render server. */
    unsigned historyCases=0;
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        static std::vector<uint32_t> flags;
        flags.clear();
        const NativeAdrenoRingWaiter waiter={nullptr,
            [](void *,uint32_t,uint32_t,uint32_t *observed)->IOReturn {*observed=fencemem[1];return kIOReturnTimeout;},
            [](void *,uint32_t,uint32_t f) {flags.push_back(f);}};
        p.ring.setWaiter(&waiter);
        constexpr uint32_t R=NativeAdrenoRingSourceRender,E=NativeAdrenoRingSourceExternal,C=NativeAdrenoRingStampContext;
        NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,256};
        uint32_t fence=0,ranges=7;
        CHECK(p.submit.submit(&buffer,1,0,&fence,nullptr,5,R)==kIOReturnSuccess && fence==1);       /* range 0, the render server */
        CHECK(p.submit.submit(&buffer,1,3*8+1,&fence,nullptr,6)==kIOReturnSuccess && fence==2);     /* range 3 */
        CHECK(p.submit.submit(&buffer,1,1,&fence,nullptr,5,R)==kIOReturnSuccess && fence==3);       /* range 0 again */
        CHECK(p.submit.submit(&buffer,1,5*8,&fence,nullptr,7,E)==kIOReturnSuccess && fence==4);     /* range 5 */
        CHECK(flags==(std::vector<uint32_t>{R|C,E|C,R|C,E|C}));   /* every one a client switch */
        CHECK(p.submit.rangesInFlight(0,4,&ranges)==kIOReturnSuccess && ranges==(1u|1u<<3|1u<<5));
        CHECK(p.submit.rangesInFlight(1,2,&ranges)==kIOReturnSuccess && ranges==(1u<<3));   /* only fence 2 in flight */
        CHECK(p.submit.rangesInFlight(3,4,&ranges)==kIOReturnSuccess && ranges==(1u<<5));
        CHECK(p.submit.rangesInFlight(4,4,&ranges)==kIOReturnSuccess && ranges==0);         /* nothing in flight */
        CHECK(p.submit.rangesInFlight(4,3,&ranges)==kIOReturnUnsupported && ranges==0);    /* a fence past the issued one */
        CHECK(p.submit.rangesInFlight(0,4,nullptr)==kIOReturnBadArgument);
        /* The kernel's own submission straight into the ring: its fence is not this object's. */
        const uint32_t nop[1]={0};
        uint32_t own=0;
        CHECK(p.ring.submit(nop,1,&own)==kIOReturnSuccess && own==5 && flags.back()==NativeAdrenoRingSourceKernel);
        CHECK(p.submit.rangesInFlight(3,5,&ranges)==kIOReturnUnsupported && ranges==0);
        /* Retired jobs keep their entry; 64 later fences push the old ones out. */
        complete(5,cp.regs[0x0807]);
        CHECK(p.submit.retireCompleted()==kIOReturnSuccess && p.submit.outstanding()==0);
        CHECK(p.submit.rangesInFlight(1,2,&ranges)==kIOReturnSuccess && ranges==(1u<<3));
        for(uint32_t n=0;n<64;++n) {
            CHECK(p.submit.submit(&buffer,1,2*8,&fence,nullptr,9)==kIOReturnSuccess);
            complete(fence,cp.regs[0x0807]);
            CHECK(p.submit.retireCompleted()==kIOReturnSuccess);
        }
        CHECK(fence==69 && p.submit.rangesInFlight(5,69,&ranges)==kIOReturnSuccess && ranges==(1u<<2));   /* 64 fences of range 2 */
        CHECK(p.submit.rangesInFlight(1,2,&ranges)==kIOReturnUnsupported);   /* forgotten: its entry holds fence 66 */
        CHECK(p.submit.rangesInFlight(4,69,&ranges)==kIOReturnUnsupported);  /* 65 fences */
        p.ring.setWaiter(nullptr);
        CHECK(p.submit.detach()==kIOReturnSuccess);
        /* A new attachment forgets the old one's fences (the ring it gets may number them anew). */
        CHECK(p.submit.attach(&p.vm,&p.ring)==kIOReturnSuccess);
        CHECK(p.submit.rangesInFlight(68,69,&ranges)==kIOReturnUnsupported && ranges==0);
        CHECK(p.submit.detach()==kIOReturnSuccess);
        NativeAdrenoSubmit idle;
        CHECK(idle.rangesInFlight(0,1,&ranges)==kIOReturnNotReady && ranges==0);
        ++historyCases;
    }
    std::printf("PASS: %u fence history cases (the client range of each issued fence for the page-fault attribution: in-flight "
        "intervals, empty, past the issued fence, a kernel fence unknown, kept after retirement, forgotten after 64 and by a new "
        "attachment, more than 64 refused) and the stamp source passed through\n",historyCases);
    std::printf("PASS: %u execbuf cases over software SMMU/CP peers: exact framed IB packet and fence with no SMMU access, cache "
        "maintenance or TLBI on submit; 9 pre-reference refusals; 44 job slots with one reserved per range (32) and Busy only with work in "
        "flight; ring-full retry within 200 ms; teardown waits; halt/recover/resume; the hang's culprit range by fence (never a passed "
        "or poisoned job); poisoned-commit retention; lock hooks\n",cases);
}
/* Physical-page maintenance (imported BOs): record it; no host page is touched. */
/* Host double: the page attribute change of user BOs has no host equivalent (no host page is touched). */
IOReturn NativeAdrenoSetPagesCacheMode(const NativeAdrenoPhysicalSpan *spans,size_t count,bool){return spans && count?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoReassertWriteCombined(const NativeAdrenoPhysicalSpan *spans,size_t count,uint32_t *reset)
{return spans && count && reset?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoMaintainPhysicalPages(uint64_t physical,size_t bytes,NativeAdrenoCacheOperation op)
{
    if(!physical || !bytes || ((physical|bytes)&4095) || op>NativeAdrenoInvalidate)std::abort();
    ++fixture.physical_calls;fixture.physical_bytes+=bytes;return kIOReturnSuccess;
}
