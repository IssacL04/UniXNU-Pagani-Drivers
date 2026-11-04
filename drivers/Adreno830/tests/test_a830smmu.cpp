/* SPDX-License-Identifier: MIT */
/* A830Smmu over the model of the hypervisor-emulated SM8750 GPU SMMU (tests/A830SmmuModel.h), and the whole S3 take-over with
 * the real GMU and GPU contexts, the VM and the memory code over the IOKit doubles (ARCHITECTURE 4.5, 4.6, 4.12). Every case runs
 * under both profiles of A830Profile.h: the design (ARCHITECTURE 4.5.3-4.5.7) and annibale (kernel-sequences 7.4-7.6, section 14
 * rows 9, 11, 12, 13: SCTLR 0x27, TCR 0x803519, MAIR0 0xF404FF44, TTBR1 0, ACTLR on CB0 only, TLBIASID + TLBSYNC only, no sACR
 * or sGFSR store, 710/712 not attached).
 * host-test: A830Smmu.cpp NativeAdrenoSmmuContext.cpp NativeAdrenoGmuContext.cpp NativeAdrenoGpuContext.cpp NativeAdrenoVm.cpp NativeAdrenoMemory.cpp NativeAdrenoSmmuMemory.cpp */
#include "XnuMemoryFixture.h"
#include "A830SmmuModel.h"
#include "NativeAdrenoGpuContext.h"
#include "A830Regs.h"
#include <algorithm>
#include <cstdio>
static bool annibale() {return adreno830_annibale()!=0;}
static const char *profileName() {return annibale()?"annibale":"design";}
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d (%s): %s\n",__LINE__,profileName(),#x);std::exit(1);}}while(0)
IOReturn NativeAdrenoQueryCpu4K(NativeAdrenoCpuState *state) {CHECK(state);*state={};return kIOReturnSuccess;}
IOReturn NativeAdrenoMaintainOwnedPages(void *mapping,size_t bytes,NativeAdrenoCacheOperation op)
{CHECK(mapping && bytes && op<=NativeAdrenoInvalidate);++fixture.cache_calls;return kIOReturnSuccess;}
static unsigned wcPages;
IOReturn NativeAdrenoSetPagesCacheMode(const NativeAdrenoPhysicalSpan *spans,size_t count,bool wc)
{
    if(!spans || !count)return kIOReturnBadArgument;
    for(size_t i=0;i<count;++i)wcPages+=wc?unsigned(spans[i].bytes/4096):0;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoReassertWriteCombined(const NativeAdrenoPhysicalSpan *spans,size_t count,uint32_t *reset)
{return spans && count && reset?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoMaintainPhysicalPages(uint64_t physical,size_t bytes,NativeAdrenoCacheOperation op)
{
    if(!physical || !bytes || ((physical|bytes)&4095) || op>NativeAdrenoInvalidate)std::abort();
    ++fixture.physical_calls;fixture.physical_bytes+=bytes;return kIOReturnSuccess;
}

static A830SmmuModel model;
static unsigned cases;

/* The context values each profile writes and the SMMU reads back (ASIDPNE is not kept). */
static uint32_t sctlrRead() {return annibale()?0x27u:0x167u;}
static uint32_t tcrOf(unsigned cb) {return annibale()?0x00803519u:cb==0?0xB5A03520u:0x80803520u;}   /* CB0 with its guard */
/* CB0's TCR for an aperture: the design's T0SZ (and the guard's T1SZ) follow the input size, 34 bits for 16 GiB (T0SZ 30). */
static uint32_t tcrOf0(uint32_t aperture_gib)
{
    return annibale() || aperture_gib!=16?tcrOf(0):(0xB5A03520u&~0x003F003Fu)|0x001E001Eu;
}
static uint32_t mair0() {return annibale()?0xF404FF44u:0xFF44u;}

/* Every store the model receives through the recording I/O, in order. */
struct Store {
    uint32_t offset;uint64_t value;unsigned bits;
    bool operator==(const Store &o) const {return offset==o.offset && value==o.value && bits==o.bits;}
};
static std::vector<Store> stores;
struct Recorder {
    static int write32(void *c,size_t at,uint32_t v) {stores.push_back({uint32_t(at),v,32});return A830SmmuModel::write32(c,at,v);}
    static int write64(void *c,size_t at,uint64_t v) {stores.push_back({uint32_t(at),v,64});return A830SmmuModel::write64(c,at,v);}
};
static ArmSmmu4KIo recording;
/* The board configuration over the model just booted, its stores recorded. */
static A830SmmuConfig recordedConfig(A830SmmuHostPlatform &host,Adreno830Status *status,bool irqs=true)
{
    recording=model.io;recording.write32=Recorder::write32;recording.write64=Recorder::write64;
    stores.clear();
    A830SmmuConfig config=A830SmmuBoardConfig(model,host,status,irqs);
    config.io=&recording;
    return config;
}
static bool stored(uint32_t offset)
{
    return std::any_of(stores.begin(),stores.end(),[&](const Store &s) {return s.offset==offset;});
}
/* The annibale TLB rule on the stores from index from on: no TLBIVA/TLBIVAA/TLBIALL/TLBIVAL/TLBIVAAL, no 64-bit store; every
 * TLBIASID (32-bit, the bank's ASID: CB + 1) directly followed by the bank's TLBSYNC = 1, every TLBSYNC directly after one.
 * Returns the number of TLBSYNCs. */
static unsigned annibaleTlb(size_t from)
{
    unsigned syncs=0;
    for(size_t i=from;i<stores.size();++i) {
        const Store &s=stores[i];
        CHECK(s.bits==32);
        if(s.offset<A830SmmuModel::cb(0) || s.offset>=A830SmmuModel::cb(A830SmmuModel::Banks))continue;
        const uint32_t reg=s.offset&0xFFF,bank=(s.offset-A830SmmuModel::cb(0))/0x1000;
        CHECK(!(reg>=0x600 && reg<0x610) && !(reg>=0x618 && reg<0x630));
        if(reg==0x610)CHECK(s.value==bank+1 && i+1<stores.size() && stores[i+1]==(Store{s.offset-0x610+0x7F0,1,32}));
        if(reg==0x7F0) {CHECK(s.value==1 && i>from && stores[i-1]==(Store{s.offset-0x7F0+0x610,bank+1,32}));++syncs;}
    }
    return syncs;
}

/* The descriptor of va through a lower-region arena (32-bit input: L1 at the root page, L2, L3). */
static uint64_t leafOf(const ArmSmmu4KLiveArena *arena,uint64_t va)
{
    size_t page=0;
    for(unsigned level=1;level<3;++level) {
        /* Level 1 indexes the input size's top bits: 2 for 32 bits, 4 for the 16 GiB aperture's 34. */
        const unsigned shift=39-9*level,index=unsigned((va>>shift)&(level==1?(1u<<(arena->config.input_bits-30))-1:511));
        const uint64_t entry=arena->mapping[page*512+index];
        CHECK((entry&3)==3);
        page=size_t(((entry&0x0000fffffffff000ull)-arena->config.table_physical)/4096);
    }
    return arena->mapping[page*512+((va>>12)&511)];
}
static void fault(void *cookie,const A830SmmuFault &f)
{
    auto *seen=static_cast<std::vector<A830SmmuFault> *>(cookie);
    seen->push_back(f);
}

/* Survey: the IDs of the SoC, the preferred slots, the boot values recorded; refusals for CLIENTPD, a SID already routed, a
 * route translating to CB0/CB2, IDs without 4K pages; a moved slot. Nothing is written. */
static void surveyCases()
{
    Adreno830Status status{};
    A830SmmuHostPlatform host;
    {
        model.boot(7,0x2,true);
        A830Smmu smmu;
        CHECK(smmu.survey(nullptr)==kIOReturnNotReady);
        CHECK(smmu.init(A830SmmuBoardConfig(model,host,&status))==kIOReturnSuccess);
        CHECK(smmu.init(A830SmmuBoardConfig(model,host,&status))==kIOReturnBusy);
        A830SmmuSurvey s{};
        CHECK(smmu.survey(&s)==kIOReturnSuccess && !model.writes);
        CHECK(s.idr0==0x4C017E18 && s.idr1==0x40000015 && s.idr2==0x5511 && s.scr0==0x002E0406);
        CHECK(s.smrs==24 && s.banks==21 && s.global_pages==32 && s.output_bits==36);
        CHECK(s.slot_gpu==0 && s.slot_lpac==3 && s.slot_gmu==1);
        CHECK(s.s2cr[5]==0x03000018 && !s.smr[5] && s.sgfsr==2 && (s.sacr&(1u<<26)));
        CHECK((s.warnings&A830SmmuWarnCacheLock) && (s.warnings&A830SmmuWarnGlobalFault) && (s.warnings&A830SmmuWarnBankEnabled));
        CHECK(!(s.warnings&A830SmmuWarnUsfcfg) && !(s.warnings&A830SmmuWarnSlotMoved));
        CHECK((s.cb_sctlr[0]&0x41)==0x41 && (s.cb_fsr[2]&0xA));   /* random boot values, recorded */
        CHECK(status.sticky[0]==2);
        smmu.fillStatus(&status);
        CHECK(status.aux[0]==(0x4C017E18ull|uint64_t(0x40000015)<<32) && status.aux[1]==(0x5511ull|uint64_t(0x002E0406)<<32));
        CHECK(status.smmu_slots==(0|3u<<8|1u<<16) && ((status.aux[3]>>32)&0xFFFFFF)==status.smmu_slots &&
              uint32_t(status.aux[3])==0x03000018);
        /* The aperture outcome rides in bits 56-58 (M14 P0): recorded, not run yet. */
        CHECK((status.aux[3]>>A830StatusApertureShift)==4 && status.aux[3]==(0x03000018ull|uint64_t(status.smmu_slots)<<32|A830StatusAperture(0)));
        ++cases;
    }
    {
        model.boot(8);model.at(0x00)|=1;   /* CLIENTPD: every stream would bypass */
        A830Smmu smmu;CHECK(smmu.init(A830SmmuBoardConfig(model,host,&status))==kIOReturnSuccess);
        CHECK(smmu.survey(nullptr)==kIOReturnUnsupported && !model.writes);
        CHECK(smmu.failure().step==A830SmmuStepScr0 && smmu.failure().reason==A830ReasonUnexpectedState);
        CHECK(smmu.failure().reg==0x3DA0000 && smmu.failure().value==0x002E0407);
        CHECK(!smmu.programmer());
        ++cases;
    }
    {
        model.boot(9);model.at(0x800+4*7)=0x80000000|0x10005;   /* slot 7: SID 5 with mask 1 covers SID 5 */
        A830Smmu smmu;CHECK(smmu.init(A830SmmuBoardConfig(model,host,&status))==kIOReturnSuccess);
        CHECK(smmu.survey(nullptr)==kIOReturnBusy && smmu.failure().step==A830SmmuStepSlots);
        CHECK(smmu.failure().reason==A830ReasonSmmuRoute && smmu.failure().reg==0x3DA0800+4*7 && smmu.failure().expect==5);
        ++cases;
    }
    {
        model.boot(10);model.at(0x800+4*9)=0x80000000|0x40;model.at(0xC00+4*9)=0x00000302;   /* someone translates to CB2 */
        A830Smmu smmu;CHECK(smmu.init(A830SmmuBoardConfig(model,host,&status))==kIOReturnSuccess);
        CHECK(smmu.survey(nullptr)==kIOReturnBusy && smmu.failure().reason==A830ReasonSmmuRoute);
        CHECK(smmu.failure().reg==0x3DA0C00+4*9 && smmu.failure().value==0x302);
        ++cases;
    }
    {
        model.boot(11);model.at(0x24)|=1u<<31;   /* 64 KiB pages */
        A830Smmu smmu;CHECK(smmu.init(A830SmmuBoardConfig(model,host,&status))==kIOReturnSuccess);
        CHECK(smmu.survey(nullptr)==kIOReturnUnsupported && smmu.failure().step==A830SmmuStepIds);
        model.boot(11);model.at(0x28)&=~(1u<<12);   /* no 4K granule */
        A830Smmu other;CHECK(other.init(A830SmmuBoardConfig(model,host,&status))==kIOReturnSuccess);
        CHECK(other.survey(nullptr)==kIOReturnUnsupported && other.failure().reg==0x3DA0028);
        ++cases;
    }
    {
        /* Preferred GPU slot 0 and LPAC slot 3 hold unrelated streams: the lowest invalid slots, never 2. */
        model.boot(12);model.at(0x800)=0x80000000|0x200;model.at(0x800+12)=0x80000000|0x201;model.at(0xC00)=0x10000;
        A830Smmu smmu;CHECK(smmu.init(A830SmmuBoardConfig(model,host,&status))==kIOReturnSuccess);
        A830SmmuSurvey s{};CHECK(smmu.survey(&s)==kIOReturnSuccess);
        CHECK(s.slot_gpu==4 && s.slot_lpac==5 && s.slot_gmu==1 && (s.warnings&A830SmmuWarnSlotMoved));
        CHECK(smmu.gpuStream().slot==4 && smmu.lpacStream().slot==5 && smmu.lpacStream().sid==1 && smmu.gmuStream().cb==2);
        ++cases;
    }
}

/* Global steps 3 and 4. Design: sACR's CACHE_LOCK cleared, the read-back recorded; an sGFSR write-back the hypervisor drops is
 * recorded and never fails. Annibale: neither register is ever stored, even where the hypervisor would take the store; a changed
 * sGFSR is recorded once. Bank reset: SCTLR 0, FSR cleared. Design: the 710/712 handlers only after it, a bank not yet enabled
 * has its random FSR cleared without a fault being counted. Annibale: 710/712 never attached (both in irq_off), the same FSR
 * found and cleared by poll(). */
static void globalAndResetCases()
{
    Adreno830Status status{};
    A830SmmuHostPlatform host;
    model.boot(21,0x2,true);
    A830Smmu smmu;
    CHECK(smmu.init(A830SmmuBoardConfig(model,host,&status))==kIOReturnSuccess && smmu.survey(nullptr)==kIOReturnSuccess);
    CHECK(smmu.annibale()==annibale());
    if(!annibale()) {
        CHECK(smmu.releaseCacheLock()==kIOReturnSuccess && !(model.at(0x10)&(1u<<26)));
        CHECK(smmu.clearGlobalFault()==kIOReturnSuccess && model.at(0x48)==2 && status.sticky[0]==2);   /* write-back ignored */
        model.sgfsr_write_ignored=false;
        CHECK(smmu.clearGlobalFault()==kIOReturnSuccess && model.at(0x48)==0);
    } else {
        model.sgfsr_write_ignored=false;
        CHECK(smmu.releaseCacheLock()==kIOReturnSuccess && (model.at(0x10)&(1u<<26)) && !model.writes);
        CHECK(smmu.clearGlobalFault()==kIOReturnSuccess && model.at(0x48)==2 && status.sticky[0]==2 && !model.writes);
        /* The poll records a changed sGFSR once and stores nothing; the same value again is not a new global fault. */
        status.sticky[0]=0;smmu.poll();
        CHECK(!status.sticky[0] && !model.writes);
        model.at(0x48)=6;smmu.poll();
        CHECK(status.sticky[0]==6 && model.at(0x48)==6 && !model.writes);
        status.sticky[0]=0;smmu.poll();
        CHECK(!status.sticky[0] && !model.writes);
    }
    /* The interrupts need the resets first. */
    CHECK(smmu.attachIrqs()==0 && host.lines.empty() && (status.irq_off&(A830SmmuIrqOffGpu|A830SmmuIrqOffGmu)));
    CHECK(smmu.resetBank(2)==kIOReturnSuccess && smmu.resetBank(0)==kIOReturnSuccess);
    CHECK(model.at(A830SmmuModel::cb(0))==0 && model.at(A830SmmuModel::cb(2))==0);
    CHECK(!(model.at(A830SmmuModel::cb(0)+0x58)&0xC00001FE) && !(model.at(A830SmmuModel::cb(2)+0x58)&0xC00001FE));
    if(!annibale()) {
        CHECK(smmu.attachIrqs()==2 && host.lines.size()==2 && host.lines[0].intid==710 && host.lines[1].intid==712);
        CHECK(!(status.irq_off&(A830SmmuIrqOffGpu|A830SmmuIrqOffGmu)));
        CHECK(smmu.attachIrqs()==2 && host.lines.size()==2);   /* never attached twice */
        model.fault(0,0x2,0x1234000,0);
        CHECK(host.fire(710)==1 && !(model.at(A830SmmuModel::cb(0)+0x58)&0xC00001FE));
    } else {
        CHECK(smmu.attachIrqs()==0 && host.lines.empty());
        CHECK((status.irq_off&(A830SmmuIrqOffGpu|A830SmmuIrqOffGmu))==(A830SmmuIrqOffGpu|A830SmmuIrqOffGmu));
        CHECK(!smmu.bankState(0)->irq_attached && smmu.bankState(0)->irq_off && smmu.bankState(2)->irq_off);
        model.fault(0,0x2,0x1234000,0);
        smmu.poll();   /* CB0 is reset, not enabled: its FSR is cleared, nothing counted */
        CHECK(!(model.at(A830SmmuModel::cb(0)+0x58)&0xC00001FE));
    }
    CHECK(!status.smmu_faults && !host.notifies && smmu.drainFaults(nullptr,nullptr)==0);
    /* A reset bank whose SCTLR does not read 0 stops S3. */
    model.boot(22);
    A830Smmu stuck;
    CHECK(stuck.init(A830SmmuBoardConfig(model,host,&status))==kIOReturnSuccess && stuck.survey(nullptr)==kIOReturnSuccess);
    CHECK(stuck.resetBank(30)==kIOReturnBadArgument);
    ++cases;
}

/* The 13T take-over over the model with the real contexts: CB2 for the GMU, CB0 for the GPU (design: with its TTBR1 guard) and
 * 4 GiB aperture, LPAC, aperture; then the VM through the programmer: PTEs, the TLB stores of its transactions (design: page
 * TLBIVAL as single 64-bit stores; annibale: TLBIASID + TLBSYNC only), a client fault (design: 710; annibale: poll) that never
 * poisons the VM, the null guard and GMEM windows; the global stores of each profile. */
static void takeOverCases(uint32_t aperture_gib)
{
    Adreno830Status status{};
    A830SmmuHostPlatform host;
    /* Design: CACHE_LOCK set at boot, releaseCacheLock clears it. Annibale: sACR is never written; a boot without CACHE_LOCK, as
     * K90 wrote ACTLR 0x303 (with CACHE_LOCK kept the write is only recorded: readbackCases). */
    model.boot(31+aperture_gib,0x2,!annibale());
    A830Smmu smmu;
    host.fenced=true;host.completed=7;host.issued=9;   /* the ring's fences the 710 top half reads first (M14 P0) */
    CHECK(smmu.init(recordedConfig(host,&status))==kIOReturnSuccess);
    CHECK(smmu.survey(nullptr)==kIOReturnSuccess && smmu.releaseCacheLock()==kIOReturnSuccess);
    CHECK(smmu.clearGlobalFault()==kIOReturnSuccess);
    CHECK(smmu.resetBank(2)==kIOReturnSuccess && smmu.resetBank(0)==kIOReturnSuccess && smmu.attachIrqs()==(annibale()?0u:2u));
    const NativeAdrenoSmmuProgrammer *programmer=smmu.programmer();
    CHECK(programmer && programmer->id2==0x5511 && programmer->io==smmu.tableIo());
    const uint32_t cb2=A830SmmuModel::cb(2),cb0=A830SmmuModel::cb(0);
    const uint32_t cb2Actlr=model.at(cb2+4);   /* the boot value: the annibale profile leaves it */
    /* CB2: slot 1, SID 5, ASID 3. */
    NativeAdrenoGmuContext gmu;
    NativeAdrenoGmuContextReport gmuReport;
    NativeAdrenoGmuContextConfig gmuConfig{programmer,2,3,smmu.gmuStream().slot,5,0,nullptr,nullptr};
    CHECK(gmu.bringUp(gmuConfig,nullptr,&gmuReport)==kIOReturnSuccess && gmu.ready());
    CHECK(gmuReport.step==NativeAdrenoGmuContext::Ready && gmuReport.output_bits==36 && gmuReport.streams==1);
    CHECK(model.at(0x800+4)==0x80000005 && model.at(0xC00+4)==0x302);
    CHECK(model.at(0x1000+8)==0x1F300 && model.at(0x1800+8)==1);
    CHECK(model.at(cb2+0x30)==tcrOf(2) && model.at(cb2+0x10)==0x38071 && model.at(cb2)==sctlrRead());
    CHECK(model.at64(cb2+0x20)==(uint64_t(3)<<48|gmu.tables().spans()[0].physical) && model.at64(cb2+0x28)==0);
    CHECK(model.at(cb2+0x38)==mair0() && model.at(cb2+0x3C)==0 && model.at(cb2+4)==(annibale()?cb2Actlr:0x303u));
    CHECK(gmuReport.program.mismatch==0 && gmuReport.program.got.tcr2==0x38071 && gmuReport.program.got.sctlr==sctlrRead());
    CHECK(gmuReport.program.actlr_read==(annibale()?cb2Actlr:0x303u) && gmuReport.program.context_offset==cb2);
    CHECK(gmuReport.program.actlr==(annibale()?0u:0x303u) && smmu.bankState(2)->actlr_written==!annibale());
    CHECK(stored(cb2+4)==!annibale());   /* annibale: the GMU bank's ACTLR is never stored */
    /* The GMU memory maps through its VM (privileged code at 0x4000, the HFI area at 0x60008000). */
    NativeAdrenoVm *gmuVm=gmu.vm();
    CHECK(gmuVm && gmuVm->bank()==2);
    NativeAdrenoMemory code,hfi;
    CHECK(!code.allocate(0x16000,36,false,0x16000) && !hfi.allocate(0x4000,36,true,0x4000));
    uint64_t codeHandle=0,hfiHandle=0;
    CHECK(gmuVm->map(&code,0,0x4000,ArmSmmu4KRead|ArmSmmu4KWrite|ArmSmmu4KExecute|ArmSmmu4KPrivileged,0,2,&codeHandle)==0);
    CHECK(gmuVm->map(&hfi,0,0x60008000,ArmSmmu4KRead|ArmSmmu4KWrite|ArmSmmu4KPrivileged,0,2,&hfiHandle)==0);
    CHECK((leafOf(gmu.smmu().arena(),0x4000)&0xFFF)==0xE03 && !(leafOf(gmu.smmu().arena(),0x4000)>>53));
    /* CB0: slot 0, SID 0, ASID 1, LPAC slot 3. */
    NativeAdrenoGpuContext gpu;
    NativeAdrenoGmuContextReport gpuReport;
    NativeAdrenoGpuContextConfig gpuConfig{programmer,0,1,smmu.gpuStream().slot,smmu.lpacStream().slot,0,1,0,aperture_gib,0,true};
    const unsigned wcBefore=wcPages;
    CHECK(gpu.bringUp(gpuConfig,nullptr,&gpuReport)==kIOReturnSuccess && gpu.ready());
    CHECK(gpuReport.streams==2 && gpuReport.table_pages==(aperture_gib==16?8209u:aperture_gib==4?2053u:1027u));
    CHECK(gpu.apertureBytes()==(uint64_t(aperture_gib)<<30) && gpu.inputBits()==(aperture_gib==16?34u:32u));
    CHECK(gpu.smmu().arena()->bytes==gpu.apertureBytes() && gpu.smmu().stateHandle()->tables.table_pages==gpuReport.table_pages);
    CHECK(wcPages-wcBefore==(0x1C000+0x7000+0x8000+3*0x1000)/4096);   /* every kernel buffer write-combined */
    CHECK(model.at(0x800)==0x80000000 && model.at(0xC00)==0x300 && model.at(0x800+12)==0x80000001 && model.at(0xC00+12)==0x300);
    CHECK(model.at(cb0+0x30)==tcrOf0(aperture_gib) && model.at(cb0)==sctlrRead() && model.at(cb0+4)==0x303 && model.at(cb0+0x38)==mair0());
    CHECK(gpuReport.program.actlr==0x303 && gpuReport.program.actlr_read==0x303 && smmu.bankState(0)->actlr_written);
    if(annibale())CHECK(model.at64(cb0+0x28)==0 && gpuReport.program.want.ttbr1==0);   /* TTBR1 0 whatever the guard */
    else CHECK(model.at64(cb0+0x28)==(uint64_t(1)<<48|gpu.guardTable()) && gpu.guardTable());
    CHECK((model.at64(cb0+0x20)>>48)==1);
    /* LPAC after CB0 was enabled: its S2CR went FAULT then translate, in that order; no write ever aborted. */
    std::vector<uint32_t> slot3;
    for(auto &w:model.s2cr_writes)if(w.first==3)slot3.push_back(w.second);
    CHECK(slot3.size()==2 && slot3[0]==0x20300 && slot3[1]==0x300 && model.aborts==0 && model.violations==0);
    uint32_t readback=0;
    CHECK(smmu.lpacRouted(&readback)==kIOReturnSuccess && readback==0x300);
    smmu.fillStatus(&status);
    CHECK((status.aux[3]>>A830StatusApertureShift)==4);   /* recorded: not run yet (AP0) */
    CHECK(smmu.grantAperture(0)==kIOReturnSuccess && host.apertures==1 && host.aperture_cb==0);
    int64_t x0=-1;
    CHECK(smmu.apertureState(&x0)==1 && !x0);
    smmu.fillStatus(&status);
    CHECK((status.aux[3]>>A830StatusApertureShift)==5 && ((status.aux[3]>>32)&0xFFFFFF)==status.smmu_slots);   /* AP1 */
    host.aperture_status=1;host.aperture_x0=-4;
    CHECK(smmu.grantAperture(0)==kIOReturnSuccess && smmu.apertureState(&x0)==2 && x0==-4);   /* recorded, not a failure */
    smmu.fillStatus(&status);
    CHECK((status.aux[3]>>A830StatusApertureShift)==6);   /* AP2 */
    /* Kernel buffers: fixed addresses, CPU views, PTEs of ARCHITECTURE 4.6.3. */
    NativeAdrenoGpuBuffer buffers[GpuBufferCount];
    CHECK(gpu.buffers(buffers)==kIOReturnSuccess && NativeAdrenoGpuContextBuffers(buffers)==kIOReturnSuccess);
    CHECK(buffers[GpuBufferSqe].iova==0x100000 && buffers[GpuBufferSqe].bytes==0x1C000 && buffers[GpuBufferAqe].iova==0x180000);
    CHECK(buffers[GpuBufferAqe].bytes==0x7000 && buffers[GpuBufferRing].iova==0x200000 && buffers[GpuBufferMemptrs].iova==0x300000);
    CHECK(buffers[GpuBufferPwrup].iova==0x301000 && buffers[GpuBufferScratch].iova==0x302000 && buffers[GpuBufferRing].cpu);
    const ArmSmmu4KLiveArena *arena=gpu.smmu().arena();
    CHECK((leafOf(arena,0x100000)&~0x0000fffffffff000ull)==0x0000000000000EC3ull);
    CHECK((leafOf(arena,0x180000)&~0x0000fffffffff000ull)==0x0000000000000EC3ull);
    CHECK((leafOf(arena,0x200000)&~0x0000fffffffff000ull)==0x0060000000000E83ull);
    CHECK((leafOf(arena,0x300000)&~0x0000fffffffff000ull)==0x0060000000000E03ull);
    CHECK((leafOf(arena,0x302000)&~0x0000fffffffff000ull)==0x0060000000000E03ull);
    CHECK(leafOf(arena,0x300000)==(buffers[GpuBufferMemptrs].physical|0x0060000000000E03ull));
    /* TCR 0x803519 walks the same root page with 512 slots: the ones past the aperture the IOVAs use (4 or 16 GiB) are all invalid. */
    for(unsigned slot=aperture_gib==16?16u:4u;slot<512;++slot)CHECK(!arena->mapping[slot]);
    if(aperture_gib==16)for(unsigned slot=0;slot<16;++slot)CHECK(arena->mapping[slot]&1);   /* every 1 GiB slot has its table */
    NativeAdrenoVm *vm=gpu.vm();
    CHECK(vm && NativeAdrenoGpuContextVm()==vm && vm->bank()==0);
    /* Client mappings: write-combined own BOs non-cacheable outer shareable (0x0060...E43). */
    NativeAdrenoMemory bo,big,gmem,nullPage;
    CHECK(!bo.allocate(0x4000,36,false,0x4000) && !bo.makeWriteCombined());
    uint64_t handle=0;
    const uint64_t top=(uint64_t(aperture_gib)<<30)-0x4000;
    CHECK(vm->mapClient(&bo,top,ArmSmmu4KRead|ArmSmmu4KWrite,1,3,&handle)==kIOReturnSuccess && handle);
    CHECK((leafOf(arena,top)&~0x0000fffffffff000ull)==0x0060000000000E43ull && !vm->maintainedCount());
    CHECK(!gmem.allocate(0x2000,36,false,0x2000) && !nullPage.allocate(0x1000,36,false,0x1000));
    uint64_t refused=0;
    CHECK(vm->map(&gmem,0,0x4BFF000,3,0,2,&refused)==kIOReturnBadArgument && !refused);   /* GMEM window */
    CHECK(vm->map(&nullPage,0,0x0,3,0,2,&refused)==kIOReturnBadArgument && !refused);        /* null guard */
    CHECK(vm->map(&gmem,0,top+0x4000,3,0,2,&refused)==kIOReturnBadArgument);              /* past the aperture */
    /* A client page fault in CB0 (design: 710; annibale: the poll): recorded with FAR and SID, counted, the bottom half
     * notified; the VM keeps working. */
    model.fault(0,0x8,0x20001000,0);
    if(annibale())smmu.poll();
    else CHECK(host.fire(710)==1);
    CHECK(status.smmu_faults==1 && host.notifies==1 && status.sticky[1]==0x8);
    std::vector<A830SmmuFault> seen;
    CHECK(smmu.drainFaults(fault,&seen)==1 && seen.size()==1 && seen[0].far==0x20001000 && seen[0].cb==0 && (seen[0].fsr&0xC00001FE)==0x8);
    CHECK(seen[0].polled==annibale());
    /* Its log line: all of FAR, the registers that say what kind of fault it was, at the widest values inside the buffer. */
    {
        char line[A830SmmuFaultLineBytes+8];
        memset(line,'#',sizeof line);
        A830SmmuFault one={};
        one.far=0x3E0001000ull;one.fsr=2;one.fsynr0=0x10;one.fsynr1=0x20002;one.sid=0x1000;one.cb=0;
        A830SmmuFaultLine(line,one);
        CHECK(!strcmp(line,"gpu fault cb0 far 0x3e0001000 fsr 0x2 syn 0x10 0x20002 sid 0x1000"));
        one.far=UINT64_MAX;one.fsr=one.fsynr0=one.fsynr1=UINT32_MAX;one.sid=UINT16_MAX;one.cb=255;
        A830SmmuFaultLine(line,one);
        CHECK(!strcmp(line,"gpu fault cb255 far 0xffffffffffffffff fsr 0xffffffff syn 0xffffffff 0xffffffff sid 0xffff") &&
              strlen(line)==A830SmmuFaultLineBytes-2 && line[A830SmmuFaultLineBytes]=='#');
        one.cb=12;
        A830SmmuFaultLine(line,one);
        CHECK(!strncmp(line,"gpu fault cb12 far ",19));
    }
    /* The interrupt's fault carries the ring's fences, read before any register (M14 P0); the poll's carries none. */
    if(annibale())CHECK(!seen[0].sampled && !seen[0].completed && !seen[0].issued && !host.fence_reads);
    else CHECK(seen[0].sampled && seen[0].completed==7 && seen[0].issued==9 && host.fence_reads==1);
    model.fault(0,0x2,0x20002000,0);   /* still pending while the VM maps and unmaps: nothing polls it */
    CHECK(!big.allocate(0x202000,36,false,0x202000));   /* 514 pages: more than the 512 a page-by-page invalidation covers */
    uint64_t bigHandle=0;
    CHECK(vm->mapClient(&big,0x20000000,ArmSmmu4KRead,0,2,&bigHandle)==kIOReturnSuccess);
    model.tlbival.clear();
    const unsigned asid0=model.tlbiasid,all0=model.tlbiall;
    size_t mark=stores.size();
    CHECK(vm->unmap(handle)==kIOReturnSuccess && !vm->poisoned());
    if(!annibale()) {
        CHECK(model.tlbival.size()==4 && model.violations==0);
        for(size_t i=0;i<4;++i)CHECK(model.tlbival[i]==(uint64_t(1)<<48|((top>>12)+i)));   /* TLBIVAL: VA[55:12], ASID 1 */
        CHECK(vm->unmap(bigHandle)==kIOReturnSuccess && model.tlbiasid==asid0+1 && model.last_tlbiasid==1);   /* TLBIASID */
        CHECK(model.tlbival.size()==4);
    } else {
        /* Each unmap: TLBIASID of ASID 1 right before its TLBSYNC = 1; no TLBIVAL, no TLBIALL, no 64-bit store. */
        CHECK(model.tlbival.empty() && model.violations==0 && annibaleTlb(mark)>=1 && model.last_tlbiasid==1);
        mark=stores.size();
        CHECK(vm->unmap(bigHandle)==kIOReturnSuccess && annibaleTlb(mark)>=1 && model.last_tlbiasid==1);
        CHECK(model.tlbival.empty() && model.tlbiall==all0 && model.tlbiasid>asid0);
    }
    /* The VM's whole-bank invalidation (the kit's TLBIALL): annibale turns it into TLBIASID of ASID 1. */
    const unsigned asid1=model.tlbiasid,all1=model.tlbiall;
    mark=stores.size();
    CHECK(vm->invalidateTlb()==kIOReturnSuccess);
    if(annibale())CHECK(model.tlbiall==all1 && model.tlbiasid==asid1+1 && model.last_tlbiasid==1 && annibaleTlb(mark)==1);
    else CHECK(model.tlbiall==all1+1 && model.tlbiasid==asid1);
    if(annibale())smmu.poll();
    else CHECK(host.fire(710)==1);
    CHECK(!vm->poisoned() && status.smmu_faults==2);
    /* The GMU VM's unmap: ASID 3. */
    mark=stores.size();
    CHECK(gmuVm->unmap(hfiHandle)==kIOReturnSuccess && gmuVm->objectCount()==1);
    if(annibale())CHECK(annibaleTlb(mark)>=1 && model.last_tlbiasid==3 && model.tlbival.empty());
    else CHECK(!model.tlbival.empty() && (model.tlbival.back()>>48)==3);
    /* Routes and context as the VM's attach verified them; a CP that cleared TCR.EPD1 is tolerated, a changed TTBR0 is not. */
    CHECK(smmu.verifyRoutes(0)==kIOReturnSuccess && smmu.verifyRoutes(2)==kIOReturnSuccess);
    model.at(cb0+0x30)&=~(1u<<23);
    CHECK(smmu.verifyRoutes(0)==kIOReturnSuccess && smmu.routesIntact());
    const uint32_t ttbr=model.at(cb0+0x20);
    model.at(cb0+0x20)=ttbr+0x1000;
    CHECK(smmu.verifyRoutes(0)==kIOReturnIOError && smmu.failure().reason==A830ReasonReadback && !smmu.routesIntact());
    model.at(cb0+0x20)=ttbr;
    CHECK(model.aborts==0 && model.violations==0);
    /* The global stores of the whole run: design sACR (CACHE_LOCK) and the sGFSR write-back; annibale none, only SMR/S2CR and
     * CBAR/CBA2R in the global space, and no 64-bit store at all. */
    if(annibale()) {
        CHECK(!stored(0x00) && !stored(0x10) && !stored(0x48) && annibaleTlb(0)>=6);
        auto in=[](uint32_t o,uint32_t base,uint32_t count) {return o>=base && o<base+4*count;};
        for(const Store &s:stores)
            CHECK(s.offset>=A830SmmuModel::cb(0) || in(s.offset,0x800,A830SmmuModel::Smrs) ||
                  in(s.offset,0xC00,A830SmmuModel::Smrs) || in(s.offset,0x1000,A830SmmuModel::Banks) ||
                  in(s.offset,0x1800,A830SmmuModel::Banks));
    } else CHECK(stored(0x10) && stored(0x48) && !stored(0x00));
    /* The board release path of a failure after S3: every claimed stream FAULT with its SMR kept, both banks off. */
    CHECK(smmu.releaseAll()==kIOReturnSuccess);
    CHECK(model.at(0xC00)==0x20300 && model.at(0xC00+4)==0x20300 && model.at(0xC00+12)==0x20300);
    CHECK(model.at(0x800)==0x80000000 && model.at(0x800+4)==0x80000005 && model.at(0x800+12)==0x80000001);
    const unsigned tlbiall=model.tlbiall,tlbiasid=model.tlbiasid;
    CHECK(!model.at(cb0) && !model.at(cb2));
    CHECK(smmu.releaseAll()==kIOReturnSuccess && model.tlbiall==tlbiall && model.tlbiasid==tlbiasid);   /* no TLB maintenance */
    ++cases;
}

/* The VM's TLB sync through the programmer's I/O (ARCHITECTURE 4.6.4): spins, back-off, and a sync still busy after 10 ms fails
 * the unmap: the VM is poisoned (no more submissions), every page stays referenced, both GPU streams FAULT. The same budget in
 * both profiles (annibale: the sync is the TLBIASID + TLBSYNC the I/O writes). */
static void vmSyncBudget()
{
    Adreno830Status status{};
    A830SmmuHostPlatform host;
    model.boot(81);
    A830Smmu smmu;
    CHECK(smmu.init(A830SmmuBoardConfig(model,host,&status))==kIOReturnSuccess && smmu.survey(nullptr)==kIOReturnSuccess);
    CHECK(smmu.resetBank(2)==0 && smmu.resetBank(0)==0);
    NativeAdrenoGpuContext gpu;
    NativeAdrenoGmuContextReport report;
    NativeAdrenoGpuContextConfig config{smmu.programmer(),0,1,0,3,0,1,0,2,0,true};
    CHECK(gpu.bringUp(config,nullptr,&report)==kIOReturnSuccess);
    NativeAdrenoVm *vm=gpu.vm();
    NativeAdrenoMemory a,b;
    CHECK(!a.allocate(0x2000,36,false,0x2000) && !b.allocate(0x2000,36,false,0x2000));
    uint64_t ha=0,hb=0;
    CHECK(!vm->mapClient(&a,0x20000000,3,0,2,&ha) && !vm->mapClient(&b,0x20004000,3,0,2,&hb));
    model.tlb_busy=14;   /* busy for ten spins and four back-offs: 1+2+4+8 us */
    host.delays=0;host.delayed=0;
    const unsigned asid=model.tlbiasid;
    CHECK(vm->unmap(ha)==kIOReturnSuccess && host.delays==4 && host.delayed==15 && !vm->poisoned());
    if(annibale())CHECK(model.tlbival.empty() && model.tlbiasid>asid && model.last_tlbiasid==1);
    else CHECK(model.tlbival.size()==2 && model.tlbiasid==asid);
    model.tlb_stuck=true;
    const uint64_t start=host.clock;
    CHECK(vm->unmap(hb)==kIOReturnIOError && vm->poisoned() && smmu.tlbTimeouts()==1);
    CHECK(host.clock-start>=10000 && host.clock-start<12000 && b.quarantined() && b.deviceReferences());
    CHECK(model.at(0xC00)==0x20300 && model.at(0xC00+12)==0x20300 && model.at(0x800)==0x80000000);   /* streams FAULT, SMRs kept */
    model.tlb_stuck=false;model.tlb_busy=2;
    uint64_t again=0;
    CHECK(vm->mapClient(&a,0x20000000,3,0,2,&again)==kIOReturnNotReady && !again);
    ++cases;
}

/* A VM map (invalid to valid) through the real programmer: annibale sets sync_after_map, so the map ends with TLBIASID of the
 * bank's ASID and TLBSYNC = 1 and nothing else, as K90 invalidated after a map (kernel-sequences section 14 row 12), on the GPU
 * bank (ASID 1) and the GMU bank (ASID 3); the design maps without any register store (ARCHITECTURE 4.6.4). */
static void vmMapSyncCases()
{
    Adreno830Status status{};
    A830SmmuHostPlatform host;
    model.boot(61);
    A830Smmu smmu;
    CHECK(smmu.init(recordedConfig(host,&status))==kIOReturnSuccess && smmu.survey(nullptr)==kIOReturnSuccess);
    CHECK(smmu.resetBank(2)==0 && smmu.resetBank(0)==0);
    const NativeAdrenoSmmuProgrammer *programmer=smmu.programmer();
    CHECK(programmer && programmer->sync_after_map==annibale());
    NativeAdrenoGmuContext gmu;
    NativeAdrenoGmuContextReport gmuReport;
    NativeAdrenoGmuContextConfig gmuConfig{programmer,2,3,smmu.gmuStream().slot,5,0,nullptr,nullptr};
    CHECK(gmu.bringUp(gmuConfig,nullptr,&gmuReport)==kIOReturnSuccess);
    NativeAdrenoGpuContext gpu;
    NativeAdrenoGmuContextReport gpuReport;
    NativeAdrenoGpuContextConfig gpuConfig{programmer,0,1,smmu.gpuStream().slot,smmu.lpacStream().slot,0,1,0,2,0,true};
    CHECK(gpu.bringUp(gpuConfig,nullptr,&gpuReport)==kIOReturnSuccess);
    struct Map {NativeAdrenoVm *vm;uint64_t iova;unsigned permissions;uint32_t asid;} maps[2]={
        {gpu.vm(),0x20000000,ArmSmmu4KRead|ArmSmmu4KWrite,1},
        {gmu.vm(),0x60008000,ArmSmmu4KRead|ArmSmmu4KWrite|ArmSmmu4KPrivileged,3}};
    NativeAdrenoMemory memory[2];
    for(unsigned i=0;i<2;++i) {
        CHECK(maps[i].vm && !memory[i].allocate(0x3000,36,false,0x3000));
        const unsigned asids=model.tlbiasid,syncs=model.tlbsync,alls=model.tlbiall;
        const size_t mark=stores.size();
        uint64_t handle=0;
        CHECK(maps[i].vm->map(&memory[i],0,maps[i].iova,maps[i].permissions,0,2,&handle)==kIOReturnSuccess && handle);
        CHECK(!maps[i].vm->poisoned() && model.tlbiall==alls && model.tlbival.empty());
        if(annibale())
            CHECK(stores.size()==mark+2 && annibaleTlb(mark)==1 && model.tlbiasid==asids+1 && model.last_tlbiasid==maps[i].asid &&
                  model.tlbsync==syncs+1);
        else CHECK(stores.size()==mark && model.tlbiasid==asids && model.tlbsync==syncs);
    }
    CHECK(model.aborts==0 && model.violations==0);
    ++cases;
}

/* ARCHITECTURE 4.12.1 storm guard (design): 64 handlings without progress mask CFIE and the SPI and the bank is polled;
 * restoreIrqs turns them back on. Annibale: nothing is attached, SCTLR has no CFIE, a fault is found by poll() and restoreIrqs
 * has nothing to restore. A fault on the GMU bank with the interrupts unwired is found by poll(). */
static void stormAndPollCases()
{
    Adreno830Status status{};
    A830SmmuHostPlatform host;
    model.boot(41);
    A830Smmu smmu;
    CHECK(smmu.init(A830SmmuBoardConfig(model,host,&status))==kIOReturnSuccess && smmu.survey(nullptr)==kIOReturnSuccess);
    CHECK(smmu.resetBank(2)==kIOReturnSuccess && smmu.resetBank(0)==kIOReturnSuccess);
    CHECK(smmu.attachIrqs()==(annibale()?0u:2u) && host.lines.size()==(annibale()?0u:2u));
    CHECK(smmu.claimStream({0,0,0})==kIOReturnSuccess);
    ArmSmmu4KContextImage image{0x1F300,1,0x80803520,0x38011,0xFF44,0,0x1167,0,uint64_t(1)<<48|0x9A000000,uint64_t(1)<<48};
    A830SmmuReadback rb;
    CHECK(smmu.programContext(0,image,0x303,0,&rb)==kIOReturnSuccess && !rb.mismatch && rb.sctlr==sctlrRead() && rb.tcr2==0x38071);
    CHECK(smmu.routeStream({0,0,0})==kIOReturnSuccess);
    if(!annibale()) {
        /* A level line nobody clears (the status stays the same fault): 64 times and it is off. */
        CHECK(host.fire(710,63)==63 && !(status.irq_off&A830SmmuIrqOffGpu));
        for(unsigned i=0;i<63;++i)model.fault(0,0x2,0x1000,0);
        unsigned ran=0;
        for(unsigned i=0;i<200 && host.lines[0].enabled;++i) {model.at(A830SmmuModel::cb(0)+0x58)=0;ran+=host.fire(710);}
        CHECK(!host.lines[0].enabled && (status.irq_off&A830SmmuIrqOffGpu) && smmu.storms()==1 && host.disables==1);
        CHECK(!(model.at(A830SmmuModel::cb(0))&A830_SMMU_SCTLR_CFIE) && ran);
    } else CHECK((status.irq_off&A830SmmuIrqOffGpu) && model.at(A830SmmuModel::cb(0))==0x27 && !smmu.storms());
    /* Polled now: the next fault is found once a second by the DCVS thread. */
    model.fault(0,0x8,0x2000,0);
    const uint32_t before=status.smmu_faults;
    smmu.poll();
    CHECK(status.smmu_faults==before+1 && !(model.at(A830SmmuModel::cb(0)+0x58)&0xC00001FE));
    std::vector<A830SmmuFault> seen;
    smmu.drainFaults(fault,&seen);
    CHECK(!seen.empty() && seen.back().polled && seen.back().far==0x2000);
    if(!annibale()) {
        CHECK(smmu.restoreIrqs()==1 && host.lines[0].enabled && !(status.irq_off&A830SmmuIrqOffGpu) && host.enables==1);
        CHECK(model.at(A830SmmuModel::cb(0))&A830_SMMU_SCTLR_CFIE);
    } else {
        CHECK(smmu.restoreIrqs()==0 && model.at(A830SmmuModel::cb(0))==0x27 && (status.irq_off&A830SmmuIrqOffGpu));
        CHECK(!host.enables);
    }
    /* No SPI interface: nothing wired, both banks polled. */
    model.boot(42);
    Adreno830Status quiet{};
    A830Smmu unwired;
    CHECK(unwired.init(A830SmmuBoardConfig(model,host,&quiet,false))==kIOReturnSuccess && unwired.survey(nullptr)==0);
    CHECK(unwired.resetBank(0)==0 && unwired.resetBank(2)==0 && unwired.attachIrqs()==0);
    CHECK((quiet.irq_off&(A830SmmuIrqOffGpu|A830SmmuIrqOffGmu))==(A830SmmuIrqOffGpu|A830SmmuIrqOffGmu));
    model.fault(2,0x2,0x60000000,5);
    unwired.poll();   /* the GMU bank is not enabled yet: cleared, not counted */
    CHECK(!quiet.smmu_faults && !(model.at(A830SmmuModel::cb(2)+0x58)&0xC00001FE));
    /* Design: an attach whose readback is not an enabled level Group 1 SPI at 0xA0 is polled as if unwired. Annibale: no
     * attach at all. */
    model.boot(43);
    A830SmmuHostPlatform bad;bad.readback=0xA001;
    Adreno830Status other{};
    A830Smmu badwired;
    CHECK(badwired.init(A830SmmuBoardConfig(model,bad,&other))==kIOReturnSuccess && badwired.survey(nullptr)==0);
    CHECK(badwired.resetBank(0)==0 && badwired.resetBank(2)==0 && badwired.attachIrqs()==0 && bad.lines.size()==(annibale()?0u:2u));
    CHECK(other.irq_off&A830SmmuIrqOffGpu);
    ++cases;
}

/* TLB synchronization (ARCHITECTURE 4.6.4): ten spins, then 1, 2, 4 ... us; a sync still busy after 10 ms fails the step with
 * the timeout reason and keeps the bank disabled. Step 12 is TLBIALL (design) or TLBIASID of ASID 1 (annibale). */
static void tlbCases()
{
    Adreno830Status status{};
    A830SmmuHostPlatform host;
    model.boot(51);model.tlb_busy=20;
    A830Smmu smmu;
    CHECK(smmu.init(A830SmmuBoardConfig(model,host,&status))==kIOReturnSuccess && smmu.survey(nullptr)==kIOReturnSuccess);
    CHECK(smmu.claimStream({0,0,0})==kIOReturnSuccess);
    ArmSmmu4KContextImage image{0x1F300,1,0x80803520,0x38011,0xFF44,0,0x1167,0,uint64_t(1)<<48|0x9A000000,uint64_t(1)<<48};
    A830SmmuReadback rb;
    host.delays=0;host.delayed=0;
    CHECK(smmu.programContext(0,image,0x303,0,&rb)==kIOReturnSuccess);
    CHECK(host.delays==10 && host.delayed==(1u+2+4+8+16+32+64+128+256+512));
    if(annibale())CHECK(model.tlbiasid==1 && model.last_tlbiasid==1 && !model.tlbiall && model.tlbsync==1);
    else CHECK(model.tlbiall==1 && !model.tlbiasid && model.tlbsync==1);
    model.tlb_stuck=true;
    A830Smmu stuck;
    model.boot(52);model.tlb_busy=2;model.tlb_stuck=true;
    CHECK(stuck.init(A830SmmuBoardConfig(model,host,&status))==kIOReturnSuccess && stuck.survey(nullptr)==kIOReturnSuccess);
    const uint64_t start=host.clock;
    CHECK(stuck.programContext(0,image,0x303,0,&rb)==kIOReturnTimeout);
    CHECK(stuck.failure().step==A830SmmuStepGpu+A830SmmuStepTlb && stuck.failure().reason==A830ReasonTimeout);
    CHECK(host.clock-start>=10000 && host.clock-start<12000 && stuck.tlbTimeouts()==1);
    CHECK(!(model.at(A830SmmuModel::cb(0))&1));   /* never enabled */
    model.tlb_stuck=false;model.tlb_busy=2;
    /* A ttbr1 with a different ASID or an unaligned table is refused before any write (both profiles). */
    CHECK(smmu.programContext(0,image,0x303,uint64_t(2)<<48|0x9B000000,&rb)==kIOReturnBadArgument);
    CHECK(smmu.programContext(0,image,0x303,uint64_t(1)<<48|0x9B000800,&rb)==kIOReturnBadArgument);
    ++cases;
}

/* Step-14 tolerance rules and the refusals around them: a TCR write the hardware drops disables the bank again and names
 * the register; an ACTLR write ignored under CACHE_LOCK is only recorded (annibale: CACHE_LOCK is never cleared). */
static void readbackCases()
{
    Adreno830Status status{};
    A830SmmuHostPlatform host;
    model.boot(61,0,true);   /* CACHE_LOCK left set: ACTLR does not take 0x303 */
    A830Smmu smmu;
    CHECK(smmu.init(A830SmmuBoardConfig(model,host,&status))==kIOReturnSuccess && smmu.survey(nullptr)==kIOReturnSuccess);
    CHECK(smmu.releaseCacheLock()==kIOReturnSuccess && (model.at(0x10)&(1u<<26))==(annibale()?1u<<26:0u));
    model.at(0x10)|=1u<<26;   /* both profiles from here: CACHE_LOCK set when CB0 is programmed */
    CHECK(smmu.resetBank(0)==kIOReturnSuccess && smmu.claimStream({0,0,0})==kIOReturnSuccess);
    const uint32_t actlr=model.at(A830SmmuModel::cb(0)+4);
    ArmSmmu4KContextImage image{0x1F300,1,0x80803520,0x38011,0xFF44,0,0x1167,0,uint64_t(1)<<48|0x9A000000,uint64_t(1)<<48};
    A830SmmuReadback rb;
    CHECK(smmu.programContext(0,image,0x303,0,&rb)==kIOReturnSuccess && rb.actlr==actlr && actlr!=0x303);
    CHECK(smmu.bankState(0)->actlr_read==actlr && smmu.bankState(0)->enabled && smmu.bankState(0)->actlr_written);
    /* Route before the bank is programmed is refused; the S2CR keeps FAULT. */
    A830Smmu early;
    model.boot(62);
    CHECK(early.init(A830SmmuBoardConfig(model,host,&status))==kIOReturnSuccess && early.survey(nullptr)==kIOReturnSuccess);
    CHECK(early.claimStream({1,2,5})==kIOReturnSuccess && model.at(0xC00+4)==0x20300 && model.at(0x800+4)==0x80000005);
    CHECK(early.routeStream({1,2,5})==kIOReturnNotReady && model.at(0xC00+4)==0x20300);
    CHECK(early.claimStream({1,2,6})==kIOReturnBusy && early.claimStream({4,2,5})==kIOReturnBusy);
    /* A dropped TCR write: step 7 of the GMU bank, the bank left disabled. */
    struct Dropper {static int write32(void *c,size_t at,uint32_t v)
        {if(at==A830SmmuModel::cb(2)+0x30)return 0;return A830SmmuModel::write32(c,at,v);}};
    model.boot(63);   /* the claims above left SID 5 routed: a fresh boot */
    ArmSmmu4KIo dropping=model.io;dropping.write32=Dropper::write32;
    A830SmmuConfig config=A830SmmuBoardConfig(model,host,&status);config.io=&dropping;
    A830Smmu dropped;
    CHECK(dropped.init(config)==kIOReturnSuccess && dropped.survey(nullptr)==kIOReturnSuccess);
    CHECK(dropped.claimStream({1,2,5})==kIOReturnSuccess);
    CHECK(dropped.programContext(2,image,0x303,0,&rb)==kIOReturnIOError);
    CHECK(dropped.failure().step==A830SmmuStepGmu+A830SmmuStepTcr && dropped.failure().reason==A830ReasonReadback);
    CHECK(dropped.failure().reg==0x3DA0000+A830SmmuModel::cb(2)+0x30 && dropped.failure().expect==tcrOf(2));
    CHECK(!(model.at(A830SmmuModel::cb(2))&1) && dropped.routeStream({1,2,5})==kIOReturnNotReady);
    CHECK(model.aborts==0 && model.violations==0);
    ++cases;
}

/* Slumber with CX collapsed (M13): the SMMU comes back with boot values; routesIntact says so and restore() replays the
 * take-over from the recorded images over the unchanged tables (annibale: its values again, the GMU ACTLR left as booted). */
static void restoreCases()
{
    Adreno830Status status{};
    A830SmmuHostPlatform host;
    model.boot(71);
    A830Smmu smmu;
    CHECK(smmu.init(A830SmmuBoardConfig(model,host,&status))==kIOReturnSuccess && smmu.survey(nullptr)==kIOReturnSuccess);
    CHECK(smmu.resetBank(2)==0 && smmu.resetBank(0)==0);
    ArmSmmu4KContextImage gmuImage{0x1F300,1,0x80803520,0x38011,0xFF44,0,0x1167,0,uint64_t(3)<<48|0x9A100000,uint64_t(3)<<48};
    ArmSmmu4KContextImage gpuImage{0x1F300,1,0x80803520,0x38011,0xFF44,0,0x1167,0,uint64_t(1)<<48|0x9A200000,uint64_t(1)<<48};
    A830SmmuReadback rb;
    CHECK(smmu.claimStream(smmu.gmuStream())==0 && smmu.programContext(2,gmuImage,0x303,0,&rb)==0);
    CHECK(smmu.routeStream(smmu.gmuStream())==0);
    CHECK(smmu.claimStream(smmu.gpuStream())==0 && smmu.programContext(0,gpuImage,0x303,uint64_t(1)<<48|0x9A300000,&rb)==0);
    CHECK(smmu.routeStream(smmu.gpuStream())==0 && smmu.attachLpac(3,1,0)==0 && smmu.grantAperture(0)==0);
    CHECK(smmu.routesIntact() && smmu.save()==kIOReturnSuccess);
    const uint32_t cb0=A830SmmuModel::cb(0),cb2=A830SmmuModel::cb(2);
    const uint64_t ttbr1=annibale()?0:uint64_t(1)<<48|0x9A300000;
    CHECK(model.at(cb0+0x30)==tcrOf(0) && model.at64(cb0+0x28)==ttbr1 && model.at(cb0+0x38)==mair0());
    model.boot(72);   /* CX collapsed: boot values again */
    const uint32_t cb2Actlr=model.at(cb2+4);
    CHECK(!smmu.routesIntact());
    const unsigned apertures=host.apertures;
    CHECK(smmu.restore()==kIOReturnSuccess && smmu.routesIntact() && host.apertures==apertures+1);
    CHECK(model.at(0xC00)==0x300 && model.at(0xC00+4)==0x302 && model.at(0xC00+12)==0x300 && model.at(0x800+12)==0x80000001);
    CHECK(model.at(cb0+0x30)==tcrOf(0) && model.at64(cb0+0x20)==gpuImage.ttbr0 && model.at(cb0)==sctlrRead());
    CHECK(model.at64(cb0+0x28)==ttbr1 && model.at(cb2+0x30)==tcrOf(2) && model.at(cb2)==sctlrRead() && model.at(cb2+0x38)==mair0());
    CHECK(model.at(cb2+4)==(annibale()?cb2Actlr:0x303u) && model.at(cb0+4)==0x303);
    CHECK(model.at64(cb2+0x20)==gmuImage.ttbr0 && model.aborts==0 && model.violations==0);
    /* Releasing one stream: FAULT and its bank off, its TLB invalidated (design TLBIALL, annibale TLBIASID 3). */
    CHECK(smmu.release(smmu.gmuStream())==kIOReturnSuccess && model.at(0xC00+4)==0x20300 && !model.at(cb2));
    if(annibale())CHECK(model.tlbiasid==3 && model.last_tlbiasid==3 && !model.tlbiall);
    else CHECK(model.tlbiall==3 && !model.tlbiasid);
    ++cases;
}

/* The exact stores of 4.5.4 steps 3-13 for each bank (annibale: kernel-sequences 7.4 steps 4-15, with our ASID CB + 1), of the
 * bank's disable, and the VM's TLB I/O: design forwards the kit's stores, annibale sends TLBIASID of the bank's ASID and
 * TLBSYNC = 1 for them. */
static void sequenceCases()
{
    Adreno830Status status{};
    A830SmmuHostPlatform host;
    model.boot(91);
    A830Smmu smmu;
    CHECK(smmu.init(recordedConfig(host,&status))==kIOReturnSuccess && smmu.survey(nullptr)==kIOReturnSuccess);
    const ArmSmmu4KContextImage kit{0x1F300,1,0x80803520,0x38011,0xFF44,0,0x1167,0,0,0};
    const uint32_t sync=annibale()?1u:0xFFFFFFFFu;
    for(unsigned cb:{2u,0u}) {
        const uint64_t guard=cb?0:(uint64_t(1)<<48|0x9A300000ull);   /* the design's CB0 guard; annibale writes TTBR1 0 */
        ArmSmmu4KContextImage image=kit;
        image.ttbr0=uint64_t(cb+1)<<48|(cb?0x9A100000ull:0x9A200000ull);image.ttbr1=uint64_t(cb+1)<<48;
        const uint32_t off=A830SmmuModel::cb(cb);
        const uint64_t ttbr1=annibale()?0:guard;
        const uint32_t tcr=tcrOf(cb);
        stores.clear();
        A830SmmuReadback rb;
        CHECK(smmu.programContext(cb,image,0x303,guard,&rb)==kIOReturnSuccess && !rb.mismatch);
        std::vector<Store> want={{off,0,32}};
        if(!annibale() || cb==0)want.push_back({off+4,0x303,32});
        const std::vector<Store> rest={{0x1800+4*cb,1,32},{0x1000+4*cb,0x1F300,32},{off+0x10,0x38011,32},{off+0x30,tcr,32},
            {off+0x20,uint32_t(image.ttbr0),32},{off+0x24,uint32_t(image.ttbr0>>32),32},{off+0x28,uint32_t(ttbr1),32},
            {off+0x2C,uint32_t(ttbr1>>32),32},{off+0x38,mair0(),32},{off+0x3C,0,32},{off+0x58,0xC00001FE,32},
            annibale()?Store{off+0x610,cb+1,32}:Store{off+0x618,0xFFFFFFFF,32},{off+0x7F0,sync,32},
            {off,annibale()?0x27u:0x1167u,32}};
        want.insert(want.end(),rest.begin(),rest.end());
        CHECK(stores==want);
        CHECK(rb.sctlr==sctlrRead() && rb.tcr==tcr && rb.mair0==mair0() && rb.mair1==0 && rb.ttbr1==ttbr1 && rb.ttbr0==image.ttbr0);
        CHECK(smmu.bankState(cb)->actlr_written==(!annibale() || cb==0) && smmu.bankState(cb)->image.tcr==tcr);
    }
    /* The VM's TLB I/O on CB0 (ASID 1): two TLBIVAL, a TLBIALL, a TLBIASID, the TLBSYNC; then a global store passes as is. */
    const ArmSmmu4KIo *io=smmu.tableIo();
    const uint32_t off0=A830SmmuModel::cb(0);
    stores.clear();
    CHECK(!io->write64(io->cookie,off0+0x620,uint64_t(1)<<48|0x20000));
    CHECK(!io->write64(io->cookie,off0+0x620,uint64_t(1)<<48|0x20001));
    CHECK(!io->write32(io->cookie,off0+0x618,0) && !io->write32(io->cookie,off0+0x610,1));
    CHECK(!io->write32(io->cookie,off0+0x7F0,0xFFFFFFFF));
    if(annibale())CHECK(stores==(std::vector<Store>{{off0+0x610,1,32},{off0+0x7F0,1,32}}) && annibaleTlb(0)==1);
    else CHECK(stores==(std::vector<Store>{{off0+0x620,uint64_t(1)<<48|0x20000,64},{off0+0x620,uint64_t(1)<<48|0x20001,64},
                                          {off0+0x618,0,32},{off0+0x610,1,32},{off0+0x7F0,0xFFFFFFFF,32}}));
    uint32_t busy=1;
    for(unsigned i=0;i<4 && busy;++i)CHECK(!io->read32(io->cookie,off0+0x7F4,&busy));
    CHECK(!busy);
    stores.clear();
    CHECK(!io->write32(io->cookie,0x800+4*20,0));
    CHECK(stores==(std::vector<Store>{{0x800+4*20,0,32}}));
    /* Disabling CB2: SCTLR 0, then the bank's invalidation (ASID 3) and sync. */
    stores.clear();
    const uint32_t off2=A830SmmuModel::cb(2);
    CHECK(smmu.disableBank(2)==kIOReturnSuccess);
    CHECK(stores==(std::vector<Store>{{off2,0,32},annibale()?Store{off2+0x610,3,32}:Store{off2+0x618,0xFFFFFFFF,32},
                                      {off2+0x7F0,sync,32}}));
    CHECK(model.aborts==0 && model.violations==0);
    ++cases;
}

/* The device binding on an ordinary page: offsets, widths and the values travel unchanged; out-of-range and unaligned
 * accesses are refused. */
static void mmioCases()
{
#if defined(__aarch64__)
    alignas(4096) static uint32_t page[0x2000/4];
    A830SmmuMmio mmio{reinterpret_cast<volatile uint8_t *>(page),sizeof page};
    ArmSmmu4KIo io;
    CHECK(A830SmmuBindMmio(&mmio,&io)==kIOReturnSuccess && io.register_bytes==sizeof page);
    CHECK(!io.write32(io.cookie,0x10,0) && !io.write32(io.cookie,0x14,0xDEADBEEF) && page[5]==0xDEADBEEF && !page[4]);
    CHECK(!io.write64(io.cookie,0x620,0x0001000000012345ull) && page[0x620/4]==0x12345 && page[0x624/4]==0x10000);
    uint32_t v=0;uint64_t w=0;
    CHECK(!io.read32(io.cookie,0x14,&v) && v==0xDEADBEEF && !io.read64(io.cookie,0x620,&w) && w==0x0001000000012345ull);
    CHECK(io.write32(io.cookie,0x2000,1) && io.write64(io.cookie,0x624,1) && io.read32(io.cookie,0x2,&v) && !io.barrier(io.cookie));
    A830SmmuMmio unaligned{reinterpret_cast<volatile uint8_t *>(page)+8,0x1000};
    CHECK(A830SmmuBindMmio(&unaligned,&io)==kIOReturnBadArgument && A830SmmuBindMmio(nullptr,&io)==kIOReturnBadArgument);
#endif
    ++cases;
}

/* pagani-gpu-va=16 and the SMMU's stage-1 input size, IDR2.UBS (bits 11:8; what the kit's encoder checks a context's input bits
 * against), not IDR2.IAS (bits 3:0, the stage-2 input size): with a 32-bit UBS and a 36-bit IAS the GPU context takes the 4 GiB
 * aperture (2053 pages, a 32-bit input, the 4 GiB client windows) instead of failing in the encoder; with a 48-bit UBS and a 32-bit
 * IAS it builds the 16 GiB one (8209 pages, 34 bits). SM8750's 0x5511 is UBS 48, IAS 36 (takeOverCases(16)). */
static void wideFallbackCases()
{
    struct Case {uint32_t idr2;uint64_t aperture;uint32_t pages;unsigned input_bits;};
    const Case table[]={{0x1011,0x100000000ull,2053,32},{0x5510,0x400000000ull,8209,34}};
    for(const Case &c:table) {
        Adreno830Status status{};
        A830SmmuHostPlatform host;
        model.boot(77,0x2,!annibale());
        model.at(0x28)=c.idr2;
        A830Smmu smmu;
        CHECK(smmu.init(recordedConfig(host,&status))==kIOReturnSuccess);
        CHECK(smmu.survey(nullptr)==kIOReturnSuccess && smmu.releaseCacheLock()==kIOReturnSuccess);
        CHECK(smmu.clearGlobalFault()==kIOReturnSuccess && smmu.resetBank(0)==kIOReturnSuccess);
        const NativeAdrenoSmmuProgrammer *programmer=smmu.programmer();
        CHECK(programmer && programmer->id2==c.idr2);
        NativeAdrenoGpuContext gpu;
        NativeAdrenoGmuContextReport report;
        NativeAdrenoGpuContextConfig config{programmer,0,1,smmu.gpuStream().slot,smmu.lpacStream().slot,0,1,0,16,0,true};
        CHECK(gpu.bringUp(config,nullptr,&report)==kIOReturnSuccess && gpu.ready());
        CHECK(report.table_pages==c.pages && gpu.apertureBytes()==c.aperture && gpu.inputBits()==c.input_bits);
        CHECK(model.at(A830SmmuModel::cb(0)+0x30)==(c.input_bits==34?tcrOf0(16):tcrOf(0)) && model.aborts==0 && model.violations==0);
        CHECK(smmu.releaseAll()==kIOReturnSuccess);
    }
    ++cases;
}

int main()
{
    CHECK(adreno830_profile()==A830ProfileAnnibale);   /* the default */
    unsigned perProfile[A830ProfileCount]={};
    for(uint32_t profile:{uint32_t(A830ProfileDesign),uint32_t(A830ProfileAnnibale)}) {
        adreno830_profile_value=profile;
        const unsigned before=cases;
        surveyCases();
        globalAndResetCases();
        takeOverCases(4);
        takeOverCases(2);
        takeOverCases(16);
        wideFallbackCases();
        vmSyncBudget();
        vmMapSyncCases();
        stormAndPollCases();
        tlbCases();
        readbackCases();
        restoreCases();
        sequenceCases();
        perProfile[profile]=cases-before;
    }
    mmioCases();
    std::printf("PASS: A830Smmu over the hypervisor-emulated SMMU model: %u cases, %u per profile (design, annibale) and the MMIO "
        "binding (survey and slot choice, sACR/sGFSR record-only vs never stored, bank reset before 710/712 vs not attached, "
        "CB2/CB0/LPAC take-over at 4, 2 and 16 GiB with each profile's exact context stores, 16 GiB on a 32-bit UBS falls back to "
        "4 GiB (a 32-bit IAS does not), VM TLB stores as 64-bit TLBIVAL/32-bit "
        "TLBIASID vs TLBIASID+TLBSYNC only, a VM map with no TLB store vs TLBIASID+TLBSYNC (sync_after_map), faults never "
        "poison the VM, the ring's fences on an interrupt's fault (none on a polled one), the aperture outcome on the S3 page, storm "
        "guard vs polling, TLB sync budget, restore after CX collapse)\n",cases,perProfile[A830ProfileDesign]);
    return perProfile[A830ProfileDesign]==perProfile[A830ProfileAnnibale]?0:1;
}
