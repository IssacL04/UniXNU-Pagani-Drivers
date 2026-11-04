/* SPDX-License-Identifier: MIT */
/* NativeAdrenoSmmuContext with an injected programmer (ARCHITECTURE 2.2, 4.5.4): the table pool is prepared by the kit, the
 * image comes from the kit's encoder with the SMMU's own IDs, the programmer claims every stream before the bank is programmed,
 * routes them after it, verifies, faults and disables; a TTBR1 guard table is handed over as ASID << 48 | table. Host doubles
 * only; the programmer here records calls (the real one, A830Smmu, runs in test_a830smmu and test_gmuctx). */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoSmmuContext.h"
#include <cstdio>
#include <string>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
IOReturn NativeAdrenoQueryCpu4K(NativeAdrenoCpuState *state)
{CHECK(state);*state={};return kIOReturnSuccess;}
IOReturn NativeAdrenoMaintainOwnedPages(void *mapping,size_t bytes,NativeAdrenoCacheOperation op)
{CHECK(mapping && bytes && op<=NativeAdrenoInvalidate);return kIOReturnSuccess;}

struct Recorder {
    std::string log;
    ArmSmmu4KContextImage image{};
    uint64_t ttbr1=0;
    IOReturn fail_claim=kIOReturnSuccess,fail_program=kIOReturnSuccess,fail_route=kIOReturnSuccess,fail_verify=kIOReturnSuccess;
    uint64_t offset=0x20000;
};
static Recorder rec;
static IOReturn claim(void *c,unsigned bank,const ArmSmmu4KStream *s)
{CHECK(c==&rec);rec.log+="c"+std::to_string(bank)+":"+std::to_string(s->slot)+"/"+std::to_string(s->sid)+" ";return rec.fail_claim;}
static IOReturn program(void *c,unsigned bank,const ArmSmmu4KContextImage *image,uint64_t ttbr1,NativeAdrenoSmmuProgramReport *r)
{
    CHECK(c==&rec && image && r);
    rec.log+="p"+std::to_string(bank)+" ";rec.image=*image;rec.ttbr1=ttbr1;
    *r={};r->want=*image;r->want.ttbr1=ttbr1;r->got=r->want;r->context_offset=rec.offset;r->step=14;
    return rec.fail_program;
}
static IOReturn route(void *c,unsigned bank,const ArmSmmu4KStream *s)
{CHECK(c==&rec);rec.log+="r"+std::to_string(bank)+":"+std::to_string(s->slot)+" ";return rec.fail_route;}
static IOReturn verify(void *c,unsigned bank,const ArmSmmu4KStream *,size_t count)
{CHECK(c==&rec);rec.log+="v"+std::to_string(bank)+"x"+std::to_string(count)+" ";return rec.fail_verify;}
static IOReturn faultStreams(void *c,unsigned bank,const ArmSmmu4KStream *,size_t count)
{CHECK(c==&rec);rec.log+="f"+std::to_string(bank)+"x"+std::to_string(count)+" ";return kIOReturnSuccess;}
static IOReturn disable(void *c,unsigned bank)
{CHECK(c==&rec);rec.log+="d"+std::to_string(bank)+" ";return kIOReturnSuccess;}
static int r32(void *,size_t,uint32_t *v){*v=0;return 0;}
static int r64(void *,size_t,uint64_t *v){*v=0;return 0;}
static int w32(void *,size_t,uint32_t){return 0;}
static int w64(void *,size_t,uint64_t){return 0;}
static int bar(void *){return 0;}
static const ArmSmmu4KIo tableIo{&rec,0x40000,r32,r64,w32,w64,bar};
static NativeAdrenoSmmuProgrammer programmer()
{return {&rec,&tableIo,0x4C017E18,0x40000015,0x5511,claim,program,route,verify,faultStreams,disable,false};}

int main()
{
    unsigned cases=0;
    const uint64_t base=UINT64_C(0xffffffff00000000)+0x10000;
    /* Prepare: an upper and a lower region; refusals touch nothing. */
    {
        NativeAdrenoMemory tables;
        CHECK(tables.allocate(32*4096,40,true,32*4096)==kIOReturnSuccess);
        NativeAdrenoSmmuContext context;
        const NativeAdrenoSmmuProgrammer p=programmer();
        const ArmSmmu4KStream s{0,0};
        CHECK(context.program(&p,&s,1)==kIOReturnBadArgument && context.attach()==kIOReturnBadArgument);
        CHECK(context.observe()==kIOReturnBadArgument && context.disable()==kIOReturnBadArgument && !context.binding().programmer);
        CHECK(context.prepare(&tables,1,base,0x4000,8,2,3,1000)==kIOReturnSuccess);
        CHECK(context.prepared() && !context.programmed() && !context.attached());
        CHECK(context.configuration()->upper!=nullptr && context.configuration()->lower==nullptr);
        CHECK(context.configuration()->asid==3 && context.configuration()->coherent_walk==1);
        CHECK(context.configuration()->stall_on_fault==0 && context.configuration()->continue_after_fault==1);
        CHECK(context.prepare(&tables,1,base,0x4000,8,2,3,1000)==kIOReturnBusy);
        CHECK(context.setUpperGuard(0x9A000000)==kIOReturnBadArgument);   /* an upper context has no free TTBR1 */
        NativeAdrenoSmmuContext bad;
        CHECK(bad.prepare(nullptr,1,base,0x4000,8,2,1,1000)==kIOReturnBadArgument);
        CHECK(bad.prepare(&tables,1,base+1,0x4000,8,2,1,1000)==kIOReturnBadArgument);
        CHECK(bad.prepare(&tables,1,base,0,8,2,1,1000)==kIOReturnBadArgument);
        CHECK(bad.prepare(&tables,1,base,0x4000,0,2,1,1000)==kIOReturnBadArgument);
        CHECK(bad.prepare(&tables,1,base,0x4000,(size_t(1)<<20)+1,2,1,1000)==kIOReturnBadArgument);
        CHECK(bad.prepare(&tables,1,base,0x4000,8,64,1,1000)==kIOReturnBadArgument);
        CHECK(bad.prepare(&tables,1,base,0x4000,8,2,0,1000)==kIOReturnBadArgument);   /* ASID 0 */
        CHECK(bad.prepare(&tables,1,base,0x4000,8,2,0x10000,1000)==kIOReturnBadArgument);
        CHECK(bad.prepare(&tables,1,base,0x4000,8,2,1,0)==kIOReturnBadArgument);
        CHECK(bad.prepare(&tables,2,base,0x4000,8,2,1,1000)==kIOReturnBadArgument);
        CHECK(!bad.prepared());
        CHECK(tables.retireAfterQuiescence()==kIOReturnSuccess && tables.release()==kIOReturnSuccess);
        ++cases;
    }
    /* The 13T order: claim (steps 1-2), program (3-14) with the kit's image, route (15-16); LPAC added on the enabled bank;
     * observe, binding; disable faults the streams before the bank goes off. */
    {
        NativeAdrenoMemory tables,guard;
        CHECK(tables.allocate(1027*4096,36,true,1027*4096)==kIOReturnSuccess && guard.allocate(4096,36,true,4096)==kIOReturnSuccess);
        NativeAdrenoSmmuContext context;
        CHECK(context.prepare(&tables,0,0,0x80000000,size_t(1)<<19,0,1,1000,36)==kIOReturnSuccess);
        CHECK(context.setUpperGuard(guard.spans()[0].physical+1)==kIOReturnBadArgument);
        CHECK(context.setUpperGuard(UINT64_C(1)<<36)==kIOReturnBadArgument);
        CHECK(context.setUpperGuard(guard.spans()[0].physical)==kIOReturnSuccess);
        NativeAdrenoSmmuProgrammer p=programmer();
        NativeAdrenoSmmuProgrammer missing=p;missing.verify=nullptr;
        const ArmSmmu4KStream gpu{0,0},dup[2]={{0,0},{0,1}};
        CHECK(context.program(&missing,&gpu,1)==kIOReturnBadArgument && context.program(&p,dup,2)==kIOReturnBadArgument);
        CHECK(rec.log.empty());
        CHECK(context.program(&p,&gpu,1)==kIOReturnSuccess && context.programmed());
        CHECK(rec.log=="c0:0/0 p0 ");
        CHECK(rec.image.tcr==0x80803520 && rec.image.tcr2==0x38011 && rec.image.cbar==0x1F300 && rec.image.cba2r==1);
        CHECK(rec.image.ttbr0==(uint64_t(1)<<48|tables.spans()[0].physical) && rec.image.mair0==0xFF44 && rec.image.sctlr==0x1167);
        CHECK(rec.ttbr1==(uint64_t(1)<<48|guard.spans()[0].physical));
        CHECK(context.setUpperGuard(0)==kIOReturnNotReady && context.programReport()->context_offset==0x20000);
        CHECK(context.addStream({3,1})==kIOReturnBadArgument);   /* not attached yet */
        rec.log.clear();
        CHECK(context.attach()==kIOReturnSuccess && rec.log=="r0:0 " && context.attached());
        CHECK(context.attach()==kIOReturnBadArgument);
        rec.log.clear();
        CHECK(context.addStream({0,1})==kIOReturnBadArgument && context.addStream({3,0})==kIOReturnBadArgument);
        CHECK(context.addStream({3,1})==kIOReturnSuccess && rec.log=="c0:3/1 r0:3 " && context.streamCount()==2);
        rec.log.clear();
        CHECK(context.observe()==kIOReturnSuccess && rec.log=="v0x2 ");
        const NativeAdrenoSmmuBinding b=context.binding();
        CHECK(b.programmer && b.programmer->cookie==&rec && b.context==context.configHandle() && b.bank==0);
        CHECK(b.context_offset==0x20000 && b.stream_count==2 && b.streams[1].slot==3 && b.streams[1].sid==1);
        CHECK(context.configuration()->id2==0x5511 && context.configuration()->id0==0x4C017E18);
        rec.log.clear();
        CHECK(context.disable()==kIOReturnSuccess && rec.log=="f0x2 d0 " && !context.attached());
        CHECK(context.disable()==kIOReturnBadArgument && context.observe()==kIOReturnBadArgument && !context.binding().programmer);
        rec.log.clear();
        ++cases;
    }
    /* The GMU bank: TTBR1 0 without a guard. */
    {
        NativeAdrenoMemory tables;
        CHECK(tables.allocate(1027*4096,36,true,1027*4096)==kIOReturnSuccess);
        NativeAdrenoSmmuContext context;
        CHECK(context.prepare(&tables,0,0,0x80000000,size_t(1)<<19,2,3,1000,36)==kIOReturnSuccess);
        NativeAdrenoSmmuProgrammer p=programmer();
        const ArmSmmu4KStream gmu{1,5};
        CHECK(context.program(&p,&gmu,1)==kIOReturnSuccess && rec.log=="c2:1/5 p2 " && !rec.ttbr1);
        CHECK(rec.image.ttbr0==(uint64_t(3)<<48|tables.spans()[0].physical));
        rec.log.clear();
        ++cases;
    }
    /* Failures poison: a refused claim never programs; a refused program never routes; a refused route leaves the context
     * unusable; an encoder that cannot use the IDs (no 4K granule) refuses before any call; a report without a register offset
     * is an error. */
    {
        for(unsigned which=0;which<5;++which) {
            NativeAdrenoMemory tables;
            CHECK(tables.allocate(32*4096,40,true,32*4096)==kIOReturnSuccess);
            NativeAdrenoSmmuContext context;
            CHECK(context.prepare(&tables,0,0x60000000,0x8000,8,1,2,1000,36)==kIOReturnSuccess);
            NativeAdrenoSmmuProgrammer p=programmer();
            rec=Recorder{};
            if(which==0)rec.fail_claim=kIOReturnIOError;
            if(which==1)rec.fail_program=kIOReturnIOError;
            if(which==2)rec.fail_route=kIOReturnIOError;
            if(which==3)p.id2&=~(1u<<12);
            if(which==4)rec.offset=0;
            const ArmSmmu4KStream s{2,7};
            const IOReturn programmed=context.program(&p,&s,1);
            if(which==0)CHECK(programmed==kIOReturnIOError && rec.log=="c1:2/7 " && context.poisoned());
            if(which==1)CHECK(programmed==kIOReturnIOError && rec.log=="c1:2/7 p1 " && context.poisoned());
            if(which==3)CHECK(programmed==kIOReturnUnsupported && rec.log.empty() && !context.poisoned());
            if(which==4)CHECK(programmed==kIOReturnIOError && context.poisoned());
            if(which==2) {
                CHECK(programmed==kIOReturnSuccess && context.attach()==kIOReturnIOError && context.poisoned());
                CHECK(context.observe()==kIOReturnBadArgument && context.addStream({4,8})==kIOReturnBadArgument);
            } else CHECK(context.attach()==kIOReturnBadArgument);
            CHECK(tables.retireAfterQuiescence()==kIOReturnSuccess && tables.release()==kIOReturnSuccess);
        }
        rec=Recorder{};
        ++cases;
    }
    CHECK(fixture.live.size()==3);   /* the pools of the two programmed contexts (never torn down) and the guard page */
    std::printf("PASS: %u SMMU context cases with an injected programmer: kit image from the SMMU's IDs, claim before program, route "
        "after, LPAC on the enabled bank, TTBR1 guard as ASID|table, fault before disable, 5 poisoning failures\n",cases);
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
