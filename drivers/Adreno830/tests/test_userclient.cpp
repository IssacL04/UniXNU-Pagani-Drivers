/* SPDX-License-Identifier: MIT */
/* Real user-client body over software SMMU/CP register peers, and the publish shell's pure policies (windows, fault attribution,
 * open refusal, slot choice). No IOKit. ARCHITECTURE 4.13, 5.4, 6; IMPLEMENTATION_PLAN 2.1 test_userclient (M7, M9, M10).
 * host-test: NativeAdrenoMemory.cpp NativeAdrenoSmmuMemory.cpp NativeAdrenoVm.cpp NativeAdrenoRing.cpp NativeAdrenoSubmit.cpp NativeAdrenoUserClient.cpp NativeAdrenoTimer.cpp */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoUserClient.h"
#include "NativeAdrenoPublish.h"
#include "NativeAdrenoMeter.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#if __has_include("../../UniXNU/drivers/AdrenoMetal/FrontendSecond.h")
#include "../../UniXNU/drivers/AdrenoMetal/FrontendSecond.h"
static_assert(uint32_t(frontendsecond::kMethod)==uint32_t(NativeAdrenoFrontendMethod),"the plugin's tunnel method");
#endif
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
struct CpPeer {
    std::vector<uint32_t> regs,writes;
    unsigned publish=0,acquire=0,barriers=0,fail_publish=0;
    CpPeer():regs(0x10000,0){}
};
static CpPeer cp;
static uint64_t clock_us=0;
static int cwrite(void *c,uint32_t reg,uint32_t v)
{CHECK(c==&cp && reg<cp.regs.size());cp.regs[reg]=v;cp.writes.push_back(reg);return 0;}
static int cread(void *c,uint32_t reg,uint32_t *v)
{CHECK(c==&cp && v && reg<cp.regs.size());*v=cp.regs[reg];return 0;}
static int cbarrier(void *c){CHECK(c==&cp);++cp.barriers;return 0;}
static int cpublish(void *c,const volatile uint32_t *w,size_t bytes)
{CHECK(c==&cp && w && bytes && bytes<=32768);++cp.publish;return cp.publish==cp.fail_publish;}
static int cacquire(void *c,volatile uint32_t *w,size_t bytes)
{CHECK(c==&cp && w && bytes>=8 && bytes<=4096);++cp.acquire;return 0;}
static uint64_t cnow(void *c){CHECK(c==&cp);return clock_us;}
static int cidle(void *c){CHECK(c==&cp);clock_us+=1000;return 0;}
struct Path {
    NativeAdrenoMemory tables;
    ArmSmmu4KLiveArena arena;
    ArmSmmu4KLiveState state{};
    ArmSmmu4KLiveVisibility visibility;
    NativeAdrenoVm vm;
    NativeAdrenoRing ring;
    NativeAdrenoSubmit execbuf;
    NativeAdrenoUserClient client;
};
static uint32_t ringmem[8192],fencemem[1024];
static NativeAdrenoRingIo ringio{&cp,cwrite,cread,cbarrier,nullptr,nullptr,nullptr};
static NativeAdrenoRingCache ringcache{&cp,cpublish,cacquire};
static NativeAdrenoRingClock ringclock{&cp,cnow,cidle};
/* The same clock, 100 us further at each read: what the call meter sees of a call is its reads. */
static uint64_t ctick(void *c){CHECK(c==&cp);return clock_us+=100;}
static NativeAdrenoRingClock tickclock{&cp,ctick,cidle};
static int timeread(void *c,uint32_t address,uint32_t *value)
{CHECK(c==&cp && value && address<cp.regs.size());*value=cp.regs[address];return 0;}
static NativeAdrenoTimeIo timeio{&cp,timeread};
static void smmuReset(void)
{
    std::memset(mmio,0,sizeof(mmio));mf={};streams=(ArmSmmu4KStreamState){};
    smmuio=(ArmSmmu4KIo){&mf,SMMU_BYTES,mread32,mread64,mwrite32,mwrite64,mbarrier};
    mmio[0x20/4]=(1u<<30)|(1u<<27)|(4u<<9)|8;
    mmio[0x24/4]=0x40000010;mmio[0x28/4]=0x1433;mmio[0]=0x436;
    for(unsigned i=0;i<8;++i)mmio[(0xc00+4*i)/4]=0x20000;
    mmio[0x800/4]=1u<<31|9;mmio[0xc00/4]=3;
}
/* The GPU address space of these tests: 0x800000 + 64 KiB (the arena: 16 pages), for windows small enough to map into. */
enum : uint64_t {Base=0x800000,ArenaBytes=0x10000};
static int failed(int step,int status)
{
    std::fprintf(stderr,"exposed: step %d returned %d\n",step,status);
    return status?status:kIOReturnBadArgument;
}
static int exposed(Path &p,uint32_t output_bits=36)
{
    smmuReset();cp=CpPeer{};clock_us=0;
    std::memset(ringmem,0xef,sizeof(ringmem));std::memset(fencemem,0,sizeof(fencemem));
    int status=p.tables.allocate(32*4096,40,true,32*4096);
    if(status)return failed(1,status);
    status=NativeAdrenoPrepareSmmuMemory(&p.tables,32,40,Base,ArenaBytes,0,16,&p.arena,&p.state,&p.visibility);
    if(status)return failed(2,status);
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
    status=ArmSmmu4KAttachStreams(&smmuio,&streams,routes,2,2,&context,&route_result);
    if(status)return failed(3,status);
    NativeAdrenoVmRegion lower={&p.tables,&p.arena,&p.state};
    status=p.vm.attach(&smmuio,&context,&streams,&lower,nullptr,1024,1000);
    if(status)return failed(4,status);
    p.vm.setFenceSource(&fencemem[1]);
    status=p.ring.attach(&ringio,&ringcache,ringmem,sizeof(ringmem),0x100000,fencemem,sizeof(fencemem),0x108000);
    if(status)return failed(5,status);
    status=p.execbuf.attach(&p.vm,&p.ring);
    if(status)return failed(6,status);
    status=p.client.open(NativeAdrenoAbiVersion,&p.vm,&p.execbuf,&p.ring,32,output_bits);
    return status?failed(7,status):kIOReturnSuccess;
}
static void complete(uint32_t fence) {fencemem[1]=fence;fencemem[0]=cp.regs[0x0807];}
static uint64_t allocMapped(NativeAdrenoUserClient &client,uint64_t bytes,uint64_t iova)
{
    NativeAdrenoAbiBoAlloc alloc{bytes,12,0};NativeAdrenoAbiBoAllocReply allocated{};
    CHECK(client.allocBo(&alloc,&allocated)==kIOReturnSuccess && allocated.handle);
    NativeAdrenoAbiBoMap map{allocated.handle,iova,3,0,2,0};NativeAdrenoAbiBoMapReply mapped{};
    CHECK(client.mapBo(&map,&mapped)==kIOReturnSuccess && mapped.iova==iova);
    return allocated.handle;
}
static NativeAdrenoAbiSubmit request;
/* The M10 lock block's Memory lock depth, as the page-attribute double sees it: changes made under the lock and outside it. */
static const int *gMemoryDepth;
static unsigned gPagesUnder,gPagesOutside;
extern "C" volatile uint32_t native_adreno_client_stats[NativeAdrenoSubmitRanges+1][6];
int main()
{
    unsigned blocks=0,koptBlocks=0;
    /* Full client transaction: query (output bits as opened: IDR2.OAS 36), allocate, map, submit, wait, free, close. A submission's
     * wait_fence is only validated against the ring (ARCHITECTURE 4.13.3): no CPU wait, nothing written for a refused one. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        NativeAdrenoAbiParams params{};
        CHECK(p.client.query(&params)==kIOReturnSuccess && params.abi_version==NativeAdrenoAbiVersion);
        CHECK(params.input_bits==32 && params.output_bits==36 && params.flags==NativeAdrenoParamsHostImport);
        CHECK(params.max_bo_bytes==(UINT64_C(1)<<NativeAdrenoAbiMaxBoBytesShift));
        NativeAdrenoAbiTimestamp no_timer{};
        CHECK(p.client.timestamp(&no_timer)==kIOReturnUnsupported);
        NativeAdrenoAbiStatus no_status{};
        CHECK(p.client.status(&no_status)==kIOReturnUnsupported);
        CHECK(p.client.close()==kIOReturnSuccess); /* empty client closes */
        CHECK(p.client.open(NativeAdrenoAbiVersion,&p.vm,&p.execbuf,&p.ring,32,36)==kIOReturnSuccess);
        NativeAdrenoTimer timer;
        CHECK(timer.attach(&timeio,NativeAdrenoTimeCp)==kIOReturnSuccess);
        CHECK(p.client.attachTimer(&timer)==kIOReturnSuccess && p.client.attachTimer(&timer)==kIOReturnBadArgument);
        cp.regs[NativeAdrenoTimer::cpLowRegister()]=0x12345678u;cp.regs[NativeAdrenoTimer::cpHighRegister()]=9;
        NativeAdrenoAbiTimestamp ticks{};
        CHECK(p.client.timestamp(&ticks)==kIOReturnSuccess && ticks.ticks==UINT64_C(0x0000000912345678));
        NativeAdrenoStatus counters;
        CHECK(p.client.attachStatus(&counters)==kIOReturnSuccess && p.client.attachStatus(&counters)==kIOReturnBadArgument);
        counters.noteFault();counters.noteFault();counters.noteSuspend();
        NativeAdrenoAbiStatus queried{};
        CHECK(p.client.status(&queried)==kIOReturnSuccess && queried.faults==2 && queried.suspends==1);
        const uint64_t handle=allocMapped(p.client,8192,Base);
        CHECK(p.client.mapped(handle) && p.client.liveObjects()==1);
        NativeAdrenoAbiSubmit submit{};
        submit.buffer_count=1;submit.buffers[0]={handle,Base,1024};
        NativeAdrenoAbiSubmitReply reply{};
        CHECK(p.client.submit(&submit,&reply,&ringclock)==kIOReturnSuccess && reply.fence==1);
        const uint32_t ib=NativeAdrenoRingPrologueWords+NativeAdrenoRingResetWords;
        CHECK((ringmem[ib]>>16&0x7fu)==0x3f && ringmem[ib+1]==Base && ringmem[ib+3]==256);
        CHECK(p.execbuf.stats().references_held==1);
        NativeAdrenoAbiBoFree released{handle};
        CHECK(p.client.freeBo(&released)==kIOReturnBusy); /* job outstanding */
        NativeAdrenoAbiWaitFence wait{1,0};
        CHECK(p.client.waitFence(&wait,&ringclock)==kIOReturnTimeout && p.execbuf.outstanding()==1);
        /* Earlier fences are satisfied by ring order: wait_fence 1 (issued, still running) submits at once. */
        NativeAdrenoAbiSubmit second=submit;second.wait_fence=1;second.wait_timeout_ns=UINT64_C(5000000000);
        const uint64_t before=clock_us;
        CHECK(p.client.submit(&second,&reply,&ringclock)==kIOReturnSuccess && reply.fence==2 && clock_us==before);
        /* A fence the ring never issued, or one wider than 32 bits, is refused before anything is written. */
        const uint32_t wptr=cp.regs[0x0807];
        NativeAdrenoAbiSubmit future=submit;future.wait_fence=3;
        CHECK(p.client.submit(&future,&reply,&ringclock)==kIOReturnBadArgument && !reply.fence && cp.regs[0x0807]==wptr);
        future.wait_fence=UINT64_C(1)<<32|1;
        CHECK(p.client.submit(&future,&reply,&ringclock)==kIOReturnBadArgument && cp.regs[0x0807]==wptr);
        CHECK(p.client.validateWaitFence(0)==kIOReturnSuccess && p.client.validateWaitFence(2)==kIOReturnSuccess &&
            p.client.validateWaitFence(3)==kIOReturnBadArgument);
        complete(2);
        wait.timeout_ns=UINT64_C(1000000);
        CHECK(p.client.waitFence(&wait,&ringclock)==kIOReturnSuccess);
        wait.fence=2;CHECK(p.client.waitFence(&wait,&ringclock)==kIOReturnSuccess && p.execbuf.outstanding()==0);
        wait.fence=UINT64_C(1)<<32|2;CHECK(p.client.waitFence(&wait,&ringclock)==kIOReturnBadArgument);
        NativeAdrenoAbiMetadata meta{};
        meta.handle=handle;meta.bytes=8;
        std::memcpy(meta.text,"ring-buf",8);
        CHECK(p.client.setMetadata(&meta)==kIOReturnSuccess);
        NativeAdrenoAbiMetadata got{};
        got.handle=handle;got.bytes=16;
        CHECK(p.client.getMetadata(&got,&got)==kIOReturnSuccess);
        CHECK(got.bytes==8 && !std::memcmp(got.text,"ring-buf",8));
        got.bytes=4;CHECK(p.client.getMetadata(&got,&got)==kIOReturnBadArgument);
        meta.bytes=65;CHECK(p.client.setMetadata(&meta)==kIOReturnBadArgument);
        meta.bytes=8;meta.handle=handle+9;
        CHECK(p.client.setMetadata(&meta)==kIOReturnBadArgument);
        CHECK(p.client.freeBo(&released)==kIOReturnSuccess);
        CHECK(p.client.liveObjects()==0 && !p.client.mapped(handle));
        CHECK(p.client.close()==kIOReturnSuccess);
        ++blocks;
    }
    /* Faults per connection (M7, ARCHITECTURE 4.13.4): two connections in their own windows; a GPU page fault charges only the
     * connection whose window holds FAR, a global loss every connection, a recovery a suspend each. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        static const NativeAdrenoClientWindow windows[NativeAdrenoMaxClients]={{Base,0x4000},{Base+0x4000,0x4000},
            {Base+0x8000,0x4000},{Base+0xC000,0x4000}};
        NativeAdrenoUserClient other;
        NativeAdrenoStatus mine,theirs;
        CHECK(other.open(NativeAdrenoAbiVersion,&p.vm,&p.execbuf,&p.ring,32,36)==kIOReturnSuccess);
        CHECK(p.client.attachStatus(&mine)==kIOReturnSuccess && other.attachStatus(&theirs)==kIOReturnSuccess);
        CHECK(p.client.setScope(0,windows[0].start,windows[0].bytes)==kIOReturnSuccess);
        CHECK(other.setScope(NativeAdrenoAbiMaxQueues,windows[1].start,windows[1].bytes)==kIOReturnSuccess);
        NativeAdrenoUserClient *bySlot[NativeAdrenoMaxClients]={&p.client,&other,nullptr,nullptr};
        NativeAdrenoStatus *statusOf[NativeAdrenoMaxClients]={&mine,&theirs,nullptr,nullptr};
        const uint64_t fars[]={Base+0x4234,Base+0x1000,Base+0x7fff,0x100000,Base+0x9000};
        for(uint64_t far:fars) {
            const unsigned slot=NativeAdrenoClientSlotAt(windows,far);
            if(slot<NativeAdrenoMaxClients && bySlot[slot]) {CHECK(bySlot[slot]->windowContains(far));statusOf[slot]->noteFault();}
        }
        NativeAdrenoAbiStatus a{},b{};
        CHECK(p.client.dispatch(NativeAdrenoMethodGetStatus,nullptr,0,&a,sizeof(a),&ringclock)==kIOReturnSuccess);
        CHECK(other.dispatch(NativeAdrenoMethodGetStatus,nullptr,0,&b,sizeof(b),&ringclock)==kIOReturnSuccess);
        CHECK(a.faults==1 && b.faults==2 && !a.suspends && !b.suspends);   /* 0x100000: the kernel range; Base+0x9000: a free window */
        CHECK(!p.client.windowContains(Base+0x4000) && other.windowContains(Base+0x4000) && !other.windowContains(Base+0x8000));
        mine.noteFault();theirs.noteFault();mine.noteSuspend();theirs.noteSuspend();   /* a global loss, then a recovery */
        CHECK(p.client.status(&a)==kIOReturnSuccess && other.status(&b)==kIOReturnSuccess);
        CHECK(a.faults==2 && b.faults==3 && a.suspends==1 && b.suspends==1);
        CHECK(p.client.close()==kIOReturnSuccess && other.close()==kIOReturnSuccess);
        ++blocks;
    }
    /* The windows of ARCHITECTURE 6.2 and the shell's open policy (pure functions of NativeAdrenoPublish.h): thirty-two slots. The
     * 4 GiB aperture: slot 0 the render server's 1.5 GiB, three large (352 MiB) and eight small (124 MiB) windows filling its client
     * range without a gap, slots 12-31 without a window. The 16 GiB aperture (pagani-gpu-va=16): slot 0 the same, three large (512
     * MiB) and twenty-eight small (448 MiB, the last up to 0x3FFFFE000) windows. The 2 GiB aperture keeps the old four windows. */
    {
        const NativeAdrenoClientWindow *w4=NativeAdrenoClientWindowsFor(0x20000000ull+0xE0000000ull);
        const NativeAdrenoClientWindow *w2=NativeAdrenoClientWindowsFor(0x20000000ull+0x60000000ull);
        const NativeAdrenoClientWindow *w16=NativeAdrenoClientWindowsFor(0x20000000ull+0x3E0000000ull);
        CHECK(w4==NativeAdrenoClientWindows4G && w2==NativeAdrenoClientWindows2G && NativeAdrenoClientWindowsFor(0)==w4 &&
              w16==NativeAdrenoClientWindows16G && NativeAdrenoClientWindowsFor(0x100000000ull)==w4 &&
              NativeAdrenoClientWindowsFor(0x100001000ull)==w16 && NativeAdrenoClientWindowsFor(0x80000000ull)==w2);
        CHECK(NativeAdrenoMaxClients==32 && NativeAdrenoLargeSlots==3 && unsigned(NativeAdrenoMaxClients)<=unsigned(NativeAdrenoSubmitRanges));
        enum : unsigned {Windows4G=12};
        CHECK(w4[0].start==0x20000000 && w4[0].bytes==0x60000000 && w4[1].start==0x80000000);
        for(unsigned i=1;i<Windows4G;++i) {
            CHECK(w4[i-1].start+w4[i-1].bytes==w4[i].start && !(w4[i].start&0xFFF) && !(w4[i].bytes&0xFFF));   /* contiguous, no overlap */
            if(i<=NativeAdrenoLargeSlots)CHECK(w4[i].bytes==0x16000000 && w4[i].start==0x80000000ull+(i-1)*0x16000000ull);
            else CHECK(w4[i].start==0xC2000000ull+(i-1-NativeAdrenoLargeSlots)*0x07C00000ull &&
                       w4[i].bytes==(i+1<Windows4G?0x07C00000ull:0x07BFE000ull));
        }
        for(unsigned i=Windows4G;i<NativeAdrenoMaxClients;++i)CHECK(!w4[i].bytes);
        CHECK(NativeAdrenoClientWindowsEnd(w4)==0xFFFFE000ull && NativeAdrenoClientWindowsEnd(w4)<=(UINT64_C(1)<<32));
        uint64_t total=0;
        for(unsigned i=0;i<NativeAdrenoMaxClients;++i)total+=w4[i].bytes;
        CHECK(total==0xFFFFE000ull-0x20000000ull);   /* the whole client range of the 4 GiB aperture */
        CHECK(w16[0].start==0x20000000 && w16[0].bytes==0x60000000 && w16[1].start==0x80000000);
        for(unsigned i=1;i<NativeAdrenoMaxClients;++i) {
            /* Slots 5, 14 and 23 start on a multiple of 4 GiB, the 64 MiB under it nobody's: no window, so no BO, crosses one. */
            const unsigned lines=(i>=5)+(i>=14)+(i>=23);
            const uint64_t skipped=(i==5 || i==14 || i==23)?0x4000000ull:0;
            CHECK(w16[i-1].start+w16[i-1].bytes+skipped==w16[i].start && !(w16[i].start&0xFFF) && !(w16[i].bytes&0xFFF));
            CHECK(w16[i].start>>32==(w16[i].start+w16[i].bytes-1)>>32 && (!skipped || !(w16[i].start&0xFFFFFFFFull)));
            if(i<=NativeAdrenoLargeSlots)CHECK(w16[i].bytes==0x20000000 && w16[i].start==0x80000000ull+(i-1)*0x20000000ull);
            else CHECK(w16[i].start==0xE0000000ull+(i-1-NativeAdrenoLargeSlots)*0x1C000000ull+lines*0x4000000ull &&
                       w16[i].bytes==(i+1<NativeAdrenoMaxClients?0x1C000000ull:0x1FFFE000ull));
        }
        total=0;
        for(unsigned i=0;i<NativeAdrenoMaxClients;++i)total+=w16[i].bytes;
        CHECK(total==0x3FFFFE000ull-0x20000000ull-3*0x4000000ull && NativeAdrenoClientWindowsEnd(w16)==0x3FFFFE000ull &&
              NativeAdrenoClientWindowsEnd(w16)<=(UINT64_C(1)<<34) && NativeAdrenoClientWindowsEnd(w16)>(UINT64_C(1)<<33));
        CHECK(w2[0].start==0x20000000 && w2[0].bytes==0x40000000 && w2[1].start==0x60000000 && w2[2].start==0x6AAA9000 &&
            w2[3].start==0x75552000 && w2[3].bytes==0x0AAA9000 && NativeAdrenoClientWindowsEnd(w2)==0x7FFFB000ull);
        for(unsigned i=4;i<NativeAdrenoMaxClients;++i)CHECK(!w2[i].bytes);
        CHECK(NativeAdrenoClientWindowsValid(w4,0x20000000ull,0xFFFFE000ull) && NativeAdrenoClientWindowsValid(w2,0x20000000ull,0x80000000ull) &&
              NativeAdrenoClientWindowsValid(w16,0x20000000ull,0x3FFFFE000ull) && !NativeAdrenoClientWindowsValid(w16,0x20000000ull,0x3FFFFD000ull));
        const NativeAdrenoClientWindow overlap[NativeAdrenoMaxClients]={{0x20000000,0x2000},{0x21000,0x1000}};
        const NativeAdrenoClientWindow hole[NativeAdrenoMaxClients]={{0x20000000,0x1000},{0,0},{0x30000000,0x1000}};
        CHECK(!NativeAdrenoClientWindowsValid(overlap,0x20000000ull,0x80000000ull) &&
              !NativeAdrenoClientWindowsValid(hole,0x20000000ull,0x80000000ull) && !NativeAdrenoClientWindowsValid(w4,0x20000000ull,0xFFFFD000ull));
        CHECK(NativeAdrenoClientSlotAt(w4,0x1FFFF000)==NativeAdrenoMaxClients && NativeAdrenoClientSlotAt(w4,0x20000000)==0);
        for(unsigned i=0;i<Windows4G;++i)
            CHECK(NativeAdrenoClientSlotAt(w4,w4[i].start)==i && NativeAdrenoClientSlotAt(w4,w4[i].start+w4[i].bytes-1)==i);
        for(unsigned i=0;i<NativeAdrenoMaxClients;++i)
            CHECK(NativeAdrenoClientSlotAt(w16,w16[i].start)==i && NativeAdrenoClientSlotAt(w16,w16[i].start+w16[i].bytes-1)==i);
        CHECK(NativeAdrenoClientSlotAt(w4,0x7FFFFFFF)==0 && NativeAdrenoClientSlotAt(w4,0x80000000)==1 &&
            NativeAdrenoClientSlotAt(w4,0xC1FFFFFF)==3 && NativeAdrenoClientSlotAt(w4,0xC2000000)==4 &&
            NativeAdrenoClientSlotAt(w4,0xFFFFDFFF)==Windows4G-1 && NativeAdrenoClientSlotAt(w4,0xFFFFE000)==NativeAdrenoMaxClients &&
            NativeAdrenoClientSlotAt(w4,0x200000000ull)==NativeAdrenoMaxClients);
        CHECK(NativeAdrenoClientSlotAt(w16,0x3FFFFDFFFull)==NativeAdrenoMaxClients-1 && NativeAdrenoClientSlotAt(w16,0x3FFFFE000ull)==NativeAdrenoMaxClients &&
            NativeAdrenoClientSlotAt(w16,0xDFFFFFFF)==3 && NativeAdrenoClientSlotAt(w16,0xE0000000)==4 &&
            NativeAdrenoClientSlotAt(w16,0x400000000ull)==NativeAdrenoMaxClients);
        CHECK(NativeAdrenoClientSlotAt(w2,0x7FFFFFFF)==NativeAdrenoMaxClients && NativeAdrenoClientSlotAt(w2,0x75552000)==3 &&
            NativeAdrenoClientSlotAt(w2,0)==NativeAdrenoMaxClients && NativeAdrenoClientSlotAt(w2,0xC2000000)==NativeAdrenoMaxClients);
        /* Slot choice (ARCHITECTURE 6.2): the render server takes slot 0, its second connection and the large class a free large slot
         * first and then a small one. A small-class process the grant list names (the poster extensions) takes a free small slot, else
         * borrows a free large one; any other process (an app, an extension) takes a small slot only while more than three of the
         * 4 GiB table's eight (four of the 16 GiB table's 28) are free and never a large one, so the listed clients find a window
         * however many apps hold one (the lock-screen poster went black when they did not). Slot 0 is never another process's. */
        bool taken[NativeAdrenoMaxClients]={};
        CHECK(NativeAdrenoListedSmallKeep4G==3 && NativeAdrenoListedSmallKeep16G==4);
        CHECK(NativeAdrenoClientSlotFor(true,false,true,w4,taken)==0 && NativeAdrenoClientSlotFor(false,true,true,w4,taken)==1 &&
              NativeAdrenoClientSlotFor(false,false,true,w4,taken)==NativeAdrenoLargeSlots+1 &&
              NativeAdrenoClientSlotFor(false,false,false,w4,taken)==NativeAdrenoLargeSlots+1);
        CHECK(NativeAdrenoClientSlotFor(false,false,true,nullptr,taken)==NativeAdrenoMaxClients &&
              NativeAdrenoClientSlotFor(false,false,true,w4,nullptr)==NativeAdrenoMaxClients);
        taken[0]=true;CHECK(NativeAdrenoClientSlotFor(true,false,true,w4,taken)==1);   /* a second render-server connection */
        unsigned order[NativeAdrenoMaxClients]={},count=0;
        for(unsigned s;(s=NativeAdrenoClientSlotFor(false,false,false,w4,taken))<NativeAdrenoMaxClients;taken[s]=true)order[count++]=s;
        CHECK(count==Windows4G-1-NativeAdrenoLargeSlots-NativeAdrenoListedSmallKeep4G);   /* apps: slots 4-8, the last three kept */
        for(unsigned i=0;i<count;++i)CHECK(order[i]==NativeAdrenoLargeSlots+1+i);
        count=0;
        for(unsigned s;(s=NativeAdrenoClientSlotFor(false,false,true,w4,taken))<NativeAdrenoMaxClients;taken[s]=true)order[count++]=s;
        CHECK(count==NativeAdrenoListedSmallKeep4G+NativeAdrenoLargeSlots);   /* posters: the kept 9-11, then the large 1-3 */
        CHECK(order[0]==9 && order[1]==10 && order[2]==11 && order[3]==1 && order[4]==2 && order[5]==3);
        CHECK(NativeAdrenoClientSlotFor(true,false,true,w4,taken)==NativeAdrenoMaxClients &&
              NativeAdrenoClientSlotFor(false,true,true,w4,taken)==NativeAdrenoMaxClients);
        taken[0]=false;CHECK(NativeAdrenoClientSlotFor(false,true,true,w4,taken)==NativeAdrenoMaxClients);   /* slot 0 is the render server's */
        taken[0]=true;taken[2]=false;   /* a large slot comes free: never an app's, even with every small slot taken */
        CHECK(NativeAdrenoClientSlotFor(false,false,false,w4,taken)==NativeAdrenoMaxClients &&
              NativeAdrenoClientSlotFor(false,false,true,w4,taken)==2 && NativeAdrenoClientSlotFor(false,true,true,w4,taken)==2);
        taken[6]=false;   /* and a small one: a listed process takes the small one, the large class the large one, an app neither */
        CHECK(NativeAdrenoClientSlotFor(false,false,true,w4,taken)==6 && NativeAdrenoClientSlotFor(false,true,true,w4,taken)==2 &&
              NativeAdrenoClientSlotFor(false,false,false,w4,taken)==NativeAdrenoMaxClients);
        taken[2]=true;CHECK(NativeAdrenoClientSlotFor(false,true,true,w4,taken)==6);   /* large slots full: the large class a small one */
        for(unsigned i=7;i<=8;++i)taken[i]=false;   /* 6-8 free, three small ones: still none for an app */
        CHECK(NativeAdrenoClientSlotFor(false,false,false,w4,taken)==NativeAdrenoMaxClients);
        taken[5]=false;CHECK(NativeAdrenoClientSlotFor(false,false,false,w4,taken)==5);   /* four: the first */
        /* The 16 GiB table: apps take 24 of the 28 small slots, the posters the last four and then the large ones. */
        for(auto &t:taken)t=false;
        taken[0]=true;count=0;
        for(unsigned s;(s=NativeAdrenoClientSlotFor(false,false,false,w16,taken))<NativeAdrenoMaxClients;taken[s]=true)order[count++]=s;
        CHECK(count==NativeAdrenoMaxClients-1-NativeAdrenoLargeSlots-NativeAdrenoListedSmallKeep16G && order[0]==4 &&
              order[count-1]==NativeAdrenoMaxClients-1-NativeAdrenoListedSmallKeep16G);
        count=0;
        for(unsigned s;(s=NativeAdrenoClientSlotFor(false,false,true,w16,taken))<NativeAdrenoMaxClients;taken[s]=true)order[count++]=s;
        CHECK(count==NativeAdrenoListedSmallKeep16G+NativeAdrenoLargeSlots && order[0]==28 && order[3]==31 && order[4]==1);
        for(unsigned i=1;i<=NativeAdrenoLargeSlots;++i)taken[i]=false;
        CHECK(NativeAdrenoClientSlotFor(false,true,true,w16,taken)==1 && NativeAdrenoClientSlotFor(true,false,true,w16,taken)==1 &&
              NativeAdrenoClientSlotFor(false,false,false,w16,taken)==NativeAdrenoMaxClients);
        /* The 2 GiB table has no small window: only listed processes get one of 1-3, slots 4-31 never. */
        for(auto &t:taken)t=false;
        CHECK(NativeAdrenoClientSlotFor(false,false,true,w2,taken)==1 && NativeAdrenoClientSlotFor(true,false,true,w2,taken)==0 &&
              NativeAdrenoClientSlotFor(false,false,false,w2,taken)==NativeAdrenoMaxClients);
        taken[0]=taken[1]=taken[2]=true;
        CHECK(NativeAdrenoClientSlotFor(false,false,true,w2,taken)==3 && NativeAdrenoClientSlotFor(false,true,true,w2,taken)==3);
        taken[3]=true;
        CHECK(NativeAdrenoClientSlotFor(false,false,true,w2,taken)==NativeAdrenoMaxClients &&
              NativeAdrenoClientSlotFor(true,false,true,w2,taken)==NativeAdrenoMaxClients);
        /* The refusal's line (6.2): the opener, its class, the free small windows against the kept ones, the holders with their quiet
         * seconds, "~" for an orphan, names cut to 10 characters, what the rate limit dropped. */
        bool held[NativeAdrenoMaxClients]={};
        for(unsigned s=0;s<9;++s)held[s]=true;
        CHECK(NativeAdrenoClientSmallFree(w4,held)==3 && NativeAdrenoClientSmallFree(w16,held)==23 && NativeAdrenoClientSmallFree(w2,held)==0 &&
              NativeAdrenoClientSmallFree(nullptr,held)==0 && NativeAdrenoListedSmallKeep(w4)==3 && NativeAdrenoListedSmallKeep(w16)==4 &&
              NativeAdrenoClientSlotFor(false,false,false,w4,held)==NativeAdrenoMaxClients);
        NativeAdrenoSlotHolder holders[NativeAdrenoMaxClients]={};
        holders[0]={"backboardd",0,false};holders[1]={"WidgetRenderer_Default",12,false};holders[4]={"Maps",301,false};
        holders[5]={"MobileSlideShow",100000,true};
        char slotLine[NativeAdrenoSlotLineBytes];
        NativeAdrenoSlotLine(slotLine,"Spotlight",'a',3,3,0,holders);
        CHECK(!strcmp(slotLine,"gpu noslot Spotlight a small 3/3 0:backboardd/0 1:WidgetRend/12 4:Maps/301 5:~MobileSlid/9999"));
        NativeAdrenoSlotHolder none[NativeAdrenoMaxClients]={};
        NativeAdrenoSlotLine(slotLine,"AegirPoster",'l',0,4,7,none);
        CHECK(!strcmp(slotLine,"gpu noslot AegirPoster l small 0/4 +7"));
        /* The longest line fits: thirty-two holders, the name and the numbers at their widest. */
        NativeAdrenoSlotHolder full[NativeAdrenoMaxClients];
        for(auto &holder:full)holder={"WidgetRenderer_Default",9999,true};
        NativeAdrenoSlotLine(slotLine,"WidgetRenderer_Defa",'a',28,4,UINT32_MAX,full);
        CHECK(strlen(slotLine)<NativeAdrenoSlotLineBytes-1 && !strcmp(slotLine+strlen(slotLine)-20," 31:~WidgetRend/9999"));
        /* The first address past 4 GiB (6.2): slot 5 of the 16 GiB table starts on the line, so its client's first mapping is there; a mapping's end is
         * exclusive, so one that ends at 4 GiB, or in the gap under the line, is still below. A page of a gap belongs to no client. */
        CHECK(w16[5].start==NativeAdrenoVa4G && w16[4].start+w16[4].bytes==NativeAdrenoVa4G-0x4000000ull);
        CHECK(NativeAdrenoClientSlotAt(w16,NativeAdrenoVa4G-0x1000)==NativeAdrenoMaxClients && NativeAdrenoClientSlotAt(w16,NativeAdrenoVa4G)==5);
        CHECK(!NativeAdrenoPast4G(0) && !NativeAdrenoPast4G(w16[4].start+w16[4].bytes) && NativeAdrenoPast4G(w16[5].start+0x2000) &&
              !NativeAdrenoPast4G(NativeAdrenoVa4G) && NativeAdrenoPast4G(NativeAdrenoVa4G+0x1000) &&
              NativeAdrenoPast4G(w16[6].start+0x1000) && NativeAdrenoPast4G(0x3FFFFE000ull));
        for(unsigned s=0;s<NativeAdrenoMaxClients;++s)   /* no window of the smaller tables gets there */
            CHECK(!w4[s].bytes || !NativeAdrenoPast4G(w4[s].start+w4[s].bytes));
        char pastLine[NativeAdrenoPast4GLineBytes];
        NativeAdrenoPast4GLine(pastLine,"FitnessWidget",5,0xFFFFE000ull,0x100002000ull);
        CHECK(!strcmp(pastLine,"gpu va past 4 GiB: FitnessWidget slot 5 iova 0xffffe000 end 0x100002000"));
        NativeAdrenoPast4GLine(pastLine,"x",31,0,NativeAdrenoVa4G);
        CHECK(!strcmp(pastLine,"gpu va past 4 GiB: x slot 31 iova 0x0 end 0x100000000"));
        NativeAdrenoPast4GLine(pastLine,"WidgetRenderer_Default",31,UINT64_MAX,UINT64_MAX);
        CHECK(!strcmp(pastLine,"gpu va past 4 GiB: WidgetRenderer_Defa slot 31 iova 0xffffffffffffffff end 0xffffffffffffffff") &&
              strlen(pastLine)<NativeAdrenoPast4GLineBytes-1);
        /* One line per 5 s, 40 a boot, the rest counted. */
        NativeAdrenoSlotLog slotLog{};
        CHECK(NativeAdrenoSlotLogDue(&slotLog,1000) && !NativeAdrenoSlotLogDue(&slotLog,1001) && !NativeAdrenoSlotLogDue(&slotLog,5000999) &&
              NativeAdrenoSlotLogDue(&slotLog,5001000) && slotLog.lines==2 && slotLog.skipped==2);
        for(uint64_t now=20000000;slotLog.lines<NativeAdrenoSlotLogLines;now+=NativeAdrenoSlotLogUs)CHECK(NativeAdrenoSlotLogDue(&slotLog,now));
        CHECK(!NativeAdrenoSlotLogDue(&slotLog,UINT64_C(1)<<40));
        /* Objects per connection: the render server's (and the kernel client's) 16384, a large client's 8192, everyone else's 4096. */
        CHECK(NativeAdrenoClientObjectsFor(true,false)==16384 && NativeAdrenoClientObjectsFor(true,true)==16384 &&
              NativeAdrenoClientObjectsFor(false,true)==8192 && NativeAdrenoClientObjectsFor(false,false)==4096);
        /* What mount() gives besides the slot: the render server's connections 16384 objects and no reserve in any slot (a backboardd
         * restarted while its predecessor's orphan held slot 0 lands in a large one and must map as the render server), the mailbox
         * only in slot 0; the kernel client 16384 and no reserve; a large client 8192 and the reserve; everyone else 4096 and it. */
        for(unsigned slot:{0u,1u,5u}) {
            const NativeAdrenoClientTerms render=NativeAdrenoClientTermsFor(false,true,false,slot);
            CHECK(render.objects==16384 && !render.reserve && render.mailbox==(slot==0));
        }
        const NativeAdrenoClientTerms kernelTerms=NativeAdrenoClientTermsFor(true,false,false,0),
            largeTerms=NativeAdrenoClientTermsFor(false,false,true,1),smallTerms=NativeAdrenoClientTermsFor(false,false,false,7);
        CHECK(kernelTerms.objects==16384 && !kernelTerms.reserve && !kernelTerms.mailbox && largeTerms.objects==8192 &&
              largeTerms.reserve && !largeTerms.mailbox && smallTerms.objects==4096 && smallTerms.reserve && !smallTerms.mailbox);
        /* The per-name rows (64 in the shell): a name finds its own row, a new one the first unused row; with every row used, the
         * row of a name with no live connection and no hang (the fewest opens first) is cleared for it. Rows charged with hangs and
         * rows of live names are never given up, so a name refused for its hangs stays refused however many apps opened since.
         * Liveness (a walk of the 32 slots in the shell) is asked only with every row used: a name's own row, which every BoMap looks
         * up under g.lock, and an unused one are found without it. */
        {
            NativeAdrenoClientRecord rows[4]={};
            std::vector<std::string> live;
            unsigned asked=0;
            auto isLive=[&](const char *name) {++asked;for(const std::string &n:live)if(n==name)return true;return false;};
            bool fresh=false;
            NativeAdrenoClientRecord *sms=NativeAdrenoClientRecordFor(rows,4,"MobileSMS",isLive,&fresh);
            CHECK(sms==&rows[0] && fresh && !std::strcmp(sms->name,"MobileSMS") && sms->slot==0xff);
            CHECK(NativeAdrenoClientRecordFor(rows,4,"MobileSMS",isLive,&fresh)==sms && !fresh);
            sms->opens=3;
            NativeAdrenoClientRecord *aegir=NativeAdrenoClientRecordFor(rows,4,"AegirPoster",isLive,&fresh);
            NativeAdrenoClientRecord *board=NativeAdrenoClientRecordFor(rows,4,"PosterBoard",isLive,&fresh);
            NativeAdrenoClientRecord *weather=NativeAdrenoClientRecordFor(rows,4,"WeatherPoster",isLive,&fresh);
            CHECK(aegir==&rows[1] && board==&rows[2] && weather==&rows[3] && fresh && !asked);
            aegir->hangs=NativeAdrenoClientHangLimit;aegir->opens=1;board->opens=1;board->slot=2;live.push_back("PosterBoard");
            weather->opens=2;weather->refused=4;weather->closes=2;weather->peak_mib=9;weather->last_refusal=7;
            CHECK(NativeAdrenoClientRecordFor(rows,4,"WeatherPoster",isLive,&fresh)==weather && !fresh && !asked);
            NativeAdrenoClientRecord *maps=NativeAdrenoClientRecordFor(rows,4,"Maps",isLive,&fresh);
            CHECK(asked);
            CHECK(maps==weather && fresh && !std::strcmp(maps->name,"Maps") && !maps->opens && !maps->refused && !maps->closes &&
                  !maps->peak_mib && !maps->last_refusal && !maps->hangs && maps->slot==0xff);
            maps->opens=5;live.push_back("Maps");
            CHECK(NativeAdrenoClientRecordFor(rows,4,"Notes",isLive,&fresh)==sms && fresh && !std::strcmp(sms->name,"Notes"));
            live.push_back("Notes");
            CHECK(!NativeAdrenoClientRecordFor(rows,4,"Calendar",isLive,&fresh) && !fresh);   /* live or charged: none given up */
            CHECK(NativeAdrenoClientRecordFor(rows,4,"AegirPoster",isLive,&fresh)==aegir && !fresh &&
                  NativeAdrenoOpenRefusal(false,0,0,aegir->hangs)==NativeAdrenoReturnNotPermitted);
            live.clear();rows[0].opens=4;
            CHECK(NativeAdrenoClientRecordFor(rows,4,"Calendar",isLive,&fresh)==board && fresh);   /* opens 1 before Notes' 4, Maps' 5 */
            NativeAdrenoClientRecord one[1]={};   /* names as the 20-byte buffer holds them: 19 characters compared and kept */
            NativeAdrenoClientRecord *wide=NativeAdrenoClientRecordFor(one,1,"WidgetRenderer_Defa",isLive,&fresh);
            wide->hangs=1;
            CHECK(wide==one && !std::strcmp(wide->name,"WidgetRenderer_Defa") &&
                  NativeAdrenoClientRecordFor(one,1,"WidgetRenderer_Defa",isLive,&fresh)==wide && !fresh &&
                  NativeAdrenoClientRecordFor(one,1,"WidgetRenderer_Def",isLive,&fresh)==nullptr);
            CHECK(!NativeAdrenoClientRecordFor(nullptr,4,"x",isLive,&fresh) && !NativeAdrenoClientRecordFor(rows,4,nullptr,isLive,&fresh));
        }
        /* The process lists. With 17 bytes the kernel's grant (zzw-native-adreno-sandbox-grant.patch) sees 16 characters of a name: a long
         * entry matches the truncated name, a short one only the whole name. The publish shell compares 19 characters (20-byte buffer). */
        const char *granted[]={"backboardd","iconservicesagen","WidgetRenderer_D","PosterBoard","AegirPoster","KaleidoscopePost",
            "PhotosPosterProv","AmbientPhotoFram","MercuryPosterExt","MonogramPosterEx","HelloPosterExten","PridePosterExten",
            "RhizomePoster","ExtragalacticPos","EmojiPosterExten","AvatarPosterExte","WeatherPoster","LegacyPoster"};
        for(const char *name:granted)CHECK(NativeAdrenoListNamesProcess(NativeAdrenoSandboxGrantList,name,17));
        const char *refused[]={"SpringBoard","Poster","PosterBoardX","backboard","WidgetRenderer","GradientPosterEx","CollectionsPoste",
            "UnityPosterExten","ClockPosterExten","InfographPoster","","mediaserverd"};
        for(const char *name:refused)CHECK(!NativeAdrenoListNamesProcess(NativeAdrenoSandboxGrantList,name,17));
        CHECK(!NativeAdrenoListNamesProcess(nullptr,"backboardd",17) && !NativeAdrenoListNamesProcess(NativeAdrenoSandboxGrantList,nullptr,17) &&
              !NativeAdrenoListNamesProcess(NativeAdrenoSandboxGrantList,"backboardd",1) && !NativeAdrenoListNamesProcess(",,","",17));
        unsigned entries=0;
        for(const char *at=NativeAdrenoSandboxGrantList;*at;++entries) {   /* every entry, as proc_selfname gives it, is granted */
            char name[17]={};
            size_t length=0;
            while(at[length] && at[length]!=',')++length;
            CHECK(length>0 && length<32);
            std::memcpy(name,at,length<16?length:16);
            CHECK(NativeAdrenoListNamesProcess(NativeAdrenoSandboxGrantList,name,17));
            char full[20]={};std::memcpy(full,at,length<19?length:19);
            CHECK(NativeAdrenoListNamesProcess(NativeAdrenoSandboxGrantList,full,20));
            CHECK(NativeAdrenoListNamesProcess(NativeAdrenoLargeClientList,full,20)==
                  (!std::strncmp(at,"WidgetRenderer_Default",length) || !std::strncmp(at,"PosterBoard",length) ||
                   !std::strncmp(at,"iconservicesagent",length)));
            at+=length+(at[length]==','?1:0);
        }
        CHECK(entries==sizeof granted/sizeof granted[0]);
        CHECK(NativeAdrenoListNamesProcess(NativeAdrenoLargeClientList,"WidgetRenderer_Defa",20) &&
              NativeAdrenoListNamesProcess(NativeAdrenoLargeClientList,"PosterBoard",20) &&
              NativeAdrenoListNamesProcess(NativeAdrenoLargeClientList,"iconservicesagent",20) &&
              !NativeAdrenoListNamesProcess(NativeAdrenoLargeClientList,"backboardd",20) &&
              !NativeAdrenoListNamesProcess(NativeAdrenoLargeClientList,"AegirPoster",20) &&
              !NativeAdrenoListNamesProcess(NativeAdrenoLargeClientList,"WidgetRenderer_Def",20));
        /* A global loss refuses; no per-process cap beyond sheng-gpu-metal-opens (the old eight per boot is gone); a name whose jobs
         * hung the GPU twice is refused (the shell never passes the render server's hangs). */
        CHECK(NativeAdrenoOpenRefusal(false,0,1000)==kIOReturnSuccess && NativeAdrenoOpenRefusal(false,40,39)==kIOReturnSuccess);
        CHECK(NativeAdrenoOpenRefusal(false,40,40)==kIOReturnBusy && NativeAdrenoOpenRefusal(true,0,0)==NativeAdrenoReturnNotResponding);
        CHECK(NativeAdrenoOpenRefusal(false,0,8)==kIOReturnSuccess && NativeAdrenoOpenRefusal(false,0,0,1)==kIOReturnSuccess);
        CHECK(NativeAdrenoOpenRefusal(false,0,0,NativeAdrenoClientHangLimit)==NativeAdrenoReturnNotPermitted &&
              NativeAdrenoOpenRefusal(true,0,0,9)==NativeAdrenoReturnNotResponding && NativeAdrenoClientHangLimit==2 &&
              NativeAdrenoReturnNotPermitted==IOReturn(0xe00002e2u));
        CHECK(NativeAdrenoHandoffLast==0x7005 && NativeAdrenoHandoffBase==0x7000 && NativeAdrenoScanoutType==0x5343);
        /* M9: BAND (0x7004) and WAIT_SWAP2 (0x7005) reach the presenter; 0x7006 and the export method 0x7100 do not. */
        CHECK(NativeAdrenoIsHandoffMethod(0x7000) && NativeAdrenoIsHandoffMethod(0x7003) && NativeAdrenoIsHandoffMethod(0x7004) &&
            NativeAdrenoIsHandoffMethod(0x7005) && !NativeAdrenoIsHandoffMethod(0x7006) && !NativeAdrenoIsHandoffMethod(0x6fff) &&
            !NativeAdrenoIsHandoffMethod(0x7100) && !NativeAdrenoIsHandoffMethod(NativeAdrenoFrontendMethod) && !NativeAdrenoIsHandoffMethod(NativeAdrenoMethodSubmit));
        ++blocks;
    }
    /* Host import: a caller's pages become a BO with scattered spans and no CPU-map token; the dispatch path (no task) refuses it.
     * Maintenance (ARCHITECTURE 4.14.1, coherence level 0): a GPU-written import is cleaned and invalidated once when mapped and never
     * again; a GPU-read-only import is cleaned before each submission (the VM's maintained list); own BOs are never maintained. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        void *task=reinterpret_cast<void *>(0x77);
        NativeAdrenoAbiBoAllocReply imported{};
        NativeAdrenoAbiBoImport bad{0x10000001,8192};
        CHECK(p.client.importBo(&bad,task,&imported)==kIOReturnBadArgument && !imported.handle);
        bad={0x10000000,0};CHECK(p.client.importBo(&bad,task,&imported)==kIOReturnBadArgument);
        NativeAdrenoAbiBoImport request_{0x10000000,4*4096};
        CHECK(p.client.importBo(&request_,nullptr,&imported)==kIOReturnBadArgument);
        const unsigned before=fixture.imports_live;
        CHECK(p.client.importBo(&request_,task,&imported)==kIOReturnSuccess && imported.handle);
        CHECK(fixture.imports_live==before+1);
        NativeAdrenoMemory *memory=p.client.memoryOf(imported.handle);
        CHECK(memory && memory->imported() && memory->spanCount()==4 && !memory->mapping() && !memory->scanout());
        NativeAdrenoAbiBoCpuMap cpu{imported.handle};NativeAdrenoAbiBoCpuMapReply token{};
        CHECK(p.client.cpuMap(&cpu,&token)==kIOReturnUnsupported && !token.token);
        NativeAdrenoAbiBoAllocReply untouched{};
        CHECK(p.client.dispatch(NativeAdrenoMethodBoImport,&request_,sizeof(request_),&untouched,sizeof(untouched),&ringclock)==kIOReturnUnsupported);
        CHECK(!untouched.handle);
        const unsigned physical=fixture.physical_calls;
        NativeAdrenoAbiBoMap map{imported.handle,Base+0x8000,3,0,2,0};NativeAdrenoAbiBoMapReply mapped{};
        CHECK(p.client.mapBo(&map,&mapped)==kIOReturnSuccess && mapped.iova==Base+0x8000);
        CHECK(fixture.physical_calls==physical+4);   /* once at map, one call per scattered span */
        NativeAdrenoAbiBoAllocReply source{};NativeAdrenoAbiBoImport source_request{0x20000000,2*4096};
        CHECK(p.client.importBo(&source_request,task,&source)==kIOReturnSuccess);
        NativeAdrenoAbiBoMap read_only{source.handle,Base+0xC000,1,0,2,0};
        CHECK(p.client.mapBo(&read_only,&mapped)==kIOReturnSuccess && p.vm.maintainedCount()==1);
        const uint64_t ib=allocMapped(p.client,4096,Base);
        NativeAdrenoAbiSubmit submit{};submit.buffer_count=1;submit.buffers[0]={ib,Base,256};
        NativeAdrenoAbiSubmitReply reply{};
        const size_t bytes=fixture.physical_bytes;const unsigned owned=fixture.cache_calls;
        CHECK(p.client.submit(&submit,&reply,&ringclock)==kIOReturnSuccess);
        CHECK(fixture.physical_bytes==bytes+2*4096 && fixture.cache_calls==owned);   /* the read-only import only */
        complete(reply.fence);
        NativeAdrenoAbiWaitFence wait{reply.fence,0};
        CHECK(p.client.waitFence(&wait,&ringclock)==kIOReturnSuccess);
        CHECK(fixture.physical_bytes==bytes+2*4096);   /* nothing on completion */
        NativeAdrenoAbiBoFree frees[]={{ib},{source.handle},{imported.handle}};
        for(auto &f:frees)CHECK(p.client.freeBo(&f)==kIOReturnSuccess);
        CHECK(fixture.imports_live==before && p.client.liveObjects()==0);
        /* Memory the owner maps read-only is pinned for reading and mapped read-only, whatever was asked. */
        fixture.write_protected_imports=true;
        NativeAdrenoAbiBoAllocReply ro{};NativeAdrenoAbiBoImport ro_request{0x20000000,2*4096};
        CHECK(p.client.importBo(&ro_request,task,&ro)==kIOReturnSuccess && ro.handle);
        NativeAdrenoMemory *ro_memory=p.client.memoryOf(ro.handle);
        CHECK(ro_memory && ro_memory->readOnly() && ro_memory->imported());
        NativeAdrenoAbiBoMap ro_map{ro.handle,Base+0xa000,3,0,2,0};NativeAdrenoAbiBoMapReply ro_mapped{};
        CHECK(p.client.mapBo(&ro_map,&ro_mapped)==kIOReturnSuccess);
        uint64_t iova=0;size_t length=0;unsigned permissions=0;
        const uint64_t mapping=p.vm.handleAt(Base+0xa000);
        CHECK(mapping && p.vm.describe(mapping,&iova,&length,&permissions)==kIOReturnSuccess && iova==Base+0xa000 &&
            permissions==ArmSmmu4KRead);   /* asked read/write, mapped read-only */
        NativeAdrenoAbiBoFree ro_free{ro.handle};
        CHECK(p.client.freeBo(&ro_free)==kIOReturnSuccess);
        fixture.write_protected_imports=false;
        fixture.prepare_fail=true;
        CHECK(p.client.importBo(&ro_request,task,&ro)==kIOReturnIOError);
        fixture.prepare_fail=false;
        CHECK(fixture.imports_live==before && p.client.liveObjects()==0);
        CHECK(p.client.close()==kIOReturnSuccess);
        ++blocks;
    }
    /* M9 scanout import (ARCHITECTURE 5.3.2): a range inside the calling task's 0x5343 mapping becomes a BO of the scanout's own
     * physical range (nothing wired), mapped writable and non-cacheable; another task, a straddling or an outside range is an
     * ordinary import. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        void *task=reinterpret_cast<void *>(0x77),*stranger=reinterpret_cast<void *>(0x78);
        IOMemoryDescriptor *scanout=IOMemoryDescriptor::withAddressRange(0x7f000000,0x10000,kIOMemoryMapperNone,task);
        CHECK(scanout);
        const unsigned imports=fixture.imported;
        const NativeAdrenoScanoutMapping mapping{task,0x50000000,0x10000,scanout};
        p.client.setScanout(&mapping);
        uint64_t offset=0;
        CHECK(p.client.isScanoutAddress(task,0x50001000,0x2000,&offset) && offset==0x1000);
        CHECK(!p.client.isScanoutAddress(stranger,0x50001000,0x2000,&offset) && !offset);
        CHECK(!p.client.isScanoutAddress(task,0x5000f000,0x2000,&offset) && !p.client.isScanoutAddress(task,0x4ffff000,0x2000,&offset));
        CHECK(p.client.isScanoutAddress(task,0x50000000,0x10000,&offset) && !offset);
        NativeAdrenoAbiBoImport request_{0x50001000,0x2000};NativeAdrenoAbiBoAllocReply reply{};
        CHECK(p.client.importBo(&request_,task,&reply)==kIOReturnSuccess && reply.handle && fixture.imported==imports);
        NativeAdrenoMemory *memory=p.client.memoryOf(reply.handle);
        CHECK(memory && memory->scanout() && !memory->imported() && memory->bytes()==0x2000 && memory->spanCount()==2);
        CHECK(memory->spans()[0].physical==scanout->physical_base+2*0x1000);   /* the scanout's own pages at offset 0x1000 */
        NativeAdrenoAbiBoCpuMap cpu{reply.handle};NativeAdrenoAbiBoCpuMapReply token{};
        CHECK(p.client.cpuMap(&cpu,&token)==kIOReturnUnsupported);
        NativeAdrenoAbiBoMap ro{reply.handle,Base+0x4000,1,0,2,0},wb{reply.handle,Base+0x4000,3,1,3,0};NativeAdrenoAbiBoMapReply mapped{};
        CHECK(p.client.mapBo(&ro,&mapped)==kIOReturnBadArgument);   /* the GPU writes the scanout */
        const unsigned physical=fixture.physical_calls;
        CHECK(p.client.mapBo(&wb,&mapped)==kIOReturnSuccess && mapped.iova==Base+0x4000 && fixture.physical_calls==physical);
        unsigned writeback=9,shareability=9;
        NativeAdrenoVm::attributes(*memory,3,1,3,&writeback,&shareability);
        CHECK(writeback==0 && shareability==2);   /* non-cacheable whatever was asked */
        NativeAdrenoAbiBoFree free_{reply.handle};
        CHECK(p.client.freeBo(&free_)==kIOReturnSuccess && p.client.liveObjects()==0);
        /* Without the mapping (or for another task) the same address is an ordinary user import. */
        p.client.setScanout(nullptr);
        CHECK(!p.client.isScanoutAddress(task,0x50001000,0x2000,&offset));
        CHECK(p.client.importBo(&request_,task,&reply)==kIOReturnSuccess && fixture.imported==imports+1);
        CHECK(!p.client.memoryOf(reply.handle)->scanout() && p.client.memoryOf(reply.handle)->imported());
        NativeAdrenoAbiBoFree free2{reply.handle};CHECK(p.client.freeBo(&free2)==kIOReturnSuccess);
        const NativeAdrenoScanoutMapping malformed{task,0x50000800,0x10000,scanout};
        p.client.setScanout(&malformed);
        CHECK(!p.client.isScanoutAddress(task,0x50001000,0x1000,&offset));
        scanout->release();
        CHECK(p.client.close()==kIOReturnSuccess);
        ++blocks;
    }
    /* Handle slots (ARCHITECTURE 6.5, M10): FIFO recycling with a 4096-allocation quarantine, monotonic handles by default, the slot
     * encoding only when switched on. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        NativeAdrenoAbiBoAlloc alloc{4096,12,0};
        NativeAdrenoAbiBoAllocReply first{};
        CHECK(p.client.allocBo(&alloc,&first)==kIOReturnSuccess && first.handle==1);
        NativeAdrenoAbiBoCpuMap cm{first.handle};NativeAdrenoAbiBoCpuMapReply cr{};
        CHECK(p.client.cpuMap(&cm,&cr)==kIOReturnSuccess);
        const uint32_t token=cr.token;
        NativeAdrenoAbiBoCpuUnmap cu{first.handle,token,0};CHECK(p.client.cpuUnmap(&cu)==kIOReturnSuccess);
        NativeAdrenoAbiBoFree f{first.handle};CHECK(p.client.freeBo(&f)==kIOReturnSuccess);
        uint64_t expected=2;
        for(unsigned k=1;k<=NativeAdrenoClientSlotQuarantine;++k) {
            NativeAdrenoAbiBoAllocReply next{};
            CHECK(p.client.allocBo(&alloc,&next)==kIOReturnSuccess && next.handle==expected++);
            NativeAdrenoAbiBoCpuMap m{next.handle};NativeAdrenoAbiBoCpuMapReply r{};
            CHECK(p.client.cpuMap(&m,&r)==kIOReturnSuccess && r.token!=token);
            NativeAdrenoAbiBoCpuUnmap u{next.handle,r.token,0};CHECK(p.client.cpuUnmap(&u)==kIOReturnSuccess);
            NativeAdrenoAbiBoFree fr{next.handle};CHECK(p.client.freeBo(&fr)==kIOReturnSuccess);
        }
        /* The 4097th allocation after the free reuses the first slot (FIFO): same token, a new monotonic handle. */
        NativeAdrenoAbiBoAllocReply again{};
        CHECK(p.client.allocBo(&alloc,&again)==kIOReturnSuccess && again.handle==expected);
        NativeAdrenoAbiBoCpuMap m{again.handle};NativeAdrenoAbiBoCpuMapReply r{};
        CHECK(p.client.cpuMap(&m,&r)==kIOReturnSuccess && r.token==token && !p.client.earlyReuses());
        NativeAdrenoAbiBoCpuUnmap u{again.handle,r.token,0};CHECK(p.client.cpuUnmap(&u)==kIOReturnSuccess);
        NativeAdrenoAbiBoFree fa{again.handle};CHECK(p.client.freeBo(&fa)==kIOReturnSuccess);
        /* The slot encoding (step 3): low word = slot + 1 (the token), high word = the slot's generation. */
        native_adreno_slot_handles=1;
        NativeAdrenoAbiBoAllocReply slotted{};
        CHECK(p.client.allocBo(&alloc,&slotted)==kIOReturnSuccess);
        NativeAdrenoAbiBoCpuMap sm{slotted.handle};NativeAdrenoAbiBoCpuMapReply sr{};
        CHECK(p.client.cpuMap(&sm,&sr)==kIOReturnSuccess && uint32_t(slotted.handle)==sr.token && (slotted.handle>>32)>=1);
        NativeAdrenoAbiBoCpuUnmap su{slotted.handle,sr.token,0};CHECK(p.client.cpuUnmap(&su)==kIOReturnSuccess);
        NativeAdrenoAbiBoFree fs{slotted.handle};CHECK(p.client.freeBo(&fs)==kIOReturnSuccess);
        CHECK(p.client.freeBo(&fs)==kIOReturnBadArgument);   /* a freed handle is gone */
        native_adreno_slot_handles=0;
        CHECK(p.client.allocations()==NativeAdrenoClientSlotQuarantine+3 && p.client.close()==kIOReturnSuccess);
        ++blocks;
    }
    /* The ring is full and this client has nothing in flight: the core answers the internal retry with the fence that frees room
     * (the IOKit shell sleeps on it without a lock and submits again); a client with work in flight gets Busy (ARCHITECTURE 5.4). */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        NativeAdrenoUserClient second,third;
        CHECK(p.client.setScope(0,Base,0x4000)==kIOReturnSuccess);
        CHECK(second.open(NativeAdrenoAbiVersion,&p.vm,&p.execbuf,&p.ring,32,36)==kIOReturnSuccess &&
            second.setScope(NativeAdrenoAbiMaxQueues,Base+0x4000,0x4000)==kIOReturnSuccess);
        CHECK(third.open(NativeAdrenoAbiVersion,&p.vm,&p.execbuf,&p.ring,32,36)==kIOReturnSuccess &&
            third.setScope(2*NativeAdrenoAbiMaxQueues,Base+0x8000,0x4000)==kIOReturnSuccess);
        const uint64_t h0=allocMapped(p.client,4096,Base),h1=allocMapped(second,4096,Base+0x4000),h2=allocMapped(third,4096,Base+0x8000);
        NativeAdrenoAbiSubmitReply reply{};
        request={};request.buffer_count=NativeAdrenoAbiMaxBuffers;
        for(auto &b:request.buffers)b={h0,Base,256};
        for(uint32_t n=1;n<=1+NativeAdrenoSubmitSharedJobs;++n)CHECK(p.client.submit(&request,&reply,&ringclock)==kIOReturnSuccess && reply.fence==n);
        for(auto &b:request.buffers)b={h1,Base+0x4000,256};
        CHECK(second.submit(&request,&reply,&ringclock)==kIOReturnSuccess && reply.fence==14);
        for(auto &b:request.buffers)b={h2,Base+0x8000,256};
        reply={};
        CHECK(third.dispatch(NativeAdrenoMethodSubmit,&request,sizeof(request),&reply,sizeof(reply),&ringclock)==NativeAdrenoSubmitRetry);
        CHECK(third.retryFence()==1 && !reply.fence);   /* no output byte on failure */
        for(auto &b:request.buffers)b={h1,Base+0x4000,256};
        CHECK(second.submit(&request,&reply,&ringclock)==kIOReturnBusy && !second.retryFence());
        /* The CP finishes fence 1 and consumes its words: the third client gets in. */
        fencemem[1]=1;fencemem[0]=NativeAdrenoRingPrologueWords+NativeAdrenoRingResetWords+4*NativeAdrenoAbiMaxBuffers+NativeAdrenoRingFramedFenceWords;
        for(auto &b:request.buffers)b={h2,Base+0x8000,256};
        CHECK(third.submit(&request,&reply,&ringclock)==kIOReturnSuccess && reply.fence==15 && !third.retryFence());
        complete(15);
        CHECK(p.client.releaseAll(&ringclock,1000)==kIOReturnSuccess && second.releaseAll(&ringclock,1000)==kIOReturnSuccess &&
            third.releaseAll(&ringclock,1000)==kIOReturnSuccess && p.execbuf.outstanding()==0);
        ++blocks;
    }
    /* M10 locks: a BO from the BO pool (and back to it) under the submit object's Memory lock, the submission under its Ring lock, never
     * both. A BO outside the pool touches nothing the cores share: its allocation (the contiguous attempt, the scattered fallback), its
     * write-combining and its release run without the Memory lock, so another client's surface-sized allocation (int-152: 90-130 ms of
     * page attribute changes) never holds up the render server's BoAlloc/BoImport/BoFree; an import still takes it. No pool here. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        static int depth[2],taken[2],deepest;
        static unsigned allocUnder,allocOutside;
        depth[0]=depth[1]=taken[0]=taken[1]=deepest=0;allocUnder=allocOutside=0;
        const NativeAdrenoSubmitLocks locks={nullptr,
            [](void *,NativeAdrenoSubmitLock w) {CHECK(!depth[w]);++depth[w];++taken[w];if(depth[0]+depth[1]>deepest)deepest=depth[0]+depth[1];},
            [](void *,NativeAdrenoSubmitLock w) {CHECK(depth[w]==1);--depth[w];}};
        CHECK(p.execbuf.attachLocks(&locks)==kIOReturnSuccess);
        fixture.allocation_hook=[](unsigned,size_t) {++(depth[1]?allocUnder:allocOutside);};
        gMemoryDepth=&depth[1];gPagesUnder=gPagesOutside=0;
        const uint64_t handle=allocMapped(p.client,4096,Base);
        CHECK(taken[1]==1 && allocOutside==1 && !allocUnder && gPagesOutside==1 && !gPagesUnder);   /* the pool step alone locked */
        NativeAdrenoAbiSubmit submit{};submit.buffer_count=1;submit.buffers[0]={handle,Base,256};
        NativeAdrenoAbiSubmitReply reply{};
        const int ring_before=taken[0];
        CHECK(p.client.submit(&submit,&reply,&ringclock)==kIOReturnSuccess && taken[0]>ring_before);
        complete(reply.fence);
        NativeAdrenoAbiBoFree f{handle};
        CHECK(p.client.freeBo(&f)==kIOReturnSuccess && taken[1]==1 && gPagesOutside==2 && !gPagesUnder && deepest==1 && !depth[0] &&
              !depth[1]);
        /* 128 KiB: the contiguous attempt outside the lock; then that attempt refused and the scattered fallback, both outside it. */
        NativeAdrenoAbiBoAlloc big{128u<<10,12,0};NativeAdrenoAbiBoAllocReply first{},second{};
        CHECK(p.client.allocBo(&big,&first)==kIOReturnSuccess && taken[1]==2 && allocOutside==2 && gPagesOutside==3);
        fixture.allocation_fail_count=1;
        CHECK(p.client.allocBo(&big,&second)==kIOReturnSuccess && !fixture.allocation_fail_count && taken[1]==3 && allocOutside==4 &&
              gPagesOutside==4 && !allocUnder && !gPagesUnder);
        void *task=reinterpret_cast<void *>(0x77);
        NativeAdrenoAbiBoImport ir{0x30000000,2*4096};NativeAdrenoAbiBoAllocReply im{};
        CHECK(p.client.importBo(&ir,task,&im)==kIOReturnSuccess && taken[1]==4);   /* an import: under the lock */
        CHECK(p.client.releaseAll(&ringclock,1000)==kIOReturnSuccess && taken[1]==5 && gPagesOutside==6 && !gPagesUnder &&
              !depth[0] && !depth[1] && deepest==1);
        fixture.allocation_hook=nullptr;gMemoryDepth=nullptr;
        CHECK(p.execbuf.attachLocks(nullptr)==kIOReturnSuccess);
        ++blocks;
    }
    /* The external-method entry enforces exact record sizes and copies only on success. This is the entire surface the IOKit wrapper
     * may expose. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        NativeAdrenoAbiParams params{};
        CHECK(p.client.dispatch(NativeAdrenoMethodQuery,nullptr,0,&params,sizeof(params),&ringclock)==kIOReturnSuccess);
        CHECK(params.abi_version==NativeAdrenoAbiVersion && params.output_bits==36);
        unsigned char untouched[sizeof(NativeAdrenoAbiParams)];
        std::memset(untouched,0x5a,sizeof(untouched));
        CHECK(p.client.dispatch(NativeAdrenoMethodQuery,nullptr,1,untouched,sizeof(untouched),&ringclock)==kIOReturnBadArgument);
        CHECK(p.client.dispatch(NativeAdrenoMethodQuery,nullptr,0,untouched,sizeof(untouched)-1,&ringclock)==kIOReturnBadArgument);
        CHECK(p.client.dispatch(NativeAdrenoMethodQuery,nullptr,0,nullptr,0,&ringclock)==kIOReturnBadArgument);
        CHECK(p.client.dispatch(99,nullptr,0,nullptr,0,&ringclock)==kIOReturnUnsupported);
        CHECK(untouched[0]==0x5a && untouched[sizeof(untouched)-1]==0x5a);
        NativeAdrenoAbiBoAlloc alloc{8192,12,1};
        NativeAdrenoAbiBoAllocReply allocated{};
        CHECK(p.client.dispatch(NativeAdrenoMethodBoAlloc,&alloc,sizeof(alloc),&allocated,sizeof(allocated),&ringclock)==kIOReturnSuccess);
        CHECK(allocated.handle);
        NativeAdrenoAbiBoMap map{allocated.handle,Base,3,0,2,0};
        NativeAdrenoAbiBoMapReply mapped{};
        CHECK(p.client.dispatch(NativeAdrenoMethodBoMap,&map,sizeof(map),&mapped,sizeof(mapped),&ringclock)==kIOReturnSuccess);
        NativeAdrenoAbiSubmit submit{};
        submit.buffer_count=1;submit.buffers[0]={allocated.handle,Base,2048};
        NativeAdrenoAbiSubmitReply reply{};
        CHECK(p.client.dispatch(NativeAdrenoMethodSubmit,&submit,sizeof(submit),&reply,sizeof(reply),&ringclock)==kIOReturnSuccess);
        CHECK(reply.fence==1 && cp.regs[0x0807]==NativeAdrenoRingPrologueWords+NativeAdrenoRingResetWords+4+NativeAdrenoRingFramedFenceWords);
        NativeAdrenoAbiWaitFence wait{reply.fence,0};
        CHECK(p.client.dispatch(NativeAdrenoMethodWaitFence,&wait,sizeof(wait),nullptr,0,&ringclock)==kIOReturnTimeout);
        complete(1);
        wait.timeout_ns=UINT64_C(1000000);
        CHECK(p.client.dispatch(NativeAdrenoMethodWaitFence,&wait,sizeof(wait),nullptr,0,&ringclock)==kIOReturnSuccess);
        NativeAdrenoAbiBoFree released{allocated.handle};
        CHECK(p.client.dispatch(NativeAdrenoMethodBoFree,&released,sizeof(released),nullptr,0,&ringclock)==kIOReturnSuccess);
        CHECK(p.client.dispatch(NativeAdrenoMethodBoFree,&released,sizeof(released),nullptr,0,&ringclock)==kIOReturnBadArgument);
        NativeAdrenoTimer timer;CHECK(timer.attach(&timeio,NativeAdrenoTimeCp)==kIOReturnSuccess);
        CHECK(p.client.attachTimer(&timer)==kIOReturnSuccess);
        cp.regs[NativeAdrenoTimer::cpLowRegister()]=0x11;cp.regs[NativeAdrenoTimer::cpHighRegister()]=0x22;
        NativeAdrenoAbiTimestamp ticks{};
        CHECK(p.client.dispatch(NativeAdrenoMethodGetTimestamp,nullptr,0,&ticks,sizeof(ticks),&ringclock)==kIOReturnSuccess);
        CHECK(ticks.ticks==UINT64_C(0x0000002200000011));
        NativeAdrenoStatus counters;
        CHECK(p.client.attachStatus(&counters)==kIOReturnSuccess);
        counters.noteFault();
        NativeAdrenoAbiStatus status{};
        CHECK(p.client.dispatch(NativeAdrenoMethodGetStatus,nullptr,0,&status,sizeof(status),&ringclock)==kIOReturnSuccess);
        CHECK(status.faults==1 && status.suspends==0);
        unsigned char small[4]{};
        CHECK(p.client.dispatch(NativeAdrenoMethodGetStatus,nullptr,0,small,sizeof(small),&ringclock)==kIOReturnBadArgument);
        CHECK(p.client.close()==kIOReturnSuccess);
        ++blocks;
    }
    /* Refusals never reach a device reference, ring write or unmap. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        const uint32_t wptr=cp.regs[0x0807];
        NativeAdrenoAbiBoAllocReply allocated{};
        NativeAdrenoAbiBoAlloc bad[]={{0,12,0},{4097,12,0},{8192,11,0},
            {8192,25,0},{8192,12,2},{0x10000001,12,0}};
        for(const auto &r:bad)CHECK(p.client.allocBo(&r,&allocated)==kIOReturnBadArgument);
        CHECK(!allocated.handle);
        NativeAdrenoAbiBoAlloc good{4096,12,0};
        CHECK(p.client.allocBo(&good,&allocated)==kIOReturnSuccess);
        NativeAdrenoAbiBoMap wrong[]={{0,Base,3,0,2,0},{allocated.handle,Base,2,0,2,0},
            {allocated.handle,Base+1,3,0,2,0},{allocated.handle,Base,3,2,2,0},
            {allocated.handle,Base,3,0,1,0},{allocated.handle,Base,3,0,2,1},
            {allocated.handle+9,Base,3,0,2,0}};
        NativeAdrenoAbiBoMapReply mapped{};
        for(const auto &r:wrong)CHECK(p.client.mapBo(&r,&mapped)==kIOReturnBadArgument);
        CHECK(!mapped.iova && !p.client.mapped(allocated.handle));
        NativeAdrenoAbiBoMap r{allocated.handle,Base,3,0,2,0};
        CHECK(p.client.mapBo(&r,&mapped)==kIOReturnSuccess);
        CHECK(p.client.mapBo(&r,&mapped)==kIOReturnBusy);
        NativeAdrenoAbiSubmit empty{};
        NativeAdrenoAbiSubmitReply reply{};
        CHECK(p.client.submit(&empty,&reply,&ringclock)==kIOReturnBadArgument);
        empty.buffer_count=NativeAdrenoAbiMaxBuffers+1;CHECK(p.client.submit(&empty,&reply,&ringclock)==kIOReturnBadArgument);
        empty.buffer_count=1;empty.reserved=1;CHECK(p.client.submit(&empty,&reply,&ringclock)==kIOReturnBadArgument);
        empty.reserved=0;empty.queue=NativeAdrenoAbiMaxQueues;CHECK(p.client.submit(&empty,&reply,&ringclock)==kIOReturnBadArgument);
        empty.queue=1;CHECK(p.client.submit(&empty,&reply,&ringclock)==kIOReturnBadArgument);   /* not created */
        empty.queue=0;empty.buffers[0]={allocated.handle+5,Base,1024};
        CHECK(p.client.submit(&empty,&reply,&ringclock)==kIOReturnBadArgument);
        empty.buffers[0]={allocated.handle,Base,1026};
        CHECK(p.client.submit(&empty,&reply,&ringclock)==kIOReturnBadArgument);
        CHECK(cp.regs[0x0807]==wptr && p.execbuf.outstanding()==0);
        NativeAdrenoAbiWaitFence wait{0,0};
        CHECK(p.client.waitFence(&wait,&ringclock)==kIOReturnBadArgument);
        wait.fence=7;
        CHECK(p.client.waitFence(&wait,&ringclock)==kIOReturnBadArgument);
        NativeAdrenoAbiBoFree unknown{allocated.handle+5};
        CHECK(p.client.freeBo(&unknown)==kIOReturnBadArgument);
        CHECK(p.client.close()==kIOReturnBusy); /* object still live */
        NativeAdrenoAbiBoFree final_free{allocated.handle};
        CHECK(p.client.freeBo(&final_free)==kIOReturnSuccess);
        CHECK(p.client.close()==kIOReturnSuccess);
        CHECK(p.client.query(nullptr)==kIOReturnBadArgument);
        ++blocks;
    }
    /* Connection teardown: an in-flight job, a cpu-map lease and a second queue are all drained by releaseAll. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        const uint64_t handle=allocMapped(p.client,4096,Base);
        NativeAdrenoAbiBoCpuMap cm{handle};
        NativeAdrenoAbiBoCpuMapReply cr{};
        CHECK(p.client.cpuMap(&cm,&cr)==kIOReturnSuccess && cr.bytes==4096);
        CHECK(p.client.memoryForToken(cr.token)==p.client.memoryOf(handle));
        CHECK(p.client.memoryForToken(0)==nullptr && p.client.memoryForToken(cr.token+1)==nullptr);
        NativeAdrenoAbiQueueReply queue{};
        CHECK(p.client.createQueue(&queue)==kIOReturnSuccess);
        NativeAdrenoAbiSubmit submit{};
        submit.buffer_count=1;submit.buffers[0]={handle,Base,1024};
        NativeAdrenoAbiSubmitReply reply{};
        CHECK(p.client.submit(&submit,&reply,&ringclock)==kIOReturnSuccess);
        CHECK(p.client.releaseAll(&ringclock,1000)==kIOReturnTimeout); /* the CP has not retired the job */
        CHECK(p.client.liveObjects()==1);
        complete(reply.fence);
        CHECK(p.client.releaseAll(nullptr,1000)==kIOReturnBadArgument);
        CHECK(p.client.releaseAll(&ringclock,1000)==kIOReturnSuccess);
        CHECK(p.client.liveObjects()==0 && p.client.memoryForToken(cr.token)==nullptr);
        CHECK(p.client.query(nullptr)==kIOReturnBadArgument && p.client.close()==kIOReturnNotReady);
        CHECK(p.client.open(NativeAdrenoAbiVersion,&p.vm,&p.execbuf,&p.ring,31,36)==kIOReturnSuccess);
        ++blocks;
    }
    /* More than the old 64-object limit: 300 unmapped objects live at once, then drained by releaseAll. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        NativeAdrenoAbiBoAlloc alloc{4096,12,0};
        for(unsigned i=0;i<300;++i) {
            NativeAdrenoAbiBoAllocReply allocated{};
            CHECK(p.client.allocBo(&alloc,&allocated)==kIOReturnSuccess && allocated.handle);
        }
        CHECK(p.client.liveObjects()==300);
        CHECK(p.client.releaseAll(&ringclock,1000)==kIOReturnSuccess && p.client.liveObjects()==0);
        ++blocks;
    }
    /* Constant device facts (ARCHITECTURE 6.4): unsupported until attached, refused when malformed, then returned with the client's
     * window as the VA. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        NativeAdrenoAbiDeviceInfo got{};
        CHECK(p.client.deviceInfo(&got)==kIOReturnUnsupported);
        NativeAdrenoAbiDeviceInfo bad{};
        CHECK(p.client.attachDeviceInfo(&bad)==kIOReturnBadArgument);
        static const NativeAdrenoAbiDeviceInfo info={0x44050001u,12u<<20,0x4000000ull,0x1fffffffff000ull,0x20000000ull,
            0xE0000000ull,16,6,1,5,NativeAdrenoDeviceClientIova,0};
        CHECK(p.client.attachDeviceInfo(&info)==kIOReturnSuccess && p.client.attachDeviceInfo(&info)==kIOReturnBadArgument);
        CHECK(p.client.dispatch(NativeAdrenoMethodGetDeviceInfo,nullptr,0,&got,sizeof(got),&ringclock)==kIOReturnSuccess);
        CHECK(got.chip_id==0x44050001u && got.gmem_bytes==12u<<20 && got.gmem_base==0x4000000 && got.va_start==0x20000000ull &&
            got.va_size==0xE0000000ull && got.highest_bank_bit==16 && got.ubwc_swizzle==6 && got.macrotile_mode==1 && got.ubwc_version==5);
        CHECK(p.client.setScope(0,Base,0x4000)==kIOReturnSuccess && p.client.deviceInfo(&got)==kIOReturnSuccess &&
            got.va_start==Base && got.va_size==0x4000);
        CHECK(p.client.dispatch(NativeAdrenoMethodGetDeviceInfo,nullptr,0,&got,sizeof(got)-1,&ringclock)==kIOReturnBadArgument);
        CHECK(p.client.close()==kIOReturnSuccess);
        CHECK(p.client.deviceInfo(&got)==kIOReturnNotReady);
        ++blocks;
    }
    /* Two clients on one VM, submit object and ring (one per process): each maps only inside its own window and reports it as its VA,
     * their queues stay apart, and one tears down while the other's job is still outstanding. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        NativeAdrenoUserClient other;
        CHECK(other.setScope(NativeAdrenoAbiMaxQueues,Base+0x8000,0x8000)==kIOReturnNotReady);
        CHECK(other.open(NativeAdrenoAbiVersion,&p.vm,&p.execbuf,&p.ring,32,36)==kIOReturnSuccess);
        CHECK(p.client.setScope(0,Base,0x8000)==kIOReturnSuccess);
        CHECK(other.setScope(3,Base+0x8000,0x8000)==kIOReturnBadArgument);
        CHECK(other.setScope(NativeAdrenoAbiMaxQueues,Base+0x8001,0x8000)==kIOReturnBadArgument);
        CHECK(other.setScope(NativeAdrenoAbiMaxQueues,0xfffff000,0x2000)==kIOReturnBadArgument);   /* past the 32-bit input range */
        /* One range per connection slot (twelve) and none past them. */
        CHECK(other.setScope(uint32_t(NativeAdrenoSubmitRanges)*uint32_t(NativeAdrenoAbiMaxQueues),Base+0x8000,0x8000)==kIOReturnBadArgument);
        const uint32_t last_range=(uint32_t(NativeAdrenoMaxClients)-1)*uint32_t(NativeAdrenoAbiMaxQueues);
        CHECK(other.setScope(last_range,Base+0x8000,0x8000)==kIOReturnSuccess && other.queueBase()==last_range);
        CHECK(other.setScope(NativeAdrenoAbiMaxQueues,Base+0x8000,0x8000)==kIOReturnSuccess);
        NativeAdrenoAbiBoAlloc alloc{8192,12,0};
        NativeAdrenoAbiBoAllocReply mine{},theirs{};
        CHECK(p.client.allocBo(&alloc,&mine)==kIOReturnSuccess && other.allocBo(&alloc,&theirs)==kIOReturnSuccess);
        NativeAdrenoAbiBoMapReply mapped{};
        NativeAdrenoAbiBoMap trespass{mine.handle,Base+0x8000,3,0,2,0},straddle{theirs.handle,Base+0xf000,3,0,2,0},below{theirs.handle,Base,3,0,2,0};
        CHECK(p.client.mapBo(&trespass,&mapped)==kIOReturnBadArgument && other.mapBo(&straddle,&mapped)==kIOReturnBadArgument);
        CHECK(other.mapBo(&below,&mapped)==kIOReturnBadArgument && !other.windowPeak());
        NativeAdrenoAbiBoMap ours{mine.handle,Base,3,0,2,0},yours{theirs.handle,Base+0xe000,3,0,2,0};
        CHECK(p.client.mapBo(&ours,&mapped)==kIOReturnSuccess && other.mapBo(&yours,&mapped)==kIOReturnSuccess);
        CHECK(p.client.windowPeak()==Base+0x2000 && other.windowPeak()==Base+0x10000);
        CHECK(p.client.setScope(0,Base,0x8000)==kIOReturnBusy);   /* objects are live */
        NativeAdrenoAbiSubmit submit{};
        NativeAdrenoAbiSubmitReply reply{};
        submit.buffer_count=1;submit.buffers[0]={mine.handle,Base,1024};
        CHECK(p.client.submit(&submit,&reply,&ringclock)==kIOReturnSuccess && reply.fence==1);
        submit.buffers[0]={theirs.handle,Base+0xe000,1024};
        CHECK(other.submit(&submit,&reply,&ringclock)==kIOReturnSuccess && reply.fence==2);
        CHECK(p.execbuf.queueOutstanding(0) && p.execbuf.queueOutstanding(NativeAdrenoAbiMaxQueues));
        fencemem[1]=1;
        CHECK(p.client.releaseAll(&ringclock,1000)==kIOReturnSuccess && p.client.liveObjects()==0);
        CHECK(p.execbuf.outstanding()==1 && other.liveObjects()==1);
        CHECK(other.close()==kIOReturnBusy);
        CHECK(other.releaseAll(&ringclock,1000)==kIOReturnTimeout && other.liveObjects()==1);
        fencemem[1]=2;
        CHECK(other.releaseAll(&ringclock,1000)==kIOReturnSuccess && p.execbuf.outstanding()==0);
        NativeAdrenoAbiDeviceInfo got{};
        CHECK(other.deviceInfo(&got)==kIOReturnNotReady);
        ++blocks;
    }
    /* int-200: a client opened while another one has a job in flight owns no queue range before its first setScope, so neither its
     * first scope nor the teardown of a client that never got one may be refused or held up by the first client's jobs. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        CHECK(p.client.setScope(0,Base,0x8000)==kIOReturnSuccess);
        const uint64_t mine=allocMapped(p.client,8192,Base);
        NativeAdrenoAbiSubmit submit{};
        NativeAdrenoAbiSubmitReply reply{};
        submit.buffer_count=1;submit.buffers[0]={mine,Base,1024};
        CHECK(p.client.submit(&submit,&reply,&ringclock)==kIOReturnSuccess && p.execbuf.queueOutstanding(0));
        NativeAdrenoUserClient late,never;
        CHECK(late.open(NativeAdrenoAbiVersion,&p.vm,&p.execbuf,&p.ring,32,36)==kIOReturnSuccess);
        CHECK(late.setScope(NativeAdrenoAbiMaxQueues,Base+0x8000,0x8000)==kIOReturnSuccess);
        CHECK(late.ownRangeOutstanding()==false && late.close()==kIOReturnSuccess);
        CHECK(never.open(NativeAdrenoAbiVersion,&p.vm,&p.execbuf,&p.ring,32,36)==kIOReturnSuccess);
        CHECK(never.ownRangeOutstanding()==false);
        CHECK(never.releaseAll(&ringclock,0)==kIOReturnSuccess);   /* the mount's cleanup after a refused scope */
        CHECK(p.execbuf.queueOutstanding(0) && p.client.ownRangeOutstanding());
        fencemem[1]=1;
        CHECK(p.client.releaseAll(&ringclock,1000)==kIOReturnSuccess);
        ++blocks;
    }
    /* Completed work never pins a buffer: no wait() call, the CP only advances its fence, and BoFree still succeeds. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        const uint64_t handle=allocMapped(p.client,4096,Base);
        NativeAdrenoAbiSubmit submit{};
        submit.buffer_count=1;submit.buffers[0]={handle,Base,1024};
        NativeAdrenoAbiSubmitReply reply{};
        CHECK(p.client.submit(&submit,&reply,&ringclock)==kIOReturnSuccess);
        NativeAdrenoAbiBoFree r{handle};
        CHECK(p.client.freeBo(&r)==kIOReturnBusy); /* the job is still in flight */
        complete(reply.fence);
        CHECK(p.client.freeBo(&r)==kIOReturnSuccess && p.client.liveObjects()==0);
        NativeAdrenoAbiWaitFence wait{reply.fence,0};
        CHECK(p.client.waitFence(&wait,&ringclock)==kIOReturnSuccess); /* retired job, fence already passed */
        NativeAdrenoAbiWaitFence future{reply.fence+5,0};
        CHECK(p.client.waitFence(&future,&ringclock)==kIOReturnBadArgument);
        CHECK(p.client.close()==kIOReturnSuccess);
        ++blocks;
    }
    /* Objects of 64 to 128 KiB try one contiguous run first and fall back to scattered pages when that fails (and a refused retry). */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        NativeAdrenoAbiBoAlloc big{128u<<10,12,0};
        NativeAdrenoAbiBoAllocReply allocated{};
        fixture.allocation_fail_count=1;
        CHECK(p.client.allocBo(&big,&allocated)==kIOReturnSuccess && allocated.handle && fixture.allocation_fail_count==0);
        NativeAdrenoAbiBoAllocReply second{};
        fixture.allocation_fail_count=2; /* contiguous and scattered both fail */
        CHECK(p.client.allocBo(&big,&second)==kIOReturnNoMemory && !second.handle && fixture.allocation_fail_count==0);
        NativeAdrenoAbiBoAlloc demanded{1u<<20,12,1};
        fixture.allocation_fail_count=1; /* a demanded contiguous run is not retried scattered */
        CHECK(p.client.allocBo(&demanded,&second)==kIOReturnNoMemory);
        CHECK(p.client.releaseAll(&ringclock,1000)==kIOReturnSuccess);
        ++blocks;
    }
    /* pagani-gpu-kopt bit 1 (upstream Adreno740 01ed692): a BO outside the pool asks for a contiguous run only from 64 to 128 KiB; a failure
     * keeps that size and larger from asking for 5 s by the clock, any connection's; a smaller size still asks and lowers the mark; a
     * demanded run always asks. Bit off: every size from 64 KiB asks, every time. No pool yet in this file. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        static unsigned runs;runs=0;
        fixture.allocation_hook=[](unsigned options,size_t) {if(options&kIOMemoryPhysicallyContiguous)++runs;};
        native_adreno_contig_backoff[0]=0;native_adreno_contig_backoff[1]=0;
        native_adreno_meter_contig[0]=0;native_adreno_meter_contig[1]=0;native_adreno_meter_contig[2]=0;
        auto asked=[&](NativeAdrenoUserClient &client,uint64_t bytes,uint32_t contiguous=0) {
            const unsigned before=runs;
            NativeAdrenoAbiBoAlloc request{bytes,12,contiguous};NativeAdrenoAbiBoAllocReply reply{};
            CHECK(client.dispatch(NativeAdrenoMethodBoAlloc,&request,sizeof(request),&reply,sizeof(reply),&ringclock)==kIOReturnSuccess && reply.handle);
            return runs!=before;
        };
        CHECK(!asked(p.client,60u<<10) && asked(p.client,64u<<10) && asked(p.client,128u<<10) && !asked(p.client,132u<<10) &&
              !asked(p.client,1u<<20) && !native_adreno_contig_backoff[0] && native_adreno_meter_contig[0]==2 && !native_adreno_meter_contig[1] &&
              native_adreno_meter_contig[2]==5);                                                     /* every BO counts, also those without an attempt */
        ++koptBlocks;
        /* The free memory holds no run above 64 KiB. */
        fixture.contiguous_limit=64u<<10;
        clock_us=1000;
        CHECK(asked(p.client,128u<<10) && native_adreno_contig_backoff[0]==(128u<<10) && native_adreno_contig_backoff[1]==1000 &&
              native_adreno_meter_contig[1]==1);
        CHECK(!asked(p.client,128u<<10) && asked(p.client,96u<<10) && native_adreno_contig_backoff[0]==(96u<<10));   /* smaller: still asks */
        CHECK(!asked(p.client,96u<<10) && !asked(p.client,128u<<10) && asked(p.client,64u<<10) &&                     /* and a success */
              native_adreno_contig_backoff[0]==(96u<<10));                                                            /* changes nothing */
        ++koptBlocks;
        /* Whichever connection failed: the free memory is everyone's. */
        NativeAdrenoUserClient other;
        CHECK(other.open(NativeAdrenoAbiVersion,&p.vm,&p.execbuf,&p.ring,32,36)==kIOReturnSuccess);
        CHECK(!asked(other,96u<<10) && asked(other,64u<<10));
        ++koptBlocks;
        /* By the clock: 5 s from the failure, to the microsecond. */
        clock_us=1000+NativeAdrenoClientContiguousBackoffUs-1;
        CHECK(!asked(p.client,96u<<10));
        clock_us=1000+NativeAdrenoClientContiguousBackoffUs;
        CHECK(asked(p.client,128u<<10) && native_adreno_contig_backoff[0]==(128u<<10) && native_adreno_contig_backoff[1]==clock_us);
        CHECK(!asked(other,128u<<10));
        clock_us+=NativeAdrenoClientContiguousBackoffUs;
        CHECK(asked(other,96u<<10));
        ++koptBlocks;
        /* A caller that demands contiguity is served (or refused) whatever the mark says, and a call without a clock remembers nothing. */
        fixture.contiguous_limit=0;
        const uint32_t attempts=native_adreno_meter_contig[0],outside=native_adreno_meter_contig[2];
        CHECK(asked(p.client,1u<<20,1) && native_adreno_contig_backoff[0]==(96u<<10));
        /* Its one search is the demanded one, not an attempt that may fall back and search again, at any size. */
        CHECK(asked(p.client,64u<<10,1) && native_adreno_meter_contig[0]==attempts && native_adreno_meter_contig[2]==outside+2);
        native_adreno_contig_backoff[0]=0;
        fixture.contiguous_limit=64u<<10;
        NativeAdrenoAbiBoAlloc plain{96u<<10,12,0};NativeAdrenoAbiBoAllocReply reply{};
        CHECK(p.client.allocBo(&plain,&reply)==kIOReturnSuccess && !native_adreno_contig_backoff[0]);
        ++koptBlocks;
        /* Bit off: as before the change, large sizes ask, a failure asks again next time, the mark is neither read nor written. */
        native_adreno_kopt=NativeAdrenoKoptAll&~NativeAdrenoKoptContiguous;
        native_adreno_contig_backoff[0]=96u<<10;native_adreno_contig_backoff[1]=clock_us;
        CHECK(!asked(p.client,60u<<10) && asked(p.client,1u<<20) && asked(p.client,1u<<20) && asked(p.client,128u<<10) &&
              native_adreno_contig_backoff[0]==(96u<<10) && native_adreno_contig_backoff[1]==clock_us);
        native_adreno_kopt=NativeAdrenoKoptAll;
        native_adreno_contig_backoff[0]=0;native_adreno_contig_backoff[1]=0;
        fixture.contiguous_limit=0;fixture.allocation_hook=nullptr;
        CHECK(other.releaseAll(&ringclock,1000)==kIOReturnSuccess);
        CHECK(p.client.releaseAll(&ringclock,1000)==kIOReturnSuccess);
        ++koptBlocks;
    }
    /* Version and mount refusals, then a commit that poisons everything. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        NativeAdrenoUserClient client;
        CHECK(client.open(NativeAdrenoAbiVersion+1,&p.vm,&p.execbuf,&p.ring,32,36)==kIOReturnUnsupported);
        CHECK(client.open(NativeAdrenoAbiVersion,nullptr,&p.execbuf,&p.ring,32,36)==kIOReturnBadArgument);
        CHECK(client.open(NativeAdrenoAbiVersion,&p.vm,nullptr,&p.ring,32,36)==kIOReturnBadArgument);
        CHECK(client.open(NativeAdrenoAbiVersion,&p.vm,&p.execbuf,&p.ring,30,36)==kIOReturnBadArgument);
        CHECK(client.open(NativeAdrenoAbiVersion,&p.vm,&p.execbuf,&p.ring,32,31)==kIOReturnBadArgument);
        CHECK(client.close()==kIOReturnNotReady && client.query(nullptr)==kIOReturnBadArgument);
        const uint64_t handle=allocMapped(p.client,4096,Base);
        cp.fail_publish=cp.publish+1;
        NativeAdrenoAbiSubmit submit{};
        submit.buffer_count=1;submit.buffers[0]={handle,Base,4096};
        NativeAdrenoAbiSubmitReply reply{};
        CHECK(p.client.submit(&submit,&reply,&ringclock)==kIOReturnIOError && !reply.fence);
        CHECK(p.execbuf.poisoned() && p.execbuf.outstanding()==1);
        NativeAdrenoAbiBoFree retained{handle};
        CHECK(p.client.freeBo(&retained)==kIOReturnBusy);
        CHECK(p.client.close()==kIOReturnBusy);
        ++blocks;
    }
    /* CPU mapping leases keep the backing object alive for the user mapping. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        NativeAdrenoAbiBoAlloc alloc{4096,12,0};
        NativeAdrenoAbiBoAllocReply allocated{};
        CHECK(p.client.allocBo(&alloc,&allocated)==kIOReturnSuccess);
        NativeAdrenoAbiBoCpuMap map{allocated.handle};
        NativeAdrenoAbiBoCpuMapReply mapped{};
        CHECK(p.client.cpuMap(&map,&mapped)==kIOReturnSuccess);
        CHECK(mapped.token && mapped.bytes==4096);
        const uint32_t token=mapped.token;
        CHECK(p.client.cpuMap(&map,&mapped)==kIOReturnSuccess && mapped.token==token && p.client.mappingLeases(token)==2);
        NativeAdrenoAbiBoFree released{allocated.handle};
        CHECK(p.client.freeBo(&released)==kIOReturnBusy);
        CHECK(p.client.close()==kIOReturnBusy);
        NativeAdrenoAbiBoCpuUnmap wrong{allocated.handle,token+1,0};
        CHECK(p.client.cpuUnmap(&wrong)==kIOReturnBadArgument);
        NativeAdrenoAbiBoCpuUnmap unmap{allocated.handle,token,0};
        CHECK(p.client.cpuUnmap(&unmap)==kIOReturnSuccess);
        CHECK(p.client.freeBo(&released)==kIOReturnBusy); /* one lease left */
        CHECK(p.client.cpuUnmap(&unmap)==kIOReturnSuccess);
        CHECK(p.client.cpuUnmap(&unmap)==kIOReturnBusy);
        CHECK(p.client.freeBo(&released)==kIOReturnSuccess);
        CHECK(p.client.cpuMap(&map,&mapped)==kIOReturnBadArgument);
        CHECK(p.client.close()==kIOReturnSuccess);
        ++blocks;
    }
    /* Queue objects bind submissions and must be drained before close. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        for(unsigned i=1;i<NativeAdrenoAbiMaxQueues;++i) {
            NativeAdrenoAbiQueueReply r{};
            CHECK(p.client.createQueue(&r)==kIOReturnSuccess && r.queue==i);
        }
        NativeAdrenoAbiQueueReply extra{};
        CHECK(p.client.createQueue(&extra)==kIOReturnNoResources);
        const uint64_t handle=allocMapped(p.client,4096,Base);
        NativeAdrenoAbiSubmit first{};
        first.buffer_count=1;first.queue=1;first.buffers[0]={handle,Base,512};
        NativeAdrenoAbiSubmitReply reply{};
        CHECK(p.client.submit(&first,&reply,&ringclock)==kIOReturnSuccess && reply.fence==1);
        NativeAdrenoAbiQueue close{1};
        CHECK(p.client.closeQueue(&close)==kIOReturnBusy);
        /* An unscoped client owns range 0, which may have several jobs in flight (the shared slots). */
        NativeAdrenoAbiSubmit second{};
        second.buffer_count=1;second.queue=2;second.buffers[0]={handle,Base,512};
        CHECK(p.client.submit(&second,&reply,&ringclock)==kIOReturnSuccess && reply.fence==2);
        fencemem[1]=1;
        NativeAdrenoAbiWaitFence wait{1,0};
        CHECK(p.client.waitFence(&wait,&ringclock)==kIOReturnSuccess);
        CHECK(p.client.closeQueue(&close)==kIOReturnSuccess);   /* queue 2's job does not hold queue 1 */
        complete(2);wait.fence=2;
        CHECK(p.client.waitFence(&wait,&ringclock)==kIOReturnSuccess);
        CHECK(p.client.closeQueue(&close)==kIOReturnBadArgument);
        CHECK(p.client.submit(&first,&reply,&ringclock)==kIOReturnBadArgument);
        NativeAdrenoAbiQueue none{0};
        CHECK(p.client.closeQueue(&none)==kIOReturnBadArgument);
        NativeAdrenoAbiQueue two{2};
        CHECK(p.client.close()==kIOReturnBusy); /* queue 2 still open */
        CHECK(p.client.closeQueue(&two)==kIOReturnSuccess);
        for(uint64_t id=3;id<NativeAdrenoAbiMaxQueues;++id) {
            NativeAdrenoAbiQueue rest{id};
            CHECK(p.client.closeQueue(&rest)==kIOReturnSuccess);
        }
        NativeAdrenoAbiBoFree released{handle};
        CHECK(p.client.freeBo(&released)==kIOReturnSuccess);
        CHECK(p.client.close()==kIOReturnSuccess);
        NativeAdrenoAbiQueueReply after{};
        CHECK(p.client.createQueue(&after)==kIOReturnNotReady);
        ++blocks;
    }
    /* The connection's bytes by kind (the MEM row's client figures, ARCHITECTURE 8.4): own BOs outside the pool, imports, pooled BOs;
     * the token of a handle (the wrapper drops a freed BO's CPU view by it); and a teardown that frees everything it can: an object
     * whose release failed stays and is the answer, the objects after it in the table go anyway (before, the first refusal kept every
     * later object of a closed connection for the rest of the boot). Runs last: it reserves the BO pool, which lives for the process. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        uint64_t pool=9,other=9,imported=9;
        p.client.usage(&pool,&other,&imported);CHECK(!pool && !other && !imported);
        NativeAdrenoAbiBoAlloc alloc{8192,12,0};
        NativeAdrenoAbiBoAllocReply first{},second{},pooled{};
        CHECK(p.client.allocBo(&alloc,&first)==kIOReturnSuccess && p.client.allocBo(&alloc,&second)==kIOReturnSuccess);
        p.client.usage(&pool,&other,&imported);CHECK(!pool && other==16384 && !imported);   /* no pool: own BOs outside it */
        const uint32_t token=p.client.tokenOf(first.handle);
        CHECK(token && token!=p.client.tokenOf(second.handle) && !p.client.tokenOf(second.handle+1000) && !p.client.tokenOf(0));
        NativeAdrenoAbiBoCpuMap cm{first.handle};NativeAdrenoAbiBoCpuMapReply cr{};
        CHECK(p.client.cpuMap(&cm,&cr)==kIOReturnSuccess && cr.token==token);
        NativeAdrenoAbiBoCpuUnmap cu{first.handle,cr.token,0};
        CHECK(p.client.cpuUnmap(&cu)==kIOReturnSuccess);
        void *task=reinterpret_cast<void *>(0x77);
        NativeAdrenoAbiBoImport ir{0x30000000,3*4096};NativeAdrenoAbiBoAllocReply im{};
        CHECK(p.client.importBo(&ir,task,&im)==kIOReturnSuccess);
        p.client.usage(&pool,&other,&imported);CHECK(!pool && other==16384 && imported==3*4096);
        uint32_t mib=0;
        /* Cacheable for now: the block after this one needs the clean of the zero fill, then write-combines the pool. */
        CHECK(NativeAdrenoMemory::reservePoolBestEffort(36,16,16,false,&mib)==kIOReturnSuccess && mib==16);
        NativeAdrenoAbiBoAlloc big{65536,12,0};
        CHECK(p.client.allocBo(&big,&pooled)==kIOReturnSuccess && p.client.memoryOf(pooled.handle)->pooled());
        p.client.usage(&pool,&other,&imported);CHECK(pool==65536 && other==16384 && imported==3*4096);
        /* The second BO's release fails (its descriptor refuses complete()): it stays live and keeps its bytes. */
        fixture.complete_fail=true;
        NativeAdrenoAbiBoFree refused{second.handle};
        CHECK(p.client.freeBo(&refused)!=kIOReturnSuccess);
        fixture.complete_fail=false;
        p.client.usage(&pool,&other,&imported);CHECK(pool==65536 && other==16384 && imported==3*4096);
        /* Teardown: the first BO (before it), the import and the pooled BO (after it) go; it stays and answers Busy. */
        CHECK(p.client.releaseAll(&ringclock,1000)==kIOReturnBusy && p.client.liveObjects()==1);
        p.client.usage(&pool,&other,&imported);CHECK(!pool && other==8192 && !imported);
        CHECK(p.client.releaseAll(&ringclock,1000)==kIOReturnBusy && p.client.liveObjects()==1);   /* an orphan's retry: the same */
        ++blocks;
    }
    /* pagani-gpu-kopt bit 0: a pooled BO's zero fill comes after the Memory lock (what the last owner left is still there when it is
     * dropped), with the bit off under it, and in both cases a BO whose fill failed gives its pages back under that lock, its slot
     * free again and nothing published. The pool is still cacheable here; this block write-combines it. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        static int depth,takes,atUnlock;
        static const unsigned char *watched;
        depth=takes=0;watched=nullptr;atUnlock=-1;
        const NativeAdrenoSubmitLocks locks={nullptr,
            [](void *,NativeAdrenoSubmitLock w) {if(w==NativeAdrenoSubmitLockMemory) {CHECK(!depth);++depth;++takes;}},
            [](void *,NativeAdrenoSubmitLock w) {if(w==NativeAdrenoSubmitLockMemory) {CHECK(depth==1);--depth;if(watched)atUnlock=*watched;}}};
        CHECK(p.execbuf.attachLocks(&locks)==kIOReturnSuccess);
        NativeAdrenoAbiBoAlloc alloc{16384,12,0};NativeAdrenoAbiBoAllocReply reply{};
        CHECK(p.client.allocBo(&alloc,&reply)==kIOReturnSuccess && p.client.memoryOf(reply.handle)->pooled());
        unsigned char *pages=static_cast<unsigned char *>(p.client.memoryOf(reply.handle)->mapping());
        auto dirtied=[&] {
            std::memset(pages,0x5a,16384);
            NativeAdrenoAbiBoFree f{reply.handle};
            CHECK(p.client.freeBo(&f)==kIOReturnSuccess);
        };
        dirtied();
        watched=pages;atUnlock=-1;
        CHECK(p.client.allocBo(&alloc,&reply)==kIOReturnSuccess && p.client.memoryOf(reply.handle)->mapping()==pages);
        CHECK(atUnlock==0x5a && !pages[0] && !pages[16383]);   /* the zeros came after the lock */
        watched=nullptr;dirtied();++koptBlocks;
        native_adreno_kopt=NativeAdrenoKoptAll&~NativeAdrenoKoptPoolZero;
        watched=pages;atUnlock=-1;
        CHECK(p.client.allocBo(&alloc,&reply)==kIOReturnSuccess && p.client.memoryOf(reply.handle)->mapping()==pages);
        CHECK(atUnlock==0 && !pages[16383]);                   /* under it */
        watched=nullptr;dirtied();++koptBlocks;
        /* The fill's clean fails, with the bit on (then off): an error for the caller, the pool whole, the slot back, no live object. */
        const size_t free0=NativeAdrenoMemory::poolFreeBytes();
        const uint32_t refusals=native_adreno_client_stats[NativeAdrenoSubmitRanges][2];
        for(const uint32_t kopt:{uint32_t(NativeAdrenoKoptAll),uint32_t(NativeAdrenoKoptAll&~NativeAdrenoKoptPoolZero)}) {
            native_adreno_kopt=kopt;
            fixture.cache_fail_call=fixture.cache_calls+1;
            const int before=takes;
            NativeAdrenoAbiBoAllocReply failed{};
            CHECK(p.client.allocBo(&alloc,&failed)==kIOReturnIOError && !failed.handle && !p.client.liveObjects() && !depth);
            CHECK(NativeAdrenoMemory::poolFreeBytes()==free0 && takes==before+2);   /* taken from the pool, then given back, each under the lock */
            fixture.cache_fail_call=0;
            CHECK(p.client.allocBo(&alloc,&reply)==kIOReturnSuccess && p.client.liveObjects()==1 && NativeAdrenoMemory::poolFreeBytes()==free0-16384);
            NativeAdrenoAbiBoFree f{reply.handle};
            CHECK(p.client.freeBo(&f)==kIOReturnSuccess && NativeAdrenoMemory::poolFreeBytes()==free0);
            ++koptBlocks;
        }
        CHECK(native_adreno_client_stats[NativeAdrenoSubmitRanges][2]==refusals+2);
        native_adreno_kopt=NativeAdrenoKoptAll;
        /* Write-combined from here on, as on the device: a barrier and no clean (nothing left that can fail). */
        uint32_t mib=0;
        CHECK(NativeAdrenoMemory::reservePoolBestEffort(36,16,16,true,&mib)==kIOReturnSuccess && mib==16);
        const unsigned calls=fixture.cache_calls;
        fixture.cache_fail_call=calls+1;
        CHECK(p.client.allocBo(&alloc,&reply)==kIOReturnSuccess && fixture.cache_calls==calls);
        fixture.cache_fail_call=0;
        CHECK(p.client.releaseAll(&ringclock,1000)==kIOReturnSuccess);
        CHECK(p.execbuf.attachLocks(nullptr)==kIOReturnSuccess);
        ++koptBlocks;
    }
    /* What the core gives the call meter (pagani-gpu-kopt bit 4, ARCHITECTURE 8.8; tickclock: a read moves 100 us; the calls themselves are
     * timed by the publish shell, the timer in tests/test_meter.cpp): a pooled BO's zero fill between its two reads, the VM's kinds as
     * differences; the tick takes everything once; off, nothing. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        Adreno830StatusCalls calls{};uint64_t seen[NativeAdrenoVmKinds][2]={};
        NativeAdrenoMeterTake(&calls,seen);   /* what the blocks before left */
        NativeAdrenoAbiBoAlloc alloc{8192,12,0};NativeAdrenoAbiBoAllocReply allocated{};
        CHECK(p.client.dispatch(NativeAdrenoMethodBoAlloc,&alloc,sizeof(alloc),&allocated,sizeof(allocated),&tickclock)==kIOReturnSuccess);
        void *task=reinterpret_cast<void *>(0x77);
        NativeAdrenoAbiBoImport ir{0x30000000,2*4096};NativeAdrenoAbiBoAllocReply im{};
        CHECK(p.client.importBo(&ir,task,&im)==kIOReturnSuccess);
        NativeAdrenoAbiBoMap imap{im.handle,Base+0x4000,3,0,2,0};NativeAdrenoAbiBoMapReply mapped{};
        CHECK(p.client.dispatch(NativeAdrenoMethodBoMap,&imap,sizeof(imap),&mapped,sizeof(mapped),&tickclock)==kIOReturnSuccess);
        NativeAdrenoMeterTake(&calls,seen);
        CHECK(calls.zero_kib==8 && calls.zero_us==100 && calls.import_maps==1 && !calls.shared_maps && !calls.contig_tries);
        CHECK(!calls.map_us && !calls.unmap_us && !calls.shared_us);   /* the VM's clock is not set here */
        ++koptBlocks;
        NativeAdrenoMeterTake(&calls,seen);
        CHECK(!calls.import_maps && !calls.zero_kib && !calls.zero_us);
        ++koptBlocks;
        native_adreno_kopt=NativeAdrenoKoptAll&~NativeAdrenoKoptMeter;
        NativeAdrenoAbiBoAllocReply unmetered{};
        CHECK(p.client.dispatch(NativeAdrenoMethodBoAlloc,&alloc,sizeof(alloc),&unmetered,sizeof(unmetered),&tickclock)==kIOReturnSuccess);
        native_adreno_kopt=NativeAdrenoKoptAll;
        NativeAdrenoMeterTake(&calls,seen);
        CHECK(!calls.zero_kib && !calls.zero_us);
        CHECK(p.client.releaseAll(&ringclock,1000)==kIOReturnSuccess);
        ++koptBlocks;
    }
    /* Capacity and the render server's reserve (2026-10-30, every app a Metal client): open() takes a power of two from 256 to 16384
     * objects and the 257th allocation of a 256-object connection is refused; with setReserve a client's BoMap leaves the shared VM's
     * mapping records and table ranges the render server keeps (NoResources, counted in native_adreno_vm_reserve_refused), and its
     * BoAlloc takes from the BO pool only while the kept share stays free (else outside it, as when the pool is full). */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        CHECK(p.client.capacity()==NativeAdrenoClientMaxObjects && p.client.close()==kIOReturnSuccess && p.client.capacity()==0);
        for(size_t bad:{size_t(0),size_t(128),size_t(3000),size_t(32768)})
            CHECK(p.client.open(NativeAdrenoAbiVersion,&p.vm,&p.execbuf,&p.ring,32,36,bad)==kIOReturnBadArgument && p.client.capacity()==0);
        CHECK(p.client.setReserve(1,1,1)==kIOReturnNotReady);
        CHECK(p.client.open(NativeAdrenoAbiVersion,&p.vm,&p.execbuf,&p.ring,32,36,NativeAdrenoClientMinObjects)==kIOReturnSuccess &&
              p.client.capacity()==NativeAdrenoClientMinObjects);
        NativeAdrenoAbiBoAlloc page{4096,12,0};
        std::vector<uint64_t> handles;
        for(size_t i=0;i<NativeAdrenoClientMinObjects;++i) {
            NativeAdrenoAbiBoAllocReply r{};
            CHECK(p.client.allocBo(&page,&r)==kIOReturnSuccess && r.handle);
            handles.push_back(r.handle);
        }
        NativeAdrenoAbiBoAllocReply over{};
        CHECK(p.client.allocBo(&page,&over)==kIOReturnNoResources && !over.handle && p.client.liveObjects()==NativeAdrenoClientMinObjects);
        for(uint64_t handle:handles) {NativeAdrenoAbiBoFree f{handle};CHECK(p.client.freeBo(&f)==kIOReturnSuccess);}
        CHECK(p.client.liveObjects()==0 && p.vm.freeObjects()==NativeAdrenoVmMaxObjects);
        /* Mapping records: leaving all but two, two BOs map and the third is refused; without a reserve (the render server) it maps. */
        const uint32_t refused0=native_adreno_vm_reserve_refused[0],refused1=native_adreno_vm_reserve_refused[1];
        CHECK(p.client.setReserve(NativeAdrenoVmMaxObjects-2,0,0)==kIOReturnSuccess);
        allocMapped(p.client,4096,Base);allocMapped(p.client,4096,Base+0x1000);
        NativeAdrenoAbiBoAllocReply c{};CHECK(p.client.allocBo(&page,&c)==kIOReturnSuccess);
        NativeAdrenoAbiBoMap mc{c.handle,Base+0x2000,3,0,2,0};NativeAdrenoAbiBoMapReply mr{};
        CHECK(p.client.mapBo(&mc,&mr)==kIOReturnNoResources && !mr.iova && native_adreno_vm_reserve_refused[0]==refused0+1 &&
              !p.client.mapped(c.handle));
        CHECK(p.client.setReserve(0,0,0)==kIOReturnSuccess && p.client.mapBo(&mc,&mr)==kIOReturnSuccess && mr.iova==Base+0x2000);
        CHECK(p.vm.freeObjects()==NativeAdrenoVmMaxObjects-3);
        /* Table ranges: leaving all free ranges but one, a one-page BO maps and the next is refused. */
        CHECK(p.client.setReserve(0,p.vm.freeSpans()-1,0)==kIOReturnSuccess);
        allocMapped(p.client,4096,Base+0x3000);
        NativeAdrenoAbiBoAllocReply e{};CHECK(p.client.allocBo(&page,&e)==kIOReturnSuccess);
        NativeAdrenoAbiBoMap me{e.handle,Base+0x4000,3,0,2,0};
        CHECK(p.client.mapBo(&me,&mr)==kIOReturnNoResources && native_adreno_vm_reserve_refused[1]==refused1+1 &&
              native_adreno_vm_reserve_refused[0]==refused0+1);
        /* The BO pool (16 MiB, reserved by the block before; the five pages above came from it): leaving all but 1 MiB of what is free,
         * 2 MiB come from outside the pool, allocated with the Memory lock free; leaving all but 8 MiB, from it, under the lock and
         * with nothing allocated anew. */
        static int depth,taken;
        static unsigned allocUnder,allocOutside;
        depth=taken=0;allocUnder=allocOutside=0;
        const NativeAdrenoSubmitLocks locks={nullptr,
            [](void *,NativeAdrenoSubmitLock w) {if(w==NativeAdrenoSubmitLockMemory) {CHECK(!depth);++depth;++taken;}},
            [](void *,NativeAdrenoSubmitLock w) {if(w==NativeAdrenoSubmitLockMemory) {CHECK(depth==1);--depth;}}};
        CHECK(p.execbuf.attachLocks(&locks)==kIOReturnSuccess);
        fixture.allocation_hook=[](unsigned,size_t) {++(depth?allocUnder:allocOutside);};
        const size_t free0=NativeAdrenoMemory::poolFreeBytes();
        CHECK(NativeAdrenoMemory::poolBytes()==(16u<<20) && free0==(16u<<20)-5*4096);
        NativeAdrenoAbiBoAlloc two{2u<<20,12,0};NativeAdrenoAbiBoAllocReply outside{},inside{};
        CHECK(p.client.setReserve(0,0,free0-(1u<<20))==kIOReturnSuccess && p.client.allocBo(&two,&outside)==kIOReturnSuccess &&
              !p.client.memoryOf(outside.handle)->pooled() && NativeAdrenoMemory::poolFreeBytes()==free0);
        CHECK(taken==1 && allocOutside==1 && !allocUnder);
        CHECK(p.client.setReserve(0,0,free0-(8u<<20))==kIOReturnSuccess && p.client.allocBo(&two,&inside)==kIOReturnSuccess &&
              p.client.memoryOf(inside.handle)->pooled() && NativeAdrenoMemory::poolFreeBytes()==free0-(2u<<20));
        CHECK(taken==2 && allocOutside==1 && !allocUnder);
        CHECK(p.client.releaseAll(&ringclock,1000)==kIOReturnSuccess && NativeAdrenoMemory::poolFreeBytes()==(16u<<20) &&
              p.vm.freeObjects()==NativeAdrenoVmMaxObjects && !p.client.capacity() && !depth);
        fixture.allocation_hook=nullptr;
        CHECK(p.execbuf.attachLocks(nullptr)==kIOReturnSuccess);
        ++blocks;
    }
    /* M14 P0 (ARCHITECTURE 4.12.3, 4.13.5): the publish shell names the render server's connections, whose jobs then reach the ring's
     * stamp as the render server's (every other connection's as external; only while open, and open starts external); a CB0 page
     * fault is charged to the connection whose window holds FAR, as before, whatever was in flight, and the jobs in flight when the top
     * half read the fences (the submit object's fence history) only give the verdict the shell counts (NativeAdrenoFaultNote, which
     * native_adreno_note_page_fault runs before it charges the slot returned). */
    unsigned attribution=0;
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        static std::vector<uint32_t> flags;
        flags.clear();
        const NativeAdrenoRingWaiter waiter={nullptr,
            [](void *,uint32_t,uint32_t,uint32_t *observed)->IOReturn {*observed=fencemem[1];return kIOReturnTimeout;},
            [](void *,uint32_t,uint32_t f) {flags.push_back(f);}};
        p.ring.setWaiter(&waiter);
        static const NativeAdrenoClientWindow windows[NativeAdrenoMaxClients]={{Base,0x4000},{Base+0x4000,0x4000},
            {Base+0x8000,0x4000},{Base+0xC000,0x4000}};
        NativeAdrenoUserClient other;
        CHECK(other.open(NativeAdrenoAbiVersion,&p.vm,&p.execbuf,&p.ring,32,36)==kIOReturnSuccess);
        CHECK(p.client.setScope(0,windows[0].start,windows[0].bytes)==kIOReturnSuccess);
        CHECK(other.setScope(3*NativeAdrenoAbiMaxQueues,windows[3].start,windows[3].bytes)==kIOReturnSuccess);
        CHECK(p.client.source()==NativeAdrenoRingSourceExternal && other.source()==NativeAdrenoRingSourceExternal);
        p.client.setRenderServer(true);
        CHECK(p.client.source()==NativeAdrenoRingSourceRender && other.source()==NativeAdrenoRingSourceExternal);
        const uint64_t mine=allocMapped(p.client,8192,windows[0].start),theirs=allocMapped(other,8192,windows[3].start);
        NativeAdrenoAbiSubmit a{},b{};
        a.buffer_count=1;a.buffers[0]={mine,windows[0].start,1024};
        b.buffer_count=1;b.buffers[0]={theirs,windows[3].start,1024};
        NativeAdrenoAbiSubmitReply reply{};
        CHECK(p.client.submit(&a,&reply,&ringclock)==kIOReturnSuccess && reply.fence==1);
        CHECK(other.submit(&b,&reply,&ringclock)==kIOReturnSuccess && reply.fence==2);
        CHECK(flags==(std::vector<uint32_t>{NativeAdrenoRingSourceRender|NativeAdrenoRingStampContext,
                                            NativeAdrenoRingSourceExternal|NativeAdrenoRingStampContext}));
        volatile uint32_t counts[NativeAdrenoFaultVerdicts]={};
        auto note=[&](uint64_t far,uint32_t completed,uint32_t issued,bool sampled) {
            return NativeAdrenoFaultNote(windows,&p.execbuf,far,completed,issued,sampled,counts);
        };
        auto counted=[&](uint32_t alike,uint32_t apart,uint32_t undecided,uint32_t polled) {
            return counts[NativeAdrenoFaultAlike]==alike && counts[NativeAdrenoFaultApart]==apart &&
                   counts[NativeAdrenoFaultUndecided]==undecided && counts[NativeAdrenoFaultPolled]==polled;
        };
        /* Both in flight: undecided; the window's connection is charged. */
        CHECK(note(windows[0].start+0x100,0,2,true)==0 && counted(0,0,1,0));
        /* The render server's job done, the other connection's alone in flight: a fault in the render server's window still charges the
         * render server (the faulting job may have completed just before the top half read the fences) and counts apart; one in the
         * other connection's own window is alike; one in the kernel range charges nobody (an SQE prefetch is nobody's) and counts apart. */
        complete(1);
        CHECK(note(windows[0].start+0x100,1,2,true)==0 && counted(0,1,1,0));
        CHECK(note(windows[3].start+0x100,1,2,true)==3 && counted(1,1,1,0));
        CHECK(note(0x100000,1,2,true)==NativeAdrenoMaxClients && counted(1,2,1,0));
        /* Found by the poll (no fences at the fault): polled. */
        CHECK(note(windows[0].start+0x100,1,2,false)==0 && counted(1,2,1,1));
        /* Nothing in flight, or more fences than the history keeps: undecided. */
        complete(2);
        CHECK(note(windows[3].start,2,2,true)==3 && counted(1,2,2,1));
        CHECK(note(windows[0].start,0,70,true)==0 && counted(1,2,3,1));
        /* Before the shell published (no windows, no submit object): nobody, undecided or polled. */
        CHECK(NativeAdrenoFaultNote(nullptr,static_cast<const NativeAdrenoSubmit *>(nullptr),windows[3].start,1,2,true,counts)==
              NativeAdrenoMaxClients && counted(1,2,4,1));
        CHECK(NativeAdrenoFaultNote(nullptr,static_cast<const NativeAdrenoSubmit *>(nullptr),windows[3].start,1,2,false,counts)==
              NativeAdrenoMaxClients && counted(1,2,4,2));
        /* The pure verdict: ranges up to 31, the kernel range or a free window as the FAR side. */
        CHECK(NativeAdrenoFaultVerdictFor(NativeAdrenoMaxClients,true,true,1u<<31)==NativeAdrenoFaultApart);
        CHECK(NativeAdrenoFaultVerdictFor(NativeAdrenoMaxClients,false,true,1u)==NativeAdrenoFaultPolled);
        CHECK(NativeAdrenoFaultVerdictFor(2,true,false,1u<<2)==NativeAdrenoFaultUndecided);
        CHECK(NativeAdrenoFaultVerdictFor(2,true,true,1u<<2|1u)==NativeAdrenoFaultUndecided);
        CHECK(NativeAdrenoFaultVerdictFor(5,true,true,1u<<5)==NativeAdrenoFaultAlike && NativeAdrenoFaultVerdicts==4);
        /* Closed: the mark is gone and cannot be set; a new open is external again. */
        CHECK(p.client.releaseAll(&ringclock,1000)==kIOReturnSuccess && other.releaseAll(&ringclock,1000)==kIOReturnSuccess);
        p.client.setRenderServer(true);
        CHECK(p.client.source()==NativeAdrenoRingSourceExternal);
        CHECK(p.client.open(NativeAdrenoAbiVersion,&p.vm,&p.execbuf,&p.ring,32,36)==kIOReturnSuccess &&
              p.client.source()==NativeAdrenoRingSourceExternal);
        CHECK(p.client.close()==kIOReturnSuccess);
        p.ring.setWaiter(nullptr);
        ++attribution;
    }
    std::printf("PASS: %u pagani-gpu-kopt blocks: the contiguous attempt from 64 to 128 KiB with its 5 s backoff by the clock (any connection's, "
        "a smaller size still asks, a demand always does), a pooled BO's zero fill after the Memory lock and its failure given back under "
        "it, with each bit off the behaviour before them; the core's figures for the call meter\n",koptBlocks);
    std::printf("PASS: %u attribution blocks: render-server connections stamp their jobs as the render server's (only while open), "
        "the page fault charged to its FAR window's connection whatever was in flight (nobody for the kernel range), the jobs in flight "
        "only counted against it (alike, apart, undecided, polled)\n",attribution);
    std::printf("PASS: %u user-client blocks over software SMMU/CP peers: transaction with wait_fence validated against the ring (no CPU "
        "wait), per-connection faults with FAR attribution, 32 slots with 4 GiB (1+3 large+8 small)/16 GiB (1+3 large+28 small)/2 GiB "
        "windows, slot classes (apps leave 3/4 small slots to the listed processes, which may borrow a large one; an app never takes a "
        "large one), terms per process (the render server's in any slot), per-name rows (an idle row without hangs given up), the "
        "grant and large-client lists and the open policy with the hang refusal, capacity 256..16384 and the render server's reserve "
        "(mapping records, table ranges, BO pool), imports with direction-split "
        "maintenance, M9 scanout import, FIFO handle quarantine (4096) and slot encoding, ring-full retry vs Busy, M10 lock use (no "
        "Memory lock outside the pool), "
        "dispatch sizes, refusals, teardown, device info, two clients, late scope, retire, contiguous fallback, poisoned commit, "
        "CPU leases, queues, connection bytes by kind, teardown past a refused object\n",blocks);
}
/* Physical-page maintenance (imported BOs): record it; no host page is touched. */
/* Host double: the page attribute change of user BOs has no host equivalent (no host page is touched); counted with or without the
 * Memory lock while the M10 lock block watches it. */
IOReturn NativeAdrenoSetPagesCacheMode(const NativeAdrenoPhysicalSpan *spans,size_t count,bool)
{
    if(gMemoryDepth)++(*gMemoryDepth?gPagesUnder:gPagesOutside);
    return spans && count?kIOReturnSuccess:kIOReturnBadArgument;
}
IOReturn NativeAdrenoReassertWriteCombined(const NativeAdrenoPhysicalSpan *spans,size_t count,uint32_t *reset)
{return spans && count && reset?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoMaintainPhysicalPages(uint64_t physical,size_t bytes,NativeAdrenoCacheOperation op)
{
    if(!physical || !bytes || ((physical|bytes)&4095) || op>NativeAdrenoInvalidate)std::abort();
    ++fixture.physical_calls;fixture.physical_bytes+=bytes;return kIOReturnSuccess;
}
