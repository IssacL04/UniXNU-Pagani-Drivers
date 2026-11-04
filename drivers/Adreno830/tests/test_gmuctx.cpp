/* SPDX-License-Identifier: MIT */
/* The GMU context (ARCHITECTURE 4.5.4 first half, 4.7.2) with the injected A830Smmu programmer over the model of the
 * hypervisor-emulated SM8750 GPU SMMU (tests/A830SmmuModel.h): CB2, ASID 3, slot 1, SID 5, the table pool for 0..2 GiB, the
 * VM, the GMU memory mapped through the map hook. A830Smmu.cpp is compiled into this test (its table entry lists the inherited
 * sources only). */
#include "XnuMemoryFixture.h"
#include "A830SmmuModel.h"
#include "NativeAdrenoGmuContext.h"
#include "../A830Smmu.cpp"
#include <cstdio>
#include <cstring>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
IOReturn NativeAdrenoQueryCpu4K(NativeAdrenoCpuState *state)
{CHECK(state);*state={};return kIOReturnSuccess;}
IOReturn NativeAdrenoMaintainOwnedPages(void *mapping,size_t bytes,NativeAdrenoCacheOperation op)
{CHECK(mapping && bytes && op<=NativeAdrenoInvalidate);return kIOReturnSuccess;}

static A830SmmuModel model;
static unsigned steps[16],step_count;
static void progress(uint32_t step){CHECK(step_count<16);steps[step_count++]=step;}
/* The GMU memory's owner maps through the hook: a CACHE range (privileged code at 0x4000) and the HFI area. */
struct GmuMemory {NativeAdrenoMemory cache,hfi;uint64_t handles[2];IOReturn fail;};
static IOReturn mapGmu(void *cookie,NativeAdrenoVm *vm)
{
    auto *m=static_cast<GmuMemory *>(cookie);
    if(m->fail)return m->fail;
    IOReturn status=m->cache.allocate(0x16000,36,true,0x16000);
    if(!status)status=m->hfi.allocate(0x4000,36,true,0x4000);
    if(!status)status=vm->map(&m->cache,0,0x4000,ArmSmmu4KRead|ArmSmmu4KWrite|ArmSmmu4KExecute|ArmSmmu4KPrivileged,0,2,&m->handles[0]);
    if(!status)status=vm->map(&m->hfi,0,0x60008000,ArmSmmu4KRead|ArmSmmu4KWrite|ArmSmmu4KPrivileged,0,2,&m->handles[1]);
    return status;
}
static A830Smmu *surveyed(A830Smmu &smmu,A830SmmuHostPlatform &host,Adreno830Status *status)
{
    CHECK(smmu.init(A830SmmuBoardConfig(model,host,status))==kIOReturnSuccess && smmu.survey(nullptr)==kIOReturnSuccess);
    CHECK(smmu.resetBank(2)==kIOReturnSuccess && smmu.resetBank(0)==kIOReturnSuccess);
    return &smmu;
}
int main()
{
    /* The expectations below are the design profile's context values; test_a830smmu covers both profiles. */
    adreno830_profile_value = A830ProfileDesign;
    unsigned cases=0;
    const uint32_t cb2=A830SmmuModel::cb(2);
    /* The whole sequence over the boot state of this SoC. */
    {
        model.boot(101,0x2,false);step_count=0;
        Adreno830Status status{};A830SmmuHostPlatform host;A830Smmu smmu;surveyed(smmu,host,&status);
        GmuMemory memory{};
        NativeAdrenoGmuContext context;
        NativeAdrenoGmuContextReport report;
        const NativeAdrenoGmuContextConfig config{smmu.programmer(),2,3,smmu.gmuStream().slot,5,0,mapGmu,&memory};
        CHECK(context.bringUp(config,progress,&report)==kIOReturnSuccess && context.ready());
        CHECK(report.step==NativeAdrenoGmuContext::Ready && report.status==kIOReturnSuccess && step_count==7);
        for(unsigned i=0;i<7;++i)CHECK(steps[i]==i+1);
        CHECK(report.output_bits==36 && report.table_pages==1027 && report.streams==1 && report.spans==2);
        CHECK(report.program.mismatch==0 && report.program.got.sctlr==0x167 && report.program.got.tcr2==0x38071);
        CHECK(report.program.want.tcr==0x80803520 && report.program.want.ttbr1==0 && report.program.context_offset==cb2);
        CHECK(model.at(0x800+4)==0x80000005 && model.at(0xC00+4)==0x302 && model.at(cb2)==0x167 && model.at(cb2+4)==0x303);
        CHECK(model.at64(cb2+0x20)==(uint64_t(3)<<48|context.tables().spans()[0].physical) && model.at64(cb2+0x28)==0);
        CHECK(model.at(cb2+0x30)==0x80803520 && model.at(0x1000+8)==0x1F300 && model.at(0x1800+8)==1);
        CHECK(context.tables().spans()[0].physical+0x1000*NativeAdrenoGmuContext::TablePages<=(UINT64_C(1)<<36));
        CHECK(context.vm() && context.vm()->objectCount()==2 && context.vm()->bank()==2 && !context.vm()->poisoned());
        CHECK(model.aborts==0 && model.violations==0);
        CHECK(context.bringUp(config,nullptr,&report)==kIOReturnBusy);   /* one attempt per object */
        ++cases;
    }
    /* The preferred GMU slot holds another stream: the context takes the surveyed slot. */
    {
        model.boot(102);model.at(0x800+4)=0x80000000|0x123;
        Adreno830Status status{};A830SmmuHostPlatform host;A830Smmu smmu;surveyed(smmu,host,&status);
        CHECK(smmu.gmuStream().slot==4);
        NativeAdrenoGmuContext context;NativeAdrenoGmuContextReport report;
        const NativeAdrenoGmuContextConfig config{smmu.programmer(),2,3,smmu.gmuStream().slot,5,0,nullptr,nullptr};
        CHECK(context.bringUp(config,nullptr,&report)==kIOReturnSuccess && report.spans==0);
        CHECK(model.at(0x800+16)==0x80000005 && model.at(0xC00+16)==0x302 && model.at(0x800+4)==0x80000123);
        ++cases;
    }
    /* A context write the hypervisor drops (TCR) stops the Program step: step 0x207 named, the bank left off, the stream
     * claimed (S2CR FAULT, SMR valid) but never routed. */
    {
        model.boot(103);
        struct Dropper {static int write32(void *c,size_t at,uint32_t v)
            {if(at==A830SmmuModel::cb(2)+0x30)return 0;return A830SmmuModel::write32(c,at,v);}};
        ArmSmmu4KIo dropping=model.io;dropping.write32=Dropper::write32;
        Adreno830Status status{};A830SmmuHostPlatform host;
        A830SmmuConfig smmuConfig=A830SmmuBoardConfig(model,host,&status);smmuConfig.io=&dropping;
        A830Smmu smmu;
        CHECK(smmu.init(smmuConfig)==kIOReturnSuccess && smmu.survey(nullptr)==kIOReturnSuccess && smmu.resetBank(2)==0);
        NativeAdrenoGmuContext context;NativeAdrenoGmuContextReport report;
        const NativeAdrenoGmuContextConfig config{smmu.programmer(),2,3,1,5,0,nullptr,nullptr};
        CHECK(context.bringUp(config,nullptr,&report)==kIOReturnIOError && !context.ready());
        CHECK(report.step==NativeAdrenoGmuContext::Program && report.program.step==0x207);
        CHECK(smmu.failure().reason==A830ReasonReadback && smmu.failure().expect==0x80803520);
        CHECK(!(model.at(cb2)&1) && model.at(0xC00+4)==0x20300 && model.at(0x800+4)==0x80000005 && !context.vm());
        ++cases;
    }
    /* The map hook's failure is the Map step's; the VM stays attached for the caller. */
    {
        model.boot(104);
        Adreno830Status status{};A830SmmuHostPlatform host;A830Smmu smmu;surveyed(smmu,host,&status);
        GmuMemory memory{};memory.fail=kIOReturnNoMemory;
        NativeAdrenoGmuContext context;NativeAdrenoGmuContextReport report;
        const NativeAdrenoGmuContextConfig config{smmu.programmer(),2,3,1,5,0,mapGmu,&memory};
        CHECK(context.bringUp(config,nullptr,&report)==kIOReturnNoMemory && report.step==NativeAdrenoGmuContext::Map);
        CHECK(!context.ready() && context.vm() && model.at(0xC00+4)==0x302);
        ++cases;
    }
    /* An OAS the kit cannot encode, an explicit 32-bit ceiling the pool cannot meet in this fixture, argument refusals. */
    {
        model.boot(105);
        Adreno830Status status{};A830SmmuHostPlatform host;A830Smmu smmu;surveyed(smmu,host,&status);
        NativeAdrenoSmmuProgrammer odd=*smmu.programmer();odd.id2=0x14F3;
        NativeAdrenoGmuContext context;NativeAdrenoGmuContextReport report;
        NativeAdrenoGmuContextConfig config{&odd,2,3,1,5,0,nullptr,nullptr};
        CHECK(context.bringUp(config,nullptr,&report)==kIOReturnUnsupported && report.step==NativeAdrenoGmuContext::Allocate);
        CHECK(!model.writes || !(model.at(cb2)&1));
        NativeAdrenoGmuContext other;
        config.programmer=nullptr;
        CHECK(other.bringUp(config,nullptr,&report)==kIOReturnBadArgument);
        config.programmer=smmu.programmer();config.bank=64;
        CHECK(other.bringUp(config,nullptr,&report)==kIOReturnBadArgument);
        config.bank=2;config.asid=0;
        CHECK(other.bringUp(config,nullptr,&report)==kIOReturnBadArgument);
        config.asid=3;config.slot=128;
        CHECK(other.bringUp(config,nullptr,&report)==kIOReturnBadArgument);
        config.slot=1;
        CHECK(other.bringUp(config,nullptr,nullptr)==kIOReturnBadArgument && report.step==0);
        CHECK(other.bringUp(config,nullptr,&report)==kIOReturnSuccess);   /* none of the refusals used the object */
        A830Smmu unsurveyed;
        CHECK(unsurveyed.init(A830SmmuBoardConfig(model,host,&status))==kIOReturnSuccess && !unsurveyed.programmer());
        ++cases;
    }
    std::printf("PASS: %u GMU context cases with the A830Smmu programmer (CB2, slot 1, SID 5, ASID 3) over the hypervisor-emulated "
        "SMMU model: complete bring-up with the GMU memory mapped, moved slot, dropped TCR write, map-hook failure, refusals\n",cases);
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
