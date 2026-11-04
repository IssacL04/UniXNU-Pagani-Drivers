/* SPDX-License-Identifier: MIT */
/* The A830 orchestrator (ARCHITECTURE 3.4, IMPLEMENTATION_PLAN 2.1 test_start): S0..S10 over link-level doubles of every
 * module the stages call (the modules have their own host tests), with the real Adreno830Status, NativeAdrenoFirmware and
 * NativeAdrenoSha256. Checked: the stage order and the module calls of each stage; pagani-gpu-stop at every stage; one
 * injected failure per module call of every stage fails that stage once with the module's step and reason, nothing is
 * retried (except the S5 cold boot: at most 3 attempts, a forced power-off between them, none after a host-resource request
 * or the GX check), nothing is published, and a failure after S3 puts the claimed streams back to FAULT; residual sGFSR/FSR
 * and CP pointer values decide nothing; the CX timer is seeded once, first in S4, and never by a recovery restart; S8 runs
 * V9b/V9c only with pagani-gpu-cohtest=1 and stops with reason 18 on their timeout; S9 never leaves half a service; S10 with
 * pagani-gpu-dcvs=0 fixes pagani-gpu-perf; the power/recovery operations re-run the stage bodies. Plus the S0 board checks
 * and the boot-argument parser. The bring-up profile: the design-era cases above run pagani-gpu-profile=0; the annibale
 * profile (the boot default) gets its own full run, stops, failure matrix and power hooks: the profile set in S0 before the
 * vote tables (p1 on row 0), the checked S1 AHB timeouts (again after a CX power-up), the 160 MHz first vote, OOB_GPU never
 * cleared, S8 without 332 on polled fences, the polled retire, the S4 DDR read-back failure; pagani-gpu-profile=2 and any
 * pagani-gpu-hwsched are refused in S0 (reason 24 for hwsched). Those counts print on a line of their own.
 * host-test: Adreno830Start.cpp Adreno830Status.cpp NativeAdrenoFirmware.cpp NativeAdrenoSha256.cpp */
/* The doubles define every out-of-line member of some module classes, so clang sees whole classes here and would flag the
 * private fields only the real implementations use. */
#pragma clang diagnostic ignored "-Wunused-private-field"
#include "XnuMemoryFixture.h"
#include "Adreno830Start.h"
#include "Adreno830Status.h"
#include "A830Regs.h"
#include "A830Votes.h"
#include "A830Smmu.h"
#include "A830GmuFirmware.h"
#include "A830Gmu.h"
#include "A830Hfi.h"
#include "A830Cp.h"
#include "A830Zap.h"
#include "A830Irq.h"
#include "A830Fence.h"
#include "A830SelfTest.h"
#include "A830PowerState.h"
#include "A830Recovery.h"
#include "A830Dcvs.h"
#include "NativeAdrenoABI.h"
#include "NativeAdrenoGmuContext.h"
#include "NativeAdrenoGmuMemory.h"
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoPublish.h"
#include "NativeAdrenoRing.h"
#include "NativeAdrenoStatus.h"
#include "NativeAdrenoSubmit.h"
#include "NativeAdrenoTimer.h"
#include "NativeAdrenoSha256.h"
#include "Sm8750AossQmp.h"
#include "Sm8750Llcc.h"
#include "Sm8750Tsens.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n  status: %s\n",__LINE__,#x,adreno830_status.rows[0]);\
    std::exit(1);}}while(0)

/* ---- The script every double follows ------------------------------------------------------------------------------ */
struct Injected {IOReturn result;uint16_t step,reason;uint32_t times;};   /* times 0: every call */
static std::vector<std::string> trace;               /* every module call, in order */
static std::map<std::string,unsigned> counts;
static std::map<std::string,Injected> faults;
static uint64_t sgfsrResidue,fsrResidue;              /* survey residue that must decide nothing */
static A830SelfTestEnv lastEnv;
static A830DcvsConfig lastDcvs;
static A830DcvsOps lastDcvsOps;
static A830PowerOps powerOps;
static A830PowerConfig powerConfig;                   /* what bindPower handed A830PowerState::init (the screen rule) */
static A830RecoveryOps recoveryOps;
static A830GmuHooks gmuHooks;
static bool powerStarted;
static uint8_t recoveredCause;
/* What the doubles saw of the bring-up profile: the profile when S0 built the vote tables, the first vote (HFI M9) and the
 * level the power state machine started at. */
static uint32_t profileAtVotes=0xff;
static uint8_t startLevel,startBus,powerLevel,powerBus;
static bool loadMismatch;
/* The wiring of the annibale open items: the CP health hooks S6 installs, what a late HFI drain finds (per drain), whether a
 * failed HFI start drains itself, the GMU's cold-boot facts of the S5 page and a fatal bit taken after a boot. */
static A830CpHealth cpHealth;
static A830HfiStats lateFound;
static bool lateOnStart,gmuFatalAfterBoot;
static uint32_t bootGeneral11,bootRsc=0x00010000,bootTcm=139;
static unsigned sgfsrStores;

static IOReturn hit(const char *name,uint16_t *step=nullptr,uint16_t *reason=nullptr)
{
    trace.emplace_back(name);
    const unsigned n=++counts[name];
    auto at=faults.find(name);
    if(at==faults.end() || (at->second.times && n>at->second.times))return kIOReturnSuccess;
    if(step)*step=at->second.step;
    if(reason)*reason=at->second.reason;
    return at->second.result;
}
static unsigned called(const char *name) {auto at=counts.find(name);return at==counts.end()?0:at->second;}
/* Register traffic for tests/test_a830trace.cpp, which builds this file with main renamed: when a hook is set, the double
 * of that module call first makes its accesses through the table the orchestrator handed it (a working GMU io over the
 * windows, the DRV-2 and power io, the SMMU io, the SCM). Null here: test_start checks module calls and touches no register
 * through them. */
struct IoScript {
    void (*qmp)(Sm8750AossQmp *qmp,int val);                                  /* Sm8750AossQmpSendAcdState */
    void (*rails)(const Sm8750RpmhIo *drv2);                                  /* Sm8750RpmhHostRails */
    void (*cn0)(const Sm8750RpmhIo *drv2);                                    /* Sm8750RpmhHostCn0 */
    void (*ddr)(const Sm8750RpmhIo *drv2,uint32_t kbps);                      /* Sm8750RpmhHostDdr */
    void (*power)(const Sm8750GpuPowerIo *io,Sm8750GpuPowerTrace *trace,bool on);   /* Sm8750GpuPowerOn/Off */
    void (*scm)(Sm8750Scm *scm,uint32_t function,uint64_t argument);          /* the SCM entry points of S2 and the aperture */
    void (*smmu)(const ArmSmmu4KIo *io,const char *call,unsigned bank);       /* the A830Smmu and context doubles of S3 */
    void (*ahb)(A830CxMisc &cx);                                              /* a830CxAhbTimeout */
    void (*seed)(A830CxMisc &cx);                                             /* a830CxTimerSeed (not yet seeded) */
    void (*gmuBind)(A830GmuMmio *mmio,A830GmuIo *io);                         /* A830GmuBindMmio: a working io */
};
static IoScript ioScript;
static const ArmSmmu4KIo *smmuIoSeen;   /* the io A830Smmu::init got, for the context doubles */
static long position(const char *name)
{
    for(size_t i=0;i<trace.size();++i)if(trace[i]==name)return long(i);
    return -1;
}

/* ---- Host kernel services --------------------------------------------------------------------------------------- */
static uint64_t clockUs;
static std::map<uint64_t,std::vector<uint8_t>> windows;
static uint64_t hostNow() {clockUs+=100;return clockUs;}
static void hostDelay(uint32_t us) {clockUs+=us;}
static volatile void *hostMap(uint64_t phys,uint64_t bytes,uint32_t)
{
    auto &memory=windows[phys];
    if(memory.size()<bytes)memory.assign(size_t(bytes),0);
    return memory.data();
}
static void hostUnmap(volatile void *,uint64_t) {}
static int hostSmc(void *,uint64_t regs[8]) {regs[0]=0;return 0;}
static int mutexes;
static void *hostMutex() {return &mutexes;}
static void hostLock(void *) {}
static void hostUnlock(void *) {}
static void hostWait(const void *,uint32_t us) {clockUs+=us;}
static void hostWake(const void *) {}
static IOReturn hostThread(void (*)(void *),void *) {return kIOReturnSuccess;}
/* Thread calls: each allocation is a record of its own and never runs by itself; the one entered last is kept for a test to run. */
struct HostCall {void (*function)(void *);void *argument;};
static std::deque<HostCall> hostCalls;
static HostCall *hostEntered;
static void *hostCall(void (*function)(void *),void *argument) {hostCalls.push_back({function,argument});return &hostCalls.back();}
static void hostEnter(void *call) {hostEntered=static_cast<HostCall *>(call);}
static IOReturn hostAttach(uint32_t,bool edge,void (*)(void *),void *,uint32_t *readback,uint64_t *router)
{
    if(readback)*readback=edge?A830IrqReadbackEdge:A830IrqReadbackLevel;
    if(router)*router=0;
    return kIOReturnSuccess;
}
static IOReturn hostDisable(uint32_t) {return kIOReturnSuccess;}
static uint64_t hostCount(uint32_t) {return 0;}
static uint64_t hostCounter() {return clockUs*192/10;}
static uintptr_t hostIrqOff() {return 0;}
static void hostIrqOn(uintptr_t) {}
static IOReturn metalPrepare(Adreno830MetalReport *report)
{
    *report={};report->step=4;report->parent_bytes=136;report->instance_bytes=136;
    const IOReturn r=hit("metal_prepare");
    if(r)report->step=1;
    return r;
}
static IOReturn metalCommit(Adreno830MetalReport *report)
{
    const IOReturn r=hit("metal_commit");
    if(!r)report->step=5;
    return r;
}
static void metalDiscard() {hit("metal_discard");}
static const Adreno830Os hostOs={hostNow,hostDelay,hostDelay,hostMap,hostUnmap,hostSmc,hostMutex,hostLock,hostUnlock,hostWait,
    hostWake,hostThread,hostCall,hostEnter,hostAttach,hostDisable,hostDisable,hostCount,hostCounter,hostIrqOff,hostIrqOn,
    metalPrepare,metalCommit,metalDiscard};
/* The device repository's board check (scripts/test-gpu-inputs.py) builds this file with main renamed and runs the real board
 * glue through these doubles. */
const Adreno830Os *test_start_host_os(void);
const Adreno830Os *test_start_host_os(void) {return &hostOs;}

/* ---- Overlay hooks (BootProgress and the publish shell) ------------------------------------------------------------ */
static std::vector<std::vector<std::string>> overlay;
static std::vector<std::string> history;
extern "C" void boot_progress_gpu_rows(const char (*rows)[Adreno830StatusRowBytes],unsigned count)
{
    std::vector<std::string> copy;
    for(unsigned i=0;i<count;++i)copy.emplace_back(rows[i]);
    overlay.push_back(copy);
}
extern "C" void boot_progress_display_status(const char *detail,uint64_t) {history.emplace_back(detail);}
extern "C" void native_adreno_publish_status(const Adreno830Status *) {}
extern "C" unsigned native_adreno_report_rows(char (*)[Adreno830StatusRowBytes]) {return 0;}

/* ---- Platform layer doubles (C) --------------------------------------------------------------------------------- */
extern "C" {
int Sm8750CmdDbCopy(const Sm8750RpmhIo *,Sm8750CmdDb *out)
{
    if(IOReturn r=hit("Sm8750CmdDbCopy"))return int(r)>0?-5:int(r);
    out->bytes=Sm8750CmdDbBytes;
    return Sm8750RpmhOk;
}
int Sm8750RpmhResourcesParse(const Sm8750CmdDb *,Sm8750RpmhResources *out)
{
    *out={};
    if(hit("Sm8750RpmhResourcesParse")) {out->failed=Sm8750RpmhResAcv;return Sm8750RpmhNotFound;}
    out->gfx.present=out->mx.present=out->cx.present=1;out->gfx.count=16;out->mx.count=out->cx.count=15;
    out->sh0.present=out->mc0.present=out->acv.present=out->cn0.present=1;
    return Sm8750RpmhOk;
}
Sm8750SocInfoStatus Sm8750SocInfoRead(const Sm8750SocInfoIo *,Sm8750SocInfo *info)
{
    *info={};
    if(hit("Sm8750SocInfoRead")) {info->last_offset=0x1ff000;return Sm8750SocInfoNoTable;}
    info->id=counts["soc639"]?639:618;info->fmt=0x10013;info->feature_code=0;info->soc_code=0;
    return Sm8750SocInfoOk;
}
Sm8750SocInfoStatus Sm8750SpeedBinRead(const Sm8750SocInfoIo *,uint32_t,unsigned,unsigned,uint32_t *bin)
{
    if(hit("Sm8750SpeedBinRead"))return Sm8750SocInfoIoError;
    *bin=0xbe;
    return Sm8750SocInfoOk;
}
Sm8750SocInfoStatus Sm8750GpuLevelsSelect(const Sm8750GpuBin *bins,unsigned count,uint32_t speed_bin,uint32_t,int fused,
                                          Sm8750GpuLevelTable *out)
{
    *out={};
    if(hit("Sm8750GpuLevelsSelect"))return Sm8750SocInfoArgument;
    CHECK(count==6 && bins[0].count==11);
    out->count=11;out->initial=1;out->bin=speed_bin==0xbe?0:-1;out->fused=uint8_t(fused);
    for(unsigned i=0;i<11;++i)out->levels[i]=bins[0].levels[i];
    return Sm8750SocInfoOk;
}
void Sm8750RpmhHostInit(Sm8750RpmhHost *host,const Sm8750RpmhIo *drv2,const Sm8750RpmhResources *res)
{
    *host={};host->drv2=drv2;host->res=res;
    hit("Sm8750RpmhHostInit");
}
int Sm8750RpmhHostRails(Sm8750RpmhHost *host,int highest)
{
    if(ioScript.rails)ioScript.rails(host->drv2);
    if(hit("Sm8750RpmhHostRails"))return Sm8750RpmhTimeout;
    host->held|=Sm8750RpmhHeldRails;host->last.tcs=0;host->last.status=1;host->last.command[0]=host->last.command[1]=0x10100;
    counts[highest?"rails-top":"rails-lowsvs"]++;
    return Sm8750RpmhOk;
}
int Sm8750RpmhHostCn0(Sm8750RpmhHost *host)
{
    if(ioScript.cn0)ioScript.cn0(host->drv2);
    if(hit("Sm8750RpmhHostCn0"))return Sm8750RpmhTimeout;
    host->held|=Sm8750RpmhHeldCn0;
    return Sm8750RpmhOk;
}
static std::vector<uint32_t> ddrVotes;
int Sm8750RpmhHostDdr(Sm8750RpmhHost *host,uint32_t kbps)
{
    if(ioScript.ddr)ioScript.ddr(host->drv2,kbps);
    if(hit("Sm8750RpmhHostDdr"))return Sm8750RpmhTimeout;
    ddrVotes.push_back(kbps);host->held|=Sm8750RpmhHeldDdr;host->ddr_kbps=kbps;
    return Sm8750RpmhOk;
}
Sm8750GpuPowerStatus Sm8750GpuPowerOn(const Sm8750GpuPowerIo *io,Sm8750GpuPowerTrace *trace)
{
    if(ioScript.power)ioScript.power(io,trace,true);
    if(hit("Sm8750GpuPowerOn")) {
        trace->step=Sm8750GpuStepPll0;trace->block=Sm8750GpuGpuCc;trace->offset=0;trace->value=0x2;
        return Sm8750GpuPowerTimeout;
    }
    trace->step=Sm8750GpuStepDone;trace->block=Sm8750GpuGpuCc;trace->offset=0x93e8;trace->value=0x80000000;
    return Sm8750GpuPowerOk;
}
Sm8750GpuPowerStatus Sm8750GpuPowerOff(const Sm8750GpuPowerIo *io,Sm8750GpuPowerTrace *trace)
{
    if(ioScript.power)ioScript.power(io,trace,false);
    trace->warnings=0;
    return hit("Sm8750GpuPowerOff")?Sm8750GpuPowerIoError:Sm8750GpuPowerOk;
}
void Sm8750ScmInit(Sm8750Scm *scm,const Sm8750ScmOps *ops) {*scm={};scm->ops=ops;hit("Sm8750ScmInit");}
void Sm8750ScmSetIrqAttached(Sm8750Scm *scm,int attached) {scm->irq_attached=uint32_t(attached);}
Sm8750ScmStatus Sm8750ScmWaitqInfo(Sm8750Scm *scm,uint32_t *queues,uint32_t *intid,Sm8750SmcResult *)
{
    if(ioScript.scm)ioScript.scm(scm,SM8750_SCM_WAITQ_GET_INFO,0);
    *queues=2;*intid=994;
    return hit("Sm8750ScmWaitqInfo")?Sm8750ScmError:Sm8750ScmOk;
}
Sm8750ScmStatus Sm8750ScmProbeGpu(Sm8750Scm *scm,uint32_t *mask)
{
    if(ioScript.scm)
        for(const uint32_t function:{SM8750_SCM_GPU_INIT_REGS,SM8750_SCM_CP_SMMU_APERTURE,SM8750_SCM_PAS_IS_SUPPORTED})
            ioScript.scm(scm,SM8750_SCM_IS_CALL_AVAIL,function);
    hit("Sm8750ScmProbeGpu");
    *mask=scm->available=7|Sm8750ScmProbed;
    return Sm8750ScmOk;
}
Sm8750ScmStatus Sm8750ScmGpuInitRegs(Sm8750Scm *scm,uint32_t gpu_req,Sm8750SmcResult *out)
{
    CHECK(gpu_req==0x9);
    if(ioScript.scm)ioScript.scm(scm,SM8750_SCM_GPU_INIT_REGS,gpu_req);
    *out={};out->calls=1;
    if(hit("Sm8750ScmGpuInitRegs")) {out->x0=-1;return Sm8750ScmError;}
    return Sm8750ScmOk;
}
Sm8750ScmStatus Sm8750ScmSetGpuSmmuAperture(Sm8750Scm *scm,uint32_t bank,Sm8750SmcResult *out)
{
    if(ioScript.scm)ioScript.scm(scm,SM8750_SCM_CP_SMMU_APERTURE,bank);
    *out={};
    return hit("Sm8750ScmSetGpuSmmuAperture")?Sm8750ScmError:Sm8750ScmOk;
}
void Sm8750ScmWaitqIrq(void *) {}
void Sm8750AossQmpInit(Sm8750AossQmp *qmp,const Sm8750AossQmpIo *msgram,uint32_t bytes,const Sm8750AossQmpIo *ipcc)
{
    *qmp={};qmp->msgram=msgram;qmp->ipcc=ipcc;qmp->msgram_bytes=bytes;
}
int Sm8750AossQmpSendAcdState(Sm8750AossQmp *qmp,int val)
{
    if(ioScript.qmp)ioScript.qmp(qmp,val);
    counts[val?"qmp-acd-on":"qmp-acd-off"]++;
    return hit("Sm8750AossQmpSendAcdState")?Sm8750AossQmpTimeout:Sm8750AossQmpOk;
}
unsigned Sm8750AossQmpStatusCode(int rc) {return rc==0?1:rc==Sm8750AossQmpTimeout?2:3;}
Sm8750LlccStatus Sm8750LlccProbe(const Sm8750LlccIo *,Sm8750Llcc *llcc)
{
    *llcc={};
    return hit("Sm8750LlccProbe")?Sm8750LlccUnsupported:Sm8750LlccOk;
}
Sm8750LlccStatus Sm8750LlccEnableGpu(const Sm8750LlccIo *,Sm8750Llcc *llcc)
{
    if(hit("Sm8750LlccEnableGpu"))return Sm8750LlccTimeout;
    llcc->active=1ull<<9|1ull<<11;
    return Sm8750LlccOk;
}
int Sm8750LlccGpuScid(const Sm8750Llcc *llcc) {return llcc->active&(1ull<<9)?9:-1;}
}

/* ---- Driver module doubles (C++) -------------------------------------------------------------------------------- */
unsigned A830Votes::boardBins(const Adreno830Board &board,Sm8750GpuBin bins[Adreno830BinCount])
{
    hit("A830Votes::boardBins");
    for(unsigned i=0;i<Adreno830BinCount;++i) {
        bins[i]={};bins[i].count=board.level_count[i];bins[i].levels=reinterpret_cast<const Sm8750GpuLevel *>(board.levels[i]);
    }
    return Adreno830BinCount;
}
IOReturn A830Votes::build(const Sm8750RpmhResources &,const Adreno830Board &,const Sm8750GpuLevelTable &levels,uint32_t options,
                          A830VoteTables *out)
{
    *out={};
    profileAtVotes=adreno830_profile();   /* S0 sets the profile before any module may read it */
    if(IOReturn r=hit("A830Votes::build")) {out->error_step=A830VotesStepGx;out->error_value=3;out->error_resource=0x34;return r;}
    out->levels=levels.count;out->initial=uint8_t(levels.initial+1);out->options=options;out->ddr_levels=12;
    for(unsigned i=0;i<levels.count;++i) {
        const Sm8750GpuLevel &l=levels.levels[i];
        out->level[i]={l.khz,l.vlvl,l.bus_min,l.bus,l.bus_max,0,l.acd};
    }
    out->acd_levels=8;out->acd_mask=0x0ff0;
    counts[options&A830VotesLinuxDependency?"votes-linux":"votes-kgsl"]++;
    if(options&A830VotesFullTable)counts["votes-full"]++;   /* pagani-gpu-maxlevel: no annibale narrowing */
    return kIOReturnSuccess;
}
uint8_t A830Votes::levelAtOrAbove(const A830VoteTables &t,uint32_t mhz)
{
    for(unsigned i=0;i<t.levels;++i)if(t.level[i].khz>=mhz*1000)return uint8_t(i+1);
    return 0;
}
uint8_t A830Votes::levelAtOrBelow(const A830VoteTables &t,uint32_t mhz)
{
    uint8_t found=0;
    for(unsigned i=0;i<t.levels;++i)if(t.level[i].khz<=mhz*1000)found=uint8_t(i+1);
    return found;
}
uint64_t A830Votes::statusAux(const Sm8750RpmhResources &,uint64_t aux[6])
{
    aux[1]=0xff0f0f10;aux[2]=1000|uint64_t(16)<<32;aux[3]=1000|uint64_t(4)<<32;aux[4]=0x0200;
    return A830S0Arc|A830S0Bcm;
}

IOReturn A830GmuFirmware::parse(const uint8_t *image,size_t bytes,A830GmuBlocks *out)
{
    *out={};
    if(IOReturn r=hit("A830GmuFirmware::parse"))return r;
    out->image=image;out->bytes=bytes;out->count=15;out->metadata=11;out->data=4;out->requests=11;
    out->block[11]={0,0x1ec,0,0,0,A830GmuTargetItcm};out->block[12]={0x4000,0x15824,0,0,0,A830GmuTargetDdr};
    out->block[13]={0x10007f98,0x28,0,0,0,A830GmuTargetDtcm};out->block[14]={0x10007fdc,0x18,0,0,0,A830GmuTargetDtcm};
    return kIOReturnSuccess;
}
IOReturn A830GmuFirmware::plan(const A830GmuBlocks &,size_t table_bytes,NativeAdrenoGmuPlan *out)
{
    *out={};out->count=7;out->cache_bytes=0x76000;out->table_bytes=table_bytes;
    return hit("A830GmuFirmware::plan");
}
IOReturn A830GmuFirmware::load(const A830GmuBlocks &,A830GmuIo &,NativeAdrenoGmuMemory &,unsigned what,A830GmuLoadCheck *check)
{
    counts[what==A830GmuLoadDdr?"load-ddr":"load-other"]++;
    const IOReturn r=hit("A830GmuFirmware::load");
    /* loadMismatch: what the annibale profile's DDR comparison reports for a word that differed. */
    if(r && check && loadMismatch) {*check={};check->mismatch=true;check->addr=0x60001234;check->read=0x11;check->expect=0x22;}
    return r;
}

/* NativeAdrenoMemory and the GMU memory: host buffers. */
IOReturn NativeAdrenoMemory::allocate(size_t bytes,unsigned,bool,size_t,bool)
{
    if(IOReturn r=hit("NativeAdrenoMemory::allocate"))return r;
    mapping_=std::calloc(1,bytes);bytes_=bytes;pooled_=true;
    static NativeAdrenoPhysicalSpan span{0,0x9a0000000ull,0x1000};
    spans_=&span;span_count_=1;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::release()
{
    hit("NativeAdrenoMemory::release");
    std::free(mapping_);mapping_=nullptr;bytes_=0;pooled_=false;spans_=nullptr;span_count_=0;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoMaintainOwnedPages(void *,size_t,NativeAdrenoCacheOperation) {return kIOReturnSuccess;}
static uint32_t lastKopt=0xdead;   /* what S0 stored (pagani-gpu-kopt) */
void NativeAdrenoMemory::setKopt(uint32_t bits) {lastKopt=bits;}
/* The MEM row's pool usage: no pool is reserved in this double, so nothing is in use. */
void NativeAdrenoMemory::usage(uint32_t *pool_mib,uint32_t *used_mib,uint32_t *imports)
{
    if(pool_mib)*pool_mib=0;
    if(used_mib)*used_mib=0;
    if(imports)*imports=0;
}
IOReturn NativeAdrenoMemory::reservePoolBestEffort(unsigned bits,uint32_t mib,uint32_t min_mib,bool write_combined,uint32_t *reserved)
{
    CHECK(bits==36 && mib==512 && min_mib==64 && write_combined);
    *reserved=mib;
    return hit("NativeAdrenoMemory::reservePoolBestEffort");
}
IOReturn NativeAdrenoGmuMemory::allocate(const NativeAdrenoGmuPlan &plan,unsigned bits)
{
    if(IOReturn r=hit("NativeAdrenoGmuMemory::allocate"))return r;
    CHECK(bits==36 && plan.count==7);
    plan_=plan;plan_.region[0]={NativeAdrenoGmuHfiIova,NativeAdrenoGmuHfiBytes,NativeAdrenoGmuRegionHfi,11};
    region_[0].allocate(NativeAdrenoGmuHfiBytes,bits,true,NativeAdrenoGmuHfiBytes);
    layout_.cache_bytes=plan.cache_bytes;layout_.hfi_iova=NativeAdrenoGmuHfiIova;
    ready_=true;
    return kIOReturnSuccess;
}
int NativeAdrenoGmuMemory::indexOf(uint32_t kind) const {return kind==NativeAdrenoGmuRegionHfi && ready_?0:-1;}
IOReturn NativeAdrenoGmuMemory::clear(uint32_t) {hit("NativeAdrenoGmuMemory::clear");return kIOReturnSuccess;}

/* SMMU: a register model for the few reads the orchestrator makes itself (the LPAC route, CB FSR/FAR). */
static uint32_t smmuRegs[0x40000/4];
static int smmuRd(void *,size_t offset,uint32_t *value) {*value=smmuRegs[offset/4];return 0;}
static int smmuWr(void *,size_t offset,uint32_t value)
{
    if(offset==A830_SMMU_GR0_SGFSR)++sgfsrStores;   /* the annibale CP health hook must never write sGFSR back */
    smmuRegs[offset/4]=value;
    return 0;
}
static int smmuRd64(void *,size_t,uint64_t *value) {*value=0;return 0;}
static int smmuWr64(void *,size_t,uint64_t) {return 0;}
static int smmuBarrier(void *) {return 0;}
IOReturn A830SmmuBindMmio(A830SmmuMmio *mmio,ArmSmmu4KIo *io)
{
    CHECK(mmio->base && mmio->bytes==0x40000);
    *io={};io->register_bytes=0x40000;io->read32=smmuRd;io->write32=smmuWr;io->read64=smmuRd64;io->write64=smmuWr64;
    io->barrier=smmuBarrier;
    return hit("A830SmmuBindMmio");
}
static void smmuFail(A830SmmuFailure &failure,const char *name,IOReturn *r)
{
    uint16_t step=0,reason=0;
    *r=hit(name,&step,&reason);
    if(*r)failure={step,reason,0x3da0000ull+step,0x55,0x66};
}
static A830SmmuConfig smmuConfigSeen;   /* the configuration S3 gave A830Smmu::init: its platform hooks */
IOReturn A830Smmu::init(const A830SmmuConfig &config)
{
    CHECK(config.actlr && config.cb_gmu==2 && config.cb_gpu==0 && config.status==&adreno830_status);
    config_=config;smmuIoSeen=config.io;smmuConfigSeen=config;
    return hit("A830Smmu::init");
}
IOReturn A830Smmu::survey(A830SmmuSurvey *out)
{
    if(ioScript.smmu)ioScript.smmu(config_.io,"survey",0);
    IOReturn r;smmuFail(failure_,"A830Smmu::survey",&r);
    survey_={};survey_.idr0=0x4c017e18;survey_.idr1=0x40000015;survey_.idr2=0x5511;survey_.scr0=0x002e0406;
    survey_.sgfsr=uint32_t(sgfsrResidue);survey_.global_pages=32;survey_.output_bits=36;
    survey_.slot_gpu=config_.slot_gpu;survey_.slot_lpac=config_.slot_lpac;survey_.slot_gmu=config_.slot_gmu;
    survey_.cb_fsr[0]=uint32_t(fsrResidue);survey_.cb_fsr[2]=uint32_t(fsrResidue);
    if(out)*out=survey_;
    smmuRegs[A830_SMMU_GR0_S2CR(config_.slot_lpac)/4]=0x300;   /* the LPAC route as S3 leaves it */
    return r;
}
IOReturn A830Smmu::releaseCacheLock()
{
    if(ioScript.smmu)ioScript.smmu(config_.io,"releaseCacheLock",0);
    return hit("A830Smmu::releaseCacheLock");
}
IOReturn A830Smmu::clearGlobalFault()
{
    if(ioScript.smmu)ioScript.smmu(config_.io,"clearGlobalFault",0);
    return hit("A830Smmu::clearGlobalFault");
}
IOReturn A830Smmu::resetBank(unsigned cb)
{
    if(ioScript.smmu)ioScript.smmu(config_.io,"resetBank",cb);
    IOReturn r;smmuFail(failure_,cb==2?"A830Smmu::resetBank(2)":"A830Smmu::resetBank(0)",&r);
    return r;
}
unsigned A830Smmu::attachIrqs() {hit("A830Smmu::attachIrqs");return 2;}
IOReturn A830Smmu::grantAperture(unsigned cb)
{
    aperture_state_=0;aperture_x0_=0;
    if(ioScript.smmu && config_.platform.aperture) {   /* the real one asks TZ through the orchestrator's SCM, and records */
        int64_t x0=0;
        const int status=config_.platform.aperture(config_.platform.cookie,cb,&x0);
        aperture_x0_=x0;aperture_state_=!status && !x0?1:2;
    }
    hit("A830Smmu::grantAperture");
    return kIOReturnSuccess;
}
IOReturn A830Smmu::releaseAll() {return hit("A830Smmu::releaseAll");}
IOReturn A830Smmu::restore() {return hit("A830Smmu::restore");}
void A830Smmu::poll() {hit("A830Smmu::poll");}
/* The fault ring as a test fills it: drained in order into the bottom half's sink. */
static std::vector<A830SmmuFault> faultsToDrain;
unsigned A830Smmu::drainFaults(void (*sink)(void *,const A830SmmuFault &),void *context)
{
    const unsigned drained=unsigned(faultsToDrain.size());
    for(const A830SmmuFault &fault:faultsToDrain)sink(context,fault);
    faultsToDrain.clear();
    return drained;
}
const NativeAdrenoSmmuProgrammer *A830Smmu::programmer() {return &programmer_;}
void A830Smmu::fillStatus(Adreno830Status *status) const
{
    status->aux[0]=survey_.idr0|uint64_t(survey_.idr1)<<32;status->aux[1]=survey_.idr2|uint64_t(survey_.scr0)<<32;
    status->aux[2]=uint64_t(survey_.sgfsr)<<32;
    status->aux[3]=0x300|uint64_t(survey_.slot_gpu|survey_.slot_lpac<<8|survey_.slot_gmu<<16)<<32;
    status->aux[4]=0x167|uint64_t(0x303)<<32;status->aux[5]=0x167|uint64_t(0x303)<<32;
    adreno830_status_sticky(A830StickySmmuGfsr,survey_.sgfsr);
    adreno830_status_sticky(A830StickyCb0Fsr,survey_.cb_fsr[0]);
}
IOReturn NativeAdrenoGmuContext::bringUp(const NativeAdrenoGmuContextConfig &config,void (*)(uint32_t),
                                         NativeAdrenoGmuContextReport *report)
{
    CHECK(config.bank==2 && config.asid==3 && config.slot==1 && config.sid==5 && config.map && config.programmer);
    if(ioScript.smmu)ioScript.smmu(smmuIoSeen,"context",config.bank);
    *report={};report->step=Ready;
    uint16_t step=0,reason=0;
    if(IOReturn r=hit("NativeAdrenoGmuContext::bringUp",&step,&reason)) {report->step=step;return r;}
    ready_=true;used_=true;
    return config.map(config.map_cookie,&vm_);
}
static uint8_t buffers[GpuBufferCount][0x1c000];
/* pagani-gpu-va=16 as the real context answers on an SMMU without a 34-bit IAS or the 32 MiB of tables: the 4 GiB aperture. */
static bool wideFallback;
IOReturn NativeAdrenoGpuContext::bringUp(const NativeAdrenoGpuContextConfig &config,void (*)(uint32_t),
                                         NativeAdrenoGmuContextReport *report)
{
    CHECK(config.bank==0 && config.asid==1 && config.slot==0 && config.lpac_slot==3 && config.sid==0 && config.lpac_sid==1);
    CHECK((config.aperture_gib==4 || config.aperture_gib==2 || config.aperture_gib==16) && config.pool_mib==512 &&
          config.write_combined_pool);
    if(ioScript.smmu)ioScript.smmu(smmuIoSeen,"context",config.bank);
    *report={};report->step=Ready;
    uint16_t step=0,reason=0;
    if(IOReturn r=hit("NativeAdrenoGpuContext::bringUp",&step,&reason)) {report->step=step;return r;}
    ready_=true;used_=true;output_bits_=36;pool_mib_=config.pool_mib;
    aperture_=uint64_t(config.aperture_gib==16 && wideFallback?4:config.aperture_gib)<<30;
    report->table_pages=aperture_>GpuApertureBytes?uint32_t(TablePages16G):aperture_<GpuApertureBytes?uint32_t(TablePages2G):
        uint32_t(TablePages);
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoGpuContext::buffers(NativeAdrenoGpuBuffer out[GpuBufferCount]) const
{
    const uint64_t iova[GpuBufferCount]={GpuSqeIova,GpuAqeIova,GpuRingIova,GpuMemptrsIova,GpuPwrupIova,GpuScratchIova};
    const uint64_t bytes[GpuBufferCount]={GpuSqeBytes,GpuAqeBytes,GpuRingBytes,GpuPageBytes,GpuPageBytes,GpuPageBytes};
    for(unsigned i=0;i<GpuBufferCount;++i)out[i]={0x9b0000000ull+i*0x100000ull,bytes[i],iova[i],::buffers[i]};
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::map(NativeAdrenoMemory *,unsigned,uint64_t,unsigned,unsigned,unsigned,uint64_t *handle)
{
    *handle=1;
    return hit("NativeAdrenoVm::map");
}
IOReturn NativeAdrenoVm::unmap(uint64_t) {return kIOReturnSuccess;}
void NativeAdrenoVm::setCoherence(unsigned level) {counts[level==2?"coherence-2":level==1?"coherence-1":"coherence-0"]++;}
IOReturn NativeAdrenoVm::setTableWalkCoherent(bool coherent) {coherent_walk_=coherent;return kIOReturnSuccess;}

/* GMU, CX timer, HFI. */
IOReturn A830GmuBindMmio(A830GmuMmio *mmio,A830GmuIo *io)
{
    CHECK(mmio->base[A830GmuWindowGmu] && mmio->base[A830GmuWindowCxMisc] && mmio->base[A830GmuWindowGpu]);
    *io={};
    if(ioScript.gmuBind)ioScript.gmuBind(mmio,io);
    return kIOReturnSuccess;
}
void A830GmuDefaultConfig(A830GmuConfig *config) {*config={};config->chip_id=0x08000100;}
int A830GmuGxVote(void *,int) {return 0;}
IOReturn a830CxTimerSeed(A830CxMisc &cx)
{
    CHECK(cx.io && cx.os && cx.os->counter && cx.os->irq_disable);
    if(cx.seeded) {counts["timer-noop"]++;return kIOReturnSuccess;}
    if(ioScript.seed)ioScript.seed(cx);
    if(IOReturn r=hit("a830CxTimerSeed"))return r;
    cx.seeded=true;cx.seed=0x1234;cx.skew=7;++cx.seeds;
    return kIOReturnSuccess;
}
void a830CxTimerLost(A830CxMisc &cx) {hit("a830CxTimerLost");cx.seeded=false;}
IOReturn a830CxAhbTimeout(A830CxMisc &cx,uint32_t value)
{
    CHECK(cx.io && value==0x1B12);
    if(ioScript.ahb)ioScript.ahb(cx);
    const IOReturn r=hit("a830CxAhbTimeout");
    if(r) {cx.ahb_reg=0x3d9e048;cx.ahb_read=0xffffffff;}   /* the third register did not answer */
    return r;
}
IOReturn A830Gmu::bind(const A830GmuIo &io,const A830GmuOs &os,const A830GmuConfig &config,const A830GmuBlocks *firmware,
                       NativeAdrenoGmuMemory *memory,const A830GmuHooks *hooks)
{
    CHECK(firmware && memory && hooks && hooks->reset_queues && hooks->lock && hooks->drain_debug && hooks->fault && hooks->gx_vote);
    CHECK(config.hfi_irq==336 && config.gmu_irq==337 && os.sleep_us && os.counter);
    io_=io;os_=os;config_=config;firmware_=firmware;memory_=memory;hooks_=*hooks;gmuHooks=*hooks;
    bound_=true;
    return hit("A830Gmu::bind");
}
static uint16_t gmuStep(const char *name,A830GmuFailure &failure,IOReturn *r)
{
    uint16_t step=0,reason=0;
    *r=hit(name,&step,&reason);
    if(*r)failure={step,reason,0x3d7e070,0,0x100};
    return step;
}
IOReturn A830Gmu::coldBoot(uint32_t *init_result)
{
    IOReturn r;gmuStep("A830Gmu::coldBoot",failure_,&r);
    if(hooks_.reset_queues)hooks_.reset_queues(hooks_.cookie);
    report_.boot_attempts++;
    if(r)return r;
    *init_result=0x100;report_.init_result=0x100;report_.core=0x50010019;report_.hfi=0x20070001;report_.init_us=37000;
    if(adreno830_annibale()) {   /* what the annibale cold boot records: B3 RSC status, the B4 read-back, GENERAL_11 */
        report_.rsc_status=bootRsc;report_.tcm_verified=bootTcm;report_.ddr_verified=0x15824;report_.general11=bootGeneral11;
    }
    booted_=true;
    if(gmuFatalAfterBoot)__atomic_store_n(&fatal_,1u,__ATOMIC_RELEASE);   /* a top half or a poll took a fatal bit since */
    return kIOReturnSuccess;
}
IOReturn A830Gmu::forceOff() {hit("A830Gmu::forceOff");booted_=false;return kIOReturnSuccess;}
IOReturn A830Gmu::oobSet(uint32_t *ack_us,uint32_t)
{
    IOReturn r;gmuStep("A830Gmu::oobSet",failure_,&r);
    if(!r) {*ack_us=150;report_.oob_us=150;++oob_refs_[0];}
    return r;
}
void A830Gmu::oobClear(uint32_t) {hit("A830Gmu::oobClear");if(oob_refs_[0])--oob_refs_[0];}
IOReturn A830Gmu::waitGxOn(uint32_t timeout_us)
{
    CHECK(timeout_us==1000);
    IOReturn r;gmuStep("A830Gmu::waitGxOn",failure_,&r);
    return r;
}
IOReturn A830Gmu::writeCounters() {IOReturn r;gmuStep("A830Gmu::writeCounters",failure_,&r);return r;}
IOReturn A830Gmu::attachInterrupts(const A830GmuIrqOps &ops)
{
    CHECK(ops.attach && ops.work_allocate && ops.work_kick);
    irq_wired_=3;
    return hit("A830Gmu::attachInterrupts");
}
IOReturn A830Gmu::reenableInterrupts() {return hit("A830Gmu::reenableInterrupts");}
IOReturn A830Gmu::rsccSleep() {return hit("A830Gmu::rsccSleep");}
IOReturn A830Gmu::pollFatal() {return hit("A830Gmu::pollFatal");}
void A830Gmu::gmuWork() {}
void A830Hfi::setIo(const A830HfiIo &io) {CHECK(io.read && io.write && io.sleep && io.now && io.lock && io.unlock);io_=io;}
IOReturn A830Hfi::init(volatile uint32_t *shared,uint64_t iova,A830HfiLayout layout)
{
    CHECK(shared && iova==NativeAdrenoGmuHfiIova);
    layout_=layout;ready_=true;started_=false;
    counts[layout==A830HfiLayoutKgsl?"hfi-kgsl":"hfi-linux"]++;
    return hit("A830Hfi::init");
}
IOReturn A830Hfi::resetQueues() {hit("A830Hfi::resetQueues");return kIOReturnSuccess;}
void A830Hfi::stop() {hit("A830Hfi::stop");started_=false;}
IOReturn A830Hfi::start(const A830VoteTables &tables,const A830HfiStartOptions &options)
{
    CHECK(tables.levels==11 && options.level>=1 && options.level<=11 && options.hfi_version==0x20070001);
    startLevel=options.level;startBus=options.bus;
    counts[options.flags&A830HfiStartDeferStart?"start-deferred":"start-before-oob"]++;
    if(options.flags&A830HfiStartAcd)counts["start-acd"]++;
    uint16_t step=0,reason=0;
    const IOReturn r=hit("A830Hfi::start",&step,&reason);
    if(r) {
        failure_={};failure_.step=step;failure_.reason=reason;failure_.ior=r;failure_.hdr=0x0010310F;
        first_ack_=counts["first-ack"]>0;
        /* annibale: a command that reached the queue and failed drains the late messages itself (A830Hfi::transact). */
        if(lateOnStart && adreno830_annibale()) {
            ++stats_.late_drains;stats_.late_messages+=lateFound.late_messages;stats_.late_acks+=lateFound.late_acks;
            stats_.late_errors+=lateFound.late_errors;
        }
        return r;
    }
    for(auto &status:status_)status=0;
    status_[A830HfiM8Start]=options.flags&A830HfiStartDeferStart?A830HfiStatusNone:0;
    first_ack_=true;started_=true;level_=options.level;bus_=options.bus;
    return kIOReturnSuccess;
}
IOReturn A830Hfi::sendStart() {status_[A830HfiM8Start]=0;return hit("A830Hfi::sendStart");}
IOReturn A830Hfi::send(uint8_t,const uint32_t *,uint8_t,A830HfiAck *) {return hit("A830Hfi::send");}
IOReturn A830Hfi::perfVote(uint8_t level,uint8_t bus) {level_=level;bus_=bus;return hit("A830Hfi::perfVote");}
IOReturn A830Hfi::perfVote(uint8_t level,uint8_t bus,uint32_t) {level_=level;bus_=bus;return hit("A830Hfi::perfVote(ab)");}
IOReturn A830Hfi::drainDebug() {return hit("A830Hfi::drainDebug");}
/* The late drain: the design profile drains nothing (the real one returns at once); annibale finds lateFound. */
uint32_t A830Hfi::drainLate(uint32_t microseconds)
{
    CHECK(microseconds==A830HfiLateDrainUs);
    hit("A830Hfi::drainLate");
    if(!adreno830_annibale())return 0;
    ++stats_.late_drains;stats_.late_messages+=lateFound.late_messages;stats_.late_acks+=lateFound.late_acks;
    stats_.late_errors+=lateFound.late_errors;
    return lateFound.late_messages;
}

/* Ring, CP, ZAP. */
IOReturn NativeAdrenoRing::attach(const NativeAdrenoRingIo *io,const NativeAdrenoRingCache *,void *ring,size_t ring_bytes,
                                  uint64_t ring_iova,void *fence,size_t,uint64_t fence_iova,const NativeAdrenoRingRegs *)
{
    CHECK(io && io->fence_status && ring && ring_bytes==GpuRingBytes && ring_iova==GpuRingIova && fence_iova==GpuMemptrsIova);
    if(IOReturn r=hit("NativeAdrenoRing::attach"))return r;
    ring_=static_cast<volatile uint32_t *>(ring);fence_=static_cast<volatile uint32_t *>(fence);attached_=true;
    return kIOReturnSuccess;
}
void NativeAdrenoRing::setWaiter(const NativeAdrenoRingWaiter *waiter)
{
    CHECK(waiter && waiter->wait && waiter->stamp==A830Fence::ringStamp);   /* the queue meter stamps every fenced submission */
    hit("NativeAdrenoRing::setWaiter");
}
IOReturn NativeAdrenoRing::recover(uint32_t *last) {hit("NativeAdrenoRing::recover");*last=seqno_;return kIOReturnSuccess;}
IOReturn NativeAdrenoRing::submit(const uint32_t *,size_t,uint32_t *fence) {*fence=++seqno_;return kIOReturnSuccess;}
A830HwInitConfig A830HwInitDefaults() {A830HwInitConfig c={};c.ubwc_mode=5;c.highest_bank_bit=16;c.min_access_length=32;return c;}
static A830HwInitConfig lastHwInit;
static void cpFail(A830CpFailure &failure,const char *name,IOReturn *r)
{
    uint16_t step=0,reason=0;
    *r=hit(name,&step,&reason);
    if(*r)failure={step,reason?reason:uint16_t(A830ReasonCpFault),0x3d00000ull+4u*0x806u,9,10};
}
IOReturn A830Cp::attach(const A830CpIo *io,NativeAdrenoRing *ring,const A830CpSmmu *smmu)
{
    CHECK(io && io->lock && ring && smmu && smmu->lpacRoute && smmu->recordSticky);
    io_=*io;smmu_=*smmu;ring_=ring;attached_=true;
    return hit("A830Cp::attach");
}
void A830Cp::setHealth(const A830CpHealth *health)
{
    CHECK(attached_ && health && health->global_fault && health->gmu_fault);
    health_=*health;cpHealth=*health;
    hit("A830Cp::setHealth");
}
IOReturn A830Cp::hwInit(const A830HwInitConfig &config) {lastHwInit=config;IOReturn r;cpFail(failure_,"A830Cp::hwInit",&r);return r;}
IOReturn A830Cp::loadMicrocode(uint64_t sqe,uint64_t aqe)
{
    CHECK(sqe==0x100000 && aqe==0x180000);
    IOReturn r;cpFail(failure_,"A830Cp::loadMicrocode",&r);return r;
}
IOReturn A830Cp::startRing(uint64_t ring,uint64_t bytes,uint64_t memptrs)
{
    CHECK(ring==0x200000 && bytes==0x8000 && memptrs==0x300000);
    IOReturn r;cpFail(failure_,"A830Cp::startRing",&r);return r;
}
IOReturn A830Cp::startCp(uint64_t pwrup,uint32_t *fence)
{
    CHECK(pwrup==0x301000);
    uint32_t route=0;
    CHECK(smmu_.lpacRoute(smmu_.cookie,&route)==kIOReturnSuccess && route==0x300);   /* the S6 gate the orchestrator supplies */
    smmu_.recordSticky(smmu_.cookie);
    IOReturn r;cpFail(failure_,"A830Cp::startCp",&r);
    *fence=10;
    return r;
}
IOReturn A830Cp::stageMicrocode(const uint8_t *fw,uint64_t fw_bytes,void *dst,size_t dst_bytes,size_t *copied)
{
    CHECK(fw && dst && fw_bytes>4 && fw_bytes-4<=dst_bytes);
    *copied=size_t(fw_bytes-4);
    return hit("A830Cp::stageMicrocode");
}
void A830Cp::dumpStall(Adreno830Status *status)
{
    hit("A830Cp::dumpStall");
    if(status) {status->aux[0]=10|uint64_t(10)<<32;status->aux[2]=0xe3|uint64_t(0x4a)<<32;}
}
IOReturn A830Cp::finishSecure(void *pwrup_page,uint32_t timeout_us)
{
    CHECK(timeout_us==2000000);
    counts[pwrup_page?"finish-with-pwrup":"finish-resume"]++;
    IOReturn r;cpFail(failure_,"A830Cp::finishSecure",&r);return r;
}
IOReturn A830Cp::readPipe(uint8_t,uint32_t,uint32_t *value) {*value=0;return kIOReturnSuccess;}
IOReturn A830Zap::init(Sm8750Scm *scm,const A830ZapMemoryOps *) {CHECK(scm && scm->ops);scm_=scm;return hit("A830Zap::init");}
IOReturn A830Zap::load(const uint8_t *image,size_t length,uint64_t region,uint64_t bytes)
{
    CHECK(image && length==12088 && region==0x9b09a000 && bytes==0x2000);
    uint16_t step=0,reason=0;
    if(IOReturn r=hit("A830Zap::load",&step,&reason)) {
        failed_reason_=reason;failed_reg_=0x42000205;failed_value_=uint64_t(int64_t(-1));report_.step=step;
        return r;
    }
    loaded_=true;report_.step=A830ZapStepRelease;report_.loads=1;
    return kIOReturnSuccess;
}
void A830Zap::failure(A830ZapFailure *out) const {*out={report_.step,failed_reason_,failed_reg_,failed_value_,0};}
void A830Zap::noteStatus() const {adreno830_status.aux[4]=0;adreno830_status.aux[5]=uint64_t(report_.step)<<32;}
IOReturn A830Zap::resume(A830ZapResume mode) {CHECK(mode==A830ZapResumeSecureMode);return hit("A830Zap::resume");}

/* Interrupts, fences, self-test, timer. */
A830Mmio a830MmioDirect(volatile void *base) {A830Mmio m={};m.cookie=const_cast<void *>(base);return m;}
void a830Wakeup(void *) {}
/* 332 as A830SelfTest::run attaches it (design profile); the annibale profile calls it only with pagani-gpu-irq=1 (S8
 * attaches first, keeps it only when the fence interrupt works). An injected refusal leaves the line off, unattached. */
IOReturn A830IrqGuard::attach(const A830IrqPlatform *,uint32_t intid,bool edge,uint32_t,uint32_t,void (*)(void *),void *,
                              void (*)(void *),void *,bool)
{
    intid_=intid;edge_=edge;attached_=true;
    return kIOReturnSuccess;
}
void A830IrqGuard::trip()
{
    __atomic_store_n(&off_,true,__ATOMIC_RELEASE);
    adreno830_status_irq_off(A830IrqOffGpu);   /* the only guard these doubles trip is 332's */
    hit("A830IrqGuard::trip");
}
static uint32_t irqOwnMask=0xdead;   /* A830IrqConfig::own_mask as S8 handed it over */
static A830Irq *irqSeen;             /* the device's A830Irq (S8 started it): attached(), off(), polled() */
IOReturn A830Irq::attach(uint32_t intid)
{
    CHECK(intid==332);
    if(IOReturn r=hit("A830Irq::attach")) {gpu_guard_.trip();return r;}
    return gpu_guard_.attach(nullptr,intid,false,0,0,nullptr,nullptr,nullptr,nullptr,false);
}
void A830Irq::fallBack() {__atomic_store_n(&polled_,true,__ATOMIC_RELEASE);gpu_guard_.trip();hit("A830Irq::fallBack");}
IOReturn A830Irq::start(const A830IrqConfig &config)
{
    CHECK(config.gpu.cookie && config.gmu.cookie && config.fence && config.hooks.deviceLost && config.hooks.retire);
    config_=config;irqOwnMask=config.own_mask;irqSeen=this;
    return hit("A830Irq::start");
}
IOReturn A830Irq::attachCxHost(uint32_t intid) {CHECK(intid==112);hit("A830Irq::attachCxHost");return kIOReturnSuccess;}
IOReturn A830Irq::quiesce(bool) {return hit("A830Irq::quiesce");}
IOReturn A830Irq::restore() {return hit("A830Irq::restore");}
IOReturn A830IrqGuard::resume() {return kIOReturnSuccess;}
IOReturn A830IrqGuard::suspend() {return kIOReturnSuccess;}
IOReturn A830Fence::start(const A830FenceConfig &config)
{
    CHECK(config.memptrs && config.irq);
    config_=config;started_=true;
    return hit("A830Fence::start");
}
IOReturn A830Fence::ringWaiter(void *,uint32_t,uint32_t,uint32_t *) {return kIOReturnSuccess;}
/* The queue meter (ARCHITECTURE 4.13.5): the ring's stamp hook, the recovery's skip and the DCVS thread's take. */
void A830Fence::ringStamp(void *,uint32_t,uint32_t) {}
static uint32_t queueSkippedTo;
static Adreno830StatusQueue queueToTake;
void A830Fence::queueSkip(uint32_t seq) {queueSkippedTo=seq;hit("A830Fence::queueSkip");}
void A830Fence::takeQueue(Adreno830StatusQueue *out) {hit("A830Fence::takeQueue");*out=queueToTake;}
A830SelfTest::A830SelfTest(const A830SelfTestEnv &env):env_(env) {lastEnv=env;}
A830SelfTestReport a830_selftest_last;
IOReturn A830SelfTest::run(uint32_t gpu_intid,A830SelfTestReport *report,A830SelfTestFailure *failure)
{
    CHECK(gpu_intid==332 && env_.irq && env_.fence && env_.ring && env_.submit && env_.scratch && env_.scratch_iova==0x302000);
    CHECK(env_.vm.allocate && env_.vm.map && env_.vm.policy && env_.recover && env_.ao_counter);
    CHECK(!adreno830_annibale());   /* run() attaches 332: the design profile's S8 only */
    if(!env_.irq->attached())env_.irq->attach(gpu_intid);
    *report={};report->readback=0xa003;report->scratch=0x08750830;
    report->coherence.v4=report->coherence.v9a=A830CohPass;
    report->coherence.v9b=report->coherence.v9c=env_.cohtest?A830CohPass:A830CohNotRun;
    report->coherence.limit=report->coherence.level=env_.cohtest?2:1;
    uint16_t step=0,reason=0;
    if(IOReturn r=hit(env_.cohtest?"A830SelfTest::run(cohtest)":"A830SelfTest::run",&step,&reason)) {
        *failure={step,reason,0x302000,0,0x08750830};
        return r;
    }
    /* What run() does with a VM: one probe page through the orchestrator's VM hooks, then the policy. */
    A830SelfTestPage page={};
    CHECK(env_.vm.allocate(env_.vm.cookie,&page)==kIOReturnSuccess && page.cpu);
    CHECK(env_.vm.map(env_.vm.cookie,&page,A830SelfTestMapCoherentRo)==kIOReturnSuccess && page.iova==0x311000);
    CHECK(env_.vm.unmap(env_.vm.cookie,&page)==kIOReturnSuccess);
    env_.vm.release(env_.vm.cookie,&page);
    env_.vm.policy(env_.vm.cookie,&report->coherence);
    return kIOReturnSuccess;
}
/* The pieces the annibale profile's S8 calls instead of run(): 332 is attached only with pagani-gpu-irq=1. */
IOReturn A830SelfTest::memWrite(uint64_t iova,volatile uint32_t *cpu,uint32_t value,A830SelfTestReport *report,
                                A830SelfTestFailure *failure)
{
    CHECK(adreno830_annibale() && env_.irq && (!env_.irq->attached() || adreno830_board()->knobs.irq) && env_.fence &&
          env_.submit);
    counts[env_.irq->attached() && !env_.irq->off()?"memwrite-irq":"memwrite-polled"]++;
    CHECK(iova==0x302000 && cpu==env_.scratch && value==A830SelfTestScratchValue && report && failure);
    report->scratch=value;report->fence=1;report->fence_irqs=0;report->irq_count=0;
    uint16_t step=0,reason=0;
    const IOReturn r=hit("A830SelfTest::memWrite",&step,&reason);
    if(r)*failure={step,reason,iova,0,value};
    return r;
}
IOReturn A830SelfTest::coherence(bool cohtest,A830CoherenceResult *result,A830SelfTestFailure *failure,bool *recovered)
{
    CHECK(result && failure && cohtest==env_.cohtest);
    if(recovered)*recovered=false;
    *result={};result->v4=result->v9a=A830CohPass;
    result->v9b=result->v9c=cohtest?A830CohPass:A830CohNotRun;
    result->limit=result->level=cohtest?2:1;
    uint16_t step=0,reason=0;
    if(IOReturn r=hit(cohtest?"A830SelfTest::coherence(cohtest)":"A830SelfTest::coherence",&step,&reason)) {
        *failure={step,reason,0x302000,0,0};
        return r;
    }
    env_.vm.policy(env_.vm.cookie,result);
    return kIOReturnSuccess;
}
void A830SelfTest::timestamps(uint64_t *cp,uint64_t *ao) {hit("A830SelfTest::timestamps");*cp=7;*ao=9;}
void A830SelfTest::record(const A830SelfTestReport &report)
{
    hit("A830SelfTest::record");
    adreno830_status.aux[5]=report.scratch|uint64_t(report.fence)<<32;
    adreno830_status.coh_limit=report.coherence.limit;
}
IOReturn NativeAdrenoTimer::attach(const NativeAdrenoTimeIo *io,uint32_t source)
{
    CHECK(io && io->read32 && source==NativeAdrenoTimeGmu);
    io_=*io;attached_=true;
    return hit("NativeAdrenoTimer::attach");
}
IOReturn NativeAdrenoTimer::read(uint64_t *ticks) const {*ticks=0x100000003ull;return kIOReturnSuccess;}
uint32_t NativeAdrenoTimer::gmuLowOffset() {return 0x47100;}

/* Publication, power state, recovery, DCVS. */
IOReturn NativeAdrenoSubmit::attach(NativeAdrenoVm *vm,NativeAdrenoRing *ring) {CHECK(vm && ring);return hit("NativeAdrenoSubmit::attach");}
static NativeAdrenoAbiDeviceInfo publishedInfo;
static uint32_t publishedOutputBits,publishedInputBits;
extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *vm,NativeAdrenoSubmit *submit,NativeAdrenoRing *ring,
    const NativeAdrenoRingClock *clock,const NativeAdrenoTimer *timer,const NativeAdrenoAbiDeviceInfo *info,uint32_t input_bits,
    uint32_t output_bits)
{
    /* The input size follows the aperture the context built: 34 bits for the 16 GiB one, else 32. */
    CHECK(vm && submit && ring && clock && clock->now_us && timer && info &&
          input_bits==(info->va_start+info->va_size>0x100000000ull?34u:32u));
    publishedInfo=*info;publishedOutputBits=output_bits;publishedInputBits=input_bits;
    return hit("native_adreno_publish");
}
extern "C" void native_adreno_device_lost(uint32_t reason) {CHECK(reason==A830ReasonPublish);hit("native_adreno_device_lost");}
extern "C" void native_adreno_recovery_begin(bool charge) {hit(charge?"native_adreno_recovery_begin(charge)":"native_adreno_recovery_begin");}
extern "C" void native_adreno_recovery_end(bool recovered) {hit(recovered?"native_adreno_recovery_end(ok)":"native_adreno_recovery_end(lost)");}
extern "C" void native_adreno_retire_completed(void) {hit("native_adreno_retire_completed");}
/* The CB0 faults the publish shell got: far, completed, issued and sampled as passed. */
static std::vector<A830SmmuFault> faultsNoted;
/* The persistent log: off, as without BootProgress, until a test turns it on. test_a830trace.cpp, which includes this file without running its main, has its own. */
static int logOn;
static std::vector<std::string> logLines;
#ifndef main
extern "C" int boot_progress_trace_enabled(void) {return logOn;}
extern "C" void boot_progress_trace(const char *text) {logLines.push_back(text);}
#endif
extern "C" bool native_adreno_note_page_fault(uint64_t far,uint32_t completed,uint32_t issued,bool sampled)
{
    A830SmmuFault noted={};
    noted.far=far;noted.completed=completed;noted.issued=issued;noted.sampled=sampled;
    faultsNoted.push_back(noted);
    return true;
}
extern "C" uint32_t native_adreno_coherent_imports(void) {return 1;}
IOReturn A830PowerState::init(const A830PowerConfig &config,const A830PowerIo &io,const A830PowerEnv &env,const A830PowerOps &ops)
{
    CHECK(config.slumber==A830SlumberOff && !config.ifpc && io.read32 && env.lock && env.startThread);
    CHECK(ops.cxPowerOn && ops.cxPowerOff && ops.smmuRestore && ops.cxTimerSeed && ops.gmuStart && ops.gmuForceOff &&
          ops.rsccSleep && ops.oobSet && ops.oobClear && ops.hfiSend && ops.cpStart && ops.secureResume && ops.irqEnable &&
          ops.lastSubmitted && ops.retired);
    powerOps=ops;powerConfig=config;
    return hit("A830PowerState::init");
}
void A830PowerState::start(uint8_t level,uint8_t bus)
{
    powerLevel=level;powerBus=bus;powerStarted=true;
    hit("A830PowerState::start");
}
void A830PowerState::setFaultSink(void (*sink)(void *,uint8_t,uint64_t),void *) {CHECK(sink);}
/* The GMU AO counter read (timeRead) holds the GPU awake through the power state, which the S0-S10 doubles never start. */
extern "C" A830PowerState *adreno830_power_state(void) {return nullptr;}
IOReturn A830PowerState::acquire() {return kIOReturnSuccess;}
void A830PowerState::release() {}
bool A830PowerState::accessBegin(bool) {return true;}
void A830PowerState::accessEnd(bool) {}
IOReturn A830Recovery::init(const A830RecoveryConfig &config,const A830PowerIo &,const A830PowerEnv &,const A830PowerOps &,
                            const A830RecoveryOps &ops,A830PowerState *power)
{
    CHECK(config.enabled && power && ops.gate && ops.failInflight && ops.fenceSignal && ops.irqRestore && ops.readPipe && ops.snapshot);
    recoveryOps=ops;
    return hit("A830Recovery::init");
}
void A830Recovery::report(uint8_t cause,uint64_t) {recoveredCause=cause;hit("A830Recovery::report");}
IOReturn A830Recovery::recover(uint8_t cause,uint64_t) {recoveredCause=cause;return hit("A830Recovery::recover");}
IOReturn A830Dcvs::init(const A830DcvsConfig &config,const A830DcvsTable &table,const A830PowerIo &,const A830PowerEnv &,
                        const A830DcvsOps &ops,A830PowerState *power,A830Recovery *recovery,const Sm8750TsensIo *)
{
    CHECK(table.levels && table.count==11 && table.ddrLevels==12 && ops.perfVote && ops.hostDdr && power && recovery);
    lastDcvs=config;lastDcvsOps=ops;
    return hit("A830Dcvs::init");
}
IOReturn A830Dcvs::start() {return hit("A830Dcvs::start");}

/* ---- The board ------------------------------------------------------------------------------------------------- */
static std::vector<uint8_t> images[4];
static void pin(Adreno830Firmware &firmware,const std::vector<uint8_t> &bytes)
{
    firmware.bytes=bytes.data();firmware.length=bytes.size();
    NativeAdrenoSha256::hash(bytes.data(),bytes.size(),firmware.sha256);
}
static const Adreno830Level common[11]={
    {160000,0x32,2,2,2,0,0},{222000,0x34,2,2,2,0,0},{342000,0x38,2,3,3,0,0},{389000,0x3c,2,4,6,0,0xc8285ffd},
    {443000,0x40,2,4,6,0,0xc02e5ffd},{525000,0x50,2,4,6,0,0xc02e5ffd},{607000,0x80,4,6,8,0,0xa82e5ffd},
    {660000,0x90,4,6,8,0,0x882c5ffd},{734000,0xc0,6,8,10,0,0xa82a5ffd},{832000,0xe0,7,10,10,0,0x882a5ffd},
    {900000,0x100,7,10,10,0,0x882a5ffd}};
static const uint32_t ddr[12]={0,781250,2136718,5285156,6074218,6671875,8171875,10687500,12449218,14398437,16500000,18597656};
/* The 13T board of ARCHITECTURE 3.2 / 3.5 (all six bins carry the common table here; S0 only checks their shape). The
 * checks written for the design run the design profile (pagani-gpu-profile=0); the annibale ones (the boot default) ask
 * for it. */
static Adreno830Board board13t(uint32_t profile=A830ProfileDesign)
{
    Adreno830Board b={};
    b.magic=Adreno830BoardMagic;b.version=Adreno830BoardVersion;
    b.gpu={0x3d00000,0x40000};b.rscc={0x3d50000,0x10000};b.cx_dbgc={0x3d61000,0x3000};b.cx_misc={0x3d9e000,0x2000};
    b.gmu={0x3d37000,0x68000};b.gmu_ao={0x3d40000,0x10000};b.gpucc={0x3d90000,0x9800};b.gcc={0x100000,0x1f4200};
    b.smmu={0x3da0000,0x40000};b.cmd_db={0x81c60000,0x20000};b.smem={0x81d00000,0x200000};b.zap={0x9b09a000,0x2000};
    b.qfprom={0x221c8000,0x1000};b.rsc_drv2={0x16520000,0x10000};
    b.aoss_msgram={0xc300000,0x400};b.ipcc={0x406000,0x1000};b.tsens_tm={0xc22a000,0x1ff};
    const uint64_t llcc[6]={0x24800000,0x25800000,0x24c00000,0x25c00000,0x26800000,0x26c00000};
    for(unsigned i=0;i<6;++i)b.llcc[i]={llcc[i],0x200000};
    b.chip_id=0x44050001;b.soc_id=618;
    b.irq_gpu=332;b.irq_cx_host=112;b.irq_hfi=336;b.irq_gmu=337;b.irq_smmu_global=706;
    for(unsigned i=0;i<4;++i)b.irq_smmu_cb[i]=710+i;
    b.irq_scm_waitq=994;
    b.sid_gpu=0;b.sid_lpac=1;b.sid_gmu=5;b.slot_gpu=0;b.slot_lpac=3;b.slot_gmu=1;b.cb_gpu=0;b.cb_gmu=2;
    b.smmu_actlr=0x303;b.pool_mib=512;b.qfprom_speedbin_offset=0x138;b.speedbin_bits=9;
    for(unsigned i=0;i<6;++i) {b.levels[i]=common;b.level_count[i]=11;b.initial_level[i]=9;}
    b.bin_speed[0]=0xbe;b.common_level_count=11;
    b.ddr_kbps=ddr;b.ddr_levels=12;b.cnoc_levels[1]=100;b.gmu_khz[0]=500000;b.gmu_khz[1]=650000;b.gmu_vlvl[0]=0x40;b.gmu_vlvl[1]=0x80;
    b.ubwc_mode=5;b.highest_bank_bit=16;b.min_access_length=32;
    pin(b.gmu_fw,images[0]);pin(b.sqe_fw,images[1]);pin(b.aqe_fw,images[2]);pin(b.zap_fw,images[3]);
    Adreno830KnobDefaults(&b.knobs);
    b.knobs.actlr=b.smmu_actlr;
    b.knobs.profile=profile;
    return b;
}

static IOReturn run(const Adreno830Board *board)
{
    adreno830_start_reset();
    adreno830_set_os(&hostOs);
    overlay.clear();history.clear();trace.clear();counts.clear();ddrVotes.clear();windows.clear();
    std::memset(smmuRegs,0,sizeof smmuRegs);
    powerStarted=false;powerOps={};powerConfig=A830PowerConfig();recoveryOps={};lastEnv={};lastDcvs=A830DcvsConfig();lastDcvsOps={};publishedInfo={};
    recoveredCause=0;profileAtVotes=0xff;startLevel=startBus=powerLevel=powerBus=0;a830_selftest_last={};cpHealth={};
    irqOwnMask=0xdead;irqSeen=nullptr;
    smmuConfigSeen={};faultsToDrain.clear();faultsNoted.clear();hostEntered=nullptr;
    return adreno830_start(board);
}
static bool getter(void *cookie,const char *name,uint32_t *value)
{
    auto *table=static_cast<std::map<std::string,uint32_t> *>(cookie);
    auto at=table->find(name);
    if(at==table->end())return false;
    *value=at->second;return true;
}

/* The module calls of each stage, in order, that a run must make (the injection points of the fault matrix). */
struct Point {const char *name;uint8_t stage;uint16_t step,reason;};
static const Point points[]={
    {"Sm8750CmdDbCopy",A830SInputs,A830S0StepCmdDb,A830ReasonCmdDb},
    {"Sm8750RpmhResourcesParse",A830SInputs,A830S0StepCmdDb,A830ReasonCmdDb},
    {"Sm8750SocInfoRead",A830SInputs,A830S0StepSocInfo,A830ReasonSocInfo},
    {"Sm8750GpuLevelsSelect",A830SInputs,A830S0StepLevelTable,A830ReasonArgument},
    {"A830Votes::build",A830SInputs,A830S0StepVotes,A830ReasonCmdDb},
    {"A830GmuFirmware::parse",A830SInputs,A830S0StepGmuImage,A830ReasonFirmware},
    {"Sm8750RpmhHostRails",A830SPower,A830S1StepRails,A830ReasonRpmh},
    {"Sm8750GpuPowerOn",A830SPower,Sm8750GpuStepPll0,A830ReasonTimeout},
    {"Sm8750ScmGpuInitRegs",A830SSecureCx,A830S2StepInitRegs,A830ReasonSmc},
    {"Sm8750RpmhHostCn0",A830SSecureCx,A830S2StepCn0,A830ReasonRpmh},
    {"Sm8750RpmhHostDdr",A830SSecureCx,A830S2StepDdr,A830ReasonRpmh},
    {"A830SmmuBindMmio",A830SSmmu,A830S3StepIds,A830ReasonArgument},
    {"A830Smmu::init",A830SSmmu,A830S3StepIds,A830ReasonArgument},
    {"A830Smmu::survey",A830SSmmu,A830SmmuStepSlots,A830ReasonSmmuRoute},
    {"A830Smmu::resetBank(2)",A830SSmmu,A830SmmuStepReset|A830SmmuStepGmu,A830ReasonReadback},
    {"A830Smmu::resetBank(0)",A830SSmmu,A830SmmuStepReset|A830SmmuStepGpu,A830ReasonReadback},
    {"NativeAdrenoMemory::reservePoolBestEffort",A830SSmmu,A830S3StepPool,A830ReasonNoMemory},
    {"A830GmuFirmware::plan",A830SSmmu,A830S3StepContext+A830S3Cb2,A830ReasonFirmware},
    {"NativeAdrenoGmuMemory::allocate",A830SSmmu,A830S3StepContext+A830S3Cb2,A830ReasonNoMemory},
    {"NativeAdrenoGmuContext::bringUp",A830SSmmu,A830S3StepContext+A830S3Cb2,A830ReasonSmmuRoute},
    {"NativeAdrenoGpuContext::bringUp",A830SSmmu,A830S3StepContext+A830S3Cb0,A830ReasonSmmuRoute},
    {"a830CxTimerSeed",A830SFirmware,A830S4StepTimer,A830ReasonUnexpectedState},
    {"A830GmuFirmware::load",A830SFirmware,A830S4StepGmuFirmware,A830ReasonReadback},
    {"A830Cp::stageMicrocode",A830SFirmware,A830S4StepSqe,A830ReasonFirmware},
    {"NativeAdrenoRing::attach",A830SFirmware,A830S4StepBuffers,A830ReasonArgument},
    {"A830Gmu::bind",A830SGmu,A830S5StepBind,A830ReasonArgument},
    {"A830Hfi::init",A830SGmu,A830S5StepHfiInit,A830ReasonArgument},
    {"A830Cp::attach",A830SCp,A830S6StepAttach,A830ReasonArgument},
    {"A830Cp::hwInit",A830SCp,A830CpStepNonctxt,A830ReasonCpFault},
    {"A830Cp::loadMicrocode",A830SCp,A830CpStepMicrocode,A830ReasonCpFault},
    {"A830Cp::startRing",A830SCp,A830CpStepRing,A830ReasonCpFault},
    {"A830Cp::startCp",A830SCp,A830CpStepMeInit,A830ReasonTimeout},
    {"A830Zap::init",A830SZap,A830S7StepZapInit,A830ReasonArgument},
    {"A830Zap::load",A830SZap,A830ZapStepAuthReset,A830ReasonSmc},
    {"A830Cp::finishSecure",A830SZap,A830CpStepSecureMode,A830ReasonTimeout},
    {"A830Irq::start",A830SFence,A830S8StepStart,A830ReasonNoMemory},
    {"A830Fence::start",A830SFence,A830S8StepStart,A830ReasonNoMemory},
    {"NativeAdrenoTimer::attach",A830SFence,A830S8StepTimer,A830ReasonArgument},
    {"A830SelfTest::run",A830SFence,A830S8StepAttach,A830ReasonIrqWiring},
    {"NativeAdrenoSubmit::attach",A830SMetal,A830S9StepSubmit,A830ReasonPublish},
    {"metal_prepare",A830SMetal,A830S9StepPrepare+1,A830ReasonPublish},
    {"native_adreno_publish",A830SMetal,A830S9StepPublish,A830ReasonPublish},
    {"metal_commit",A830SMetal,A830S9StepCommit+4,A830ReasonPublish},
    {"A830Dcvs::init",A830SDcvs,A830S10StepDcvs,A830ReasonArgument},
    {"A830Dcvs::start",A830SDcvs,A830S10StepDcvs,A830ReasonHfiStatus},
};

int main()
{
    const size_t sizes[4]={88896,111892,25340,12088};   /* the gen80000 lengths, filled with a counter pattern */
    for(unsigned i=0;i<4;++i) {
        images[i].resize(sizes[i]);
        for(size_t b=0;b<sizes[i];++b)images[i][b]=uint8_t(b*(i+3)+i);
    }
    unsigned stops=0,injected=0,boardFaults=0,knobCases=0,runs=0,hooks=0;
    /* The bring-up profile's own cases, on their own PASS line (the pinned line keeps its design-era counts). */
    unsigned profileRuns=0,profileStops=0,profileInjected=0,profileFaults=0,profileHooks=0,profileKnobs=0;

    /* ---- A full run: every stage in order, Running, published once, the S10 bindings ------------------------------ */
    Adreno830Board b=board13t();
    b.knobs.status=2;
    CHECK(run(&b)==kIOReturnSuccess);
    CHECK(adreno830_status.stage==A830SRunning && adreno830_status.state==A830Done && adreno830_running());
    CHECK(overlay.back()[0].rfind("GPU RUN ",0)==0 && history.empty());
    const char *order[]={"Sm8750CmdDbCopy","Sm8750SocInfoRead","Sm8750SpeedBinRead","Sm8750GpuLevelsSelect","A830Votes::build",
        "A830GmuFirmware::parse","Sm8750RpmhHostRails","Sm8750GpuPowerOn","Sm8750ScmInit","Sm8750ScmGpuInitRegs",
        "Sm8750RpmhHostCn0","Sm8750RpmhHostDdr","A830Smmu::survey","A830Smmu::releaseCacheLock","A830Smmu::clearGlobalFault",
        "A830Smmu::resetBank(2)","A830Smmu::resetBank(0)","A830Smmu::attachIrqs","NativeAdrenoMemory::reservePoolBestEffort",
        "NativeAdrenoGmuMemory::allocate",
        "NativeAdrenoGmuContext::bringUp","NativeAdrenoGpuContext::bringUp","A830Smmu::grantAperture","a830CxTimerSeed",
        "A830GmuFirmware::load","A830Cp::stageMicrocode","NativeAdrenoRing::attach","A830Gmu::bind","A830Hfi::init",
        "A830Gmu::coldBoot","A830Gmu::attachInterrupts","A830Hfi::start","A830Gmu::oobSet","A830Gmu::waitGxOn",
        "A830Gmu::writeCounters","A830Cp::attach","A830Cp::hwInit","A830Cp::loadMicrocode","A830Cp::startRing","A830Cp::startCp",
        "A830Zap::init","A830Zap::load","A830Cp::finishSecure","A830Gmu::oobClear","A830Irq::start","A830Fence::start",
        "NativeAdrenoRing::setWaiter","NativeAdrenoTimer::attach","A830PowerState::init","A830Recovery::init",
        "A830PowerState::start","A830SelfTest::run","A830Irq::attachCxHost","NativeAdrenoSubmit::attach","metal_prepare",
        "native_adreno_publish","metal_commit","A830Dcvs::init","A830Dcvs::start"};
    long last=-1;
    for(const char *name:order) {
        const long at=position(name);
        if(at<=last) {std::fprintf(stderr,"%s at %ld, expected after %ld\n",name,at,last);return 1;}
        last=at;
    }
    /* The once-per-boot rules and the defaults: the CX timer before the GMU firmware; rails at the table top, DDR level 11
     * at S2 and no floor vote (floor = boot level); START before OOB; KGSL dependency votes; LLCC and QMP off by default. */
    CHECK(called("a830CxTimerSeed")==1 && position("a830CxTimerSeed")<position("A830GmuFirmware::load"));
    CHECK(called("rails-top")==1 && ddrVotes.size()==1 && ddrVotes[0]==18597656 && called("start-before-oob")==1 &&
          called("votes-kgsl")==1 && !called("Sm8750LlccProbe") && !called("Sm8750AossQmpSendAcdState"));
    CHECK(called("A830Gmu::coldBoot")==1 && !called("A830Gmu::forceOff") && called("A830Hfi::start")==1);
    CHECK(called("finish-with-pwrup")==1 && called("A830Gmu::oobClear")==1 && called("A830SelfTest::run")==1 &&
          !called("A830SelfTest::run(cohtest)") && !lastEnv.cohtest && called("coherence-1")==1);
    CHECK(publishedInfo.chip_id==0x44050001 && publishedInfo.gmem_bytes==0xc00000 && publishedInfo.gmem_base==0x4000000 &&
          publishedInfo.uche_trap_base==0x1fffffffff000ull && publishedInfo.va_start==0x20000000 &&
          publishedInfo.va_size==0x3e0000000ull && publishedInfo.highest_bank_bit==16 && publishedInfo.ubwc_swizzle==6 &&
          publishedInfo.macrotile_mode==1 && publishedInfo.ubwc_version==5 && publishedInfo.flags==NativeAdrenoDeviceClientIova &&
          publishedOutputBits==36);
    CHECK(lastDcvs.governor && lastDcvs.minMhz==160 && lastDcvs.maxMhz==734 && lastDcvs.fixedLevel==2 && lastDcvs.initialLevel==2 &&
          lastDcvs.initialBus==2 && lastDcvs.hostBootKbps==18597656 && lastDcvs.hostFloorKbps==18597656 && powerStarted);
    CHECK(lastHwInit.ubwc_mode==5 && lastHwInit.highest_bank_bit==16 && lastHwInit.gmem_base==0x4000000 && lastHwInit.llcc_scid==0);
    CHECK(!called("A830Smmu::releaseAll") && !called("metal_discard") && !called("native_adreno_device_lost"));
    CHECK(adreno830_status.stage_us[A830SInputs] && adreno830_status.stage_us[A830SDcvs] && adreno830_board()==&b &&
          adreno830_levels() && adreno830_levels()->count==11 && adreno830_ddr_kbps(11)==18597656 && adreno830_ddr_kbps(12)==0 &&
          adreno830_rpmh_host() && adreno830_scm() && adreno830_power_io() && adreno830_window(A830WinGpu)->base &&
          !adreno830_window(A830WinLlcc0)->base && adreno830_last_failure()->reason==0);
    /* The design profile (pagani-gpu-profile=0): set in S0 before the vote tables and shown as p0; the first vote at the
     * board's initial level 2; no AHB timeouts without pagani-gpu-ahbto; OOB_GPU cleared in S7 (above); 332 attached by the
     * self-test, so the DCVS thread's fault poll leaves the retiring to the fence interrupt. */
    CHECK(adreno830_profile()==A830ProfileDesign && profileAtVotes==A830ProfileDesign && adreno830_status.profile_set==1 &&
          adreno830_status.profile==0 && overlay.back()[0]=="GPU RUN 0M b0 g0/b0 s0 f0 h0 p0");
    CHECK(startLevel==2 && startBus==2 && powerLevel==2 && powerBus==2 && !called("a830CxAhbTimeout") &&
          called("A830Irq::attach")==1 && lastDcvsOps.pollFaults);
    lastDcvsOps.pollFaults(lastDcvsOps.context);
    CHECK(!called("native_adreno_retire_completed"));
    /* The DCVS thread's queue op takes the fence's period and stores it for the IRQ row (pagani-gpu-status=2 here). */
    CHECK(lastDcvsOps.queueRow && overlay.back().size()==4 && overlay.back()[3].find(" | q")==std::string::npos);
    queueToTake={};
    queueToTake.submitted=120;queueToTake.completed=120;queueToTake.avg_us=4200;queueToTake.max_us=37200;queueToTake.slow=3;
    queueToTake.depth=5;
    lastDcvsOps.queueRow(lastDcvsOps.context);
    adreno830_status_publish();
    CHECK(called("A830Fence::takeQueue")==1 && overlay.back()[3]=="IRQ 0 wait 0 to 0 late 0 | q 4/38ms d5 s3");
    queueToTake={};
    /* M14 P0, the CB0 fault path: S3 gives the SMMU a fences hook for the 710 top half that reads the completed fence (memptrs + 4)
     * into its first output and the ring's last issued fence into its second; the bottom half (the call fault_notify enters) passes a
     * CB0 fault's FAR, fences and sampled flag to the publish shell as they are, a polled one without fences, a CB2 one not at all. */
    {
        CHECK(smmuConfigSeen.platform.fences && smmuConfigSeen.platform.fault_notify && lastEnv.ring && lastEnv.submit);
        const NativeAdrenoRing *ring=static_cast<const NativeAdrenoRing *>(lastEnv.ring);
        const uint32_t nop[1]={0};
        uint32_t issued=0,completed=0,sampledIssued=0;
        for(unsigned i=0;i<5;++i)CHECK(lastEnv.submit(lastEnv.ring,nop,1,&issued)==kIOReturnSuccess);
        volatile uint32_t *memptrs=reinterpret_cast<volatile uint32_t *>(buffers[GpuBufferMemptrs]);
        const uint32_t kept=memptrs[1];
        memptrs[1]=issued-2;
        smmuConfigSeen.platform.fences(smmuConfigSeen.platform.cookie,&completed,&sampledIssued);
        CHECK(issued>=5 && ring->lastSubmitted()==issued && completed==issued-2 && sampledIssued==issued);
        memptrs[1]=kept;
        A830SmmuFault cb0={};
        cb0.far=0x20001000;cb0.cb=0;cb0.sampled=1;cb0.completed=7;cb0.issued=9;
        A830SmmuFault cb2=cb0;
        cb2.cb=2;cb2.far=0x60000000;
        A830SmmuFault polled=cb0;
        polled.polled=1;polled.sampled=0;polled.completed=polled.issued=0;polled.far=0x20002000;
        faultsToDrain={cb0,cb2,polled};faultsNoted.clear();hostEntered=nullptr;
        smmuConfigSeen.platform.fault_notify(smmuConfigSeen.platform.cookie);
        CHECK(hostEntered && hostEntered->function);
        hostEntered->function(hostEntered->argument);
        CHECK(faultsToDrain.empty() && faultsNoted.size()==2 && faultsNoted[0].far==0x20001000 && faultsNoted[0].completed==7 &&
              faultsNoted[0].issued==9 && faultsNoted[0].sampled==1 && faultsNoted[1].far==0x20002000 && !faultsNoted[1].sampled &&
              !faultsNoted[1].completed && !faultsNoted[1].issued);
        faultsNoted.clear();
        /* Their log lines (A830SmmuFaultLine): none without pagani-diag and the log, then the first A830SmmuFaultLines of the boot of the GPU's bank
         * and of the others': another bank's faults leave the GPU's lines alone. */
        const auto faultLines=[] {
            unsigned lines=0;
            for(const std::string &line:logLines)lines+=line.rfind("gpu fault cb",0)==0;
            return lines;
        };
        logOn=1;adreno830_status_mode(2);logLines.clear();
        faultsToDrain={cb0};
        smmuConfigSeen.platform.fault_notify(smmuConfigSeen.platform.cookie);
        hostEntered->function(hostEntered->argument);
        CHECK(!faultLines());
        adreno830_status_diag(true);
        faultsToDrain={cb0,cb2,polled};
        smmuConfigSeen.platform.fault_notify(smmuConfigSeen.platform.cookie);
        hostEntered->function(hostEntered->argument);
        CHECK(faultLines()==3);
        faultsToDrain.assign(A830SmmuFaultLines,polled);
        smmuConfigSeen.platform.fault_notify(smmuConfigSeen.platform.cookie);
        hostEntered->function(hostEntered->argument);
        CHECK(faultLines()==A830SmmuFaultLines+1);
        faultsToDrain.assign(A830SmmuFaultLines,cb2);
        smmuConfigSeen.platform.fault_notify(smmuConfigSeen.platform.cookie);
        hostEntered->function(hostEntered->argument);
        CHECK(faultLines()==2*A830SmmuFaultLines);
        adreno830_status_diag(false);adreno830_status_mode(0);logOn=0;logLines.clear();faultsNoted.clear();
    }
    /* A second start in the same boot is refused and changes nothing. */
    const uint32_t seq=adreno830_status.seq;
    CHECK(adreno830_start(&b)==kIOReturnBusy && adreno830_status.seq==seq);
    ++runs;

    /* ---- The power and recovery operations re-run the stage bodies ------------------------------------------------- */
    trace.clear();counts.clear();
    A830GmuStartArgs args={2,2,false,false,false};
    uint16_t step=0;
    CHECK(powerOps.gmuStart(powerOps.context,&args,&step)==kIOReturnSuccess);
    CHECK(called("A830Gmu::coldBoot")==1 && called("A830Hfi::start")==1 && called("A830Gmu::oobSet")==1 &&
          called("A830Gmu::waitGxOn")==1 && !called("load-ddr") && !called("a830CxTimerSeed") && !called("Sm8750RpmhHostDdr"));
    args.cxLost=true;
    CHECK(powerOps.gmuStart(powerOps.context,&args,&step)==kIOReturnSuccess && called("load-ddr")==1);
    powerOps.cxTimerSeed(powerOps.context);   /* only after a CX collapse: lost, then seeded again */
    CHECK(called("a830CxTimerLost")==1 && called("a830CxTimerSeed")==1);
    /* The queue meter skips the jobs the forced fence completes, before the ring forces it. */
    CHECK(powerOps.cpStart(powerOps.context,41,&step)==kIOReturnSuccess && called("A830Fence::queueSkip")==1 &&
          position("A830Fence::queueSkip")<position("NativeAdrenoRing::recover") && queueSkippedTo==41 &&
          called("NativeAdrenoRing::recover")==1 &&
          called("A830Cp::startCp")==1 && reinterpret_cast<volatile uint32_t *>(buffers[GpuBufferMemptrs])[1]==41);
    CHECK(powerOps.secureResume(powerOps.context,&step)==kIOReturnSuccess && called("A830Zap::resume")==1 &&
          called("finish-resume")==1 && called("A830Gmu::oobClear")==1 && !called("A830Zap::load"));
    CHECK(powerOps.gmuForceOff(powerOps.context)==kIOReturnSuccess && called("A830Gmu::forceOff")==1 && called("A830Hfi::stop")==1);
    CHECK(powerOps.cxPowerOn(powerOps.context)==kIOReturnSuccess && powerOps.smmuRestore(powerOps.context)==kIOReturnSuccess &&
          !called("a830CxAhbTimeout"));
    uint32_t warnings=9;
    CHECK(powerOps.cxPowerOff(powerOps.context,&warnings)==kIOReturnSuccess && warnings==0);
    powerOps.irqEnable(powerOps.context,A830PowerIrqGpu|A830PowerIrqHfi,true);
    CHECK(called("A830Irq::restore")==1 && called("A830Gmu::reenableInterrupts")==1);
    faults["A830Gmu::coldBoot"]={kIOReturnTimeout,7,A830ReasonTimeout,0};
    CHECK(powerOps.gmuStart(powerOps.context,&args,&step)==kIOReturnTimeout && step==7 &&
          adreno830_last_failure()->stage==A830SGmu);
    faults.clear();
    /* The gate carries the recovery's cause: every one halts the shell and faults the connections with work in flight, only the
     * hang detector, CP_HW_ERROR and the software watchdog also charge the process whose job the CP stood in. */
    for(uint8_t cause=A830FaultHang;cause<A830FaultCount;++cause) {
        const unsigned charged=called("native_adreno_recovery_begin(charge)"),plain=called("native_adreno_recovery_begin");
        recoveryOps.gate(recoveryOps.context,A830GateRecovering,cause);
        recoveryOps.gate(recoveryOps.context,A830GateOpen,cause);
        const bool client=cause==A830FaultHang || cause==A830FaultCpHwError || cause==A830FaultWatchdog;
        CHECK(called("native_adreno_recovery_begin(charge)")==charged+(client?1u:0u) &&
              called("native_adreno_recovery_begin")==plain+(client?0u:1u));
    }
    recoveryOps.gate(recoveryOps.context,A830GateLost,A830FaultGmu);
    CHECK(called("native_adreno_recovery_begin(charge)")==3 && called("native_adreno_recovery_begin")==A830FaultCount-1u-3u &&
          called("native_adreno_recovery_end(ok)")==A830FaultCount-1u && called("native_adreno_recovery_end(lost)")==1);
    A830HangSnapshot snapshot={};
    recoveryOps.snapshot(recoveryOps.context,&snapshot);
    gmuHooks.fault(gmuHooks.cookie,A830ReasonGmuFault,1u<<15,1);   /* a watchdog bite goes to the recovery */
    CHECK(called("A830Recovery::report")==1 && recoveredCause==A830FaultGmuWatchdog);
    hooks+=12;

    /* ---- The annibale profile (pagani-gpu-profile=1, the boot default; kernel-sequences sections 14 and 15) ------------- */
    b=board13t(A830ProfileAnnibale);b.knobs.status=2;
    CHECK(run(&b)==kIOReturnSuccess && adreno830_running() && adreno830_status.stage==A830SRunning);
    const char *annibaleOrder[]={"Sm8750CmdDbCopy","A830Votes::build","Sm8750RpmhHostRails","Sm8750GpuPowerOn","a830CxAhbTimeout",
        "Sm8750ScmInit","Sm8750ScmGpuInitRegs","A830Smmu::survey","A830Smmu::attachIrqs","a830CxTimerSeed",
        "A830GmuFirmware::load","A830Gmu::bind","A830Gmu::coldBoot","A830Gmu::attachInterrupts","A830Hfi::start",
        "A830Gmu::oobSet","A830Gmu::waitGxOn","A830Cp::startCp","A830Zap::load","A830Cp::finishSecure","A830Irq::start",
        "A830Fence::start","NativeAdrenoTimer::attach","A830PowerState::start","A830SelfTest::memWrite",
        "A830SelfTest::timestamps","A830SelfTest::coherence","A830SelfTest::record","A830Irq::attachCxHost",
        "native_adreno_publish","metal_commit","A830Dcvs::start"};
    last=-1;
    for(const char *name:annibaleOrder) {
        const long at=position(name);
        if(at<=last) {std::fprintf(stderr,"annibale: %s at %ld, expected after %ld\n",name,at,last);return 1;}
        last=at;
    }
    /* S0 set the profile before the vote tables (row 0: p1); S1 wrote the AHB timeouts once; the first vote is the 160 MHz
     * level 1 with its DDR level 2, and the power state and the DCVS start from it; OOB_GPU set once and never cleared;
     * 332 never attached and S8 ran on polled fences; the DCVS ceiling is A830Dcvs's (no explicit maximum here). */
    CHECK(adreno830_profile()==A830ProfileAnnibale && profileAtVotes==A830ProfileAnnibale && adreno830_status.profile==1 &&
          adreno830_status.profile_set==1 && overlay.back()[0]=="GPU RUN 0M b0 g0/b0 s0 f0 h0 p1");
    CHECK(called("a830CxAhbTimeout")==1 && startLevel==1 && startBus==2 && powerLevel==1 && powerBus==2 &&
          lastDcvs.initialLevel==1 && lastDcvs.initialBus==2 && lastDcvs.maxMhz==734 && !lastDcvs.maxGiven);
    CHECK(called("A830Gmu::oobSet")==1 && !called("A830Gmu::oobClear") && !called("A830SelfTest::run") &&
          !called("A830Irq::attach") && called("A830SelfTest::record")==1 && called("coherence-1")==1);
    CHECK(a830_selftest_last.scratch==0x08750830 && a830_selftest_last.fence==1 && a830_selftest_last.cp_counter==7 &&
          a830_selftest_last.coherence.limit==1 && adreno830_status.coh_limit==1 && !lastEnv.cohtest && lastEnv.recover);
    /* Without 332 the DCVS thread's fault poll retires completed jobs. */
    lastDcvsOps.pollFaults(lastDcvsOps.context);
    CHECK(called("native_adreno_retire_completed")==1);
    ++profileRuns;
    /* Its power operations: a GMU start sets OOB_GPU, the resume keeps it; a CX power-up writes the AHB timeouts again
     * after Sm8750GpuPowerOn, read back (a failure fails the power-up). */
    trace.clear();counts.clear();
    A830GmuStartArgs firstArgs={1,2,false,false,false};
    CHECK(powerOps.gmuStart(powerOps.context,&firstArgs,&step)==kIOReturnSuccess && called("A830Gmu::oobSet")==1 &&
          startLevel==1 && startBus==2);
    CHECK(powerOps.secureResume(powerOps.context,&step)==kIOReturnSuccess && called("finish-resume")==1 &&
          !called("A830Gmu::oobClear"));
    CHECK(powerOps.cxPowerOn(powerOps.context)==kIOReturnSuccess && called("a830CxAhbTimeout")==1 &&
          position("a830CxAhbTimeout")>position("Sm8750GpuPowerOn"));
    faults["a830CxAhbTimeout"]={kIOReturnIOError,0,0,0};
    CHECK(powerOps.cxPowerOn(powerOps.context)==kIOReturnIOError && called("a830CxAhbTimeout")==2);
    faults.clear();
    profileHooks+=4;
    /* pagani-gpu-stop at every stage: p1 on row 0 from S0 on; the AHB timeouts from S1, never an OOB_GPU clear. */
    for(uint8_t stage=0;stage<A830StageCount;++stage) {
        b=board13t(A830ProfileAnnibale);b.knobs.stop=stage;b.knobs.status=2;
        CHECK(run(&b)==kIOReturnSuccess && adreno830_status.stage==stage && adreno830_status.state==A830Stopped);
        char want[48];std::snprintf(want,sizeof want,"GPU S%u %s STOP ok ",stage,adreno830_stage_name(stage));
        const std::string &row=overlay.back()[0];
        CHECK(row.rfind(want,0)==0 && row.size()>3 && row.compare(row.size()-3,3," p1")==0 && overlay.back().size()==4);
        CHECK((stage>=A830SPower)==(called("a830CxAhbTimeout")==1) && (stage>=A830SZap)==(called("A830Zap::load")==1) &&
              !called("A830Gmu::oobClear") && !called("A830Irq::attach"));
        ++profileStops;
    }
    /* The design profile writes the AHB timeouts in S1 only with pagani-gpu-ahbto=1 (M13 c), after the power-up. */
    b=board13t();b.knobs.ahbto=1;
    CHECK(run(&b)==kIOReturnSuccess && called("a830CxAhbTimeout")==1 &&
          position("a830CxAhbTimeout")>position("Sm8750GpuPowerOn") && position("a830CxAhbTimeout")<position("Sm8750ScmInit"));
    ++profileRuns;
    /* An explicit pagani-gpu-dcvs-max reaches the DCVS as given (it lifts the annibale 342 MHz ceiling, A830Dcvs). */
    b=board13t(A830ProfileAnnibale);b.flags|=1u<<Adreno830KnobDcvsMax;b.knobs.dcvs_max_mhz=607;
    CHECK(run(&b)==kIOReturnSuccess && lastDcvs.maxGiven && lastDcvs.maxMhz==607);
    ++profileRuns;

    /* ---- pagani-gpu-stop at every stage: stopped there, nothing of the later stages called -------------------------- */
    const char *firstOf[A830StageCount+1]={"Sm8750CmdDbCopy","Sm8750RpmhHostRails","Sm8750ScmInit","A830Smmu::survey",
        "a830CxTimerSeed","A830Gmu::bind","A830Cp::attach","A830Zap::init","A830Irq::start","NativeAdrenoSubmit::attach",
        "A830Dcvs::init",nullptr};
    for(uint8_t stage=0;stage<A830StageCount;++stage) {
        b=board13t();b.knobs.stop=stage;b.knobs.status=2;
        CHECK(run(&b)==kIOReturnSuccess);
        CHECK(adreno830_status.stage==stage && adreno830_status.state==A830Stopped && adreno830_status.reason==A830ReasonStopped);
        CHECK(called(firstOf[stage])>=1 && (!firstOf[stage+1] || !called(firstOf[stage+1])));
        CHECK((stage>=A830SMetal)==(called("native_adreno_publish")==1) && !adreno830_running());
        char want[40];std::snprintf(want,sizeof want,"GPU S%u %s STOP ok ",stage,adreno830_stage_name(stage));
        CHECK(overlay.back()[0].rfind(want,0)==0 && overlay.back().size()==4);
        ++stops;
    }
    /* stop=0 with status=2 keeps the M0 page shape: one publish of four rows. */
    b=board13t();b.knobs.stop=0;b.knobs.status=2;
    const uint32_t seq0=adreno830_status.seq;
    CHECK(run(&b)==kIOReturnSuccess && adreno830_status.seq==seq0+1 && overlay.size()==1 &&
          overlay[0][1]=="SOC 618 FC 0 BIN 190 FW ok" && overlay[0][2]=="ARC gfx 16 mx 15 cx 15 gmxc -");
    ++stops;

    /* ---- One injected failure per module call: that stage fails once, nothing retried or published ------------------ */
    auto inject=[&](const Point &point,uint32_t profile) {
        b=board13t(profile);
        faults.clear();
        faults[point.name]={kIOReturnIOError,point.step,point.reason,0};
        const IOReturn result=run(&b);
        faults.clear();
        if(result==kIOReturnSuccess || adreno830_status.state!=A830Failed || adreno830_status.stage!=point.stage ||
           adreno830_status.step!=point.step || adreno830_status.reason!=point.reason || called(point.name)!=1) {
            std::fprintf(stderr,"%s (profile %u): result %#x state %u stage %u step %#x reason %u calls %u\n",point.name,profile,
                         unsigned(result),adreno830_status.state,adreno830_status.stage,adreno830_status.step,
                         adreno830_status.reason,called(point.name));
            std::exit(1);
        }
        /* After S3 claimed the streams they go back to FAULT on a failure, until S9 published the services. */
        const bool published=!std::strcmp(point.name,"metal_commit") || point.stage==A830SDcvs;
        const bool claimed=(point.stage==A830SSmmu && position("A830Smmu::attachIrqs")>=0) || (point.stage>A830SSmmu && !published);
        CHECK(called("A830Smmu::releaseAll")==(claimed?1u:0u));
        CHECK(called("native_adreno_publish")==(published || !std::strcmp(point.name,"native_adreno_publish")?1u:0u));
        CHECK(!called("A830Dcvs::start") || !std::strcmp(point.name,"A830Dcvs::start"));
        CHECK(history.size()==1 && history[0]==adreno830_stage_title(point.stage) && !adreno830_running());
        /* A failure in S0 comes before the profile is in effect only when validation refused it: here S0 got that far. */
        CHECK(adreno830_status.profile_set==1 && adreno830_status.profile==profile);
    };
    for(const Point &point:points) {inject(point,A830ProfileDesign);++injected;}
    /* The annibale profile: the same calls but A830SelfTest::run (332), plus its own (S1 AHB timeouts, the polled S8). */
    std::vector<Point> annibalePoints;
    for(const Point &point:points)if(std::strcmp(point.name,"A830SelfTest::run"))annibalePoints.push_back(point);
    annibalePoints.push_back({"a830CxAhbTimeout",A830SPower,A830S1StepAhbTimeout,A830ReasonReadback});
    annibalePoints.push_back({"A830SelfTest::memWrite",A830SFence,A830S8StepReadback,A830ReasonUnexpectedState});
    annibalePoints.push_back({"A830SelfTest::coherence",A830SFence,A830S8StepV9a,A830ReasonHang});
    for(const Point &point:annibalePoints) {inject(point,A830ProfileAnnibale);++profileInjected;}
    /* S1: the AHB register that failed and what it read; S4: the DDR word the annibale comparison found different; S8: a
     * CP_MEM_WRITE that only missed its interrupt criteria (count + 1, woken by 332) does not stop the boot; V9c timeout. */
    b=board13t(A830ProfileAnnibale);faults["a830CxAhbTimeout"]={kIOReturnIOError,0,0,0};
    CHECK(run(&b)==kIOReturnIOError && adreno830_status.stage==A830SPower && adreno830_status.step==A830S1StepAhbTimeout &&
          adreno830_status.reason==A830ReasonReadback && adreno830_status.reg==0x3d9e048 && adreno830_status.value==0xffffffff &&
          adreno830_status.expect==0x1b12 && !called("Sm8750ScmInit"));
    {
        char want[80];std::snprintf(want,sizeof want,"GPU S1.30 POWER FAIL %08X 3D9E048=FFFFFFFF r7 p1",unsigned(kIOReturnIOError));
        CHECK(overlay.back()[0]==want);
    }
    faults.clear();
    b=board13t(A830ProfileAnnibale);loadMismatch=true;faults["A830GmuFirmware::load"]={kIOReturnIOError,0,0,0};
    CHECK(run(&b)==kIOReturnIOError && adreno830_status.stage==A830SFirmware && adreno830_status.step==A830S4StepGmuFirmware &&
          adreno830_status.reason==A830ReasonReadback && adreno830_status.reg==0x60001234 && adreno830_status.value==0x11 &&
          adreno830_status.expect==0x22);
    loadMismatch=false;faults.clear();
    for(const uint16_t irqStep:{uint16_t(A830S8StepIrq),uint16_t(A830S8StepWake)}) {
        b=board13t(A830ProfileAnnibale);faults["A830SelfTest::memWrite"]={kIOReturnIOError,irqStep,A830ReasonIrqWiring,0};
        CHECK(run(&b)==kIOReturnSuccess && adreno830_running() && called("A830SelfTest::coherence")==1 &&
              called("native_adreno_publish")==1 && a830_selftest_last.scratch==0x08750830);
        faults.clear();
    }
    b=board13t(A830ProfileAnnibale);b.knobs.cohtest=1;
    faults["A830SelfTest::coherence(cohtest)"]={kIOReturnTimeout,A830S8StepV9c,A830ReasonHang,0};
    CHECK(run(&b)==kIOReturnTimeout && adreno830_status.stage==A830SFence && adreno830_status.step==A830S8StepV9c &&
          adreno830_status.reason==A830ReasonHang && !called("native_adreno_publish") && called("A830Smmu::releaseAll")==1);
    faults.clear();
    profileInjected+=5;
    /* S9: a failed prepare or publish discards the prepared accelerator; a failed commit marks the direct service lost. */
    b=board13t();faults["metal_prepare"]={kIOReturnUnsupported,1,0,0};
    CHECK(run(&b)==kIOReturnUnsupported && called("metal_discard")==1 && !called("native_adreno_publish"));
    b=board13t();faults.clear();faults["native_adreno_publish"]={kIOReturnNoMemory,0,0,0};
    CHECK(run(&b)==kIOReturnNoMemory && called("metal_discard")==1 && !called("metal_commit"));
    b=board13t();faults.clear();faults["metal_commit"]={kIOReturnNoResources,0,0,0};
    CHECK(run(&b)==kIOReturnNoResources && called("native_adreno_device_lost")==1 && !called("A830Smmu::releaseAll"));
    faults.clear();
    injected+=3;

    /* ---- S5: three cold boots at most, a forced power-off between them, none after reason 22 or the GX check ---------- */
    b=board13t();faults["A830Gmu::coldBoot"]={kIOReturnTimeout,7,A830ReasonTimeout,2};
    CHECK(run(&b)==kIOReturnSuccess && called("A830Gmu::coldBoot")==3 && called("A830Gmu::forceOff")==2 &&
          adreno830_status.attempts==3 && adreno830_status.state==A830Done);
    b=board13t();faults.clear();faults["A830Gmu::coldBoot"]={kIOReturnTimeout,7,A830ReasonTimeout,0};
    CHECK(run(&b)==kIOReturnTimeout && called("A830Gmu::coldBoot")==3 && called("A830Gmu::forceOff")==2 &&
          adreno830_status.stage==A830SGmu && adreno830_status.step==7 && adreno830_status.reason==A830ReasonTimeout &&
          adreno830_status.attempts==3 && called("A830Smmu::releaseAll")==1);
    b=board13t();faults.clear();faults["A830Hfi::start"]={kIOReturnIOError,0x18,A830ReasonGmuHostResource,0};
    CHECK(run(&b)==kIOReturnIOError && called("A830Gmu::coldBoot")==1 && !called("A830Gmu::forceOff") &&
          adreno830_status.reason==A830ReasonGmuHostResource && adreno830_status.step==0x18);
    b=board13t();faults.clear();faults["A830Gmu::waitGxOn"]={kIOReturnNotReady,0x21,A830ReasonUnexpectedState,0};
    CHECK(run(&b)==kIOReturnNotReady && called("A830Gmu::coldBoot")==1 && !called("A830Gmu::forceOff") &&
          adreno830_status.step==0x21 && adreno830_status.reason==A830ReasonUnexpectedState && !called("A830Cp::attach"));
    /* No first acknowledgement in the Linux queue layout: the retry uses the KGSL one. */
    b=board13t();faults.clear();faults["A830Hfi::start"]={kIOReturnTimeout,0x11,A830ReasonTimeout,1};
    CHECK(run(&b)==kIOReturnSuccess && called("hfi-linux")==1 && called("hfi-kgsl")==1 && called("A830Gmu::forceOff")==1);
    b=board13t();faults.clear();faults["A830Gmu::oobSet"]={kIOReturnTimeout,0x20,A830ReasonTimeout,1};
    CHECK(run(&b)==kIOReturnSuccess && called("A830Gmu::coldBoot")==2 && called("A830Gmu::forceOff")==1);
    faults.clear();
    injected+=6;

    /* ---- Residues decide nothing; the optional pieces only record ----------------------------------------------------- */
    b=board13t();sgfsrResidue=2;fsrResidue=0x402;
    faults["Sm8750SpeedBinRead"]={kIOReturnIOError,0,0,0};faults["A830Smmu::clearGlobalFault"]={kIOReturnIOError,0,0,0};
    faults["Sm8750ScmWaitqInfo"]={kIOReturnIOError,0,0,0};faults["A830Gmu::attachInterrupts"]={kIOReturnIOError,0,0,0};
    CHECK(run(&b)==kIOReturnSuccess && adreno830_running() && adreno830_status.sticky[A830StickySmmuGfsr]==2 &&
          adreno830_status.sticky[A830StickyCb0Fsr]==0x402);
    sgfsrResidue=fsrResidue=0;faults.clear();
    /* QMP (pagani-gpu-qmp=1): its result is recorded, a timeout does not stop S1. */
    b=board13t();b.knobs.qmp=1;faults["Sm8750AossQmpSendAcdState"]={kIOReturnTimeout,0,0,0};
    CHECK(run(&b)==kIOReturnSuccess && adreno830_status.qmp==A830QmpTimeout);
    faults.clear();
    b=board13t();b.knobs.qmp=1;
    CHECK(run(&b)==kIOReturnSuccess && adreno830_status.qmp==A830QmpDone && called("qmp-acd-off")==1 && !lastDcvs.acd &&
          position("Sm8750AossQmpSendAcdState")<position("Sm8750GpuPowerOn"));
    /* ACD (pagani-gpu-acd=1, P3): QMP val 1 first, then the ACD table in the HFI start and the DCVS ceiling for ACD; without
     * the AOP's answer ACD stays off and the boot goes on. */
    b=board13t();b.knobs.acd=1;
    CHECK(run(&b)==kIOReturnSuccess && called("qmp-acd-on")==1 && called("start-acd")==1 && lastDcvs.acd);
    b=board13t();b.knobs.acd=1;faults["Sm8750AossQmpSendAcdState"]={kIOReturnTimeout,0,0,0};
    CHECK(run(&b)==kIOReturnSuccess && !called("start-acd") && !lastDcvs.acd && adreno830_status.qmp==A830QmpTimeout);
    faults.clear();
    /* LLCC (pagani-gpu-llcc=1): slice 9 reaches step 9a; a failed activation leaves it off and S6 goes on. */
    b=board13t();b.knobs.llcc=1;
    CHECK(run(&b)==kIOReturnSuccess && lastHwInit.llcc_scid==9);
    b=board13t();b.knobs.llcc=1;faults["Sm8750LlccEnableGpu"]={kIOReturnTimeout,0,0,0};
    CHECK(run(&b)==kIOReturnSuccess && lastHwInit.llcc_scid==0);
    faults.clear();
    /* The host votes of the experiments (M11): LOW_SVS rails, DDR boot level 2, floor 0 after the first M9 answer. */
    b=board13t();b.knobs.rails=0;b.knobs.ddr_boot=2;b.knobs.ddr_floor=0;
    CHECK(run(&b)==kIOReturnSuccess && called("rails-lowsvs")==1 && ddrVotes.size()==2 && ddrVotes[0]==2136718 && ddrVotes[1]==0);
    /* pagani-gpu-hfi: Linux dependency votes (bit0), START after the OOB (bit1). */
    b=board13t();b.knobs.hfi=3;
    CHECK(run(&b)==kIOReturnSuccess && called("votes-linux")==1 && called("start-deferred")==1 && called("A830Hfi::sendStart")==1 &&
          position("A830Hfi::sendStart")>position("A830Gmu::oobSet"));
    /* pagani-gpu-va=2 and pagani-gpu-dcvs=0 / perf=7. */
    b=board13t();b.knobs.va=2;b.knobs.dcvs=0;b.knobs.perf=7;
    CHECK(run(&b)==kIOReturnSuccess && publishedInfo.va_size==0x60000000 && !lastDcvs.governor && lastDcvs.fixedLevel==7 &&
          publishedInputBits==32);
    /* The default, pagani-gpu-va=16: the DeviceInfo and the publish shell's input size follow the aperture the GPU context built, 16 GiB
     * with a 34-bit input, or the 4 GiB it fell back to; pagani-gpu-va=4 asks for the 4 GiB one. */
    b=board13t();
    CHECK(run(&b)==kIOReturnSuccess && publishedInfo.va_start==0x20000000 && publishedInfo.va_size==0x3E0000000ull &&
          publishedInputBits==34);
    b=board13t();wideFallback=true;
    CHECK(run(&b)==kIOReturnSuccess && publishedInfo.va_size==0xE0000000ull && publishedInputBits==32);
    wideFallback=false;
    b=board13t();b.knobs.va=4;
    CHECK(run(&b)==kIOReturnSuccess && publishedInfo.va_start==0x20000000 && publishedInfo.va_size==0xE0000000ull &&
          publishedInputBits==32);
    /* S8 with pagani-gpu-cohtest=1: V9b/V9c run; their timeout stops S8 with reason 18 and nothing is published. */
    b=board13t();b.knobs.cohtest=1;
    CHECK(run(&b)==kIOReturnSuccess && lastEnv.cohtest && called("A830SelfTest::run(cohtest)")==1 && called("coherence-2")==1 &&
          publishedInfo.flags==(NativeAdrenoDeviceClientIova|NativeAdrenoDeviceCachedCoherent));
    b=board13t();b.knobs.cohtest=1;faults["A830SelfTest::run(cohtest)"]={kIOReturnTimeout,A830S8StepV9c,A830ReasonHang,0};
    CHECK(run(&b)==kIOReturnTimeout && adreno830_status.stage==A830SFence && adreno830_status.step==A830S8StepV9c &&
          adreno830_status.reason==A830ReasonHang && !called("native_adreno_publish") && called("A830Smmu::releaseAll")==1);
    faults.clear();
    /* A SoC id that is not the board's stops S0 (ARCHITECTURE 4.1.2). */
    b=board13t();counts.clear();
    adreno830_start_reset();adreno830_set_os(&hostOs);trace.clear();counts["soc639"]=1;history.clear();
    CHECK(adreno830_start(&b)==kIOReturnUnsupported && adreno830_status.step==A830S0StepSocId && adreno830_status.value==639);
    runs+=16;

    /* ---- S0 board structure: every fault fails S0 once with its step and reason, before any hardware --------------- */
    auto expectFault=[&](const Adreno830Board *board,uint16_t step,uint16_t reason,IOReturn result,unsigned *counter=nullptr) {
        const IOReturn got=run(board);
        if(got!=result || adreno830_status.state!=A830Failed || adreno830_status.stage!=A830SInputs ||
           adreno830_status.step!=step || adreno830_status.reason!=reason || called("Sm8750CmdDbCopy") ||
           history.size()!=1 || history[0]!="GPU board inputs" || adreno830_status.profile_set) {
            std::fprintf(stderr,"board fault step %#x: result %d state %u step %#x reason %u\n",step,got,adreno830_status.state,
                         adreno830_status.step,adreno830_status.reason);
            std::exit(1);
        }
        ++(counter?*counter:boardFaults);
    };
    expectFault(nullptr,A830S0StepBoard,A830ReasonArgument,kIOReturnBadArgument);
    CHECK(overlay[0][0]=="GPU S0.01 INPUTS FAIL 00000001 0=00000000 r1");
    b=board13t();b.magic=0x41383341;expectFault(&b,A830S0StepBoard,A830ReasonArgument,kIOReturnBadArgument);
    b=board13t();b.version=2;expectFault(&b,A830S0StepBoard,A830ReasonArgument,kIOReturnBadArgument);
    const unsigned windowCount=sizeof Adreno830BoardWindows/sizeof Adreno830BoardWindows[0];
    CHECK(windowCount==14);
    for(unsigned i=0;i<windowCount;++i) {
        const uint16_t s=uint16_t(A830S0StepWindow+i);
        b=board13t();(b.*Adreno830BoardWindows[i]).phys=0;expectFault(&b,s,A830ReasonArgument,kIOReturnBadArgument);
        b=board13t();(b.*Adreno830BoardWindows[i]).bytes=0;expectFault(&b,s,A830ReasonArgument,kIOReturnBadArgument);
        b=board13t();(b.*Adreno830BoardWindows[i]).phys+=0x800;expectFault(&b,s,A830ReasonArgument,kIOReturnBadArgument);
        b=board13t();(b.*Adreno830BoardWindows[i]).bytes=~0ULL;expectFault(&b,s,A830ReasonArgument,kIOReturnBadArgument);
    }
    for(unsigned i=0;i<3;++i) {   /* optional windows: absent is fine, a malformed one is refused */
        b=board13t();(b.*Adreno830BoardOptionalWindows[i]).phys+=4;
        expectFault(&b,uint16_t(A830S0StepOptionalWindow+i),A830ReasonArgument,kIOReturnBadArgument);
    }
    for(unsigned i=0;i<Adreno830LlccWindows;++i) {
        b=board13t();b.llcc[i].bytes=0;expectFault(&b,uint16_t(A830S0StepLlccWindow+i),A830ReasonArgument,kIOReturnBadArgument);
    }
    b=board13t();b.chip_id=0;expectFault(&b,A830S0StepIdentity,A830ReasonArgument,kIOReturnBadArgument);
    b=board13t();b.soc_id=0;expectFault(&b,A830S0StepIdentity,A830ReasonArgument,kIOReturnBadArgument);
    uint32_t Adreno830Board::*irqs[]={&Adreno830Board::irq_gpu,&Adreno830Board::irq_cx_host,&Adreno830Board::irq_hfi,
        &Adreno830Board::irq_gmu,&Adreno830Board::irq_smmu_global};
    for(unsigned i=0;i<5;++i) {
        b=board13t();b.*irqs[i]=31;expectFault(&b,uint16_t(A830S0StepIrq+i),A830ReasonArgument,kIOReturnBadArgument);
    }
    b=board13t();b.irq_scm_waitq=5000;expectFault(&b,uint16_t(A830S0StepIrq+9),A830ReasonArgument,kIOReturnBadArgument);
    b=board13t();b.sid_lpac=b.sid_gpu;expectFault(&b,A830S0StepStreams,A830ReasonArgument,kIOReturnBadArgument);
    b=board13t();b.cb_gmu=b.cb_gpu;expectFault(&b,A830S0StepStreams,A830ReasonArgument,kIOReturnBadArgument);
    b=board13t();b.smmu_actlr=0;expectFault(&b,A830S0StepActlr,A830ReasonArgument,kIOReturnBadArgument);
    b=board13t();b.level_count[2]=17;expectFault(&b,A830S0StepLevels,A830ReasonArgument,kIOReturnBadArgument);
    b=board13t();b.initial_level[1]=11;expectFault(&b,A830S0StepLevels,A830ReasonArgument,kIOReturnBadArgument);
    b=board13t();b.common_level_count=12;expectFault(&b,A830S0StepLevels,A830ReasonArgument,kIOReturnBadArgument);
    b=board13t();b.ddr_kbps=nullptr;expectFault(&b,A830S0StepLevels,A830ReasonArgument,kIOReturnBadArgument);
    b=board13t();b.pool_mib=32;expectFault(&b,A830S0StepPool,A830ReasonArgument,kIOReturnBadArgument);
    const struct {uint32_t Adreno830Board::Adreno830Knobs::*field;uint32_t bad;} knobs[]={
        {&Adreno830Board::Adreno830Knobs::status,4},{&Adreno830Board::Adreno830Knobs::async,2},
        {&Adreno830Board::Adreno830Knobs::rails,2},{&Adreno830Board::Adreno830Knobs::coherent,3},
        {&Adreno830Board::Adreno830Knobs::dcvs,2},{&Adreno830Board::Adreno830Knobs::llcc,2},
        {&Adreno830Board::Adreno830Knobs::speedbin,2},{&Adreno830Board::Adreno830Knobs::acd,2},
        {&Adreno830Board::Adreno830Knobs::diag,2},{&Adreno830Board::Adreno830Knobs::stop,11},
        {&Adreno830Board::Adreno830Knobs::dcvs_min_mhz,800},{&Adreno830Board::Adreno830Knobs::ddr_boot,12},
        {&Adreno830Board::Adreno830Knobs::ddr_floor,12},{&Adreno830Board::Adreno830Knobs::qmp,2},
        {&Adreno830Board::Adreno830Knobs::cohtest,2},{&Adreno830Board::Adreno830Knobs::va,3},
        {&Adreno830Board::Adreno830Knobs::hfi,4},{&Adreno830Board::Adreno830Knobs::perf,0},
        {&Adreno830Board::Adreno830Knobs::actlr,0},{&Adreno830Board::Adreno830Knobs::slumber,3},
        {&Adreno830Board::Adreno830Knobs::ifpc,2},{&Adreno830Board::Adreno830Knobs::ahbto,2}};
    for(const auto &knob:knobs) {
        b=board13t();b.knobs.*knob.field=knob.bad;expectFault(&b,A830S0StepKnobs,A830ReasonArgument,kIOReturnBadArgument);
    }
    /* pagani-gpu-profile beyond 1 is out of range; pagani-gpu-hwsched is reserved: anything but 0 is reason 24 (no HWSCHED
     * in this build), before the profile is in effect, in both profiles. */
    b=board13t();b.knobs.profile=2;
    expectFault(&b,A830S0StepKnobs,A830ReasonArgument,kIOReturnBadArgument,&profileFaults);
    CHECK(adreno830_status.reg==Adreno830KnobProfile && adreno830_status.value==2 && adreno830_status.expect==1);
    for(const uint32_t profile:{uint32_t(A830ProfileDesign),uint32_t(A830ProfileAnnibale)})
        for(const uint32_t value:{1u,7u}) {
            b=board13t(profile);b.knobs.hwsched=value;
            expectFault(&b,A830S0StepKnobs,A830ReasonUnsupported,kIOReturnUnsupported,&profileFaults);
            CHECK(adreno830_status.reg==Adreno830KnobHwsched && adreno830_status.value==value && adreno830_status.expect==0);
        }
    {   /* no p suffix: the refusal comes before S0 put the profile in effect */
        char want[80];std::snprintf(want,sizeof want,"GPU S0.49 INPUTS FAIL %08X 18=00000007 r24",unsigned(kIOReturnUnsupported));
        CHECK(overlay.back()[0]==want);
    }
    /* perf above the selected table (11 levels) fails the level-table step, after the hardware reads. */
    b=board13t();b.knobs.perf=12;
    CHECK(run(&b)==kIOReturnBadArgument && adreno830_status.step==A830S0StepLevelTable && called("Sm8750GpuLevelsSelect")==1);
    ++boardFaults;
    for(unsigned i=0;i<4;++i) {   /* firmware: missing (weak default), short, corrupted */
        const uint16_t s=uint16_t(A830S0StepFirmware+i);
        b=board13t();(b.*Adreno830BoardFirmware[i]).length=0;expectFault(&b,s,A830ReasonFirmware,kIOReturnBadArgument);
        CHECK((adreno830_status.aux[0]&(A830S0FirmwareChecked|A830S0FirmwareOk))==A830S0FirmwareChecked);
        b=board13t();(b.*Adreno830BoardFirmware[i]).length-=1;expectFault(&b,s,A830ReasonFirmware,kIOReturnIOError);
        std::vector<uint8_t> corrupt=images[i];corrupt[corrupt.size()/2]^=1;
        b=board13t();(b.*Adreno830BoardFirmware[i]).bytes=corrupt.data();expectFault(&b,s,A830ReasonFirmware,kIOReturnIOError);
        CHECK(adreno830_status.reg==i && adreno830_status.value==images[i].size());
    }
    /* Without kernel services nothing runs (host builds must install them). */
    adreno830_start_reset();adreno830_set_os(nullptr);overlay.clear();
    b=board13t();
    CHECK(adreno830_start(&b)==kIOReturnNotReady && adreno830_status.state==A830Failed);
    ++boardFaults;

    /* ---- Boot arguments: defaults, explicit values and their flag bits, the pool and the board's ACTLR default ---------- */
    b=board13t();b.flags=~0u;
    std::map<std::string,uint32_t> none;
    Adreno830ParseKnobs(&b,getter,&none);
    CHECK(b.flags==(~0u&~((1u<<Adreno830KnobCount)-1u|Adreno830BoardFlagPool)) && b.knobs.stop==Adreno830StopNever &&
          b.knobs.status==1 && b.knobs.rails==1 && b.knobs.ddr_boot==11 && b.knobs.ddr_floor==11 && b.knobs.actlr==0x303 &&
          b.knobs.dcvs==1 && b.knobs.dcvs_min_mhz==160 && b.knobs.dcvs_max_mhz==734 && b.knobs.perf==2 && b.knobs.va==16 &&
          b.knobs.async==0 && b.knobs.coherent==0 && b.knobs.qmp==0 && b.knobs.cohtest==0 && b.knobs.llcc==0 &&
          b.knobs.speedbin==0 && b.knobs.acd==0 && b.knobs.hfi==0 && b.knobs.diag==0 && b.pool_mib==512 &&
          b.knobs.profile==A830ProfileAnnibale && b.knobs.hwsched==0 && b.knobs.trace==1);
    ++knobCases;
    b=board13t();b.flags=0;b.smmu_actlr=0x32b;
    std::map<std::string,uint32_t> given={{"pagani-gpu-stop",0},{"pagani-gpu-status",2},{"pagani-gpu-rails",1},
        {"pagani-gpu-ddr",11},{"pagani-gpu-coherent",0},{"pagani-gpu-pool",256},{"pagani-gpu-diag",1},{"pagani-gpu",1}};
    Adreno830ParseKnobs(&b,getter,&given);
    CHECK(b.knobs.stop==0 && b.knobs.status==2 && b.knobs.rails==1 && b.knobs.ddr_boot==11 && b.knobs.coherent==0 &&
          b.knobs.diag==1 && b.knobs.actlr==0x32b && b.pool_mib==256 && b.knobs.ddr_floor==11);
    CHECK(b.flags==((1u<<Adreno830KnobStop)|(1u<<Adreno830KnobStatus)|(1u<<Adreno830KnobRails)|(1u<<Adreno830KnobDdrBoot)|
                    (1u<<Adreno830KnobCoherent)|(1u<<Adreno830KnobDiag)|Adreno830BoardFlagPool));
    ++knobCases;
    {   /* the DDR floor follows an explicit boot level unless it is given itself */
        Adreno830Board d=board13t();
        std::map<std::string,uint32_t> ddrArgs={{"pagani-gpu-ddr",2}};
        Adreno830ParseKnobs(&d,getter,&ddrArgs);
        CHECK(d.knobs.ddr_boot==2 && d.knobs.ddr_floor==2);
        ddrArgs["pagani-gpu-ddr-floor"]=0;
        Adreno830ParseKnobs(&d,getter,&ddrArgs);
        CHECK(d.knobs.ddr_boot==2 && d.knobs.ddr_floor==0);
        ++knobCases;
    }
    for(unsigned i=0;i<Adreno830KnobCount;++i) {   /* each name reaches exactly its own field */
        b=board13t();b.flags=0;
        std::map<std::string,uint32_t> one={{Adreno830KnobTable[i].name,0x5a5a}};
        Adreno830ParseKnobs(&b,getter,&one);
        CHECK(b.flags==(1u<<i) && b.knobs.*Adreno830KnobTable[i].field==0x5a5a);
        for(unsigned j=0;j<Adreno830KnobCount;++j)
            if(j!=i && !(i==Adreno830KnobDdrBoot && j==Adreno830KnobDdrFloor))CHECK(b.knobs.*Adreno830KnobTable[j].field!=0x5a5a);
        /* the names after the design-era ones (profile, hwsched, trace) count on the profile line */
        ++(i<Adreno830KnobProfile?knobCases:profileKnobs);
    }
    {   /* the profile and the HWSCHED switch by name, and their flag bits */
        Adreno830Board p=board13t();p.flags=0;
        std::map<std::string,uint32_t> args={{"pagani-gpu-profile",0},{"pagani-gpu-hwsched",1}};
        Adreno830ParseKnobs(&p,getter,&args);
        CHECK(p.knobs.profile==A830ProfileDesign && p.knobs.hwsched==1 &&
              p.flags==((1u<<Adreno830KnobProfile)|(1u<<Adreno830KnobHwsched)));
        CHECK(Adreno830KnobProfile==23 && Adreno830KnobHwsched==24 && Adreno830KnobTrace==25 && Adreno830KnobMaxLevel==26 &&
              Adreno830KnobIrq==27 && Adreno830KnobTtarget==28 && Adreno830KnobScreenOff==29 && Adreno830KnobKopt==30 &&
              Adreno830KnobCount==31 && Adreno830BoardFlagPool==1u<<31 &&
              !std::strcmp(Adreno830KnobTable[Adreno830KnobScreenOff].name,"pagani-screenoff") &&
              !std::strcmp(Adreno830KnobTable[Adreno830KnobKopt].name,"pagani-gpu-kopt") &&
              !std::strcmp(Adreno830KnobTable[Adreno830KnobProfile].name,"pagani-gpu-profile") &&
              !std::strcmp(Adreno830KnobTable[Adreno830KnobHwsched].name,"pagani-gpu-hwsched") &&
              !std::strcmp(Adreno830KnobTable[Adreno830KnobTrace].name,"pagani-gpu-trace"));
        ++profileKnobs;
    }

    /* ---- The annibale open items wired in (kernel-sequences 9.7, field-notes 9-1, section 14 rows 16, 19, 20): S6 hands the
     * CP its health hooks, failures of S6-S8 and hang verdicts drain the late HFI messages before the streams FAULT and the
     * status block is written, the S5 page shows the cold boot's TCM read-back, GENERAL_11 and RSC status. On a line of its
     * own, before the profile line. ----------------------------------------------------------------------------------- */
    unsigned wiringHooks=0,wiringDrains=0,wiringPages=0;
    auto gmuWord=[](uint32_t dword)->uint32_t & {
        std::vector<uint8_t> &window=windows[0x3d37000];
        CHECK(window.size()==0x68000);
        return *reinterpret_cast<uint32_t *>(window.data()+(4u*dword-A830_GMU_WIN_DELTA));
    };
    auto endsWith=[](const std::string &text,const char *tail) {
        const size_t n=std::strlen(tail);
        return text.size()>=n && text.compare(text.size()-n,n,tail)==0;
    };
    for(const uint32_t profile:{uint32_t(A830ProfileDesign),uint32_t(A830ProfileAnnibale)}) {
        b=board13t(profile);
        CHECK(run(&b)==kIOReturnSuccess && called("A830Cp::setHealth")==1 && cpHealth.global_fault && cpHealth.gmu_fault &&
              position("A830Cp::setHealth")>position("A830Cp::attach") && position("A830Cp::setHealth")<position("A830Cp::hwInit"));
        /* global_fault: sGFSR through the SMMU window, read only (a stale value under Gunyah stays as it was). */
        smmuRegs[A830_SMMU_GR0_SGFSR/4]=0x2;sgfsrStores=0;
        uint32_t sgfsr=0;
        CHECK(cpHealth.global_fault(cpHealth.cookie,&sgfsr)==0 && sgfsr==2 && !sgfsrStores && smmuRegs[A830_SMMU_GR0_SGFSR/4]==2);
        /* gmu_fault: one read-only poll of GMU2HOST bit 15 and the CM3_FW_INIT_RESULT NMI bits, else an earlier fatal. */
        uint32_t &info=gmuWord(A8XX_GMUCX_GMU2HOST_INTR_INFO),&init=gmuWord(A8XX_GMUCX_CM3_FW_INIT_RESULT);
        uint32_t &clear=gmuWord(A8XX_GMUCX_GMU2HOST_INTR_CLR);
        info=0x00800006;init=0x100;clear=0;
        CHECK(cpHealth.gmu_fault(cpHealth.cookie)==0);   /* queue bits and the Linux bit 23 are no fault */
        info=0x00008002;
        CHECK(uint32_t(cpHealth.gmu_fault(cpHealth.cookie))==0x00008002 && info==0x00008002 && !clear);   /* nothing cleared */
        info=0xffffffff;
        CHECK(cpHealth.gmu_fault(cpHealth.cookie)==0);   /* no answer is not a fault (A830Gmu reads it the same way) */
        info=0;init=0x500;
        CHECK(uint32_t(cpHealth.gmu_fault(cpHealth.cookie))==0x500 && init==0x500);
        init=0x100;
        A830GmuStartArgs restart={uint8_t(profile==A830ProfileAnnibale?1:2),2,false,false,false};
        gmuFatalAfterBoot=true;
        CHECK(powerOps.gmuStart(powerOps.context,&restart,&step)==kIOReturnSuccess);
        gmuFatalAfterBoot=false;
        CHECK(cpHealth.gmu_fault(cpHealth.cookie)==1);   /* a fatal bit a top half or pollFatal took */
        wiringHooks+=7;
    }
    /* S6-S8 failures drain once the module failed, before the streams FAULT, and the published FAIL page shows what was found
     * (the design profile's drain finds nothing: no L). */
    lateFound={};lateFound.late_messages=2;lateFound.late_acks=1;lateFound.late_errors=1;
    const struct {const char *name;uint8_t stage;uint16_t step,reason;unsigned row;bool annibale,design;} failing[]={
        {"A830Cp::startCp",A830SCp,A830CpStepMeInit,A830ReasonTimeout,3,true,true},
        {"A830Cp::finishSecure",A830SZap,A830CpStepSecureMode,A830ReasonTimeout,3,true,true},
        {"A830SelfTest::memWrite",A830SFence,A830S8StepReadback,A830ReasonUnexpectedState,2,true,false},
        {"A830SelfTest::run",A830SFence,A830S8StepAttach,A830ReasonIrqWiring,2,false,true}};
    for(const auto &point:failing)
        for(const uint32_t profile:{uint32_t(A830ProfileDesign),uint32_t(A830ProfileAnnibale)}) {
            if(!(profile==A830ProfileAnnibale?point.annibale:point.design))continue;
            b=board13t(profile);b.knobs.status=2;
            faults.clear();faults[point.name]={kIOReturnIOError,point.step,point.reason,0};
            CHECK(run(&b)==kIOReturnIOError && adreno830_status.stage==point.stage && adreno830_status.state==A830Failed &&
                  adreno830_status.step==point.step);
            faults.clear();
            CHECK(called("A830Hfi::drainLate")==1 && position("A830Hfi::drainLate")>position(point.name) &&
                  position("A830Hfi::drainLate")<position("A830Smmu::releaseAll"));
            const std::string &row=overlay.back()[point.row];
            if(profile==A830ProfileAnnibale)CHECK(adreno830_status.hfi_late==A830StatusLate(1,2,1,1) && endsWith(row," L2/1/1"));
            else CHECK(!adreno830_status.hfi_late && row.find(" L")==std::string::npos);
            ++wiringDrains;
        }
    /* No drain of the orchestrator's own for S5 (a failed HFI command drains itself, A830Hfi::transact) or S9. */
    for(const char *name:{"A830Gmu::waitGxOn","native_adreno_publish"}) {
        b=board13t(A830ProfileAnnibale);faults.clear();faults[name]={kIOReturnIOError,0x21,A830ReasonUnexpectedState,0};
        CHECK(run(&b)==kIOReturnIOError && !called("A830Hfi::drainLate") && !adreno830_status.hfi_late);
        ++wiringDrains;
    }
    b=board13t(A830ProfileAnnibale);b.knobs.status=2;lateOnStart=true;
    faults.clear();faults["A830Hfi::start"]={kIOReturnIOError,0x18,A830ReasonGmuHostResource,0};
    CHECK(run(&b)==kIOReturnIOError && adreno830_status.stage==A830SGmu && !called("A830Hfi::drainLate") &&
          adreno830_status.hfi_late==A830StatusLate(1,2,1,1) && endsWith(overlay.back()[2]," L2/1/1 G11 00000000"));
    lateOnStart=false;faults.clear();
    ++wiringDrains;
    /* The hang verdict (A830RecoveryOps::snapshot, before A830Recovery writes the status block): only with CX up and the GMU
     * running. */
    b=board13t(A830ProfileAnnibale);
    CHECK(run(&b)==kIOReturnSuccess && !adreno830_status.hfi_late);
    trace.clear();counts.clear();
    A830HangSnapshot hang={};
    recoveryOps.snapshot(recoveryOps.context,&hang);   /* CX down: no GMU register is read */
    CHECK(!called("A830Hfi::drainLate") && !adreno830_status.hfi_late);
    hang.cxOn=true;
    recoveryOps.snapshot(recoveryOps.context,&hang);
    CHECK(called("A830Hfi::drainLate")==1 && adreno830_status.hfi_late==A830StatusLate(1,2,1,1));
    CHECK(powerOps.gmuForceOff(powerOps.context)==kIOReturnSuccess);
    recoveryOps.snapshot(recoveryOps.context,&hang);   /* a GMU forced off sends nothing */
    CHECK(called("A830Hfi::drainLate")==1);
    wiringDrains+=3;
    lateFound={};
    /* The S5 page of an S5 stop: the annibale cold boot's TCM words, GENERAL_11 and RSC status; the design reads none. */
    b=board13t(A830ProfileAnnibale);b.knobs.stop=A830SGmu;b.knobs.status=2;bootGeneral11=0x1234;
    CHECK(run(&b)==kIOReturnSuccess && adreno830_status.state==A830Stopped && adreno830_status.gmu_general11==0x1234);
    CHECK(overlay.back()[1]=="GMU FW 50010019 HFI 20070001 INIT 00000100 37ms TCM 139" &&
          overlay.back()[2]=="HFI M1 0 M2 0 M6 0 M8 0 M9 0 G11 00001234" &&
          overlay.back()[3]=="OOB 150us GX 00000000 RPMH 0 AO 00000000 RSC 00010000");
    bootGeneral11=0;
    b=board13t();b.knobs.stop=A830SGmu;b.knobs.status=2;
    CHECK(run(&b)==kIOReturnSuccess && !adreno830_status.gmu_general11);
    CHECK(overlay.back()[1]=="GMU FW 50010019 HFI 20070001 INIT 00000100 37ms" && overlay.back()[2]=="HFI M1 0 M2 0 M6 0 M8 0 M9 0" &&
          overlay.back()[3]=="OOB 150us GX 00000000 RPMH 0 AO 00000000");
    wiringPages+=2;

    /* ---- The performance switches (2026-10-30): pagani-gpu-maxlevel and pagani-gpu-irq, both 0 by default ----------------
     * Counted on a line of their own, so the pinned line keeps its counts. The RUN row gets the run page's knobs through the
     * real status block: adreno830_status_dcvs is what the DCVS thread calls once a second. */
    unsigned perfRuns=0,perfRows=0,perfFaults=0,perfKnobs=0;
    {
        const uint16_t residency[11]={0,0,0,0,0,0,0,0,1000,0,0};
        /* Defaults: the first round unchanged (no full table, no ceiling level, 332 untouched, no mask of A830Irq's own). */
        b=board13t(A830ProfileAnnibale);b.knobs.status=2;
        CHECK(b.knobs.maxlevel==0 && b.knobs.irq==0);
        CHECK(run(&b)==kIOReturnSuccess && !called("votes-full") && lastDcvs.maxLevel==0 && !called("A830Irq::attach") &&
              irqOwnMask==0 && called("memwrite-polled")==1 && irqSeen && !irqSeen->attached());
        adreno830_status_dcvs(342,990,3,3,160,342,1,3,residency);
        CHECK(overlay.back()[0]=="GPU RUN 342M b99 g3/b3 s0 f0 h0 p1" && overlay.back()[1]=="DCVS 160-342 t0/0/0");
        ++perfRuns;++perfRows;
        /* pagani-gpu-maxlevel=9: the whole table to the GMU (A830VotesFullTable), the first vote still the 160 MHz level 1
         * with DDR level 2, the DCVS ceiling at level 9; the RUN row ends with m9 and the DCVS row lists the 11 levels. */
        b=board13t(A830ProfileAnnibale);b.knobs.status=2;b.knobs.maxlevel=9;
        CHECK(run(&b)==kIOReturnSuccess && adreno830_running() && called("votes-full")==1 && lastDcvs.maxLevel==9 &&
              startLevel==1 && startBus==2 && lastDcvs.initialLevel==1 && lastDcvs.initialBus==2 && !lastDcvs.maxGiven &&
              powerLevel==1 && powerBus==2 && !called("A830Irq::attach"));
        adreno830_status_dcvs(734,990,9,8,160,734,1,11,residency);
        CHECK(overlay.back()[0]=="GPU RUN 734M b99 g9/b8 s0 f0 h0 p1 m9" &&
              overlay.back()[1]=="DCVS 160-734 t0/0/0/0/0/0/0/0/1000/0/0");
        ++perfRuns;++perfRows;
        /* Beyond the table (16 > 11 levels): accepted, the top is A830Dcvs's to pick. The design profile: its table is whole
         * already, n is only the DCVS ceiling, the first vote stays the board's initial level. */
        b=board13t(A830ProfileAnnibale);b.knobs.maxlevel=16;
        CHECK(run(&b)==kIOReturnSuccess && called("votes-full")==1 && lastDcvs.maxLevel==16);
        b=board13t();b.knobs.maxlevel=7;
        CHECK(run(&b)==kIOReturnSuccess && called("votes-full")==1 && lastDcvs.maxLevel==7 && startLevel==2);
        perfRuns+=2;
        /* pagani-gpu-irq=1: S8 attaches 332 before the CP_MEM_WRITE check with A830Irq owning the fence-only mask, the check
         * runs with the line live, and the DCVS fault poll leaves the retiring to the fence interrupt. */
        b=board13t(A830ProfileAnnibale);b.knobs.status=2;b.knobs.irq=1;
        CHECK(run(&b)==kIOReturnSuccess && adreno830_running() && called("A830Irq::attach")==1 &&
              irqOwnMask==A830_INT_MASK_FENCE_ONLY && position("A830Irq::attach")>position("A830PowerState::start") &&
              position("A830Irq::attach")<position("A830SelfTest::memWrite") && called("memwrite-irq")==1 &&
              !called("A830Irq::fallBack") && irqSeen->attached() && !irqSeen->off() && !called("A830SelfTest::run") &&
              a830_selftest_last.attach_result==uint32_t(kIOReturnSuccess));
        lastDcvsOps.pollFaults(lastDcvsOps.context);
        CHECK(!called("native_adreno_retire_completed"));
        adreno830_status_dcvs(342,990,3,3,160,342,1,3,residency);
        CHECK(overlay.back()[0]=="GPU RUN 342M b99 g3/b3 s0 f0 h0 p1 i1");
        ++perfRuns;++perfRows;
        /* A refused attach is recorded (irq_off bit 0) and S8 goes on polled; the DCVS thread retires; the row says i0. */
        b=board13t(A830ProfileAnnibale);b.knobs.status=2;b.knobs.irq=1;
        faults["A830Irq::attach"]={kIOReturnNoResources,0,0,0};
        CHECK(run(&b)==kIOReturnSuccess && adreno830_running() && called("A830Irq::attach")==1 && called("memwrite-polled")==1 &&
              !called("A830Irq::fallBack") && irqSeen->off() && !irqSeen->attached() &&
              a830_selftest_last.attach_result==uint32_t(kIOReturnNoResources) && (adreno830_status.irq_off&A830IrqOffGpu));
        faults.clear();
        lastDcvsOps.pollFaults(lastDcvsOps.context);
        CHECK(called("native_adreno_retire_completed")==1);
        adreno830_status_dcvs(342,990,3,3,160,342,1,3,residency);
        CHECK(overlay.back()[0]=="GPU RUN 342M b99 g3/b3 s0 f0 h0 r23 p1 i0");
        ++perfRuns;++perfRows;
        /* Attached, but the fence interrupt did not come (count) or did not wake the waiter: 332 off for the boot
         * (A830Irq::fallBack), S8 goes on polled, the DCVS thread retires. */
        for(const uint16_t irqStep:{uint16_t(A830S8StepIrq),uint16_t(A830S8StepWake)}) {
            b=board13t(A830ProfileAnnibale);b.knobs.irq=1;
            faults["A830SelfTest::memWrite"]={kIOReturnIOError,irqStep,A830ReasonIrqWiring,0};
            CHECK(run(&b)==kIOReturnSuccess && adreno830_running() && called("memwrite-irq")==1 &&
                  called("A830Irq::fallBack")==1 && irqSeen->attached() && irqSeen->off() && irqSeen->polled() &&
                  called("A830SelfTest::coherence")==1 && called("native_adreno_publish")==1);
            faults.clear();
            lastDcvsOps.pollFaults(lastDcvsOps.context);
            CHECK(called("native_adreno_retire_completed")==1);
            ++perfRuns;
        }
        /* A CP_MEM_WRITE that really failed (read-back) still fails S8, as it does without the switch. */
        b=board13t(A830ProfileAnnibale);b.knobs.irq=1;
        faults["A830SelfTest::memWrite"]={kIOReturnIOError,A830S8StepReadback,A830ReasonUnexpectedState,0};
        CHECK(run(&b)==kIOReturnIOError && adreno830_status.stage==A830SFence && adreno830_status.step==A830S8StepReadback &&
              !called("A830Irq::fallBack") && !called("native_adreno_publish"));
        faults.clear();
        ++perfRuns;
        /* Both switches together. */
        b=board13t(A830ProfileAnnibale);b.knobs.status=2;b.knobs.maxlevel=11;b.knobs.irq=1;
        CHECK(run(&b)==kIOReturnSuccess && called("votes-full")==1 && lastDcvs.maxLevel==11 && called("memwrite-irq")==1 &&
              irqOwnMask==A830_INT_MASK_FENCE_ONLY);
        adreno830_status_dcvs(734,990,9,8,160,734,1,11,residency);
        CHECK(overlay.back()[0]=="GPU RUN 734M b99 g9/b8 s0 f0 h0 p1 m11 i1");
        ++perfRuns;++perfRows;
        /* The design profile: 332 is the self-test's (attached by run()), the mask stays A830Cp's (own_mask 0). */
        b=board13t();b.knobs.irq=1;
        CHECK(run(&b)==kIOReturnSuccess && irqOwnMask==0 && called("A830SelfTest::run")==1 && called("A830Irq::attach")==1 &&
              !called("A830SelfTest::memWrite"));
        ++perfRuns;
        /* S0 refuses maxlevel above Adreno830LevelMax (16) and irq above 1 before anything runs (step 0x49, reason 1). */
        b=board13t(A830ProfileAnnibale);b.knobs.maxlevel=17;
        expectFault(&b,A830S0StepKnobs,A830ReasonArgument,kIOReturnBadArgument,&perfFaults);
        CHECK(adreno830_status.reg==Adreno830KnobMaxLevel && adreno830_status.value==17 && adreno830_status.expect==16);
        b=board13t(A830ProfileAnnibale);b.knobs.irq=2;
        expectFault(&b,A830S0StepKnobs,A830ReasonArgument,kIOReturnBadArgument,&perfFaults);
        CHECK(adreno830_status.reg==Adreno830KnobIrq && adreno830_status.value==2 && adreno830_status.expect==1);
        /* The boot arguments by name: 0 when absent, the given values and their flag bits. */
        Adreno830Board p=board13t();p.flags=0;
        std::map<std::string,uint32_t> absent;
        Adreno830ParseKnobs(&p,getter,&absent);
        CHECK(p.knobs.maxlevel==0 && p.knobs.irq==0 && !(p.flags&((1u<<Adreno830KnobMaxLevel)|(1u<<Adreno830KnobIrq))));
        std::map<std::string,uint32_t> args={{"pagani-gpu-maxlevel",9},{"pagani-gpu-irq",1}};
        Adreno830ParseKnobs(&p,getter,&args);
        CHECK(p.knobs.maxlevel==9 && p.knobs.irq==1 && p.flags==((1u<<Adreno830KnobMaxLevel)|(1u<<Adreno830KnobIrq)) &&
              !std::strcmp(Adreno830KnobTable[Adreno830KnobMaxLevel].name,"pagani-gpu-maxlevel") &&
              !std::strcmp(Adreno830KnobTable[Adreno830KnobIrq].name,"pagani-gpu-irq"));
        perfKnobs+=2;
        /* pagani-gpu-ttarget (2026-10-30): the design thermal loop's target, 95 C when absent, handed to the DCVS in deci-C;
         * S0 refuses it outside 50..100 C (step 0x49, reason 1). */
        Adreno830Board t=board13t();t.flags=0;
        Adreno830ParseKnobs(&t,getter,&absent);
        CHECK(t.knobs.ttarget==95 && !(t.flags&(1u<<Adreno830KnobTtarget)));
        std::map<std::string,uint32_t> target={{"pagani-gpu-ttarget",75}};
        Adreno830ParseKnobs(&t,getter,&target);
        CHECK(t.knobs.ttarget==75 && t.flags==(1u<<Adreno830KnobTtarget) &&
              !std::strcmp(Adreno830KnobTable[Adreno830KnobTtarget].name,"pagani-gpu-ttarget"));
        perfKnobs+=2;
        b=board13t();
        CHECK(run(&b)==kIOReturnSuccess && lastDcvs.thermalTargetDeci==950);
        b=board13t();b.knobs.ttarget=75;
        CHECK(run(&b)==kIOReturnSuccess && lastDcvs.thermalTargetDeci==750);
        b=board13t(A830ProfileAnnibale);b.knobs.ttarget=100;
        CHECK(run(&b)==kIOReturnSuccess && lastDcvs.thermalTargetDeci==1000);
        perfRuns+=3;
        b=board13t();b.knobs.ttarget=101;
        expectFault(&b,A830S0StepKnobs,A830ReasonArgument,kIOReturnBadArgument,&perfFaults);
        CHECK(adreno830_status.reg==Adreno830KnobTtarget && adreno830_status.value==101 && adreno830_status.expect==100);
        b=board13t();b.knobs.ttarget=49;
        expectFault(&b,A830S0StepKnobs,A830ReasonArgument,kIOReturnBadArgument,&perfFaults);
        CHECK(adreno830_status.reg==Adreno830KnobTtarget && adreno830_status.value==49 && adreno830_status.expect==50);
    }

    /* ---- pagani-screenoff (bits 1 and 3 are the GPU's; the others belong to the CPU governor and the panel): the screen rule of
     * the power state machine, CX kept by default, the full collapse with bit 3; no value is refused (S0 checks no range), and
     * without bit 1 nothing changes (P0 as before). ---------------------------------------------------------------------- */
    unsigned screenRuns=0;
    {
        const struct {uint32_t bits;uint8_t mode;} cases[]={{0,A830SlumberOff},{1,A830SlumberOff},{4,A830SlumberOff},
            {5,A830SlumberOff},{8,A830SlumberOff},{2,A830SlumberCxKeep},{3,A830SlumberCxKeep},{7,A830SlumberCxKeep},
            {10,A830SlumberFull},{15,A830SlumberFull},{0xfffffff5u,A830SlumberOff},{0xffffffffu,A830SlumberFull}};
        for(const auto &c:cases)
            for(const uint32_t profile:{uint32_t(A830ProfileDesign),uint32_t(A830ProfileAnnibale)}) {
                b=board13t(profile);b.knobs.screenoff=c.bits;
                CHECK(run(&b)==kIOReturnSuccess && powerStarted && powerConfig.screenSlumber==c.mode &&
                      powerConfig.screenIdleMs==1000 && powerConfig.slumber==A830SlumberOff && !powerConfig.ifpc);
                ++screenRuns;
            }
        Adreno830Board k=board13t();k.flags=0;
        std::map<std::string,uint32_t> args={{"pagani-screenoff",7}};
        Adreno830ParseKnobs(&k,getter,&args);
        CHECK(k.knobs.screenoff==7 && k.flags==(1u<<Adreno830KnobScreenOff));
        Adreno830ParseKnobs(&k,getter,&none);
        CHECK(k.knobs.screenoff==0 && !(k.flags&(1u<<Adreno830KnobScreenOff)));
    }
    /* ---- pagani-gpu-kopt (NativeAdrenoKopt.h): every change on when absent, S0 stores what it accepted before S3 maintains the pool,
     * and a bit this build does not have is refused (step 0x49, reason 1). ------------------------------------------------ */
    unsigned koptCases=0;
    {
        CHECK(NativeAdrenoKoptAll==63);
        Adreno830Board k=board13t();k.flags=0;
        Adreno830ParseKnobs(&k,getter,&none);
        CHECK(k.knobs.kopt==NativeAdrenoKoptAll && !(k.flags&(1u<<Adreno830KnobKopt)));
        std::map<std::string,uint32_t> args={{"pagani-gpu-kopt",0}};
        Adreno830ParseKnobs(&k,getter,&args);
        CHECK(k.knobs.kopt==0 && k.flags==(1u<<Adreno830KnobKopt));
        koptCases+=2;
        for(const uint32_t bits:{uint32_t(NativeAdrenoKoptAll),0u,uint32_t(NativeAdrenoKoptMeter|NativeAdrenoKoptPoolZero)}) {
            b=board13t();b.knobs.kopt=bits;lastKopt=0xdead;
            CHECK(run(&b)==kIOReturnSuccess && lastKopt==bits);
            ++koptCases;
        }
        b=board13t();b.knobs.kopt=NativeAdrenoKoptAll+1u;lastKopt=0xdead;
        expectFault(&b,A830S0StepKnobs,A830ReasonArgument,kIOReturnBadArgument,&koptCases);
        CHECK(adreno830_status.reg==Adreno830KnobKopt && adreno830_status.value==64 && adreno830_status.expect==63 && lastKopt==0xdead);
    }
    /* ---- pagani-diag (not a knob of this driver: no flag bit, no table row): the run-time lines of ARCHITECTURE 8.8 and 6.2 stay off without it,
     * S0 hands it to the status block before anything logs, and a board that is not one (no magic) leaves them off. --------- */
    unsigned diagCases=0;
    {
        Adreno830Board d=board13t();d.flags=0;d.system_diag=7;
        Adreno830ParseKnobs(&d,getter,&none);
        CHECK(d.system_diag==0 && d.flags==0);
        std::map<std::string,uint32_t> args={{"pagani-diag",1}};
        Adreno830ParseKnobs(&d,getter,&args);
        CHECK(d.system_diag==1 && d.flags==0);
        args={{"pagani-gpu-diag",1}};
        Adreno830ParseKnobs(&d,getter,&args);
        CHECK(d.system_diag==0 && d.knobs.diag==1);
        diagCases+=3;
        b=board13t();b.system_diag=1;
        CHECK(run(&b)==kIOReturnSuccess && adreno830_status_current_diag());
        b=board13t();b.system_diag=0;
        CHECK(run(&b)==kIOReturnSuccess && !adreno830_status_current_diag());
        diagCases+=2;
    }
    std::printf("PASS: pagani-diag: %u cases (off when absent, its own name, no flag bit, S0 hands it over)\n",diagCases);
    std::printf("PASS: pagani-gpu-kopt: %u cases (all on when absent, stored by S0 as given, a bit past the known ones refused)\n",koptCases);
    std::printf("PASS: pagani-screenoff: %u full runs (bit 1 the screen rule with CX kept, bit 3 the full collapse, other bits "
                "ignored, never an S0 refusal, both profiles) and its boot argument by name\n",screenRuns);

    std::printf("PASS: annibale wiring: %u CP health hook checks (sGFSR read only, GMU bit 15 or NMI poll or an earlier fatal, "
                "both profiles), %u late-drain cases (S6-S8 failures and hang verdicts before the status block, S5 drains itself, "
                "none for S5/S9 or CX down), %u S5 pages (TCM, GENERAL_11, RSC only in annibale)\n",wiringHooks,wiringDrains,
                wiringPages);
    std::printf("PASS: bring-up profiles (annibale default, design): %u annibale/ahbto/dcvs-max full runs, %u annibale "
                "pagani-gpu-stop runs, %u annibale injected failures (S1 AHB timeouts, polled S8, S4 DDR read-back), %u S0 "
                "profile/hwsched refusals (hwsched reason 24), %u annibale power hooks (OOB_GPU kept, AHB after CX power-up), %u "
                "profile boot-argument cases\n",profileRuns,profileStops,profileInjected,profileFaults,profileHooks,profileKnobs);
    std::printf("PASS: performance switches: %u runs (pagani-gpu-maxlevel: whole DT table with the 160 MHz first vote, DCVS "
                "ceiling level; pagani-gpu-irq: 332 attached with the fence-only mask, polled after a refused attach or a missing "
                "fence interrupt, design unchanged; pagani-gpu-ttarget: 80 C by default, handed to the DCVS in deci-C), %u "
                "RUN/DCVS rows with m<n> and i1/i0, %u S0 refusals (ttarget outside 50..90 C among them), %u boot-argument "
                "cases\n",perfRuns,perfRows,perfFaults,perfKnobs);
    std::printf("PASS: %u full runs, %u pagani-gpu-stop runs, %u injected stage failures (each failing once with the module's "
                "step and reason, S5 cold boot at most 3 attempts), %u S0 board faults, %u power/recovery hooks and %u "
                "boot-argument cases over host doubles\n",runs,stops,injected,boardFaults,hooks,knobCases);
    return 0;
}
