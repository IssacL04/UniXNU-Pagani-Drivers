/* SPDX-License-Identifier: MIT */
/* The shared VM (ARCHITECTURE 4.6, 4.14.1, 5.4) over real kit arenas and the IOKit doubles, with the board programmer of the
 * 13T modelled (its TLB I/O over tests/A830SmmuModel.h: single 64-bit TLBIVAL, 32-bit TLBIASID/TLBSYNC) and, for the older
 * software peers, the kit's own stream state. What it shows: attach proves the regions once and the transactions read no SMMU
 * register; no fault status ever poisons the VM; unmap waits for the buffer's own last fence, not for an idle GPU; the 16 GiB
 * aperture (8209 table pages), the 4 GiB one (2053) and the 2 GiB fallback (1027); the coherent mapping policy and the maintained list; reserved
 * windows; the coherent table walk of M10; poisoning keeps every reference and FAULTs the streams. No hardware is touched. */
#include "XnuMemoryFixture.h"
#include "A830SmmuModel.h"
#include "NativeAdrenoVm.h"
#include "NativeAdrenoMeter.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
IOReturn NativeAdrenoQueryCpu4K(NativeAdrenoCpuState *out)
{CHECK(out);*out={};return kIOReturnSuccess;}
static unsigned tableCleans;
IOReturn NativeAdrenoMaintainOwnedPages(void *mapping,size_t bytes,NativeAdrenoCacheOperation op)
{
    CHECK(mapping && bytes && !(((uintptr_t)mapping|bytes)&4095));
    CHECK(op==NativeAdrenoClean || op==NativeAdrenoCleanInvalidate || op==NativeAdrenoInvalidate);
    ++fixture.cache_calls;if(bytes==4096 && op==NativeAdrenoClean)++tableCleans;
    return fixture.cache_calls==fixture.cache_fail_call?kIOReturnIOError:kIOReturnSuccess;
}
extern "C" {extern volatile uint64_t native_adreno_vm_phase_us[2][8];}
static uint64_t nowUs() {return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}

static A830SmmuModel model;
/* The board programmer as the VM uses it: the TLB I/O and the read-only verify, the FAULT of the streams. */
struct Programmer {
    IOReturn verify=kIOReturnSuccess;
    unsigned verifies=0,faults=0;
    bool stuck_after=false;unsigned tlb_reads=0,tlb_limit=0;
    void (*tlb_sync_hook)()=nullptr;
};
static Programmer prog;
static int tableRead32(void *cookie,size_t at,uint32_t *v)
{
    if((at&0xFFF)==0x7F4 && prog.tlb_limit && ++prog.tlb_reads>prog.tlb_limit)return 1;   /* the 10 ms budget ran out */
    if((at&0xFFF)==0x7F4 && prog.tlb_sync_hook)prog.tlb_sync_hook();                      /* an unmap's TLBSYNC, under the VM lock */
    return A830SmmuModel::read32(cookie,at,v);
}
static ArmSmmu4KIo tableIo;
static IOReturn verify(void *,unsigned bank,const ArmSmmu4KStream *s,size_t n)
{CHECK(bank==0 && s && n==2 && s[0].sid==0 && s[1].sid==1);++prog.verifies;return prog.verify;}
static IOReturn faultStreams(void *,unsigned bank,const ArmSmmu4KStream *,size_t n)
{CHECK(bank==0 && n==2);++prog.faults;model.at(0xC00)=0x20300;model.at(0xC00+12)=0x20300;return kIOReturnSuccess;}
static IOReturn unused(void *,unsigned,const ArmSmmu4KStream *) {CHECK(0);return kIOReturnUnsupported;}
static IOReturn unusedProgram(void *,unsigned,const ArmSmmu4KContextImage *,uint64_t,NativeAdrenoSmmuProgramReport *) {CHECK(0);return 1;}
static IOReturn unusedDisable(void *,unsigned) {CHECK(0);return kIOReturnUnsupported;}
static NativeAdrenoSmmuProgrammer programmer;
static bool syncAfterMap;   /* the programmer's sync_after_map (A830Smmu sets it in the annibale profile) */
static const ArmSmmu4KStream routes[2]={{0,0},{3,1}};

struct Fixture {
    NativeAdrenoMemory tables,object;
    ArmSmmu4KLiveArena arena;
    ArmSmmu4KLiveState state{};
    ArmSmmu4KLiveVisibility visibility;
    ArmSmmu4KContextConfig context{};
    NativeAdrenoVm vm;
    std::vector<uint32_t> aliasScratch;
};
static void smmuReset()
{
    model.boot(77);prog=Programmer{};
    tableIo=model.io;tableIo.read32=tableRead32;
    programmer={&prog,&tableIo,0x4C017E18,0x40000015,0x5511,unused,unusedProgram,unused,verify,faultStreams,unusedDisable,
                syncAfterMap};
}
/* A prepared lower arena of `pages` leaves at `base`, bound to CB0 as the A830 programmer left it. */
static IOReturn exposed(Fixture &s,size_t capacity=32,size_t pages=16,uint64_t base=0x800000,size_t tablePages=0,unsigned inputBits=32)
{
    smmuReset();
    if(!tablePages)tablePages=std::max(size_t(32),pages/512+8);
    IOReturn status=s.tables.allocate(tablePages*4096,36,true,tablePages*4096);if(status)return status;
    status=NativeAdrenoPrepareSmmuMemory(&s.tables,inputBits,36,base,pages*UINT64_C(4096),0,pages,&s.arena,&s.state,&s.visibility);
    if(status)return status;
    s.context=(ArmSmmu4KContextConfig){.lower=&s.arena.config,.lower_tables=&s.state.tables,
        .id0=0x4C017E18,.id2=0x5511,.asid=1,.coherent_walk=1,.stall_on_fault=0,.continue_after_fault=1};
    const NativeAdrenoSmmuBinding binding{&programmer,&s.context,0,A830SmmuModel::cb(0),routes,2};
    NativeAdrenoVmRegion lower={&s.tables,&s.arena,&s.state};
    return s.vm.attach(binding,&lower,nullptr,capacity,1000);
}
static IOReturn object(Fixture &s,size_t pages)
{return s.object.allocate(pages*4096,36,false,pages*4096);}
static void release(Fixture &s)
{
    CHECK(!s.vm.detach() && !s.tables.retireAfterQuiescence() && !s.tables.release());
    CHECK(fixture.live.empty() && fixture.metadata.empty());
}

/* The board path: attach proves both regions and the routes once and records the regions as proved at CB0's offset; a map
 * reads no SMMU register and issues no TLB invalidation; an unmap invalidates its pages as single 64-bit TLBIVAL stores with
 * ASID 1 and one TLBSYNC; a job or an unfinished last use keeps the buffer mapped. */
static void transactions()
{
    Fixture s;
    CHECK(exposed(s)==kIOReturnSuccess && prog.verifies==1 && s.state.generation==1);
    CHECK(s.state.context_offset==A830SmmuModel::cb(0) && s.vm.attached() && s.vm.bank()==0);
    CHECK(object(s,2)==kIOReturnSuccess);
    uint64_t address=0,handle=0;
    CHECK(s.vm.findAddress(0,8192,8192,&address)==kIOReturnSuccess && address==0x800000);
    const unsigned reads=model.reads,writes=model.writes;
    CHECK(s.vm.map(&s.object,0,address,3,0,2,&handle)==kIOReturnSuccess);
    CHECK(model.reads==reads && model.writes==writes);   /* no register touched by a map */
    CHECK(handle && s.vm.objectCount()==1 && s.vm.spanCount()==s.object.spanCount() && s.object.deviceReferences()==1);
    CHECK(s.vm.findAddress(0,8192,8192,&address)==kIOReturnSuccess && address==0x802000);
    CHECK(s.vm.holdJob(handle)==kIOReturnSuccess && s.object.deviceReferences()==2);
    CHECK(s.vm.unmap(handle)==kIOReturnBusy);
    CHECK(s.vm.retireJobAfterCompletion(handle)==kIOReturnSuccess);
    /* The buffer's own last use (fence 7) against the memptrs fence: no global idle. */
    volatile uint32_t fence=5;
    CHECK(s.vm.noteUse(handle,7)==kIOReturnSuccess && s.vm.lastUse(handle)==7 && !s.vm.idle(handle));
    CHECK(s.vm.unmap(handle)==kIOReturnBusy);   /* no fence source: a used mapping is busy */
    s.vm.setFenceSource(&fence);
    CHECK(s.vm.unmap(handle)==kIOReturnBusy && s.vm.noteUse(handle,6)==kIOReturnSuccess && s.vm.lastUse(handle)==7);
    fence=7;
    CHECK(s.vm.idle(handle));
    model.tlbival.clear();
    const unsigned syncs=model.tlbsync,asids=model.tlbiasid;
    CHECK(s.vm.unmap(handle)==kIOReturnSuccess);
    CHECK(model.tlbival.size()==2 && model.tlbival[0]==(uint64_t(1)<<48|0x800) && model.tlbival[1]==(uint64_t(1)<<48|0x801));
    CHECK(model.tlbsync==syncs+1 && model.tlbiasid==asids && model.violations==0 && model.aborts==0);
    CHECK(s.vm.objectCount()==0 && s.vm.spanCount()==0 && s.object.deviceReferences()==0 && s.object.release()==kIOReturnSuccess);
    /* Fences wrap at 32 bits as the memptrs word does. */
    CHECK(!object(s,1));
    CHECK(s.vm.map(&s.object,0,0x800000,3,0,2,&handle)==kIOReturnSuccess);
    fence=0xFFFFFFF0u;
    CHECK(s.vm.noteUse(handle,0x1'0000'0002ull)==kIOReturnSuccess && s.vm.unmap(handle)==kIOReturnBusy);
    fence=2;
    CHECK(s.vm.unmap(handle)==kIOReturnSuccess && !s.object.release());
    CHECK(s.tables.release()==kIOReturnBusy);
    release(s);
}

/* No fault status poisons the shared VM (ARCHITECTURE 4.12.3; the every-64th strict read-back of the inherited VM did, after
 * any client page fault): with a pending context fault and a non-zero sGFSR, hundreds of transactions still succeed. */
static unsigned faultsNeverPoison()
{
    Fixture s;
    CHECK(exposed(s,64,64)==kIOReturnSuccess);
    model.fault(0,0x8,0x20001000,0);model.at(0x48)=0x2;
    NativeAdrenoMemory bo;
    CHECK(!bo.allocate(4*4096,36,false,4*4096));
    unsigned rounds=0;
    for(unsigned i=0;i<200;++i) {
        uint64_t handle=0;
        CHECK(s.vm.map(&bo,0,0x800000+(i%16)*0x4000,3,0,2,&handle)==kIOReturnSuccess);
        CHECK(s.vm.unmap(handle)==kIOReturnSuccess);
        ++rounds;
    }
    uint32_t state[4];
    CHECK(!s.vm.poisoned() && s.vm.faultState(state)==kIOReturnSuccess && (state[0]&0x8) && state[1]==0x20001000);
    CHECK(prog.verifies==1 && !prog.faults && !bo.release());
    release(s);
    return rounds;
}

/* Refusals precede any ownership or table mutation; the reserved windows (null guard, GMEM) are never mapped and findAddress
 * steps over them. */
static void refusals()
{
    Fixture s;
    CHECK(exposed(s,32,16,0)==kIOReturnSuccess);
    CHECK(object(s,2)==kIOReturnSuccess);
    CHECK(s.vm.reserveWindow(0,0x2000)==kIOReturnSuccess && s.vm.reserveWindow(0x6000,0x2000)==kIOReturnSuccess);
    CHECK(s.vm.reserveWindow(0x1001,0x1000)==kIOReturnBadArgument && s.vm.reserveWindow(0x100000,0x1000)==kIOReturnBadArgument);
    unsigned refs=s.object.deviceReferences(),spans=s.vm.spanCount(),calls=fixture.cache_calls;
    uint64_t handle=0,address=0;
    CHECK(s.vm.map(&s.object,0,0x1,3,0,2,&handle)==kIOReturnBadArgument);
    CHECK(s.vm.map(&s.object,1,0x2000,3,0,2,&handle)==kIOReturnBadArgument);
    CHECK(s.vm.map(&s.object,0,0xF000,3,0,2,&handle)==kIOReturnBadArgument);   /* past the aperture */
    CHECK(s.vm.map(&s.object,0,0x2000,2,0,2,&handle)==kIOReturnBadArgument);   /* no read */
    CHECK(s.vm.map(&s.object,0,0x2000,3,2,2,&handle)==kIOReturnBadArgument);
    CHECK(s.vm.map(&s.object,0,0x2000,3,0,1,&handle)==kIOReturnBadArgument);
    CHECK(s.vm.map(nullptr,0,0x2000,3,0,2,&handle)==kIOReturnBadArgument);
    CHECK(s.vm.map(&s.object,0,0x0,3,0,2,&handle)==kIOReturnBadArgument);      /* window 0 */
    CHECK(s.vm.map(&s.object,0,0x5000,3,0,2,&handle)==kIOReturnBadArgument);   /* overlaps window 0x6000 */
    CHECK(!handle && refs==s.object.deviceReferences() && spans==s.vm.spanCount() && calls==fixture.cache_calls);
    CHECK(s.vm.findAddress(0,0x4000,4096,&address)==kIOReturnSuccess && address==0x2000);
    CHECK(s.vm.findAddress(0,0x6000,4096,&address)==kIOReturnSuccess && address==0x8000);
    CHECK(s.vm.map(&s.object,0,0x2000,3,0,2,&handle)==kIOReturnSuccess);
    const uint64_t mapped=handle;
    CHECK(s.vm.reserveWindow(0x2000,0x1000)==kIOReturnBusy);   /* something is mapped there */
    CHECK(s.vm.map(&s.object,0,0x2000,3,0,2,&handle)==kIOReturnBusy && !handle);
    CHECK(s.vm.unmap(mapped+100)==kIOReturnBadArgument && s.vm.holdJob(mapped+100)==kIOReturnBadArgument);
    CHECK(s.vm.retireJobAfterCompletion(mapped)==kIOReturnBadArgument && s.vm.noteUse(mapped,0)==kIOReturnBadArgument);
    CHECK(s.vm.unmap(mapped)==kIOReturnSuccess && !s.object.release());
    release(s);
}

/* Scattered BOs, one span per page, mapped in one transaction each into the middle, the end and the start of the table, and a
 * span-capacity refusal before ownership. */
static void scattered()
{
    Fixture s;
    CHECK(exposed(s,8)==kIOReturnSuccess);   /* aperture 0x800000..0x810000, 8 spans */
    fixture.mode=Scattered;
    NativeAdrenoMemory first,middle,last,huge;
    CHECK(!first.allocate(4096,36,false,4096) && !middle.allocate(4*4096,36,false,4*4096) && !last.allocate(4096,36,false,4096));
    CHECK(middle.spanCount()==4);
    uint64_t a=0,b=0,c=0,iova=0;size_t bytes=0;unsigned permissions=0;
    CHECK(!s.vm.map(&first,0,0x800000,3,0,2,&a) && !s.vm.map(&last,0,0x808000,3,0,2,&c));
    CHECK(!s.vm.map(&middle,0,0x802000,3,0,2,&b) && s.vm.spanCount()==6 && s.vm.lastResult().completed_spans==4);
    CHECK(!s.vm.describe(b,&iova,&bytes,&permissions) && iova==0x802000 && bytes==4*4096 && permissions==3);
    CHECK(s.vm.handleAt(0x803000)==b && !s.vm.handleAt(0x806000));
    CHECK(!s.vm.unmap(b) && s.vm.spanCount()==2 && s.vm.lastResult().completed_spans==4);
    CHECK(!s.vm.map(&middle,0,0x80a000,3,0,2,&b) && s.vm.spanCount()==6);
    CHECK(!s.vm.unmap(a) && s.vm.spanCount()==5);
    CHECK(!s.vm.map(&first,0,0x800000,3,0,2,&a) && s.vm.spanCount()==6);
    CHECK(!huge.allocate(3*4096,36,false,3*4096) && huge.spanCount()==3);
    uint64_t refused=0;const unsigned writes=model.writes;
    CHECK(s.vm.map(&huge,0,0x802000,3,0,2,&refused)==kIOReturnNoResources && !refused);
    CHECK(!huge.deviceReferences() && model.writes==writes && s.vm.spanCount()==6 && !s.vm.poisoned());
    CHECK(!s.vm.unmap(a) && !s.vm.unmap(b) && !s.vm.unmap(c) && s.vm.spanCount()==0);
    fixture.mode=Contiguous;
    CHECK(!first.release() && !middle.release() && !last.release() && !huge.release());
    release(s);
}

/* Physical overlap remains allowed only within the same attribute class (scratch and fallback, both smaller-side choices). Own
 * buffers own their pages (exclusive, no alias pass); imports (user ranges any process may import again) are checked. */
static void aliases()
{
    void *const task=reinterpret_cast<void *>(2);
    /* pagani-gpu-kopt bit 2 on (the table transaction takes preflight's answer) or off (it repeats the pass): the same refusals, and
     * always preflight's, before any store: a conflict the transaction found would poison the VM. */
    for(const uint32_t kopt:{uint32_t(NativeAdrenoKoptAll),uint32_t(NativeAdrenoKoptAll&~NativeAdrenoKoptAliasOnce)})for(bool scratch:{false,true}) {
        native_adreno_kopt=kopt;
        Fixture s;CHECK(exposed(s)==kIOReturnSuccess);
        s.aliasScratch.resize(32);
        if(scratch){s.arena.alias_scratch=s.aliasScratch.data();s.arena.alias_scratch_entries=s.aliasScratch.size();}
        NativeAdrenoMemory first,second,many;
        CHECK(!first.importTask(task,0x40000000,4096,36,4096));
        fixture.next_physical=first.spans()[0].physical;
        CHECK(!second.importTask(task,0x40010000,4096,36,4096) && second.spans()[0].physical==first.spans()[0].physical);
        fixture.next_physical=first.spans()[0].physical;
        CHECK(!many.importTask(task,0x40020000,8192,36,8192) && many.spanCount()==2);
        uint64_t a=0,b=0;CHECK(!s.vm.map(&first,0,0x800000,3,0,2,&a));
        CHECK(!s.vm.map(&second,0,0x802000,1,0,2,&b));CHECK(!s.vm.unmap(b));   /* the same class shares pages */
        const unsigned writes=model.writes;
        for(unsigned change=0;change<3;++change) {
            CHECK(s.vm.map(&second,0,0x802000,change==2?9:3,change==0?1:0,change==1?3:2,&b)==kIOReturnBadArgument);
            CHECK(!b&&!second.deviceReferences()&&model.writes==writes&&!s.vm.poisoned());
        }
        CHECK(s.vm.map(&many,0,0x802000,3,1,3,&b)==kIOReturnBadArgument);
        CHECK(!b&&!many.deviceReferences()&&!s.vm.poisoned());
        CHECK(!s.vm.unmap(a)&&!first.release()&&!second.release()&&!many.release());
        release(s);
    }
    native_adreno_kopt=NativeAdrenoKoptAll;
    for(const uint32_t kopt:{uint32_t(NativeAdrenoKoptAll),uint32_t(NativeAdrenoKoptAll&~NativeAdrenoKoptAliasOnce)}) {
        native_adreno_kopt=kopt;
        /* A large different-class scattered pair of imports: the alias pass sorts the smaller side in the arena's scratch. */
        constexpr size_t n=8192;
        Fixture s;CHECK(exposed(s,2*n,2*n)==kIOReturnSuccess);
        s.aliasScratch.resize(2*n);s.arena.alias_scratch=s.aliasScratch.data();s.arena.alias_scratch_entries=s.aliasScratch.size();
        NativeAdrenoMemory first,second;
        CHECK(!first.importTask(task,0x50000000,n*4096,36,n*4096)&&first.spanCount()==n);
        CHECK(!second.importTask(task,0x60000000,n*4096,36,n*4096)&&second.spanCount()==n);
        uint64_t a=0,b=0;CHECK(!s.vm.map(&first,0,0x800000,3,0,2,&a));
        native_adreno_vm_now_us=nowUs;uint64_t before=native_adreno_vm_phase_us[0][5],table=native_adreno_vm_phase_us[0][3];
        CHECK(!s.vm.map(&second,0,0x800000+n*4096,3,1,3,&b));
        std::printf("VM alias preflight diagnostic: %zu x %zu different-class spans, preflight %llu host us, table transaction with the "
            "pass %s %llu host us (not hardware timing)\n",n,n,(unsigned long long)(native_adreno_vm_phase_us[0][5]-before),
            kopt&NativeAdrenoKoptAliasOnce?"once":"twice",(unsigned long long)(native_adreno_vm_phase_us[0][3]-table));
        native_adreno_vm_now_us=nullptr;
        CHECK(s.vm.spanCount()==2*n&&!s.vm.unmap(b)&&!s.vm.unmap(a)&&!first.release()&&!second.release());
        release(s);
    }
    native_adreno_kopt=NativeAdrenoKoptAll;
}
/* Whether the table transaction repeats preflight's alias pass (pagani-gpu-kopt bit 2): both passes sort in the arena's scratch, and the
 * VM's clock runs between them and right after the transaction, so a clock that refills the scratch each time sees which stretch
 * touched it. The stretches: from the clock's first read of the map to the second (preflight), ..., the last before the map's end. */
static std::vector<uint32_t> *scratchSeen;
static unsigned scratchRead;
static uint32_t scratchTouched;   /* bit n: the scratch changed before the clock's read n */
static uint64_t scratchClock()
{
    bool touched=false;
    for(uint32_t &word:*scratchSeen) {
        if(word!=0xfeedfaceu)touched=true;
        word=0xfeedfaceu;
    }
    if(touched)scratchTouched|=1u<<scratchRead;
    ++scratchRead;
    return scratchRead;
}
static unsigned aliasPasses()
{
    unsigned checks=0;
    void *const task=reinterpret_cast<void *>(2);
    for(const bool once:{true,false}) {
        native_adreno_kopt=once?NativeAdrenoKoptAll:NativeAdrenoKoptAll&~NativeAdrenoKoptAliasOnce;
        Fixture s;CHECK(exposed(s)==kIOReturnSuccess);
        s.aliasScratch.assign(32,0xfeedfaceu);s.arena.alias_scratch=s.aliasScratch.data();s.arena.alias_scratch_entries=s.aliasScratch.size();
        NativeAdrenoMemory first,second;
        CHECK(!first.importTask(task,0x50000000,2*4096,36,2*4096) && !second.importTask(task,0x60000000,2*4096,36,2*4096));
        uint64_t a=0,b=0;
        CHECK(!s.vm.map(&first,0,0x800000,3,0,2,&a));     /* first map: no other class, nothing sorted */
        scratchSeen=&s.aliasScratch;scratchRead=0;scratchTouched=0;
        native_adreno_vm_now_us=scratchClock;
        CHECK(!s.vm.map(&second,0,0x804000,3,1,3,&b));    /* another class: not exclusive */
        native_adreno_vm_now_us=nullptr;
        /* Read 1 closes preflight, read 4 the table transaction (0 is the first read, 2 and 3 the hold and the maintenance). */
        CHECK(scratchRead>=5 && (scratchTouched&2) && !(scratchTouched&(1u|4u|8u)));
        CHECK(bool(scratchTouched&16)==!once);
        CHECK(s.vm.spanCount()==4 && !s.vm.poisoned() && !s.vm.unmap(b) && !s.vm.unmap(a) && !first.release() && !second.release());
        release(s);
        ++checks;
    }
    native_adreno_kopt=NativeAdrenoKoptAll;
    return checks;
}
/* The VM's clock (pagani-gpu-kopt bit 5, the publish shell sets it): without it the kinds still count, every time is 0; with it the
 * phase and kind counters run. And the call meter's wait for the VM lock, made deterministic: the thread holding the lock inside an
 * unmap lets go only once the other thread's failed try read the clock. */
static std::atomic<uint64_t> fakeUs;
static std::atomic<bool> holderInside,waiterSaw;
static thread_local bool waitingThread;
static uint64_t fakeClock() {if(waitingThread)waiterSaw=true;return fakeUs+=1000;}
static unsigned vmClock()
{
    unsigned checks=0;
    NativeAdrenoMemory bo,import;
    void *const task=reinterpret_cast<void *>(2);
    Fixture s;CHECK(exposed(s)==kIOReturnSuccess);
    CHECK(!bo.allocate(4096,36,false,4096) && !import.importTask(task,0x70000000,4096,36,4096));
    auto kind=[](unsigned k,unsigned word) {return uint64_t(native_adreno_vm_kind_us[k][word]);};
    uint64_t handle=0,count=kind(0,0),time=kind(0,1),phases=native_adreno_vm_phase_us[0][0];
    CHECK(!native_adreno_vm_now_us && !s.vm.map(&bo,0,0x800000,3,0,2,&handle));
    CHECK(kind(0,0)==count+1 && kind(0,1)==time && native_adreno_vm_phase_us[0][0]==phases);++checks;
    CHECK(!s.vm.unmap(handle));
    native_adreno_vm_now_us=fakeClock;fakeUs=0;
    count=kind(0,0);time=kind(0,1);
    CHECK(!s.vm.map(&bo,0,0x800000,3,0,2,&handle) && kind(0,0)==count+1 && kind(0,1)>time && native_adreno_vm_phase_us[0][0]==phases+1);++checks;
    uint64_t imported=0;count=kind(1,0);time=kind(1,1);
    CHECK(!s.vm.map(&import,0,0x801000,3,0,2,&imported) && kind(1,0)==count+1 && kind(1,1)>time);++checks;
    count=kind(3,0);time=kind(3,1);
    CHECK(!s.vm.unmap(imported) && kind(3,0)==count+1 && kind(3,1)>time);++checks;
    /* The wait: map again (the handle's unmap below holds the lock), the second thread asks while the first is inside. */
    for(unsigned i=0;i<Adreno830WaitKinds;++i)native_adreno_meter_waits[i]=0;
    holderInside=false;waiterSaw=false;
    prog.tlb_sync_hook=[] {
        holderInside=true;
        const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(10);   /* a waiter that reads no clock fails below, it does not hang */
        while(!waiterSaw && std::chrono::steady_clock::now()<limit)std::this_thread::yield();
    };
    uint64_t other=0;
    CHECK(!s.vm.map(&import,0,0x801000,3,0,2,&other));
    std::thread holder([&] {CHECK(!s.vm.unmap(handle));});
    while(!holderInside)std::this_thread::yield();
    std::thread waiter([&] {
        waitingThread=true;
        uint64_t iova=0;size_t bytes=0;unsigned permissions=0;
        CHECK(!s.vm.describe(other,&iova,&bytes,&permissions) && bytes==4096);
    });
    holder.join();waiter.join();
    prog.tlb_sync_hook=nullptr;
    CHECK(native_adreno_meter_waits[Adreno830WaitVm]>=1000 && !native_adreno_meter_waits[Adreno830WaitRing]);++checks;
    native_adreno_vm_now_us=nullptr;
    CHECK(!s.vm.unmap(other) && !bo.release() && !import.release());
    release(s);
    return checks;
}

/* ARCHITECTURE 4.14.1: the attributes of a client mapping and the per-submission maintenance at each coherence level. */
static NativeAdrenoCacheOperation lastPhysicalOp;
static unsigned coherencePolicy()
{
    unsigned checks=0;
    NativeAdrenoMemory own,ownWc,import,scanout;
    CHECK(!own.allocate(4096,36,false,4096) && !ownWc.allocate(4096,36,false,4096) && !ownWc.makeWriteCombined());
    IOMemoryDescriptor *range=IOMemoryDescriptor::withAddressRange(0x10000000,0x4000,kIODirectionInOut|kIOMemoryMapperNone,
        reinterpret_cast<task_t>(2));
    CHECK(range && !scanout.importDevice(range,0,0x4000) && scanout.scanout());
    CHECK(!import.importTask(reinterpret_cast<void *>(2),0x20000000,0x1000,36,0x1000) && import.imported());
    struct Want {const NativeAdrenoMemory *memory;unsigned permissions,requested,level,wb;};
    const Want table[]={
        {&ownWc,3,0,0,0},{&ownWc,3,1,0,0},{&ownWc,3,1,1,0},{&ownWc,3,1,2,1},{&ownWc,3,0,2,0},
        {&import,1,0,0,0},{&import,1,0,1,1},{&import,1,0,2,1},
        {&import,3,0,0,0},{&import,3,1,1,0},{&import,3,0,2,1},
        {&scanout,3,1,2,0},{&scanout,3,0,0,0}};
    for(const Want &w:table) {
        NativeAdrenoVm::setCoherence(w.level);
        unsigned wb=9,sh=9;
        NativeAdrenoVm::attributes(*w.memory,w.permissions,w.requested,3,&wb,&sh);
        CHECK(wb==w.wb && sh==(w.wb?3u:2u) && NativeAdrenoVm::coherence()==w.level);
        ++checks;
    }
    NativeAdrenoVm::setCoherence(7);
    CHECK(NativeAdrenoVm::coherence()==2 && native_adreno_io_coherent==2);
    /* mapClient: the policy replaces what was asked; the scanout maps write-only in effect (read is required by the format) and
     * takes no CPU maintenance; a GPU-read-only import at level 0 joins the maintained list and is cleaned before each
     * submission, nothing else is. */
    Fixture s;
    CHECK(exposed(s,64,64)==kIOReturnSuccess);
    NativeAdrenoVm::setCoherence(0);
    uint64_t hImport=0,hOwn=0,hWc=0,hScan=0;
    const unsigned physical=fixture.physical_calls;
    CHECK(s.vm.mapClient(&scanout,0x800000,3,1,3,&hScan)==kIOReturnSuccess && fixture.physical_calls==physical);
    CHECK(s.vm.map(&scanout,0,0x810000,3,1,3,&hImport)==kIOReturnBadArgument);   /* never write-back */
    CHECK(s.vm.mapClient(&import,0x820000,1,0,2,&hImport)==kIOReturnSuccess && fixture.physical_calls==physical+1);
    CHECK(s.vm.mapClient(&ownWc,0x830000,3,1,3,&hWc)==kIOReturnSuccess);
    CHECK(s.vm.mapClient(&own,0x838000,3,0,2,&hOwn)==kIOReturnSuccess);
    CHECK(s.vm.maintainedCount()==2);   /* the read-only import, and the own BO with a cacheable CPU view */
    const unsigned calls=fixture.physical_calls,owned=fixture.cache_calls;
    CHECK(s.vm.cleanForSubmission()==kIOReturnSuccess && fixture.physical_calls==calls+1 && fixture.cache_calls==owned+1);
    CHECK(s.vm.publishMapped()==kIOReturnSuccess && s.vm.acquireImported()==kIOReturnSuccess);
    CHECK(fixture.physical_calls==calls+2 && fixture.cache_calls==owned+3);   /* acquire: the own cacheable BO only */
    CHECK(s.vm.unmap(hImport)==kIOReturnSuccess && fixture.physical_calls==calls+2 && s.vm.maintainedCount()==1);
    NativeAdrenoVm::setCoherence(1);
    /* NativeAdrenoImportClean, what the plugin's seed rule rests on: a GPU-written import, non-cacheable at the phone's level, has its
     * whole range cleaned and invalidated through the physical pages at the map, before the table update, and never again. */
    {
        uint64_t hWritten=0;
        const unsigned before=fixture.physical_calls;
        const size_t bytes=fixture.physical_bytes;
        CHECK(s.vm.mapClient(&import,0x820000,3,0,2,&hWritten)==kIOReturnSuccess);
        CHECK(fixture.physical_calls==before+import.spanCount() && fixture.physical_bytes==bytes+import.bytes());
        CHECK(lastPhysicalOp==NativeAdrenoCleanInvalidate && s.vm.maintainedCount()==1);
        CHECK(!s.vm.cleanForSubmission() && !s.vm.acquireImported() && fixture.physical_bytes==bytes+import.bytes());
        CHECK(!s.vm.unmap(hWritten) && fixture.physical_bytes==bytes+import.bytes());
    }
    CHECK(s.vm.mapClient(&import,0x820000,1,0,2,&hImport)==kIOReturnSuccess && s.vm.maintainedCount()==1);
    CHECK(!s.vm.unmap(hImport) && !s.vm.unmap(hOwn) && !s.vm.unmap(hWc) && !s.vm.unmap(hScan) && !s.vm.maintainedCount());
    NativeAdrenoVm::setCoherence(0);
    CHECK(!own.release() && !ownWc.release() && !import.release() && !scanout.release());
    range->release();
    release(s);
    return checks;
}

/* The 4 GiB aperture of CB0 (2053 table pages, 1 << 20 leaves) and the 2 GiB fallback (1027): the first and the last page. */
static void apertures()
{
    for(unsigned gib:{4u,2u}) {
        Fixture s;
        const size_t leaves=size_t(gib)<<18,pages=gib==4?2053:1027;
        CHECK(exposed(s,64,leaves,0,pages)==kIOReturnSuccess && s.state.tables.table_pages==pages);
        CHECK(s.vm.reserveWindow(0,0x100000)==kIOReturnSuccess && s.vm.reserveWindow(0x4000000,0xC00000)==kIOReturnSuccess);
        NativeAdrenoMemory bo,ring;
        CHECK(!bo.allocate(0x2000,36,false,0x2000) && !ring.allocate(0x8000,36,true,0x8000));
        uint64_t top=0,low=0,address=0;
        const uint64_t end=uint64_t(gib)<<30;
        CHECK(s.vm.map(&bo,0,end-0x2000,3,0,2,&top)==kIOReturnSuccess);
        CHECK(s.vm.map(&ring,0,0x200000,ArmSmmu4KRead|ArmSmmu4KPrivileged,0,2,&low)==kIOReturnSuccess);
        CHECK(s.vm.map(&ring,0,end,3,0,2,&address)==kIOReturnBadArgument);
        CHECK(s.vm.findAddress(0,0x1000,0x1000,&address)==kIOReturnSuccess && address==0x100000);
        CHECK(s.vm.findAddress(0,0x3800000,0x1000000,&address)==kIOReturnSuccess && address==0x5000000);   /* past GMEM */
        CHECK(!s.vm.unmap(top) && !s.vm.unmap(low) && !bo.release() && !ring.release());
        release(s);
    }
}

/* What a stage-1 walk from the root (level 1, one contiguous pool) finds for va: the leaf, 0 for an invalid entry on the way. */
static uint64_t walked(const Fixture &s,uint64_t va)
{
    const uint64_t base=s.tables.spans()[0].physical,address=UINT64_C(0xFFFFFFFFF000);
    const volatile uint64_t *tables=static_cast<const volatile uint64_t *>(s.tables.mapping());
    uint64_t entry=tables[va>>30];
    if((entry&3)!=3)return 0;
    entry=tables[((entry&address)-base)/8+((va>>21)&511)];
    if((entry&3)!=3)return 0;
    return tables[((entry&address)-base)/8+((va>>12)&511)];
}
/* The 16 GiB aperture, the default since 2026-10-31 (8209 table pages, 1 << 22 leaves, a 34-bit input): every client window from slot 5 up
 * lies past 4 GiB, which nothing but this aperture reaches. A BO across 4 GiB (no window holds one since 2026-11-01; the VM does not care) and one in the last page of
 * the top window land in the right leaves; the unmap's TLBIVAL carry the page numbers past 32 bits whole; a leaf above 4 GiB is not
 * the leaf below it (no page number cut to 32 bits on the way); the end of the aperture is refused. */
static void wideAperture()
{
    Fixture s;
    CHECK(exposed(s,64,size_t(1)<<22,0,8209,34)==kIOReturnSuccess && s.state.tables.table_pages==8209);
    CHECK(s.vm.reserveWindow(0,0x100000)==kIOReturnSuccess && s.vm.reserveWindow(0x4000000,0xC00000)==kIOReturnSuccess);
    NativeAdrenoMemory across,top,below,above;
    CHECK(!across.allocate(0x4000,36,true,0x4000) && !top.allocate(0x1000,36,false,0x1000) && !below.allocate(0x1000,36,false,0x1000) &&
          !above.allocate(0x1000,36,false,0x1000));
    const uint64_t acrossAt=UINT64_C(0xFFFFE000),topAt=UINT64_C(0x3FFFFD000),lowAt=UINT64_C(0xE0000000),highAt=lowAt+(UINT64_C(1)<<32);
    uint64_t handles[4]={},address=0;
    CHECK(s.vm.map(&across,0,acrossAt,3,0,2,&handles[0])==kIOReturnSuccess);
    CHECK(s.vm.map(&top,0,topAt,3,0,2,&handles[1])==kIOReturnSuccess);
    CHECK(s.vm.map(&below,0,lowAt,3,0,2,&handles[2])==kIOReturnSuccess);
    for(unsigned page=0;page<4;++page)
        CHECK((walked(s,acrossAt+page*UINT64_C(0x1000))&UINT64_C(0xFFFFFFFFF000))==across.spans()[0].physical+page*UINT64_C(0x1000));
    CHECK(walked(s,acrossAt+0x4000)==0 && walked(s,topAt+0x1000)==0);
    CHECK((walked(s,topAt)&UINT64_C(0xFFFFFFFFF000))==top.spans()[0].physical);
    CHECK((walked(s,lowAt)&UINT64_C(0xFFFFFFFFF000))==below.spans()[0].physical && !walked(s,highAt));
    CHECK(s.vm.map(&above,0,highAt,3,0,2,&handles[3])==kIOReturnSuccess);
    CHECK((walked(s,highAt)&UINT64_C(0xFFFFFFFFF000))==above.spans()[0].physical &&
          (walked(s,lowAt)&UINT64_C(0xFFFFFFFFF000))==below.spans()[0].physical);
    CHECK(!s.vm.unmap(handles[3]));
    CHECK(s.vm.map(&top,0,UINT64_C(0x400000000),3,0,2,&address)==kIOReturnBadArgument);
    model.tlbival.clear();
    CHECK(!s.vm.unmap(handles[1]) && !s.vm.unmap(handles[0]));
    CHECK(model.tlbival.size()==5 && model.tlbival[0]==(uint64_t(1)<<48|0x3FFFFD));
    for(unsigned page=0;page<4;++page)CHECK(model.tlbival[1+page]==(uint64_t(1)<<48|(UINT64_C(0xFFFFE)+page)));
    CHECK(!walked(s,acrossAt) && !walked(s,topAt) && !s.vm.unmap(handles[2]));
    CHECK(!across.release() && !top.release() && !below.release() && !above.release());
    release(s);
}

/* M10: once V4 proved the walk coherent a changed table page takes only dsb ishst, no clean; switching back cleans again. */
static void coherentWalk()
{
    Fixture s;
    CHECK(exposed(s)==kIOReturnSuccess && !s.vm.tableWalkCoherent());
    NativeAdrenoMemory bo;
    CHECK(!bo.allocate(4096,36,false,4096) && !bo.makeWriteCombined());
    uint64_t handle=0;
    unsigned before=tableCleans;
    CHECK(!s.vm.map(&bo,0,0x800000,3,0,2,&handle) && tableCleans==before+1 && !s.vm.unmap(handle) && tableCleans==before+2);
    CHECK(s.vm.setTableWalkCoherent(true)==kIOReturnSuccess && s.vm.tableWalkCoherent());
    before=tableCleans;
    CHECK(!s.vm.map(&bo,0,0x800000,3,0,2,&handle) && !s.vm.unmap(handle) && tableCleans==before);
    CHECK(s.vm.setTableWalkCoherent(false)==kIOReturnSuccess);
    CHECK(!s.vm.map(&bo,0,0x800000,3,0,2,&handle) && tableCleans==before+1 && !s.vm.unmap(handle));
    CHECK(!bo.release());
    release(s);
}

/* What poisons: a failed CPU publication after the hold, a TLB sync past its budget, a refused route proof at attach. Every
 * reference stays, the streams FAULT, every later call is refused. */
static void poisoning()
{
    {
        Fixture s;
        CHECK(exposed(s)==kIOReturnSuccess);
        CHECK(object(s,2)==kIOReturnSuccess);
        fixture.cache_fail_call=fixture.cache_calls+1;
        uint64_t handle=0;
        CHECK(s.vm.map(&s.object,0,0x800000,3,0,2,&handle)==kIOReturnIOError);
        CHECK(!handle && s.vm.poisoned() && s.vm.spanCount()==0 && prog.faults==1 && model.at(0xC00)==0x20300);
        CHECK(s.object.quarantined() && s.object.deviceReferences()>=1 && s.tables.quarantined());
        CHECK(s.state.prepared && s.state.poisoned);
        CHECK(s.vm.map(&s.object,0,0x800000,3,0,2,&handle)==kIOReturnNotReady);
        CHECK(s.vm.unmap(0)==kIOReturnNotReady && s.vm.detach()==kIOReturnBusy);
        CHECK(s.object.release()==kIOReturnBusy && s.tables.release()==kIOReturnBusy);
        fixture.cache_fail_call=0;
    }
    {
        Fixture s;
        CHECK(exposed(s)==kIOReturnSuccess);
        NativeAdrenoMemory bo;CHECK(!bo.allocate(4096,36,false,4096));
        uint64_t handle=0;
        CHECK(!s.vm.map(&bo,0,0x800000,3,0,2,&handle));
        prog.tlb_limit=prog.tlb_reads+1;model.tlb_busy=100;
        CHECK(s.vm.unmap(handle)==kIOReturnIOError && s.vm.poisoned() && prog.faults==1 && bo.quarantined());
        CHECK(s.vm.invalidateTlb()==kIOReturnNotReady);
    }
    {
        Fixture s;
        model.boot(78);prog=Programmer{};prog.verify=kIOReturnIOError;
        CHECK(s.tables.allocate(32*4096,36,true,32*4096)==kIOReturnSuccess);
        CHECK(!NativeAdrenoPrepareSmmuMemory(&s.tables,32,36,0x800000,16*4096,0,16,&s.arena,&s.state,&s.visibility));
        s.context=(ArmSmmu4KContextConfig){.lower=&s.arena.config,.lower_tables=&s.state.tables,
            .id0=0x4C017E18,.id2=0x5511,.asid=1,.coherent_walk=1,.stall_on_fault=0,.continue_after_fault=1};
        NativeAdrenoVmRegion lower={&s.tables,&s.arena,&s.state};
        const NativeAdrenoSmmuBinding binding{&programmer,&s.context,0,A830SmmuModel::cb(0),routes,2};
        CHECK(s.vm.attach(binding,&lower,nullptr,32,1000)==kIOReturnIOError && s.vm.poisoned() && prog.faults==1);
        const NativeAdrenoSmmuBinding empty{};
        NativeAdrenoVm other;
        CHECK(other.attach(empty,&lower,nullptr,32,1000)==kIOReturnBadArgument);
    }
    fixture=MemoryFixture{};   /* the poisoned objects above stay quarantined on purpose */
}

/* A programmer with sync_after_map (A830Smmu's annibale profile, kernel-sequences section 14 row 12): every successful map ends
 * with exactly one TLBIASID of the context's ASID and one TLBSYNC (TLBSTATUS polled) and writes no other register; a refused
 * map reaches no register; the unmap stays its pages' TLBIVAL and one TLBSYNC; a sync past its budget fails the map and
 * poisons the VM like any table transaction (streams FAULT, the buffer stays referenced). Returns the checks made. */
static unsigned syncAfterMapCases()
{
    unsigned checks=0;
    syncAfterMap=true;
    {
        Fixture s;
        CHECK(exposed(s)==kIOReturnSuccess && object(s,2)==kIOReturnSuccess);
        unsigned writes=model.writes,reads=model.reads;
        const unsigned asids=model.tlbiasid,syncs=model.tlbsync,alls=model.tlbiall;
        model.last_tlbiasid=0;model.tlbival.clear();
        uint64_t handle=0;
        CHECK(s.vm.map(&s.object,0,0x800000,3,0,2,&handle)==kIOReturnSuccess && handle && !s.vm.poisoned());
        CHECK(model.tlbiasid==asids+1 && model.last_tlbiasid==1 && model.tlbsync==syncs+1 && model.tlbiall==alls);
        CHECK(model.writes==writes+2 && model.reads>reads && model.tlbival.empty() && model.violations==0);
        ++checks;
        writes=model.writes;reads=model.reads;
        uint64_t refused=0;
        CHECK(s.vm.map(&s.object,0,0x0,3,0,2,&refused)==kIOReturnBadArgument && !refused);   /* below the region */
        CHECK(model.writes==writes && model.reads==reads);
        ++checks;
        volatile uint32_t fence=0;
        s.vm.setFenceSource(&fence);
        CHECK(s.vm.unmap(handle)==kIOReturnSuccess && model.tlbival.size()==2 && model.tlbiasid==asids+1 &&
              model.tlbsync==syncs+2 && model.last_tlbiasid==1);
        CHECK(!s.object.release());
        ++checks;
        release(s);
    }
    {
        Fixture s;
        CHECK(exposed(s)==kIOReturnSuccess && object(s,1)==kIOReturnSuccess);
        prog.tlb_limit=prog.tlb_reads+1;model.tlb_busy=100;
        uint64_t handle=0;
        CHECK(s.vm.map(&s.object,0,0x800000,3,0,2,&handle)==kIOReturnIOError && !handle && s.vm.poisoned() && prog.faults==1);
        CHECK(s.object.quarantined() && s.object.deviceReferences()>=1 && model.at(0xC00)==0x20300 && s.state.poisoned);
        CHECK(s.vm.map(&s.object,0,0x802000,3,0,2,&handle)==kIOReturnNotReady);
        prog.tlb_limit=0;model.tlb_busy=2;
        ++checks;
    }
    syncAfterMap=false;
    fixture=MemoryFixture{};   /* the poisoned objects above stay quarantined on purpose */
    {
        Fixture s;   /* without the flag (the design's programmer) a map touches no register */
        CHECK(exposed(s)==kIOReturnSuccess && object(s,1)==kIOReturnSuccess);
        const unsigned writes=model.writes,reads=model.reads;
        uint64_t handle=0;
        CHECK(s.vm.map(&s.object,0,0x800000,3,0,2,&handle)==kIOReturnSuccess && model.writes==writes && model.reads==reads);
        CHECK(!s.vm.unmap(handle) && !s.object.release());
        release(s);
        ++checks;
    }
    return checks;
}

/* The kit's own route state (the software peers of the older tests): proved once with ArmSmmu4KObserveStreams at attach, then
 * the same transactions; its unmap's TLBIVAL goes out as one 64-bit store. */
static uint32_t peer[0x40000/4];
static int pr32(void *,size_t at,uint32_t *v){*v=peer[at/4];return 0;}
static int pr64(void *,size_t at,uint64_t *v){std::memcpy(v,peer+at/4,8);return 0;}
static int pw32(void *,size_t at,uint32_t v){if((at&0xFFF)==0x58 && at>=0x20000){peer[at/4]&=~v;return 0;}peer[at/4]=v;return 0;}
static unsigned peer64;
static int pw64(void *,size_t at,uint64_t v){CHECK((at&0xFFF)==0x620 || (at&0xFFF)==0x20 || (at&0xFFF)==0x28);++peer64;
    if((at&0xFFF)!=0x620)std::memcpy(peer+at/4,&v,8);return 0;}
static int pbar(void *){return 0;}
static void kitStreams()
{
    std::memset(peer,0,sizeof peer);
    const ArmSmmu4KIo io{peer,sizeof peer,pr32,pr64,pw32,pw64,pbar};
    peer[0x20/4]=(1u<<30)|(1u<<27)|(4u<<9)|8;peer[0x24/4]=0x40000010;peer[0x28/4]=0x1433;peer[0]=0x436;
    for(unsigned i=0;i<8;++i)peer[(0xc00+4*i)/4]=0x20000;
    NativeAdrenoMemory tables;ArmSmmu4KLiveArena arena;ArmSmmu4KLiveState state{};ArmSmmu4KLiveVisibility visibility;
    CHECK(!tables.allocate(32*4096,40,true,32*4096));
    CHECK(!NativeAdrenoPrepareSmmuMemory(&tables,32,40,0x800000,16*4096,0,16,&arena,&state,&visibility));
    ArmSmmu4KContextConfig context{.lower=&arena.config,.lower_tables=&state.tables,.id0=peer[0x20/4],.id2=peer[0x28/4],
        .asid=1,.coherent_walk=1,.stall_on_fault=0,.continue_after_fault=1};
    ArmSmmu4KProgramResult programmed;
    CHECK(ArmSmmu4KProgramDisabledContext(&io,2,&context,1000,&programmed)==ArmSmmu4KSuccess);
    ArmSmmu4KStreamState streams{};ArmSmmu4KStreamResult result;
    const ArmSmmu4KStream kitRoutes[2]={{2,0},{4,1}};
    CHECK(ArmSmmu4KAttachStreams(&io,&streams,kitRoutes,2,2,&context,&result)==ArmSmmu4KSuccess);
    NativeAdrenoVm vm;NativeAdrenoVmRegion lower={&tables,&arena,&state};
    CHECK(vm.attach(&io,&context,&streams,&lower,nullptr,32,1000)==kIOReturnSuccess && vm.bank()==2);
    CHECK(state.generation==1 && state.context_offset==0x22000);
    NativeAdrenoMemory bo;CHECK(!bo.allocate(8192,40,false,8192));
    uint64_t handle=0;
    peer64=0;
    CHECK(!vm.map(&bo,0,0x800000,3,0,2,&handle) && !vm.unmap(handle) && peer64==2);
    CHECK(!bo.release() && !vm.detach() && !tables.retireAfterQuiescence() && !tables.release());
    CHECK(fixture.live.empty() && fixture.metadata.empty());
}

/* The VM lock (M10): two threads mapping and unmapping their own buffers concurrently. */
static unsigned concurrent()
{
    Fixture s;
    CHECK(exposed(s,256,256)==kIOReturnSuccess);
    NativeAdrenoMemory a,b;
    CHECK(!a.allocate(0x4000,36,false,0x4000) && !b.allocate(0x4000,36,false,0x4000));
    unsigned done[2]={0,0};
    auto work=[&](NativeAdrenoMemory *bo,uint64_t base,unsigned *count) {
        for(unsigned i=0;i<300;++i) {
            uint64_t handle=0;
            CHECK(s.vm.map(bo,0,base+(i%8)*0x4000,3,0,2,&handle)==kIOReturnSuccess);
            uint64_t iova=0;size_t bytes=0;unsigned permissions=0;
            CHECK(s.vm.describe(handle,&iova,&bytes,&permissions)==kIOReturnSuccess && bytes==0x4000);
            CHECK(s.vm.unmap(handle)==kIOReturnSuccess);
            ++*count;
        }
    };
    std::thread first(work,&a,0x800000,&done[0]),second(work,&b,0x880000,&done[1]);
    first.join();second.join();
    CHECK(!s.vm.poisoned() && !s.vm.objectCount() && !a.release() && !b.release());
    release(s);
    return done[0]+done[1];
}

int main()
{
    transactions();
    const unsigned rounds=faultsNeverPoison();
    refusals();
    scattered();
    aliases();
    std::printf("PASS: VM alias index/fallback preserve attribute-class refusals before MMIO/ownership with and without the table "
        "transaction's second pass; 8192-span different-class map/unmap\n");
    std::printf("PASS: VM alias pass: %u maps (preflight's alone with pagani-gpu-kopt bit 2, the table transaction's as well without it)\n",aliasPasses());
    std::printf("PASS: VM clock: %u checks (kinds count without it, time with it; a contended VM lock's wait reaches the call meter)\n",vmClock());
    const unsigned policy=coherencePolicy();
    apertures();
    wideAperture();
    coherentWalk();
    poisoning();
    const unsigned synced=syncAfterMapCases();
    std::printf("PASS: VM with the annibale programmer's sync_after_map: %u checks (one TLBIASID + TLBSYNC per map, none for a "
        "refused map, unmap unchanged, a stuck sync poisons; none without the flag)\n",synced);
    kitStreams();
    const unsigned threaded=concurrent();
    std::printf("PASS: VM over the A830 programmer: attach proves regions and routes once, %u map/unmap rounds under pending faults "
        "never poison, per-buffer fence unmap, 64-bit TLBIVAL, 4 GiB (2053), 2 GiB (1027) and 16 GiB (8209, BOs across and past 4 GiB) apertures with reserved windows, "
        "%u coherence-policy checks, coherent table walk, 3 poisoning paths, kit-stream attach, %u locked concurrent transactions\n",
        rounds,policy,threaded);
}
/* Physical-page maintenance (imported BOs): record it; no host page is touched. */
/* Host double: the page attribute change of user BOs has no host equivalent (no host page is touched). */
IOReturn NativeAdrenoSetPagesCacheMode(const NativeAdrenoPhysicalSpan *spans,size_t count,bool){return spans && count?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoReassertWriteCombined(const NativeAdrenoPhysicalSpan *spans,size_t count,uint32_t *reset)
{return spans && count && reset?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoMaintainPhysicalPages(uint64_t physical,size_t bytes,NativeAdrenoCacheOperation op)
{
    if(!physical || !bytes || ((physical|bytes)&4095) || op>NativeAdrenoInvalidate)std::abort();
    lastPhysicalOp=op;
    ++fixture.physical_calls;fixture.physical_bytes+=bytes;return kIOReturnSuccess;
}
