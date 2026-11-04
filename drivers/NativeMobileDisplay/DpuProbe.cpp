// SPDX-License-Identifier: BSD-2-Clause-Patent
// Access to the display processor for the GOP display path (ARCHITECTURE 5.3.3, 5.3.4, 5.3.6; M11 and M12): one device mapping of MDSS
// behind an access guard (native_dpu_allowed, NativeDpu.h), the display probe that shows what it reads on text rows 14-18
// (pagani-dpu-probe) and the INTF_1 underrun counter. Structure after UniXNU-sheng's DisplayTimingProbe.
//
// 2026-10-30 12:15 the old probe (pagani-dpu-probe=1 then, =3 now) hung the OnePlus 13T: started with the display at about 1.4 s, it
// slept 150 s, then read the MDSS top, the MDP interrupts, INTF_1, CTL 0..5, the ten source pipes and the MDP streams' routes on the
// apps SMMU (global registers, stream-match groups, a context bank's registers). The log stopped at 150.8 s with no panic and the phone
// came back cold-reset about 10 s later: a CPU stalled on a bus read, then the watchdog. The hazard ranking is in dpu-safety.md: the apps
// SMMU (the Gunyah hypervisor owns it; its context-bank registers had never been read on this phone), the blocks outside the boot
// loader's display path (CTL 1..5, unused pipes: clock gated, possibly in the MDSS_CORE_INT2 power domain), the MDSS and MDP top. So:
//   pagani-dpu-probe=1  the safe mode: INTF_1's FRAME_COUNT, TEAR_INT_COUNT_VAL and TEAR_AUTOREFRESH_CONFIG once a second, nothing else
//                       (the tear check runs the autorefresh that sends every frame of the command-mode panel: it is clocked while the
//                       panel shows anything). Rows "TE <Hz> FC <frames>/s ...", the ten-second extremes and the mode row.
//   pagani-dpu-probe=2  also CTL 0, the pipes CTL 0 names active and MDP_INTR_STATUS, read only: what pagani-fb-flip=1 touches.
//   pagani-dpu-probe=3  the old full snapshot with the apps SMMU and the guard off. IT HUNG THE 13T: only to reproduce that.
//   pagani-dpu-probe=4  no MDSS access: the GDSCR words of MDSS_CORE and MDSS_CORE_INT2 from the display clock controller.
// Nothing outside pagani-dpu-probe=3 reads the apps SMMU (native_dpu_mdp_fsr no longer does) or sweeps CTLs or pipes. The probe itself
// never writes; the only write here is the underrun bit's W1C clear, done only while page flips drive the DPU.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/OSAtomic.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include "NativeDpu.h"
#include "NativeDpuRows.h"
#include "../BootProgress/PeripheralStatus.h"

extern "C" {
volatile uint32_t native_dpu_underruns;
volatile uint64_t native_dpu_flip_stats[8];
volatile uint64_t native_dpu_te_stats[4];
volatile uint32_t native_dpu_idle;
volatile uint32_t native_dpu_refused;
// [0] probe stage (1 waiting, 2 mapped, 3 first reading, 4 rows shown; 0xffffffff not mapped, 0xfffffff9 no thread), [1] snapshots,
// [2] row publications, [3] mode, [4] the first refused offset (bit 31: a write), [5] seconds since the first access.
volatile uint32_t native_dpu_probe[6];
}
// INTF_1 underruns as they are counted (ARCHITECTURE 5.3.6: the GPU driver raises the host DDR floor while they grow); weak, empty
// without the GPU driver.
extern "C" __attribute__((weak)) void pagani_display_underrun(uint32_t) {}
// Builds without BootProgress keep the rows to themselves.
extern "C" __attribute__((weak)) void boot_progress_dpu_rows(const char (*)[PAGANI_GPU_ROW_BYTES],unsigned) {}

namespace {
using namespace nativedpu;
IOMemoryMap *gMdss;
volatile uint32_t *gRegs;
volatile uint32_t gFull;                // pagani-dpu-probe=3 switched the guard off (set by its thread just before its first access)
volatile uint32_t gActivePipes;         // pipes CTL 0 named active while driving INTF_1, accumulated by the flip-path snapshots
NativeDpuState gProbe;                  // the probe thread's snapshot (its own; the display driver keeps another)
NativeDpuProbePlan gPlan;

unsigned bootValue(const char *name) {unsigned value=0;return PE_parse_boot_argn(name,&value,sizeof value)?value:0;}
bool enabled(const char *name) {return bootValue(name)==1;}
uint64_t ticksPerSecond() {uint64_t t=0;nanoseconds_to_absolutetime(1000000000ull,&t);return t;}

void refuse(uint32_t offset,bool write) {
    if(__atomic_fetch_add(&native_dpu_refused,1u,__ATOMIC_RELAXED)==0)native_dpu_probe[4]=offset|(write?0x80000000u:0u);
}
bool permitted(uint32_t offset,bool write) {
    if(gFull)return offset<kMdssBytes && !(offset&3);
    if(native_dpu_allowed(offset,write,uint16_t(gActivePipes)))return true;
    refuse(offset,write);
    return false;
}

void readIntf1(NativeDpuState *s) {
    const uint32_t f=kIntf1;
    s->timingEngine=native_dpu_read(f+kIntfTimingEngine);s->frameCount=native_dpu_read(f+kIntfFrameCount);
    s->lineCount=native_dpu_read(f+kIntfLineCount);s->intfStatus=native_dpu_read(f+kIntfStatus);
    s->vsyncSel=native_dpu_read(f+kTearMdpVsyncSel);s->tearCheck=native_dpu_read(f+kTearCheckEn);
    s->syncVsync=native_dpu_read(f+kTearSyncVsync);s->syncHeight=native_dpu_read(f+kTearSyncHeight);
    s->vsyncInit=native_dpu_read(f+kTearVsyncInit);s->intCount=native_dpu_read(f+kTearIntCount);
    s->startPos=native_dpu_read(f+kTearStartPos);s->rdPtrIrq=native_dpu_read(f+kTearRdPtrIrq);
    s->tearLineCount=native_dpu_read(f+kTearLineCount);s->autorefresh=native_dpu_read(f+kTearAutorefresh);
}
void readCtl(NativeDpuState *s,unsigned c) {
    const uint32_t b=kCtl0+kCtlStride*c;
    s->ctl[c]={native_dpu_read(b+kCtlTop),native_dpu_read(b+kCtlFlush),native_dpu_read(b+kCtlIntfActive),native_dpu_read(b+kCtlPipeActive),
               native_dpu_read(b+kCtlLayerActive)};
    s->fetchActive[c]=native_dpu_read(b+kCtlFetchActive);
}
NativeDpuPipe readPipe(unsigned p) {
    const uint32_t b=kPipeBase[p];
    return {native_dpu_read(b+kSrcSize),native_dpu_read(b+kSrc0Addr),native_dpu_read(b+kSrc1Addr),native_dpu_read(b+kSrcYStride0),
            native_dpu_read(b+kSrcFormat),native_dpu_read(b+kSrcOpMode),native_dpu_read(b+kMultirectOpMode)};
}

// pagani-dpu-probe=3 ONLY. THIS IS THE SNAPSHOT THAT HUNG THE 13T (2026-10-30 12:15, see the top): MDSS top, MDP interrupts, INTF_1,
// CTL 0..5 and all ten pipes, whether the boot loader's display path uses them or not. Called with the guard off (gFull).
void fullSnapshot(NativeDpuState *s) {
    const uint8_t smmuRead=s->smmuRead;
    NativeDpuStream streams[kStreamCount];
    for(unsigned i=0;i<kStreamCount;++i)streams[i]=s->stream[i];
    const uint32_t cr0=s->smmuCr0,idr0=s->smmuIdr0,idr1=s->smmuIdr1,idr2=s->smmuIdr2;
    *s={};
    s->hwIntrStatus=native_dpu_read(kHwIntrStatus);
    s->mdpIntrEn=native_dpu_read(kMdpIntrEn);s->mdpIntrStatus=native_dpu_read(kMdpIntrStatus);
    readIntf1(s);
    for(unsigned c=0;c<kCtlCount;++c)readCtl(s,c);
    for(unsigned p=0;p<kPipeCount;++p)s->pipe[p]=readPipe(p);
    s->readPipes=uint16_t((1u<<kPipeCount)-1);
    // The apps-SMMU part is kept from an earlier snapshot of the same object (snapshotSmmu reads it separately).
    s->smmuRead=smmuRead;s->smmuCr0=cr0;s->smmuIdr0=idr0;s->smmuIdr1=idr1;s->smmuIdr2=idr2;
    for(unsigned i=0;i<kStreamCount;++i)s->stream[i]=streams[i];
    s->scope=kDpuScopeFull;s->valid=1;
}
// pagani-dpu-probe=3 ONLY (part of the snapshot that hung the 13T). The MDP streams' apps-SMMU routes: global registers, the stream-match
// groups, and each translating route's context bank (SCTLR, TTBR0, TCR, MAIR0, FSR: registers no other driver on this phone has read).
// The Gunyah hypervisor owns this SMMU. The map is read only and dropped afterwards; everything stays inside it.
void snapshotSmmu(NativeDpuState *s) {
    for(auto &st: s->stream){st={};st.slot=0xff;st.bank=0xff;}
    for(unsigned i=0;i<kStreamCount;++i)s->stream[i].sid=kMdpStreams[i];
    IODeviceMemory *memory=IODeviceMemory::withRange(kAppsSmmuBase,kAppsSmmuBytes);
    IOMemoryMap *map=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache|kIOMapReadOnly):nullptr;
    OSSafeReleaseNULL(memory);
    if(!map || !map->getVirtualAddress()){OSSafeReleaseNULL(map);return;}
    const volatile uint32_t *smmu=reinterpret_cast<const volatile uint32_t *>(map->getVirtualAddress());
    s->smmuCr0=smmu[0];s->smmuIdr0=smmu[0x20/4];s->smmuIdr1=smmu[0x24/4];s->smmuIdr2=smmu[0x28/4];
    const unsigned groups=(s->smmuIdr0&0xff)>128?128:(s->smmuIdr0&0xff);
    const unsigned banks=s->smmuIdr1&0xff;
    const bool page4k=!(s->smmuIdr1>>31);
    const uint64_t pages=1ull<<(((s->smmuIdr1>>28)&7)+1);
    uint32_t smr[128]={};
    for(unsigned i=0;i<groups;++i)smr[i]=smmu[(0x800+4*i)/4];
    for(unsigned i=0;i<kStreamCount;++i) {
        NativeDpuStream &st=s->stream[i];
        const int slot=native_dpu_stream_slot(smr,groups,st.sid);
        if(slot<0)continue;
        st.slot=uint8_t(slot);st.smr=smr[slot];st.s2cr=smmu[(0xc00+4*slot)/4];
        const unsigned bank=st.s2cr&0xff;
        if(((st.s2cr>>16)&3)!=0 || !page4k || bank>=banks || (pages+bank+1)*4096>kAppsSmmuBytes)continue;
        st.bank=uint8_t(bank);
        st.cbar=smmu[(0x1000+4*bank)/4];
        const volatile uint32_t *cb=smmu+(pages+bank)*4096/4;
        st.sctlr=cb[0];st.ttbr0=cb[0x20/4]|(uint64_t)cb[0x24/4]<<32;st.tcr=cb[0x30/4];st.mair0=cb[0x38/4];st.fsr=cb[0x58/4];
    }
    s->smmuRead=1;
    map->release();
}

// pagani-dpu-probe=4: the display clock controller's GDSCR words of MDSS_CORE (0xaf09000) and MDSS_CORE_INT2 (0xaf0b000), read only.
// No MDSS register is touched. The mapping is kept for the thread's life.
const volatile uint32_t *gGdsc;
bool mapGdsc() {
    if(gGdsc)return true;
    IODeviceMemory *memory=IODeviceMemory::withRange(kGdscBase,kGdscBytes);
    IOMemoryMap *map=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache|kIOMapReadOnly):nullptr;
    OSSafeReleaseNULL(memory);
    if(!map || !map->getVirtualAddress()){OSSafeReleaseNULL(map);return false;}
    gGdsc=reinterpret_cast<const volatile uint32_t *>(map->getVirtualAddress());    // the map lives as long as the thread: forever
    return true;
}

// The underrun sample of one second (MDP_INTR_STATUS bit 26): with the DPU driven by page flips the bit is cleared after each count, else
// only sampled (a sticky bit counts once).
void sampleUnderrun(const NativeDpuProbePlan &plan,bool *seen,uint32_t *status) {
    *status=native_dpu_read(kMdpIntrStatus);
    if(!(*status&kIntf1Underrun)){*seen=false;return;}
    const uint32_t before=native_dpu_underruns;
    if(plan.clearUnderrun && native_dpu_flip_stats[7]==1) {
        native_dpu_underruns=native_dpu_underruns+1;native_dpu_write(kMdpIntrClear,kIntf1Underrun);
    } else if(!*seen)native_dpu_underruns=native_dpu_underruns+1;
    *seen=true;
    if(native_dpu_underruns!=before)pagani_display_underrun(native_dpu_underruns);
}

// The display driver's counters for the TE/FLIP rows.
NativeDpuRowInputs driverInputs() {
    NativeDpuRowInputs in={};
    in.locked=uint8_t(native_dpu_te_stats[0]!=0);in.relocks=uint32_t(native_dpu_te_stats[2]);
    in.flipping=uint8_t(native_dpu_flip_stats[0]!=0 || native_dpu_flip_stats[7]!=0);
    in.flips=native_dpu_flip_stats[0];in.latches=native_dpu_flip_stats[1];in.latchMaxUs=native_dpu_flip_stats[2];
    in.latchSumUs=native_dpu_flip_stats[3];in.late=native_dpu_flip_stats[4];in.timeouts=native_dpu_flip_stats[5];
    in.underruns=native_dpu_underruns;
    return in;
}

void probeThread(void *,wait_result_t) {
    thread_set_thread_name(current_thread(),"pagani-dpu-probe");
    const NativeDpuProbePlan plan=gPlan;
    native_dpu_probe[3]=plan.mode;
    native_dpu_probe[0]=1;
    if(plan.delayMs)IOSleep(plan.delayMs);
    // pagani-dpu-probe=3: the old behaviour, guard off. It hung the 13T 150 s after boot.
    if(plan.full)gFull=1;
    const bool mdss=plan.counters || plan.underrun;
    if(mdss && !native_dpu_map()){native_dpu_probe[0]=0xffffffffu;return;}
    const bool gdsc=plan.power && mapGdsc();
    native_dpu_probe[0]=2;
    const uint64_t hz=ticksPerSecond();
    char shown[PAGANI_DPU_ROWS][PAGANI_GPU_ROW_BYTES]={};
    unsigned shownCount=0;
    NativeDpuRates rates;
    bool urSeen=false;
    uint32_t lastCore=0,lastInt2=0,gdscChanges=0;
    for(uint32_t second=0;;++second) {
        if(second)IOSleep(1000);
        native_dpu_probe[5]=second;
        char text[PAGANI_DPU_ROWS][NATIVE_DPU_ROW_BYTES]={};
        unsigned count=0;
        if(native_dpu_idle) {
            // The display is off (with pagani-te=1 its autorefresh too): nothing proves the MDP stays clocked, so nothing is read.
            rates.reset();urSeen=false;
            if(plan.rows)count=native_dpu_idle_rows(plan.mode,second,text);
        } else {
            uint32_t status=0;
            if(native_dpu_sample_underrun(plan,native_dpu_flip_stats[7]))sampleUnderrun(plan,&urSeen,&status);
            if(!plan.rows)continue;
            NativeDpuSafeRowInputs in={};
            in.common=driverInputs();
            in.mode=plan.mode;in.underrunsCounted=uint8_t(native_dpu_sample_underrun(plan,native_dpu_flip_stats[7]));
            in.refused=native_dpu_refused;in.refusedOffset=native_dpu_probe[4];in.seconds=second;
            if(plan.power) {
                const uint32_t core=gdsc?gGdsc[kGdscCore/4]:0,int2=gdsc?gGdsc[kGdscCoreInt2/4]:0;
                if(second && (core!=lastCore || int2!=lastInt2))++gdscChanges;
                lastCore=core;lastInt2=int2;
                count=native_dpu_power_rows(gdsc,core,int2,gdscChanges,second,text);
            } else {
                NativeDpuCounters c={};
                if(!native_dpu_counters(&c))continue;
                const uint64_t now=mach_absolute_time();
                rates.add(now,c,5*hz);
                if(second==0)native_dpu_probe[0]=3;
                in.autorefresh=c.autorefresh;in.intervals=rates.intervals();
                uint64_t teLast=0;
                rates.interval(0,hz,&teLast,&in.common.frames);
                rates.extremes(hz,&in.teMinMillihertz,&in.teMaxMillihertz,&in.framesMin,&in.framesMax);
                // The TE lock's period when it holds (pagani-te=1), else the TE count over the window.
                if(native_dpu_te_stats[0] && native_dpu_te_stats[1])in.common.teMillihertz=1000000000000ull/native_dpu_te_stats[1];
                else in.common.teMillihertz=rates.teMillihertz(hz);
                if(!in.intervals && !in.common.locked)continue;              // nothing to show before the first interval
                if(plan.counters && !plan.flipPath && !plan.full) {
                    count=native_dpu_counter_rows(in,text);
                } else {
                    // Modes 2 and 3: a snapshot at the start and every ten seconds (mode 3: the full one, with the apps SMMU, at the
                    // start only), in between the registers a flip moves.
                    if(!gProbe.valid || (plan.flipPath && second%10==0)) {
                        if(plan.full){fullSnapshot(&gProbe);snapshotSmmu(&gProbe);}
                        else native_dpu_snapshot(&gProbe);
                        native_dpu_probe[1]=native_dpu_probe[1]+1;
                    } else {
                        gProbe.autorefresh=c.autorefresh;gProbe.mdpIntrStatus=status;
                        for(unsigned k=0;k<kCtlCount;++k)if(plan.full || k==0)gProbe.ctl[k].flush=native_dpu_read(kCtl0+kCtlStride*k+kCtlFlush);
                        for(unsigned p=0;p<kPipeCount;++p) {
                            if(!((gProbe.readPipes>>p)&1))continue;
                            gProbe.pipe[p].src0=native_dpu_read(kPipeBase[p]+kSrc0Addr);gProbe.pipe[p].src1=native_dpu_read(kPipeBase[p]+kSrc1Addr);
                        }
                    }
                    gProbe.autorefresh=c.autorefresh;
                    PE_Video video={};
                    NativeDpuScanout scanout={};
                    const bool fetching=!PE_current_console(&video) && video.v_baseAddr && video.v_rowBytes && video.v_height &&
                                        native_dpu_scanout(gProbe,video.v_baseAddr,kSplashBytes,&scanout);
                    count=native_dpu_rows(gProbe,fetching?&scanout:nullptr,in.common,text);
                    if(!plan.full)native_dpu_path_row(gProbe,in,text[3]);
                }
            }
        }
        if(!count)continue;
        bool changed=count!=shownCount;
        for(unsigned r=0;r<count && !changed;++r)changed=strncmp(text[r],shown[r],PAGANI_GPU_ROW_BYTES)!=0;
        if(!changed)continue;
        for(unsigned r=0;r<count;++r)strlcpy(shown[r],text[r],PAGANI_GPU_ROW_BYTES);
        shownCount=count;native_dpu_probe[0]=4;native_dpu_probe[2]=native_dpu_probe[2]+1;
        boot_progress_dpu_rows((const char (*)[PAGANI_GPU_ROW_BYTES])shown,count);
    }
}
}  // namespace

static_assert(int(NATIVE_DPU_ROW_BYTES)==int(PAGANI_GPU_ROW_BYTES),"the probe rows are BootProgress status rows");
static_assert(int(PAGANI_DPU_ROWS)>=5,"the snapshot rows and the FLIP row fit the probe's rows");

bool native_dpu_map(void) {
    if(gRegs)return true;
    // Only while the boot loader's display runs (its GOP console is up): then MDSS is powered and INTF_1 refreshes the panel.
    PE_Video video={};
    if(PE_current_console(&video) || !video.v_baseAddr)return false;
    IODeviceMemory *memory=IODeviceMemory::withRange(kMdssBase,kMdssBytes);
    IOMemoryMap *map=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
    OSSafeReleaseNULL(memory);
    if(!map || !map->getVirtualAddress()){OSSafeReleaseNULL(map);return false;}
    if(!OSCompareAndSwapPtr(nullptr,map,reinterpret_cast<void * volatile *>(&gMdss)))map->release();
    gRegs=reinterpret_cast<volatile uint32_t *>(gMdss->getVirtualAddress());
    return true;
}
// Mapping a register window is not an access; every read and write below passes the guard first (outside pagani-dpu-probe=3).
uint32_t native_dpu_read(uint32_t offset) {
    volatile uint32_t *regs=gRegs;
    return regs && permitted(offset,false)?regs[offset/4]:0;
}
void native_dpu_write(uint32_t offset,uint32_t value) {
    volatile uint32_t *regs=gRegs;
    if(regs && permitted(offset,true))regs[offset/4]=value;
}
bool native_dpu_counters(NativeDpuCounters *c) {
    if(!c || !native_dpu_map())return false;
    c->frame=native_dpu_read(kIntf1+kIntfFrameCount);
    c->intCount=native_dpu_read(kIntf1+kTearIntCount);
    c->autorefresh=native_dpu_read(kIntf1+kTearAutorefresh);
    return true;
}
// The flip path (see NativeDpu.h). CTL 0's pipes are added to the guard before they are read; pipes CTL 0 does not name are never read.
bool native_dpu_snapshot(NativeDpuState *s) {
    if(!s || !native_dpu_map())return false;
    *s={};
    readIntf1(s);
    s->mdpIntrEn=native_dpu_read(kMdpIntrEn);s->mdpIntrStatus=native_dpu_read(kMdpIntrStatus);
    readCtl(s,0);
    const uint16_t pipes=native_dpu_ctl0_pipes(s->ctl[0],s->fetchActive[0]);
    __atomic_fetch_or(&gActivePipes,uint32_t(pipes),__ATOMIC_RELAXED);
    for(unsigned p=0;p<kPipeCount;++p)if((pipes>>p)&1)s->pipe[p]=readPipe(p);
    s->readPipes=pipes;s->scope=kDpuScopeFlipPath;s->valid=1;
    return true;
}
// The flip trial's former check of the MDP streams' context-bank fault status: the apps SMMU is not read outside pagani-dpu-probe=3.
extern "C" uint32_t native_dpu_mdp_fsr(void) {
    return 0;
}
void native_dpu_probe_start(void) {
    static volatile SInt32 started;
    const NativeDpuProbePlan plan=native_dpu_probe_plan(bootValue("pagani-dpu-probe"),enabled("pagani-te"),enabled("pagani-fb-flip"));
    if(!plan.thread)return;
    if(!OSCompareAndSwap(0,1,&started))return;
    gPlan=plan;
    thread_t thread=nullptr;
    if(kernel_thread_start(probeThread,nullptr,&thread)==KERN_SUCCESS)thread_deallocate(thread);
    else native_dpu_probe[0]=0xfffffff9u;
}
