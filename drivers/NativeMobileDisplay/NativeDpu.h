// SPDX-License-Identifier: BSD-2-Clause-Patent
// The display processor (DPU 12.0, SM8750) as the GOP display path sees it: register facts, what the boot loader left running, and the
// pure decisions made from it (which registers may be touched at all, which pipes fetch the scanout, which CTL flushes them, the writes
// of a page flip). Register offsets are relative to MDSS (0xae00000); one device mapping of 0x90000 bytes covers MDSS and the MDP block at
// 0xae01000. Sources: Linux drm/msm/disp/dpu1 (dpu_12_0_sm8750.h, dpu_hw_{ctl,sspp,intf,interrupts}.c) rebased on 0xae00000, and the
// PKX110 live device tree (qcom,mdss_mdp@ae00000: sde-intf-off 0x36000 for INTF_1 = DSI0, sde-ctl-off 0x16000.., sde-ctl-display-pref
// "primary" for CTL 0; qcom,mdss_dsi_ctrl0@ae94000 maps "mdp_intf_base" <0xae36000 0x300>; apps-smmu@15000000 with qcom,handoff-smrs
// <0x800 0x2>; disp_cc_mdss_core_gdsc@af09000 and disp_cc_mdss_core_int2_gdsc@af0b000). Nothing here touches hardware: DpuProbe.cpp
// reads and writes, through the guard below.
//
// Safety (2026-10-30 12:15: the full snapshot of the old pagani-dpu-probe=1 hung the 13T about 150 s after boot, no panic, cold reset
// about 10 s later): outside pagani-dpu-probe=3 the DPU is touched only inside the INTF_1 window (its tear check runs the autorefresh that
// sends every frame of the command-mode panel, so it is clocked while the panel shows anything), MDP_INTR_EN/STATUS/CLEAR of the MDP top,
// CTL 0 and the pipes CTL 0 names active while it drives INTF_1. Never the apps SMMU, never a sweep over CTLs or pipes.
#pragma once
#include <stdint.h>

namespace nativedpu {
inline constexpr uint64_t kMdssBase=0x0ae00000, kMdssBytes=0x90000;
inline constexpr uint32_t kHwIntrStatus=0x10;                                  // bit 0: MDP
inline constexpr uint32_t kMdpIntrEn=0x1010, kMdpIntrStatus=0x1014, kMdpIntrClear=0x1018;
inline constexpr uint32_t kIntf1Underrun=1u<<26, kIntf1Vsync=1u<<27;
inline constexpr uint32_t kIntf1=0x36000;                                     // INTF_1 = DSI0
// The part of INTF_1 Android's DSI driver maps for itself (dsi ctrl0 "mdp_intf_base" <0xae36000 0x300>): the timing engine, the frame and
// line counters and the whole tear check (0x280..0x2b4). The only DPU registers pagani-dpu-probe=1 and pagani-te=1 touch.
inline constexpr uint32_t kIntf1Window=0x300;
inline constexpr uint32_t kIntfTimingEngine=0x000, kIntfFrameCount=0x0ac, kIntfLineCount=0x0b0, kIntfStatus=0x26c,
                          kTearMdpVsyncSel=0x280, kTearCheckEn=0x284, kTearSyncVsync=0x288, kTearSyncHeight=0x28c,
                          kTearVsyncInit=0x294, kTearIntCount=0x298, kTearStartPos=0x2a0, kTearRdPtrIrq=0x2a4,
                          kTearLineCount=0x2b0, kTearAutorefresh=0x2b4;
inline constexpr uint32_t kTearExternalTe=1u<<20, kAutorefreshEnable=1u<<31;
inline constexpr uint32_t kIntf1IntrEn=0x361c0, kIntf1TearIntrEn=0x36800, kIntf1TearIntrStatus=0x36804, kIntf1TearIntrClear=0x36808;
inline constexpr uint32_t kCtl0=0x16000, kCtlStride=0x1000;
inline constexpr unsigned kCtlCount=6;
inline constexpr uint32_t kCtlTop=0x14, kCtlFlush=0x18, kCtlStart=0x1c, kCtlIntfActive=0xf4, kCtlFetchActive=0xfc,
                          kCtlPipeActive=0x12c, kCtlLayerActive=0x130;
inline constexpr uint32_t kCtlIntf1=1u<<1;                                     // CTL_INTF_ACTIVE bit of INTF_1
// The ten source pipes, VIG0..3 then DMA0..5: block offset, CTL flush bit (dpu_hw_ctl.c:190-246) and CTL pipe-active bit (fetch_tbl).
inline constexpr unsigned kPipeCount=10;
inline constexpr uint32_t kPipeBase[kPipeCount]={0x5000,0x7000,0x9000,0xb000,0x25000,0x27000,0x29000,0x2b000,0x2d000,0x2f000};
inline constexpr uint32_t kPipeBytes=0x344;                                   // sde-sspp-src-size, dpu_12_0 .len
inline constexpr uint32_t kPipeFlush[kPipeCount]={1u<<0,1u<<1,1u<<2,1u<<18,1u<<11,1u<<12,1u<<24,1u<<25,1u<<13,1u<<14};
inline constexpr uint32_t kPipeActive[kPipeCount]={1u<<16,1u<<17,1u<<18,1u<<19,1u<<0,1u<<1,1u<<2,1u<<3,1u<<4,1u<<5};
inline constexpr char kPipeName[kPipeCount][5]={"VIG0","VIG1","VIG2","VIG3","DMA0","DMA1","DMA2","DMA3","DMA4","DMA5"};
inline constexpr uint32_t kSrcSize=0x00, kSrc0Addr=0x14, kSrc1Addr=0x18, kSrcYStride0=0x24, kSrcFormat=0x30, kSrcOpMode=0x38,
                          kMultirectOpMode=0x170;
inline constexpr uint32_t kMultirectRec1=1u<<1;                                // MULTIRECT_OPMODE: rect 1 in use (DPU_SSPP_RECT_1)
// Apps SMMU (apps-smmu@15000000, 0x100000 bytes on PKX110) and the MDP streams of DT qcom,handoff-smrs. Read only by
// pagani-dpu-probe=3 (see DpuProbe.cpp): the Gunyah hypervisor owns this SMMU.
inline constexpr uint64_t kAppsSmmuBase=0x15000000, kAppsSmmuBytes=0x100000;
inline constexpr unsigned kStreamCount=4;
inline constexpr uint16_t kMdpStreams[kStreamCount]={0x800,0x801,0x802,0x804};
// The display's power domains in the display clock controller (dispcc@af00000, LDT disp_cc_mdss_core_gdsc@af09000 and
// disp_cc_mdss_core_int2_gdsc@af0b000): GDSCR bit 31 PWR_ON (status), bit 1 HW_CONTROL (the hardware may collapse it), bit 0
// SW_COLLAPSE (Linux gdsc.c). Read only by pagani-dpu-probe=4; the clock controller sits behind GCC_DISP_AHB_CLK, which Linux keeps on.
inline constexpr uint64_t kGdscBase=0x0af09000, kGdscBytes=0x3000;
inline constexpr uint32_t kGdscCore=0x0000, kGdscCoreInt2=0x2000;            // offsets from kGdscBase
inline constexpr uint32_t kGdscPwrOn=1u<<31, kGdscHwControl=1u<<1, kGdscSwCollapse=1u<<0;
// Scanout buffers in the splash region (LDT /reserved-memory/splash_region <0xfc800000 0x2b00000>): B0 is the GOP framebuffer, B1 and B2
// follow it at one frame each (0xc3f000 for 1216x2640x4); 0xfecbd000 < 0xff300000. The display's context bank maps the whole region one
// to one (LDT smmu_sde_iommu_region_partition <... 0xfc800000 0x2b00000>), so B1 and B2 need no SMMU change.
inline constexpr uint64_t kSplashBase=0xfc800000, kSplashBytes=0x2b00000;
inline constexpr unsigned kScanoutBuffers=3;
// TE lock (ARCHITECTURE 5.3.3 item 2): 50 us samples for 100 ms, then a tick 50 us after each predicted TE; 5 ms dense relock.
inline constexpr unsigned kLockSampleUs=50, kLockWindowUs=100000, kTickLeadUs=50, kRelockUs=5000;
// Page flips (5.3.4): a flush latches within about one TE (sheng i562: 5.4 ms, no CTL_START); 50 ms without latching gives up.
inline constexpr unsigned kLatchPollUs=500, kLatchTimeoutUs=50000;
// Panel AA594 (sa_fhd_120 default timing): 2640 lines; the write pointer leaves 1..2639 between transfers.
inline constexpr unsigned kPanelLines=2640;
// pagani-dpu-probe: 0 off; 1 the INTF_1 counters only (the safe mode: TE and frame rate); 2 also the registers a page flip touches
// (CTL 0, the pipes it names, MDP_INTR_STATUS), read only, to try them before pagani-fb-flip=1; 3 the old full snapshot with the apps SMMU
// (it hung the 13T at 150 s: only to reproduce that); 4 no MDSS access at all, the GDSCs' power state from the display clock controller.
// Other values: off.
enum : uint8_t { kProbeOff=0, kProbeCounters=1, kProbeFlipPath=2, kProbeFull=3, kProbePower=4 };
// Modes 1, 2 and 4 start 10 s after the display (INTF_1 runs from the boot loader on); mode 3 keeps its 150 s.
inline constexpr unsigned kProbeDelayMs=10000, kProbeFullDelayMs=150000;
}

struct NativeDpuPipe {uint32_t size,src0,src1,ystride0,format,opmode,multirect;};
struct NativeDpuCtl {uint32_t top,flush,intfActive,pipeActive,layerActive;};
// One MDP stream's apps-SMMU route: the matching stream-map group (slot 0xff: none), its S2CR, and for a translating route the context
// bank's registers (bank 0xff: bypass or fault).
struct NativeDpuStream {uint32_t smr,s2cr,cbar,sctlr,tcr,mair0,fsr;uint64_t ttbr0;uint16_t sid;uint8_t slot,bank;};
// What the snapshot read: the flip path (INTF_1 window, MDP interrupts, CTL 0, the pipes CTL 0 names) or, with pagani-dpu-probe=3 only,
// everything.
enum : uint8_t { kDpuScopeNone=0, kDpuScopeFlipPath=1, kDpuScopeFull=2 };
struct NativeDpuState {
    uint32_t hwIntrStatus,mdpIntrEn,mdpIntrStatus;
    uint32_t timingEngine,frameCount,lineCount,intfStatus,vsyncSel,tearCheck,syncVsync,syncHeight,vsyncInit,intCount,
             startPos,rdPtrIrq,tearLineCount,autorefresh;
    NativeDpuCtl ctl[nativedpu::kCtlCount];
    NativeDpuPipe pipe[nativedpu::kPipeCount];
    NativeDpuStream stream[nativedpu::kStreamCount];
    uint32_t smmuCr0,smmuIdr0,smmuIdr1,smmuIdr2;
    uint8_t smmuRead,valid;
    uint8_t scope;                     // kDpuScope*
    uint16_t readPipes;                // bit p: pipe p was read
    uint32_t fetchActive[nativedpu::kCtlCount];   // CTL_FETCH_PIPE_ACTIVE (+0xfc) of the CTLs read
};
// The INTF_1 counters of the safe mode, one reading: the frame count (+0x0ac), the tear check's read pointer (+0x298: TE frames [31:16],
// line [15:0]) and the autorefresh configuration (+0x2b4: bit 31 on, [15:0] TEs per frame).
struct NativeDpuCounters {uint32_t frame,intCount,autorefresh;};
// The pipes that fetch the scanout buffer at `base` (frameBytes long) and how to move them: what the boot loader programmed is kept as
// offsets from the buffer, so a flip only replaces the buffer base.
struct NativeDpuScanout {
    uint16_t pipes;                    // bit p: pipe p's SRC0 points into the buffer
    uint16_t rec1;                     // bit p: pipe p is multirect and its rect 1 (SRC1) points into it too
    uint8_t ctl;                       // the CTL whose flush latches them
    uint32_t flush;                    // their flush bits
    uint32_t offset0[nativedpu::kPipeCount],offset1[nativedpu::kPipeCount];
};
struct NativeDpuWrite {uint32_t offset,value;};

static inline bool native_dpu_inside(uint32_t address,uint64_t base,uint64_t bytes) {
    return address>=base && (uint64_t)address-base<bytes;
}

// Where an MDSS register offset lies, in the order of the hazard ranking (dpu-safety.md), least likely to hang first. `pipe` (optional)
// receives the pipe of kDpuPipe.
enum NativeDpuBlock : uint8_t {
    kDpuIntf1Window,                   // INTF_1 0x36000..0x362ff: live while the panel refreshes (autorefresh, TE counting)
    kDpuMdpInterrupts,                 // MDP top 0x1010..0x101b: the interrupt aggregator latching INTF_1's events every frame
    kDpuCtl0,                          // CTL 0 0x16000..0x16fff: the primary display's CTL (DT), expected in the boot loader's path
    kDpuPipe,                          // a source pipe's block: live only if CTL 0 names it active
    kDpuOtherMdss,                     // everything else in the mapping: CTL 1..5, LMs, DSPPs, other INTF registers, MDSS top: unproven
    kDpuOutside                        // unaligned or past the mapping
};
static inline NativeDpuBlock native_dpu_block(uint32_t offset,unsigned *pipe=nullptr) {
    using namespace nativedpu;
    if((offset&3) || offset>=kMdssBytes)return kDpuOutside;
    if(offset>=kIntf1 && offset-kIntf1<kIntf1Window)return kDpuIntf1Window;
    if(offset>=kMdpIntrEn && offset<=kMdpIntrClear)return kDpuMdpInterrupts;
    if(offset>=kCtl0 && offset-kCtl0<kCtlStride)return kDpuCtl0;
    for(unsigned p=0;p<kPipeCount;++p)if(offset>=kPipeBase[p] && offset-kPipeBase[p]<kPipeBytes){if(pipe)*pipe=p;return kDpuPipe;}
    return kDpuOtherMdss;
}
// The guard of native_dpu_read/native_dpu_write outside pagani-dpu-probe=3. Reads: the INTF_1 window, MDP_INTR_EN/STATUS, CTL 0, and the
// blocks of the pipes in `activePipes` (those CTL 0 named, learned by the flip-path snapshot). Writes: only what TE lock and page flips
// write (TEAR_SYNC_CONFIG_VSYNC and TEAR_AUTOREFRESH_CONFIG of INTF_1, MDP_INTR_CLEAR, CTL 0's flush, an active pipe's SRC0/SRC1 address).
static inline bool native_dpu_allowed(uint32_t offset,bool write,uint16_t activePipes) {
    using namespace nativedpu;
    unsigned p=0;
    switch(native_dpu_block(offset,&p)) {
    case kDpuIntf1Window: return !write || offset==kIntf1+kTearSyncVsync || offset==kIntf1+kTearAutorefresh;
    case kDpuMdpInterrupts: return write?offset==kMdpIntrClear:offset!=kMdpIntrClear;
    case kDpuCtl0: return !write || offset==kCtl0+kCtlFlush;
    case kDpuPipe:
        if(!((activePipes>>p)&1))return false;
        return !write || offset==kPipeBase[p]+kSrc0Addr || offset==kPipeBase[p]+kSrc1Addr;
    default: return false;
    }
}
// The pipes a page flip may read and move: those CTL 0 names active (CTL_PIPE_ACTIVE, or CTL_FETCH_PIPE_ACTIVE `fetchActive`), and only
// when CTL 0 drives INTF_1. 0: CTL 0 is not the boot loader's display path, so nothing else is read (no sweep for another CTL).
static inline uint16_t native_dpu_ctl0_pipes(const NativeDpuCtl &ctl0,uint32_t fetchActive) {
    using namespace nativedpu;
    if(!(ctl0.intfActive&kCtlIntf1))return 0;
    uint16_t pipes=0;
    const uint32_t named=ctl0.pipeActive|fetchActive;
    for(unsigned p=0;p<kPipeCount;++p)if(named&kPipeActive[p])pipes|=uint16_t(1u<<p);
    return pipes;
}

// What the display probe thread does for the boot arguments pagani-dpu-probe (any value), pagani-te and pagani-fb-flip (1: on).
struct NativeDpuProbePlan {
    uint8_t mode;                      // nativedpu::kProbe*
    bool thread;                       // a probe thread runs at all
    bool rows;                         // it publishes rows 14-18 (boot_progress_dpu_rows)
    bool counters;                     // it reads the INTF_1 counters once a second (modes 1 and 2; mode 3 reads them too)
    bool flipPath;                     // it reads CTL 0 and the pipes CTL 0 names (mode 2)
    bool full;                         // the full snapshot and the apps SMMU, guard off (mode 3 only; it hung the 13T)
    bool power;                        // the GDSCs from the display clock controller, no MDSS access (mode 4)
    bool underrun;                     // it samples MDP_INTR_STATUS bit 26 once a second (the underrun counter): modes 2 and 3, and
                                       // page flips once their first-flip trial passed (mode 1 and 4 alone never read the MDP top)
    bool clearUnderrun;                // and clears it after counting (only while page flips drive the DPU)
    unsigned delayMs;                  // before its first access
};
static inline NativeDpuProbePlan native_dpu_probe_plan(unsigned probe,bool te,bool flip) {
    using namespace nativedpu;
    (void)te;                          // TE lock reads INTF_1 by itself (NativeLegacyDisplay); it starts no thread and no MDP-top reads
    NativeDpuProbePlan plan={};
    plan.mode=probe<=kProbePower?uint8_t(probe):kProbeOff;
    plan.rows=plan.mode!=kProbeOff;
    plan.counters=plan.mode==kProbeCounters || plan.mode==kProbeFlipPath || plan.mode==kProbeFull;
    plan.flipPath=plan.mode==kProbeFlipPath;
    plan.full=plan.mode==kProbeFull;
    plan.power=plan.mode==kProbePower;
    plan.underrun=flip || plan.flipPath || plan.full;
    plan.clearUnderrun=flip;
    plan.thread=plan.rows || plan.underrun;
    plan.delayMs=plan.full?kProbeFullDelayMs:plan.rows?kProbeDelayMs:0;
    return plan;
}
// Whether the probe thread reads MDP_INTR_STATUS this second: modes 2 and 3 always; with page flips only once their trial passed
// (`flipTrial`: native_dpu_flip_stats[7], 1 passed), since only then does the display drive the MDP top's path at all.
static inline bool native_dpu_sample_underrun(const NativeDpuProbePlan &plan,uint64_t flipTrial) {
    return plan.underrun && (plan.flipPath || plan.full || flipTrial==1);
}

// false when nothing fetches the buffer (the MDP reads another address: a translated IOVA, or a different buffer).
static inline bool native_dpu_scanout(const NativeDpuState &state,uint64_t base,uint64_t frameBytes,NativeDpuScanout *out) {
    using namespace nativedpu;
    *out={};
    if(!state.valid || !frameBytes || base>0xffffffffull || frameBytes>0xffffffffull-base)return false;
    uint32_t active=0;
    for(unsigned p=0;p<kPipeCount;++p) {
        const NativeDpuPipe &pipe=state.pipe[p];
        if(!pipe.size || !native_dpu_inside(pipe.src0,base,frameBytes))continue;
        out->pipes|=uint16_t(1u<<p);out->flush|=kPipeFlush[p];active|=kPipeActive[p];
        out->offset0[p]=uint32_t(pipe.src0-base);
        if((pipe.multirect&kMultirectRec1) && native_dpu_inside(pipe.src1,base,frameBytes)) {
            out->rec1|=uint16_t(1u<<p);out->offset1[p]=uint32_t(pipe.src1-base);
        }
    }
    if(!out->pipes)return false;
    // The CTL that names these pipes active; else the one driving INTF_1; else CTL 0 (the primary display's, sde-ctl-display-pref).
    // A flip-path snapshot holds CTL 0 only, so this is CTL 0 outside pagani-dpu-probe=3.
    unsigned chosen=kCtlCount;
    for(unsigned c=0;c<kCtlCount && chosen==kCtlCount;++c)if(state.ctl[c].pipeActive&active)chosen=c;
    for(unsigned c=0;c<kCtlCount && chosen==kCtlCount;++c)if(state.ctl[c].intfActive&kCtlIntf1)chosen=c;
    out->ctl=uint8_t(chosen==kCtlCount?0:chosen);
    return true;
}
// The address writes that move every scanout pipe to the buffer at `base`, then the CTL flush (always last). Returns the count.
static inline unsigned native_dpu_flip_writes(const NativeDpuScanout &scanout,uint64_t base,NativeDpuWrite *writes,unsigned room) {
    using namespace nativedpu;
    unsigned n=0;
    if(base>0xffffffffull)return 0;
    for(unsigned p=0;p<kPipeCount;++p) {
        if(!(scanout.pipes&(1u<<p)))continue;
        if(n<room)writes[n]={kPipeBase[p]+kSrc0Addr,uint32_t(base)+scanout.offset0[p]};
        ++n;
        if(scanout.rec1&(1u<<p)){if(n<room)writes[n]={kPipeBase[p]+kSrc1Addr,uint32_t(base)+scanout.offset1[p]};++n;}
    }
    if(n<room)writes[n]={kCtl0+kCtlStride*scanout.ctl+kCtlFlush,scanout.flush};
    ++n;
    return n<=room?n:0;
}
// The stream-map group that routes `sid` (valid SMR whose ID matches under its mask), or -1.
static inline int native_dpu_stream_slot(const uint32_t *smr,unsigned groups,uint16_t sid) {
    for(unsigned i=0;i<groups;++i) {
        const uint32_t s=smr[i];
        if(!(s&0x80000000u))continue;
        const uint32_t mask=(s>>16)&0x7fff;
        if(((s^sid)&0xffff&~mask)==0)return int(i);
    }
    return -1;
}

extern "C" {
// DpuProbe.cpp. Every access goes through one device mapping of MDSS, made on first use; nothing is read before the boot loader's
// display is known to be running (the GOP console is up). Outside pagani-dpu-probe=3 each access passes native_dpu_allowed (with the
// pipes CTL 0 named at the flip-path snapshots so far); a refused read returns 0, a refused write is dropped, both are counted
// (native_dpu_refused) and the first is kept for the probe's rows.
bool native_dpu_map(void);
uint32_t native_dpu_read(uint32_t offset);
void native_dpu_write(uint32_t offset,uint32_t value);
// The three INTF_1 counters of the safe mode (NativeDpuCounters). No write, no sleep.
bool native_dpu_counters(NativeDpuCounters *counters);
// The flip path, read only: the INTF_1 window's registers, MDP_INTR_EN/STATUS, CTL 0, and only when CTL 0 drives INTF_1 the pipes it
// names active (scope kDpuScopeFlipPath). Never the other CTLs, the other pipes or the apps SMMU: the full snapshot exists only inside
// DpuProbe.cpp, for pagani-dpu-probe=3. No write, no sleep.
bool native_dpu_snapshot(NativeDpuState *state);
// Formerly the MDP streams' context-bank fault status from the apps SMMU (the flip trial's check 6). The apps SMMU is no longer read for
// it (the hypervisor owns it, and the 150 s hang read it): always 0, so that check always passes.
uint32_t native_dpu_mdp_fsr(void);
// The probe thread for pagani-dpu-probe / pagani-fb-flip (native_dpu_probe_plan); started once the display runs (Stage7Probe). Mode 1
// shows the TE rate, the INTF_1 frame rate and the autorefresh configuration on rows 14-16, mode 2 adds CTL 0 and its pipes, mode 3 is
// the old full snapshot, mode 4 the GDSCs. Rows are published only when their text changed.
void native_dpu_probe_start(void);
// Underruns of INTF_1 (MDP_INTR_STATUS bit 26), counted once a second by the probe thread with pagani-fb-flip=1 once the flips run (the
// bit is cleared after each count) or pagani-dpu-probe=2/3 (only sampled: a sticky bit counts once).
extern volatile uint32_t native_dpu_underruns;
// Page-flip statistics for the FLIP row (NativeLegacyDisplay): flips, latches, latch time max and sum (us), late frames, latch timeouts,
// mailbox replacements, first-flip trial result (0 not run, 1 passed, other: the failing check).
extern volatile uint64_t native_dpu_flip_stats[8];
// TE lock state for the TE row: [0] locked, [1] period in ns, [2] relocks, [3] reported refresh Hz.
extern volatile uint64_t native_dpu_te_stats[4];
// Nonzero while the display is off (set by the display driver; with pagani-te=1 its autorefresh is off then: the MDP sends nothing, so
// nothing proves it stays clocked): the probe thread reads no register then. 0 unless the display driver sets it.
extern volatile uint32_t native_dpu_idle;
// Accesses the guard refused (see native_dpu_read).
extern volatile uint32_t native_dpu_refused;
}
